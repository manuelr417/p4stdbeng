# Tasks

## 1. Scaffold `../p4stdbeng`

- [x] 1.1 Copy this repository to a fresh `../p4stdbeng` (fail if it exists), excluding `build*/`, `data/`, `tmp/`, `.git`, `.idea`, `.opencode/`, `openspec/changes/archive/`, and `EXPLAIN.md`; keep `openspec/specs/`, `tablecatalog/`, the lecture decks, `predicates.md`, and every module source
- [x] 1.2 Rename the binaries and set the project identity: CMake project name `p4stdbeng`, check binary `p4stdbeng`, CLI source and binary `p4stdbeng_cli.cpp`/`p4stdbeng_cli`, bench `p4stdbeng_bench.cpp`/`p4stdbeng_bench`, the `P4DBENG_CLI_PATH` define retargeted, the check files' header comments and sandbox prefixes renamed; write the neutral `conf.json` (hash join enabled, timing off); verify the student tree builds cleanly as-is (no stubs yet)

## 2. Stub the two join operators

- [x] 2.1 Replace the method bodies in `../p4stdbeng/operators/IndexNestedLoopsJoin.cpp` (`open`, `next`, `close`) and `../p4stdbeng/operators/HashJoin.cpp` (`open`, `next`, `close`, `partition_side`, `load_partition`) with guided stubs per design D2 — each keeping the exact signature, opening with `// TODO(student):` plus the steps sketch and its lecture-slide reference, `(void)`-casting unused parameters, returning the type's failure value (`std::nullopt` from `next`), and keeping no asserts; leave both constructors, `HashJoin`'s provided key machinery (`join_key`, `hash_key`), and every other operator untouched
- [x] 2.2 Verify the designed failure surface (design D3): the student tree builds cleanly, the suite runs to completion, and the result splits exactly — 1,098 checks green (storage, sql, index, B+tree, loader, config, CLI suites in full; the operators suite's non-join sections; the exec suite's non-join checks) and 42 red, every failure join-dependent; suite exit code non-zero; `ctest` reports the single test as failed

## 3. Student handout

- [x] 3.1 Rewrite `../p4stdbeng/README.md` as the student handout per design D5: the engine description, the "everything works except the two join algorithms" paragraph naming the stub files and the untouched headers, the implement-until-the-suite-passes contract with the failing suites listed, the suggested order (index nested loops first, then the hash join's phases), the build/suite/CTest invocation, and the due-date placeholder
- [x] 3.2 `git init` in `../p4stdbeng`, add an initial commit of the generated tree, and confirm p4dbeng's own check suite is still green (the generation must not have touched the solution)
