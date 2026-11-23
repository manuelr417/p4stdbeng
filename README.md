# p4stdbeng

p4stdbeng is a small relational SQL database engine written from scratch in
C++20, built layer by layer: block files, a buffer pool, slotted heap pages
with two free-space lists, a tuple codec, a JSON catalog, a SQL layer that
parses, binds, plans, and executes statements over that storage, indexes
maintained across every write, and a Volcano operator layer whose pipelines
answer multi-table queries through the join lecture's algorithms (after
Ramakrishna & Gehrke).

**This is the student version: everything works except the two join
algorithms.** The methods declared in `operators/IndexNestedLoopsJoin.h`
and `operators/HashJoin.h` — documented there in full, per parameter, with
their lecture-slide references — are blank stubs in the matching `.cpp`
files, each opening with a `// TODO(student):` sketch of the algorithm.
Your task is to implement them until the check suite passes. Everything
else — the TUP2 heap file, the buffer manager, the catalog, both index
types, the SQL layer, the executors, the other Volcano operators
(`SeqScan`, `Filter`, `Project`, `Limit`, the plain `NestedLoopsJoin`,
`Partitions`, `RowIdScan`), the hash join's provided key machinery, the
bulk loader, and all the tests — is provided and must not change.

**Due date: December 3, 2026.**

## What it does

- **Storage**: a table is a heap file of fixed-width records in 4096-byte
  slotted pages organized as two header-addressed free-space lists, so
  INSERT picks its page in O(1). A structural self-check
  (`HeapFile::check_structure`) verifies the whole structure at any time.
- **Indexing**: one index per column, of either type — ISAM or B+tree —
  both provided, both maintained across INSERT, UPDATE, and DELETE.
  `CREATE INDEX <name> ON <table> (<column>) [USING ISAM | BTREE]` records
  the type in the catalog.
- **Access paths**: every SELECT announces `(via index scan)` or
  `(via heap scan)`, and `EXPLAIN SELECT` renders the whole plan — every
  node's algorithm — before anything runs. Correctness never depends on
  the choice: the differential oracle holds every path to identical
  answers.
- **Joins**: a FROM chain is planned left to right, one join per new
  table. Three algorithms compete, per the lecture: the plain nested
  loops join (provided), the index nested loops join (**your work** —
  for each outer tuple, probe the inner table's index with that tuple's
  key), and the grace hash join (**your work** — partition both inputs
  into bucket files, then build and probe in memory, partition pair by
  partition pair). The join predicate always decides over the combined
  rows — the index or the hash only narrows — so the answer never
  depends on the plan.
- **Types**: Integer, Double, and fixed-width String columns. The binder
  is type-strict, and an integer operation whose result is not
  representable in 32 bits is a named run-time error.
- **Catalogs**: each table's schema lives in a JSON file under
  `tablecatalog/`; data files are `data/<table>.tbl` with index files
  beside them.

## Your work

The stubbed methods, in the suggested implementation order:

1. **`IndexNestedLoopsJoin::open`** — the "for each r in R" initialization:
   open the outer child, pull its first tuple, probe.
2. **`IndexNestedLoopsJoin::next`** — the probe loop (slide 15): walk the
   current probe's row ids, fetch each from the inner file, filter with
   the inner-side predicate *before* combining, append the inner columns,
   let the join predicate decide; advance the outer tuple and probe again
   when the matches run dry.
3. **`IndexNestedLoopsJoin::close`** — end the run, drop the probe state.
4. **`HashJoin::partition_side`** — Phase I's routing (slide 25): every
   input row into its bucket, `hash_key(key(row)) % bucket_count`.
5. **`HashJoin::open`** — Phase I as a whole: create the bucket files,
   partition both inputs, load partition pair 0. (The Volcano contract's
   named materialization exception — the algorithm requires it.)
6. **`HashJoin::load_partition`** — Phase II's per-pair setup (slides
   26-27): build the in-memory table from the build bucket, position the
   probe scan, reset the cursor.
