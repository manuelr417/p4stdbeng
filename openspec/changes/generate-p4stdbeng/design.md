# Design

## Context

See the proposal — Why. The reference is `../p3stdbeng`: p3dbeng with one module's algorithm methods stubbed, archive/EXPLAIN/.opencode removed, binaries renamed, README rewritten as the handout, and the full check suite kept as grader. The stub bodies follow one form: the original method signature kept, a `// TODO(student):` comment sketching the steps (with the algorithm's key facts and lecture-slide references), `(void)`-casts for the unused parameters, and a compile-correct failure return. The stub set and the provided set were fixed by the assignment's shape: **the students implement the methods of `IndexNestedLoopsJoin` and `HashJoin`; every other operator is provided.**

## Goals / Non-Goals

**Goals:**
- `../p4stdbeng` builds out of the box, runs its suite, and fails exactly on the join-dependent checks.
- The student's work surface is the eight method bodies of the two join operators — the Volcano lifecycle and the lecture's two algorithms — with signatures, contracts, constructor-parameter documentation, and slide references in the untouched headers.
- The solution (`../p4dbeng`) remains untouched and green.

**Non-Goals:**
- No assignment handout document (the .docx is the professor's to write).
- No changes to p4dbeng's code, specs, or suite.
- No grading automation beyond the supplied CTest suite.

## Decisions

### D1 — The stub set is the two operators' method bodies; everything else in `operators/` is provided
Stubbed (8 bodies): `IndexNestedLoopsJoin::{open, next, close}` — the Volcano lifecycle over the probe loop, the inner predicate, and the combination — and `HashJoin::{open, next, close, partition_side, load_partition}` — Phase I, its routing, Phase II's per-pair setup, its streaming, and the query-end erasure. Provided in place: both constructors (the parameter plumbing and the bucket-count assert), `HashJoin`'s provided subtleties (`join_key`, `hash_key`, `JoinKeyHash`, `JoinKeyEq` — the header's teaching contract names them), and the whole of `NestedLoopsJoin`, `SeqScan`, `Filter`, `Project`, `Limit`, `Partitions`, `RowIdScan`. Rationale: the baseline join is the lecture's stepping stone and the sibling example the stub sketches point to; `Partitions` is the bucket-file infrastructure the hash-join phases are staged over; the key machinery is the subtlety that would derail the assignment, exactly as the header declares.

### D2 — Stub bodies compile and fail honestly, without asserting
Each stub keeps the exact signature, opens with `// TODO(student):` naming the algorithm, its steps sketch, and its lecture-slide reference, then `(void)`-casts the parameters where any exist and returns the type's failure value (`std::nullopt` from `next`, nothing from the `void` methods). Unlike the model bodies, the stubs **keep no asserts**: a `next()` that asserted `running_` would abort the suite on the first call after a no-op `open()` — a crash, not a failing check. The no-op open / nullopt-next pair makes every join check fail as an answer mismatch (zero rows), which the suite reports cleanly.

### D3 — The failure surface is designed, then verified
The generation's acceptance, verified by running the student suite: the student tree **builds cleanly**, its suite runs to completion, and the result splits exactly — 1,098 checks pass (storage, sql, index, B+tree, loader, config suites in full; the operators suite's scan/filter/project/limit sections; the exec suite's non-join checks; the CLI suite) and 42 fail, every one of them join-dependent: the operators suite's index-NLJ and hash-join sections, the join suite throughout (both grammar forms, the three-way chains, the pushdown bypass rounds, the oracle rounds), and the exec suite's join-plan runs and differential checks. The suite's exit code is non-zero. This split was asserted as part of the generation's verification, so a stub that accidentally passes or a provided method accidentally broken is caught at generation time.

### D4 — The surgery list mirrors p3std exactly
From the copied tree: delete `openspec/changes/archive/` (the answer key — the joins and hash-join changes' design.md files hold the algorithm narratives), delete `EXPLAIN.md`, delete `.opencode/`, delete `build*/`/`data/`/`tmp/`/`.git`/`.idea` from the copy, rename the binaries and the CLI and bench sources (`p4stdbeng_cli.cpp`, check binary `p4stdbeng`, bench `p4stdbeng_bench.cpp`), set the CMake project name to `p4stdbeng` (and the `P4DBENG_CLI_PATH` define to the renamed CLI), keep `openspec/specs/` (the behavioral contract, including `sql-joins` and `volcano-operators`), keep the `tablecatalog/` fixtures and the lecture decks, write a neutral `conf.json` (hash join enabled — the student CLI must exercise their own hash join; timing off), and rewrite `README.md` as the student handout. `git init` with an initial commit at the end. Rationale: p3std's students received exactly this shape; the archived design documents are the only place the full solution narrative exists. `predicates.md` stays — it is teaching material for the assignment (the predicate machinery the inner-side filter rides on), not an answer key for it.

### D5 — The README handout names the work surface precisely
Following p3std's README form: the engine description (adapted from p4dbeng's), then the student paragraph — everything works except the two join algorithms; the methods declared in `operators/IndexNestedLoopsJoin.h` and `operators/HashJoin.h`, documented there in full with their lecture-slide references and per-parameter contracts, are blank stubs in the matching `.cpp` files; implement them until the check suite passes; the headers must not change; a suggested order (index nested loops first — the probe loop over a provided baseline to imitate — then the hash join's phases); the failing suites listed as the grader. The due date is written in the handout (December 3, 2026).

## Risks / Trade-offs

- [A stub that crashed instead of failing] → D2's assert-free bodies return failure values; the generation's run of the student suite (D3) caught nothing aborting — all 42 failures are check failures.
- [The executor builds join plans whose EXPLAIN text still renders] → Annotation is build-time and does not execute the stubs, so EXPLAIN-only checks stay green; every check that runs rows through the joins fails. The split (D3) confirms this is the observed surface.
- [Copy drift: p4dbeng moves on after the generation] → The student tree is a snapshot by design (like p3std); the generation is cheap to re-run if the solution ever changes.

## Migration Plan

Generate into a fresh `../p4stdbeng` (fail if it exists), verify the designed failure surface, init git and commit. The parent repository is untouched. Re-running the generation after changes to p4dbeng regenerates the snapshot.

## Open Questions

- None — the due date on the handout README is set (December 3, 2026).
