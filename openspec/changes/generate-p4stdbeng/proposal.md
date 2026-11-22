# Proposal

## Why

The operators layer was built to be torn out and handed to students: the join headers carry per-method contracts (purpose, parameters, return value) with lecture-slide references, every constructor parameter is documented, and the operators suite, the join suite, and the exec suite's join paths form a complete grading contract. The parent course materials set the pattern: `../p3stdbeng` is p3dbeng with exactly one module's algorithm methods reduced to guided stubs (`// TODO(student):` sketches in every body), the archived changes and `EXPLAIN.md` removed (they are the answer key), `.opencode/` stripped, the binaries renamed, the README rewritten as the student handout, and the full check suite kept as both specification and grader. This change generates `../p4stdbeng` — the student version of p4dbeng — by the same pattern: the whole engine provided and working, the two join algorithms' method bodies stubbed, and the suite green everywhere except the work students must do.

## What Changes

- **A new sibling project `../p4stdbeng`**, generated from this repository by copy and surgery:
  - The full engine is provided and working — TUP2 storage, both index types, the exec lineage (parse, bind, plan, EXPLAIN), the CLI, the benchmark, the catalog fixtures, and all ten check suites unchanged.
  - **The Volcano operators are provided in full** — `SeqScan`, `Filter`, `Project`, `Limit`, `NestedLoopsJoin` (the lecture's baseline join), `Partitions` (the hash join's bucket files), `RowIdScan` — plus `HashJoin`'s provided subtleties: `join_key`'s canonicalization, `hash_key`, and the `JoinKeyHash`/`JoinKeyEq` plumbing.
  - **`operators/IndexNestedLoopsJoin.cpp` is stubbed at the operator level**: the constructor stays as-is; the three Volcano methods — `open`, `next`, `close` — become guided stubs, each opening with `// TODO(student):` plus the algorithm sketch and its lecture-slide reference, with compile-correct failure bodies (no assert, `next` answers `nullopt`) so the suite fails honestly rather than crashing.
  - **`operators/HashJoin.cpp` is stubbed at the phase level**: the provided key machinery and the constructor stay as-is; the five method bodies — `open` (Phase I), `partition_side` (Phase I routing), `load_partition` (Phase II setup), `next` (Phase II streaming), `close` (the query-end erasure) — become guided stubs in the same form. The phases are the assignment, exactly as the header's teaching contract states.
  - The header files are untouched — the contracts are the assignment.
  - The answer key is removed: no `openspec/changes/archive/`, no `EXPLAIN.md`, no `.opencode/`.
  - The binaries are renamed (`p4stdbeng`, `p4stdbeng_cli`, `p4stdbeng_bench`) and the README is rewritten as the student handout ("everything works except the two join algorithms…"), following `p3stdbeng`'s.
  - A neutral `conf.json` (hash join enabled, timing off) so the student CLI exercises their own hash join.
  - `git init` with an initial commit.
- **The designed failure surface**: in the student tree the storage, sql, index, B+tree, loader, config, and CLI suites still pass; the operators suite fails exactly in its index-NLJ and hash-join sections, the join suite fails throughout, and the exec suite fails exactly in its join-dependent checks (join plans' runs, the three-way chains, the oracle rounds). Implementing the eight stubbed methods makes the full suite pass.

## Capabilities

### New Capabilities

None — the generated project's behavior is this repository's existing behavior; the change is a distribution build. `skip_specs` is declared.

### Modified Capabilities

None.

## Impact

- A new directory `../p4stdbeng` (sibling of this repository); this repository's code is untouched except that the generation reads it.
- The suite-as-grader contract: the operators suite's two join sections, the join suite in full, and the exec suite's join-dependent checks define "done" for the students.
- Out of scope: the professor's assignment handout document (the `p3.doc`-style .docx) — the professor writes that separately.