7. **`HashJoin::next`** — Phase II streamed: probe the in-memory table,
   emit every combined pair the predicate accepts (probe columns first,
   build appended), advance probe row, then partition pair.
8. **`HashJoin::close`** — the query-end erasure of the bucket files.

`operators/NestedLoopsJoin.cpp` is provided in full — read it first: it is
the sibling of both assignments, showing the Volcano cursor style (the
outer tuple held across `next()` calls, the inner side restarted or
advanced) your implementations should match.

The headers (`operators/IndexNestedLoopsJoin.h`, `operators/HashJoin.h`,
`operators/Partitions.h`) are the assignment's documentation: every
method's contract — purpose, parameters, return value, thrown errors — is
written at its definition, every constructor parameter is documented, and
the cursor state you will drive is declared in the private section. Do not
modify the header files or any provided file.

## Repository structure

| Directory | Purpose |
|-----------|---------|
| `common/` | `common::Error`, the one error type |
| `file/` | Block-level I/O: 4096-byte block read/write on plain files |
| `bufpool/` | Buffer pool and buffer manager: pin/unpin, eviction, write-back; the join pool for hash-join scratch |
| `policy/` | Replacement policies (LRU) injected into the buffer manager |
| `config/` | The strict `conf.json` loader: both pool sizes, the policy, and the `hash_join`, `timing`, and `labeled_rows` switches |
| `heapfile/` | The TUP2 heap file: slotted pages, two free-space lists, self-check |
| `tuple/` | In-memory row values (`Tuple`, `Column`) |
| `schema/` | Table schema, JSON catalog loader + atomic writer, the tuple serializer, CSV bulk load, a row generator |
| `sql/` | SQL lexer, parser, AST (with a printer and a draw helper) |
| `index/` | `KeyCodec`, the ISAM index and the B+tree (both provided) |
| `exec/` | `Value`, `bind_expr`, `JoinedSchema`, `Pushdown` (the WHERE splitter), `Evaluator`, `Planner` (access path), `Executor` (builds the operator trees) |
| `operators/` | The Volcano layer: `Operator` (the open/next/close contract), the provided scans, filters, projections, limits, the plain nested loops join, `Partitions`, `RowIdScan` — and **the two stubbed joins you implement** |
| `tools/` | `bulk_load` — the pool-free bootstrap loader that builds TUP2 files directly |
| `openspec/` | Behavior specs per capability (the contracts your work is graded against) |

Three binaries share the library code:

- `p4stdbeng` — the check suite (`main.cpp`): the storage, SQL, index,
  exec, loader, B+tree, config, operators, join, and CLI suites, ending
  with the differential oracle.
- `p4stdbeng_cli` — the SQL session (`p4stdbeng_cli.cpp`): a REPL where
  statements accumulate until a `;`, plus the escape hatches (`scan`,
  `read`, `seek`, `bulk`, `append`, `checkindex`, `quit`).
- `p4stdbeng_bench` — the index-vs-heap benchmark over generated rows.

## Building

CMake 4.3+ with a C++20 compiler:

```sh
cmake -S . -B build
cmake --build build
```

## Testing

Run the check suite (also registered with CTest):

```sh
ctest --test-dir build --output-on-failure
# or directly:
./build/p4stdbeng
```

The suite is the specification: every check names the behavior it demands.
**Before you start, 1,119 checks pass and 42 fail — every failure is a
join check.** The failing surface is your work list: the operators suite's
`IndexNestedLoopsJoin` and `HashJoin` sections, the join suite in full
(both grammar forms, three-way chains, pushdown bypassed, randomized
oracles against an independently computed reference), and the exec suite's
join paths. The storage, SQL, index, B+tree, loader, config, and CLI
suites pass before you start — they are the provided infrastructure's own
regression net, and they must stay green. When all 1,161 checks pass, you
are done.
