# EXPLAIN — the implementation tasks of p4dbeng

This document records what was built in this project, in the order the
layers were stacked. p4dbeng is the fourth iteration of the p-series: its
tree was assembled by vendoring the strongest layers of the projects before
it — p2dbeng's storage and full data manipulation, ex13's ISAM index with
announced access paths, p3dbeng's full DML under index maintenance and its
self-balancing B+tree built to the class lecture on tree-based indexing
(after Ramakrishna & Gehrke) — and then extended where none of its parents
could go: SQL joins, built to the class lecture on join evaluation and
executed as tuple-at-a-time nested loops on a Volcano open/next/close
iterator pipeline (Tasks 13-15 below). Everything is written from scratch
against the C++20 standard library; no third-party code. The house
conventions are the parents' — 4096-byte blocks, one `BufferManager` (LRU)
per session, single-threaded — except for failure reporting, which Task 12
changed from the parents' bool plus `error` out-parameter to exceptions.

## Task 1 — The assembly

The skeleton is p2dbeng's tree — `file/`, `bufpool/`, `policy/`, `tuple/`,
`heapfile/` (the TUP2 two-list heap with its structural self-check),
`schema/` — with three seams taken from ex13: the `index/` module, the
exec lineage, and the session CLI's shape. Nothing in the vendored storage
stack changed by so much as a line; the parents' check suites carried
forward green as the regression base, and new checks were written only for
new behavior. One deliberate schema substitution: ex13's `TableSchema` (no
`data_file` key — data files are always `data/<table>.tbl` — plus the
loader's `friend` for declared widths) and ex13's `TupleSerializer` (its
extra zero-width refusal, which the bulk loader's "no record layout" check
needs). ex13's versions are supersets, and the carried schema suite passes
against them.

## Task 2 — The TUP2 heap file (`heapfile/`)

One table's rows live in one file of 4096-byte slotted pages. Block 0 is
the header page; its record area carries the magic (`TUP2`), the table
name, and five 32-bit fields: `free_head`, `free_tail`, `full_head`,
`full_tail`, and `next_page_id`. The pages form two header-addressed
lists — pages with free space, and full pages — so INSERT takes the free
list's head in O(1) instead of walking a chain:

- **Derived membership**: a page is on the full list iff every slot is
  occupied. No stored membership bit — a bit can drift silently, while
  the derived rule turns drift into a failed self-check.
- **Tail-append**: both lists append at the tail, keeping each list in
  allocation order, so bulk loads scan in insertion order and freed pages
  are reused oldest-first. A page moves between the lists exactly when it
  fills or unfills; UPDATE never moves pages.
- **List primitives**: appending, detaching, and moving pages between the
  lists are private `HeapFile` operations with a strict pin discipline —
  all auxiliary pins (header, list neighbors, destination tail; up to
  five total) are acquired up front in ascending page-id order, after
  which the surgery is pure memory writes that cannot fail mid-way.
- **The net before the surgery**: `HeapFile::check_structure` — an
  O(pages) diagnostic walk — verifies head/tail consistency, single
  membership, no cycles, membership versus occupancy, the union of both
  lists covering exactly pages 1..next_page_id−1, and on-disk coverage.
  The storage tests run it after every operation, including a randomized
  insert/erase sequence checked against a shadow model of the live rows.

## Task 3 — Tuples, schemas, catalogs (`tuple/`, `schema/`)

- `tuple/`: a `Tuple` is a vector of typed `Column` values (Integer,
  Double, String) with a variant-based value.
- `TableSchema` describes a table: name, columns, declared String widths.
- `TupleSerializer` chooses the fixed-width record layout (natural size
  for numerics, catalog width for Strings) and encodes/decodes rows,
  validating arity, types, and string widths on the way.
- `TableSchemaJson` loads and writes the JSON catalog files (one per
  table, under `tablecatalog/`), including each table's optional
  `indexes` array — name, column, and type — and installs every write
  atomically (write the sibling temp file, flush, rename). A failed write
  never touches the previous catalog.
- `TableCsv` bulk-loads rows from CSV (invalid rows reported as
  `line N: reason` and skipped, never fatal); `TableGenerator` produces
  the deterministic synthetic rows the tests and the benchmark use.

## Task 4 — SQL: lexer, parser, AST (`sql/`)

A hand-written lexer and recursive-descent parser producing an AST for the
supported grammar: `SELECT <items> FROM <table> [WHERE <expr>] [LIMIT n]`,
`INSERT INTO <table> VALUES (...), ...`, `UPDATE <table> SET col = expr,
... [WHERE <expr>]`, `DELETE FROM <table> [WHERE <expr>]`, `CREATE TABLE`,
`DROP TABLE`, `CREATE INDEX <name> ON <table> (<column>) [USING ISAM |
BTREE]`, and `DROP INDEX`. Expressions cover comparisons, arithmetic,
`AND`/`OR`/`NOT`, and literals. `JOIN`, `GROUP BY`, `ORDER BY`, and
aggregate calls parse into an explicit refusal so the error names the
unsupported feature. `AstPrinter` renders the tree for debugging.

## Task 5 — The exec lineage (`exec/`)

One expression lineage serves every statement: `Value` (int/double/string;
booleans are ints 0/1), free-function `bind_expr`, the `Evaluator`, the
`Planner`, and the `Executor` — the session state (catalog cache, session
file table with lazy index rebuild, `data/` path resolution) and the
statement dispatcher. Bind-time refusals (unknown columns, type
mismatches) cost no I/O; the evaluator carries the arithmetic contract
(promotion, truncating integer division, named division/modulo-by-zero,
and the 32-bit overflow refusal). The executors:

- `SELECT` projects named columns (aliases included), filters, honors
  `LIMIT`, and announces its access path — the planner pins the indexed
  column to literal bounds only when an equality or AND-combined range
  names it; everything else stays residual. Correctness never depends on
  the path.
- `INSERT` validates every value list first — one bad list stores nothing
  — then writes each row to the data file and every declared index.
- `UPDATE` collects matching rows, then overwrites each with new values
  computed from the row's *old* values, so `SET a = b, b = a` genuinely
  swaps; indexes whose column changed are erased and re-inserted.
- `DELETE` collects matching rows, then erases them from the data file
  and every declared index; freed slots feed later inserts.
- `CREATE TABLE`/`DROP TABLE` manage the catalog and its files — the drop
  closes the session's own handles first, then erases the catalog, the
  data file, and every index file.

All statements refuse out-of-scope features before touching data, every
scan fails fast on a record that violates the declared structure (no
partial results, no partial writes), and index maintenance lives in
exactly three places: insert-after-heap, update-on-change,
delete-with-row.

## Task 6 — The ISAM module (`index/`)

ex13's index, carried with one new seam: `KeyCodec` (the one encoding of
keys — fixed-width bytes, comparison total per instance), the three page
views (`SeparatorPage`, `LeafPage`, `OverflowPage`), and `IsamIndex` —
bulk build, point lookup with all duplicates, ascending ranges, insert
maintenance through overflow chains with byte-frozen separators, and
integrity verification (order, dangling rids, seeded-corruption
detection). The seam ex13's design reserved is the **entry erase**:
`IsamIndex::erase(key, rid)` locates the exact entry during a descent
plus overflow-chain walk and removes it with a within-page tail-shift —
separators are never touched, the leaf chain is never relinked, an
overflow page emptied by erases stays in its chain, and a later insert
may refill a leaf's freed capacity. A (key, rid) pair the index does not
hold is a named error.

## Task 7 — The bulk loader (`tools/`)

`tools::bulk_load` fills an empty heap file with generated records at one
raw block write per page — its own block I/O through `BlockFile`, no
buffer manager, run before any pool opens the path. It builds the TUP2
structure directly: every page that fills completely joins the full list
in generation order (one optimistic forward link patched on the list's
tail), a partial final page joins the free list, and the header carries
both lists' ends plus the allocation frontier. The result is
engine-equivalent — `HeapFile::open`/`scan`/`find` treat it as their own,
`check_structure` passes, and the differential twin (tool versus per-row
inserts of the same generated rows) produces identical record sequences,
identical row ids, and identical list structure. With a partial tail page
the heap scans the free list first, so generation order is guaranteed
exactly when N is a multiple of the page capacity — the same rule
sequential inserts obey.

## Task 8 — The session CLI (`p3dbeng_cli.cpp`)

The front door: one buffer manager (LRU, 8 frames) for the whole session.
SQL statements accumulate across lines until a `;` (the prompt flags an
unfinished statement); errors print and the prompt returns — the session
ends only on `quit` or end of input, with the exit code reflecting the
final flush. Piped input suppresses the prompt, so the same session text
scripts the same work. The escape hatches run bare: `scan`, `read`,
`seek` (raw row-id reads with the pool-hit marker), `bulk` (generated
rows through the index-maintaining insert path), `append <table>
<csv-file>` (CSV rows through the same path, `line N: reason` for every
skipped row), and `checkindex <table> <file>` (per-index verification).
Bare file names resolve inside `data/`; a path containing `/` is used
exactly as given.

## Task 9 — The B+tree beside ISAM (`index/BPlusTree`, `index/BptPages`)

The dynamic index, built to the class lecture so students can be asked to
implement exactly what they were taught. It lives BESIDE the ISAM module —
the catalog declares which type each index uses, and the two coexist:

- **Page castes** (`BptPages`): leaf pages hold sorted (key, rid) data
  entries chained DOUBLY (prev and next — slides 22, 35); separator pages
  hold the |P0| K1 | P1 | ... | Km | Pm | index-entry form (slide 10).
  Block 0 is a header page: magic BPT1, key type and width, table and
  column names, root, first leaf, level count, entry count, highest
  allocated block, and the head of a free-block list (pages freed by
  merges are reused before new ones are allocated).
- **Order d** (slide 21): every non-root node holds d..2d keys, with
  d = floor(capacity / 2), so a full node holds exactly 2d entries and
  every split boundary is a named position. The root is exempt.
- **The recursion contract** — each public method is a thin wrapper over
  a private recursive auxiliary whose extra parameters carry state
  upward, exactly as the lecture's own pseudocode does:
  - `tree_search` (slides 17, 24): pin one separator, take the child
    after the LAST separator <= the key, recurse; the landing leaf plus
    the equal-key run through the prev links answer lookups and ranges
    (the prev-step closes the boundary duplicates the copy-up convention
    can leave in the left sibling's tail).
  - `insert_aux` (slides 34-37): a full leaf (2d entries) keeps d and
    gives d+1 to a new leaf, copying its smallest key up; a full
    separator page moves its middle key up; the wrapper allocates a new
    root when the old root splits (the height grows by one).
  - `erase_rec` (slides 38-49): an underflowed child borrows from a
    sibling that can lend (rotating through the parent's separator,
    which is rewritten to stay exact) or merges with one (the separator
    drops); the cascade propagates upward and the wrapper drops a root
    left with a single child. Separators stay EXACT — each always equals
    the first key of the child it routes to — which verification demands.
  - `verify_rec` walks the whole tree: reachability, occupancy, order,
    separator exactness, leaf-depth uniformity, dangling rids, and the
    header total, plus the leaf chain and the free list.
- **The rebuild-on-cross trade-off**: the one case the lecture's routing
  cannot serve with a plain descent — erasing an entry that the
  equal-key run holds in a DIFFERENT subtree than the landing leaf (a
  run spanning a subtree boundary). The erase removes the entry in place
  and, only when the leaf underflowed or its first key moved, rebuilds
  the tree from the leaf chain — the rare case traded for simplicity;
  the common path rebalances recursively.
- **The tear-out seam**: removing `index/BPlusTree.h/.cpp` and
  `index/BptPages.h/.cpp` removes every line of B+tree code; the module
  compiles and its 96-check suite passes with only bufpool, KeyCodec,
  Entries.h, and schema/tuple (the suite's wrong-kind fixture also uses
  the ISAM module, which stays). Every method carries a contract comment
  stating purpose, parameters, and return value, and the recursive
  auxiliaries' signatures live in the header — the professor removes the
  `.cpp` bodies and students re-implement the methods.
- **DDL and dispatch**: the `USING ISAM | BTREE` clause records the type
  in the catalog (absent means ISAM, the shape every older catalog
  wrote); one index per column whatever its kind; a missing index file
  rebuilds lazily as the declared type, and a file whose kind contradicts
  the catalog refuses by name. The executor's session index record is one
  kind-tagged dispatch — maintenance, access paths, lazy rebuild, and
  checkindex never branch on the type themselves.

## Task 10 — The three-way benchmark (`parts_bench.cpp`)

The repeatable measurement over generated `parts` rows: clean-slate drops,
`--rows N` / `--reuse` argument rules, the deterministic generator, and
per query — heap baseline, then build an ISAM index on the searched
column and measure, drop it, build a BTREE on the same column and
measure, drop it. The drops reuse the engine's own `DROP INDEX`, so each
measurement's precondition is exactly what users would run. The report
shows all six query times, all four build times, and per-type speedups,
with the answer sets of every indexed path required to equal the heap
baseline.

## Task 11 — The check suite (`main.cpp`)

The regression net: six suites in order — storage (TUP2, self-check after
every operation), sql, index (ISAM), exec (bind refusals, access-path
markers, the DDL seam, the mutation checks), loader (the TUP2
differential), and the B+tree module suite — ending with the differential
oracle: 500 random rows, 250 randomized INSERT/UPDATE/DELETE mutations
against a shadow model with every index verifying clean and the row count
matching after every mutation, then 300 random predicates each run
index-allowed and index-suppressed with identical answers required.
Registered with CTest and green under regular and sanitizer builds
(ASan/UBSan), zero findings.

## Task 12 — The error discipline (`common/`, `bufpool/PinnedPage.h`)

Every layer used to report failure with a `bool` return plus a cleared
`error` out-parameter. The algorithms disappeared under the plumbing: each
fallible call sat inside an `if`, every early return had to unpin its
pages by hand, and internal misuse was reported with the same care as bad
SQL. Three rules replace it, applied layer by layer from `file/` up:

- **Boundary errors throw.** A state a caller or a file can genuinely
  produce — bad SQL, an unknown column, a missing or foreign file, a
  corrupt page, an exhausted pool — throws `common::Error` with the same
  named message as before. It is caught once, at the system boundary: the
  CLI's loop prints it, the check suite compares it (`check_support.h`).
  `common::with_context` prefixes a nested failure ("cannot open
  \"path\": ...").
- **Contract checks assert.** A caller misusing an internal API — a slot
  out of range, an insert into a full page, an operation on a closed
  file — is an `assert`, its message documenting the contract.
- **Pins are guards.** `bufman::PinnedPage` pins in its constructor and
  unpins in its destructor; `PinnedView<View>` pairs it with the page view
  bound over its bytes. No call site unpins by hand, and an exception
  can never leak a pin.

Results come back as return values instead of out-parameters (`insert`
returns the `RowId`, `parse` the statements, `verify` an
`idx::VerifyResult`). Behavior is unchanged: the CLI session, the
benchmark, and the B+tree's operation traces were diffed against the
previous version and match. The B+tree keeps its per-method contract
blocks and its named recursive auxiliaries — the student distribution's
work surface — with the failure convention in each `Returns:` line now
reading "Throws ..." or "Contract: ...".

## Task 13 — The operator pipeline (`operators/`)

The Volcano iterator model as a tear-out module, built to the join lecture's
execution assumption: a query plan is a tree of operators, each pulled one
tuple at a time. The whole contract is one abstract class:

- **`Operator`** — `open()` starts one run, `next()` returns
  `std::optional<tuple::Tuple>` (nullopt at exhaustion, sticky within the
  run), `close()` ends it and is idempotent. Double `open()` and `next()`
  before `open()` assert — contract misuse, not data errors. No operator
  holds a buffer-pool pin across a `next()` call: a paused pipeline frames
  nothing, which the suite proves by scanning a third file to exhaustion
  through the same pool while a join sits parked.
- **`SeqScan`** walks a `HeapFile`'s own iterator; **`Filter`** wraps a
  child with a `std::function<bool(const tuple::Tuple&)>`; **`Project`**
  emits a narrowed tuple; **`Limit`** stops pulling once enough rows are
  out (its suite includes a counting child proving the pull stops);
  **`RowIdScan`** is the index access path as an operator — an eager
  row-id list fetched one `find()` per `next()`, so `(via index scan)`
  rows flow through the same pipeline contract as a heap scan.
- **`NestedLoopsJoin`** is the lecture's algorithm as a cursor: for each
  outer tuple, the inner child is restarted (`close()` + `open()`) and
  scanned; each surviving pair is emitted as one combined row — outer
  columns then inner columns. A null predicate is a cross join. Answers
  come out outer-major, inner in scan order.
- **The tear-out seam**: the module depends only on `tuple/`, `schema/`,
  `heapfile/`, and `bufpool/`; predicates are plain functions, so nothing
  in `operators/` knows the binder or executor exist. Its 19-check suite
  links from those dependencies alone — the same proof the B+tree suite
  makes — and every method carries the per-method contract comment the
  professor's tear-out needs.

## Task 14 — Names and predicates across a FROM chain (`exec/JoinedSchema`, `exec/Pushdown`)

The binder resolved columns against one schema and refused table
qualification outright. Joins need a column space that spans the chain:

- **`JoinedSchema`** is the FROM clause as one column space: tables in
  FROM order, a joined tuple is their concatenation, and a column's
  absolute index is its position in it. A qualified reference `t.c`
  resolves against the one entry whose name or alias is `t`; a bare `c`
  must match exactly one column across all tables — none is an
  unknown-column error, more than one is an ambiguity error, both named
  before any page is pinned. `bind_expr` was generalized over this space;
  the single-table entry is a one-table wrapper, so every carried
  single-table refusal resolves exactly as before.
- **`Pushdown`** (`split_where`) is the WHERE splitter behind design
  decision D6: the expression flattens into its AND-conjuncts, each is
  bound, and each is classified by its referenced-table set. A conjunct
  whose columns all live in one FROM entry is pushed to that entry's scan,
  re-bound with table-local positions; every other conjunct — multi-table
  or column-free — stays above the join, bound over combined rows. The
  answer never depends on the split: the oracle re-runs every query with
  the classification bypassed (`Executor::set_pushdown(false)`), the whole
  WHERE held above the join.

## Task 15 — The executor on the plan tree (`exec/Executor`)

Every SELECT now builds an operator tree and drives it; the inline
scan/filter/project loop is gone. Single-table plans are
`SeqScan → Filter → Project` (the planner's index bounds turning the scan
into a `RowIdScan`; `Limit` on top when present), with the access-path
announcements and row order preserved — the carried single-table and exec
suites pass unchanged, which was the rewrite's regression net. Join plans
fold the FROM chain left-deep: each entry gets a (pushdown-filtered)
`SeqScan`, and each grammar entry — `JOIN ... ON` or comma — contributes
one `NestedLoopsJoin` whose ON binds over the prefix the join closes,
inside the join node. The residual WHERE conjuncts form one `Filter` above
the join, `Project` and `Limit` sit on top, and the query announces
`(via nested loops join)`. Both grammar forms normalize to one AST shape at
parse time (a comma entry is a join with a null ON — its predicate arrives
in WHERE), so the executor sees one chain either way; the oracle holds the
two spellings to identical answers.

## Task 16 — The index nested loops join (`operators/IndexNestedLoopsJoin`)

The join lecture's third algorithm. The plain nested loops join pays
`NPages(R) + |R| * NPages(S)` page reads — the deck's slide-18 example
takes 9.3 days on its workload. The index nested loops join replaces the
inner rescan with one index probe per outer tuple — slide 15's block:

```
for each r in R do              // the outer, scanned once
    Iterator T = I.search(r[i]) // one probe of the index on S
    for each s in T do
        add t = <r,s>
```

For a clustered B+tree the cost falls to `NPages(R) + |R| * 4` (slide 17)
— 1.12 minutes in the deck's example. The operator is that block as a
cursor: the outer child is pulled one tuple per `next()`, the probe is a
`std::function` seam the executor closes over the session index
(`range(key, key)` with the outer tuple's key as both bounds — `I.search`
verbatim, for either index type), each matching row is fetched from the
inner heap file under a single-pin scope, and an optional inner predicate
(the pushed single-table conjuncts, whose scan-side `Filter` has no home
on this path) filters the fetched row before combining. The join
predicate still evaluates over every combined pair: the index narrows,
the predicate decides — so the answer cannot depend on the plan choice,
which is what makes the oracle's job trivial.

Selection is a rule, not a cost model, decided per join node before any
node is built: the probe runs iff the join's own predicate — the ON for
the `JOIN` form, the WHERE for the comma form — contains an AND-conjunct
of the shape `prefix_col = inner_col` (either orientation) whose inner
column resolves to the newest FROM entry and carries an index. No
reordering: an index on an earlier entry changes nothing (slide 15 makes
the indexed table the inner; join reordering is optimizer work for a
future course iteration). Range or string predicates keep the plain join
— an index range could answer `a.x < b.y` only with a per-tuple bound,
which is a different algorithm. The statement announces
`(via index nested loops join)` when any node probed,
`(via nested loops join)` otherwise, and suppression (the oracle's
`allow_index` gate) flips the announcement without moving a row.

Wiring the rule exposed a latent defect in the join fold:
its inner side was `nodes.back()`, correct only for two tables — with
three, the accumulating join became its own inner child (a double-open
assert). The fold now tracks each entry's top node, and three-way
regression checks pin the repaired shape through both grammar forms and
a mixed chain.

## Task 17 — The grace hash join (`operators/HashJoin`, `operators/Partitions`)

The join lecture's fifth algorithm (slides 24–29). The nested loops join
pays `NPages(R) + |R| * NPages(S)` reads and the index join needs an
index serving the predicate; the hash join needs neither — only memory.
Slide 24's idea: hash both tables on the join attribute; matching tuples
must hash to corresponding buckets, so only corresponding buckets ever
need to meet. Phase I (slide 25) partitions each input into `B − 1`
disk-resident buckets on the join key; Phase II (slide 27) walks the
partition pairs — the build side's partition loads into an in-memory
hash table `T3`, the probe side's partition streams against it, and
every probe tuple looks its matches up in `T3`. Slide 28 prices the
whole thing at `3(NPages(R) + NPages(S))` — read both, write both, read
both again — and, symmetric in its inputs, it needs none of the plain
join's orientation care. The deck's Driver ⋈ Car example: 8,400 I/Os
(1.4 minutes), against the index join's 402,000 (slide 18) and the plain
join's 9.3 days on the same workload.

The catch is slide 29: the algorithm "is very memory consuming — should
only be used if enough buffers are available." With B buffers, `B − 1`
hold the build side's partitions; a build side of M pages puts
`M/(B − 1)` pages in each partition, and the in-memory table runs
`(M/(B − 1)) × f`, f the fudge factor compensating for hash-table
overhead. That product must fit the buffers — the sizing rule decides
whether the hash join may run at all. The engine's gate is that rule as
a heuristic: the build side's data-file size (the stand-in for its
partition's footprint) doubled for the fudge factor must fit the join
buffer pool's frames, and a join that fails the gate falls to the
nested loops join. The estimate can read low while dirty pages wait in
the pool; the failure mode is a downgrade, never a different answer.

The teaching contract splits the work the way the deck does. The bucket
layer is given support: `Partitions` owns `B − 1` heap files under
`tmp/` — built over the join buffer pool, the separate animal
`conf.json` sizes with `join_pool_size` — and exposes `write`,
`scan_start`/`scan_next`, and `erase_all`, with the erasure repeated in
the destructor so a run that dies mid-partition still leaves nothing
behind. The header also gives the hash join's delicate bits with their
contract comments: `JoinKey` (a `double`/`string` variant),
`join_key`'s canonicalization — integers promote to double so the
evaluator's `5 = 5.0` meeting holds across sides, and `−0.0` normalizes
to `0.0` so one value cannot hash into two buckets — and `hash_key`
(variant-index-salted splitmix64 over the bytes, FNV-1a for strings).
The assigned work is the join itself: the two phases in `HashJoin`.

The operator is the deck's block as a Volcano cursor. `open()` is the
model's materialization escape hatch: it partitions both children to
exhaustion (Phase I) and loads the first partition pair. `next()`
streams Phase II pair by pair, and the cursor that survives across
calls — the probe row being matched and its position in the match
vector — is what makes duplicates correct: two probe rows and two build
rows on one key emit four combinations, each once, in probe-major
order. The combined tuple is probe columns then build columns — the
accumulated prefix, then the newest FROM entry — matching the nested
loops orientation, and the join predicate still evaluates over every
pair: the buckets narrow, the predicate decides, so the answer cannot
depend on the path. `close()` erases both bucket sets (the destructor
would anyway), and `tmp/` never holds anything between queries.

Selection stays a rule, now three lines: the index join when an open
inner index serves the equality shape; otherwise the hash join when the
session enables it and the gate passes; otherwise the nested loops
join. The session switch is one executor flag — initialized from
`conf.json`'s `hash_join`, toggled by the oracle's `set_allow_hash` —
and the statement announces `(via hash join)` when any node ran hash.
Wiring taught one lifetime lesson the checks now pin: the bucket sets
and the probe side's synthetic prefix schema must be statement-owned
(function scope in the fold), because the plan references them only
while the statement runs — a stack-local schema dangled into the
partitioning pass, and the sanitizer caught the corruption the first
time the exec suite drove a real hash join.

## Task 18 — The primary key (inline `PRIMARY KEY`, `schema/TableSchemaJson`, `exec/Executor`)

The last piece of the DDL story: a way to state that a column identifies
its rows, and an engine that holds the line. The grammar is one suffix,
`pid INTEGER PRIMARY KEY`, matched as a pair of identifier words rather
than reserved keywords — the lexer's keyword table stays untouched, so a
column named `key` or `primary` still parses as a column, the same
contextual matching `USING ISAM` uses for its type word. At most one
column per table carries it, and the table-level `PRIMARY KEY (col)` form
is refused by name: the index layer is single-column, so a composite key
would need a composite key codec, and the honest answer is a refusal, not
a misparse.

The catalog records a `primary_key` column name, and the constraint
**owns** its index rather than being one: `CREATE TABLE` declares a
backing index through the ordinary machinery — ISAM, the default type,
named by the ordinary convention (`account_aid`) — and the file itself
materializes through the session's lazy open, first use building it over
the (empty) data file. Ownership is tracked by the catalog field, never
inferred from the name, which is what gives `DROP INDEX` its clean
refusal ("backs the primary key") instead of a silent constraint drop;
`CREATE INDEX` on the column needs no new rule at all, because the
constraint's index already occupies the one-index-per-column slot. The
field is optional end to end — a catalog written before primary keys
existed loads and behaves unchanged, and the writer emits the member only
when the constraint is there.

Enforcement is the house's check-everything-then-mutate discipline taken
one constraint further. INSERT already validated every row before any
page was pinned; the primary key adds a check phase after that
validation and before the first insert, where every row's key probes the
constraint's index — the keys already stored — and a per-statement
seen-set — the keys this statement itself will store. The seen-set is
the trap fix: the index alone would pass rows 1 and 3 of one INSERT
holding the same new key (both probe against the pre-statement index),
and the pair would land. Any hit refuses the whole statement with the
duplicate named, nothing stored. UPDATE computes every new row in a
planning pass — which also closes a pre-existing hole where a mid-loop
width refusal could leave earlier rows applied — and checks the same two
sources there, with one exception the check suite pins: a row assigning its own
current value skips the probe, so `SET pid = pid` passes; and in the
changed branch an index hit cannot be the row's own entry (the row's own
rid sits under its old key), so any hit is another row and a refusal.
DELETE needs nothing — removal cannot violate uniqueness.

The probe is `lookup(key)` through the index seam and the identity is a
canonicalized key text — type-tagged, the double as its IEEE bits with
`−0.0` folded onto `0.0`, strings length-prefixed — the same lesson the
join key learned: two values collide in the identity exactly when the
engine calls them equal. And the guarantee reaches the paths that bypass
SQL: the probe also guards `insert_row`, the seam every insert flows
through, so the `append`/`bulk` escape hatches refuse a duplicate row
(naming it, per their line-report contract) instead of storing a
violation. The last door is the lazy rebuild: a data file that arrived
sideways can carry duplicates the engine never saw, so when a missing
primary-key index rebuilds, it verifies the data file's uniqueness first
and refuses on the first duplicate, naming it and leaving no index file
behind. The constraint's promise now holds over every load path that
produces a table the session can open.

## Task 19 — The foreign key (inline `REFERENCES`, `schema/TableSchemaJson`, `exec/Executor`)

The second half of the constraint story: a way to state that a column
points at another table's rows, and an engine that holds both ends of the
line. The grammar is the primary key's trick again — one inline suffix,
`owner_id INTEGER REFERENCES person(pid)`, the table and column read as
ordinary identifiers (nothing is added to the keyword table, a column
named `references` still parses), at most one per column, and the
table-level `FOREIGN KEY (col) REFERENCES ...` form refused by name for
the same reason the table-level `PRIMARY KEY (col)` form is: the index
layer is single-column, so composite references have no honest
implementation to fall back on.

Creation-time validation is where the reference earns its meaning: the
parent table must already exist (parent before child — no forward
references, which is what keeps the reference graph acyclic and makes
every later check resolvable against a catalog that is really there), the
referenced column must be the parent's declared primary key, and the two
columns' types must match — the probe compares encoded keys, and a
mismatched pair could only ever refuse, misleadingly. The self-reference
is the one special case: the parent is the table being created, so its
primary key is validated against the statement itself (`manager_id
INTEGER REFERENCES employee(eid)` on `employee` checks that `eid` carries
`PRIMARY KEY` in this same CREATE). The catalog gains a `foreign_keys`
array — `{column, parent_table, parent_column}` per entry, an array
because a table may reference several parents — optional end to end, the
writer emitting it only when non-empty, and the loader parsing it
syntactically: the referencing column must resolve within the same
catalog, but the parent side is deliberately not cross-checked against
other files at load time (the loader reads one file in isolation; the
enforced entry point for validity is CREATE, and a hand-edited catalog
naming a bogus parent is caught at first enforcement, naming it).

Child-side enforcement is the primary key's check phase with one table
over. In INSERT's check phase — after literal validation, before any row
lands — each row's referencing value probes the parent's primary-key
index (the keys already stored there; a cross-table probe is an ordinary
read-only session open, the thing every join already does), or the
statement's own **parent-provided set**: for a self-reference, the
primary-key values the statement itself inserts. That set is what makes
the natural teaching example work — a manager and their report arriving
in one INSERT, in either order, because nothing lands until every check
has passed. Any miss refuses the whole statement, naming the value, the
parent table and column, and the referencing `child.column`. UPDATE's
planning pass probes the same two sources for collected rows whose
referencing column changes, with the unchanged branch left alone: an
unchanged reference was valid when stored, and if its parent row moves
away in this same statement, the parent-side check refuses that instead —
the honest refusal, naming the constraint that actually breaks. And the
probe guards `insert_row` again, the seam the `append`/`bulk` escape
hatches flow through, so a dangling reference cannot arrive sideways.

The parent side is the genuinely new machinery, and its question is not
"does anyone reference this key now?" but "**will anyone still reference
this key when this statement completes?**" — the check runs over the
statement's plan, not its inputs. DELETE collects its matches first, and
a collected row's primary key is a removed key; UPDATE's planning pass
knows exactly which rows move which keys. For each removed key the engine
walks the catalog directory (the walk DROP INDEX performs, now looking
for `foreign_keys` entries instead of index names), scans each
referencing table's column for the key, and refuses — naming the
referencing table and column — for any hit that survives the statement.
The exemptions fall out of the plan, not from special cases: a child row
this same statement deletes does not block it (a self-referencing row may
remove itself — its reference dies with the row), and in UPDATE a
collected row whose own computed new values stop referencing the old key
does not block it either (the `SET eid = 9, manager_id = 9` self-move
keeps up with itself). Cross-table children have no exemptions — this
statement cannot touch them. `DROP TABLE` of a referenced parent refuses
the same way; a self-referencing table drops fine, its constraint dying
with it. There is no CASCADE: the refusal names the referencing table,
which is precisely the information a cascade decision would need —
RESTRICT-only is a scope choice, not a missing feature.

One lesson the implementation paid for twice, and the error discipline
now states plainly: **a refusal thrown inside its own `try` is swallowed
by its own `catch`**. Both parent-side checks guard their catalog loads
and child-file opens with `try`/`catch (const common::Error&)` — an
unreadable catalog is not this statement's error — and the first version
had the `fail(...)` inside the guarded block, so the refusal ate itself
and the statement proceeded. The fix is structural, not a narrower catch:
only the open is guarded; the scan and its refusal live outside the
`try`. The check suite pins both refusals end to end, so the shape cannot
quietly come back.

## Task 20 — EXPLAIN (the plan as text, `sql/Parser`, `exec/Executor`)

The engine had decided every plan detail since the joins landed — which
index, which bounds, what survives as residual, which algorithm each join
node takes — and then announced exactly one word of it at runtime: the
`(via ...)` marker names the root's access path and nothing else. A
student watching the Toyota join could not see that the inner node ran a
plain nested loops while the root probed an index, because the marker
speaks only for the root. EXPLAIN is the answer, and the design question
was where the plan *text* comes from.

The fork: extract planning into a `Plan` structure that the builder
interprets and EXPLAIN renders (plan-as-data — the production shape, and
a rewrite of two hundred working, test-pinned lines), or annotate — the
executing code path appends one line per operator at the very sites where
it already decides. Annotation won, and not just on effort: shown == run
holds *by construction*, because there is no second planner to drift
from. `execute_select` (the body `handle_select` became) takes a plan
collector; a run passes `nullptr` and never builds a string; EXPLAIN
passes the collector, skips only the pipeline drive, and renders. The
marker assignments are gated the same way, so an EXPLAIN result carries
no `(via ...)` line — the CLI's existing report path (a multi-line
message printed verbatim) needed zero changes.

The grammar is the foreign key's trick once more: `EXPLAIN` is matched
contextually at statement start, the lexer's keyword table untouched, so
a table named `explain` still parses. It wraps a SELECT into an
`ExplainStatement`; everything else after the word is refused at parse
time, by name — `EXPLAIN INSERT/UPDATE/DELETE` (`EXPLAIN supports SELECT
statements`), and the two forms a user reaches for from another engine,
`EXPLAIN EXPLAIN` and `EXPLAIN ANALYZE` (the word is recognized
specifically to refuse it; silence would misparse ANALYZE as a table
name).

What makes the shown plan *honest* is what EXPLAIN still does: the bind
refusals fire exactly as a run's would (unknown column, unknown table —
before any file opens), and the session's files open exactly as a run
opens them, lazy index rebuild included. That last point is the subtle
one: a catalog index whose file was deleted rebuilds during EXPLAIN, so
the plan never shows an index path that the next run would not take.
Building the operators is side-effect-free (scans read on pull, and
nothing pulls), so "build, render, don't drive" decides nothing twice.

Rendering details worth pinning, because each one is a decision about
truthfulness:

- **Build order is leaves-first, print order is root-first.** The lines
  accumulate bottom-up (scans, then joins, then filter, project, limit);
  the render reverses them, so the tree reads top-down like Postgres's.
  Each site computes its indentation depth from the root chain
  (limit → project → residual → top join, each join hanging the pipeline
  of the entry it consumes one level below itself).
- **The probed entry has no scan line.** When a join node probes an
  index, the inner scan is *replaced* — so the join line carries the
  probe (`probe emp_dept: e.dept_id = d.did`) and the probed entry's
  pushed conjuncts ride the same line as `inner filter ...`. A plan that
  showed a scan there would describe a node that does not exist.
- **Bounds render as the comparisons they pin** (`person.pid = 2`, or
  `person.pid > 1 AND person.pid < 3`), a conflicting intersection
  renders `bounds conflict: answers nothing` — the execution returns
  empty there, and the plan says so. Suppressed indexes render the heap
  plan, because the suppression gates the planner, not the printer.
- **Expressions render parenthesized at every sub-level** — no
  precedence table to get wrong, and an `(a OR b) AND c` conjunct is
  unambiguous at a glance.

The check suite pins the shape of every plan (heap with residual, index
with equality and with range, the conflict note, the hash and cross
joins, aliases on scans and in probe lines, the three-way plan showing
every node's algorithm at once), the refusals, the no-rows/no-marker
contract, the rebuild-during-EXPLAIN honesty — and, the pin that makes
the whole idea safe: a differential that EXPLAINs each of several
statements and then runs the same statement, demanding the rendered root
access path match the run's announcement, in every access-path setting
the join suite cycles through. Annotation can drift the day someone edits
a decision site and forgets its line; the differential makes that
day fail loudly.

## Task 21 — The timing switch (`config/Config`, `p4dbeng_cli.cpp`)

The engine makes real choices — index versus heap, hash versus loops,
pushdown or not — and the only way to feel their cost was an external
stopwatch, which measures the student's reaction time along with the
engine's work. `timing` in `conf.json` hands the clock to the session
itself: when `true`, every SQL statement reports `time: 0.001234
seconds` as the last line of its report; when `false`, nothing changes.

Three decisions shape it:

- **Required, not optional.** The reader's contract had no optional-key
  concept, and its philosophy is "never silently run with different
  behavior than declared" — so `timing` joined `policy`, the two pool
  sizes, and `hash_join` as a fifth required key, refused by name when
  absent. The accepted cost: every existing `conf.json` must add the
  key before its session starts. An absent switch is a config that
  under-declares the session, and under-declaring is what the strict
  reader exists to catch.
- **The clock wraps the dispatch, not the parse.** The two
  `steady_clock` stamps sit immediately outside the
  `report_errors([&]{ execute_statement(...); })` call — one placement
  earning three properties at once: the span is dispatch through the
  *last line printed* (a student's stopwatch comparison end to end,
  printing included), a refused statement is timed (the named error
  prints inside the same span — the refusal is that statement's
  report), and multi-line pastes exclude typing time (the accumulator
  hands over only complete statements; the user's time between
  continuation lines never enters the clock). The escape-hatch dispatch
  sits at a different call site and stays untimed — debugging tools
  with their own report shapes.
- **`snprintf`, never `std::fixed`.** `cout`'s floating-point state is
  global, and the session prints tuple values elsewhere; flipping the
  stream to fixed six decimals for the time line would silently corrupt
  every later number. The report is assembled in a `char` buffer and
  printed through `operator<<`, leaving the stream untouched. The clock
  is `steady_clock` (monotonic — an NTP jump mid-session cannot corrupt
  a measurement) formatted as `duration<double>` seconds.

The new `check_cli.cpp` drives the real binary through scripted sessions
in a scratch sandbox: one time line per statement (two statements
sharing one input line are timed one by one), the time line last, the
refusal case, `bulk` printing no time line, the line's exact shape, and
`timing: false` keeping the output free of time lines. The config suite
gains the fifth key's parse, missing, duplicate, and non-boolean cases.

## Where the project stands

All twenty-one tasks are implemented; the check suite — now with the
operators suite, the join suite (behavioral pins plus the
two-grammar-forms-versus-reference oracle, pushdown bypass included,
every round run through three access-path settings — everything allowed,
indexes suppressed, both suppressed — once the join columns are indexed),
and the CLI suite driving the real session binary —
is green under regular and sanitizer builds, and the CLI session
(configured by `conf.json`, both pools sized from it) runs the full
statement set over either index type and across joined tables through
all three join algorithms, with the primary key and the foreign keys —
child-side existence, parent-side RESTRICT, self-references included —
enforced on every path that stores or removes a row, and with `EXPLAIN
SELECT` rendering the plan it is about to take — every node's algorithm,
not just the root's — before anything runs. The `operators/`
module is deliberately
self-standing: its files lift out whole, its methods carry per-method
contracts (purpose, parameters, return value), and the join lecture's
algorithms sit in one class each — a student can be asked to
re-implement any of them against what they were taught (for the hash
join, the bucket layer and the key machinery are given support; the
phases are the assignment). `predicates.md` is the teaching companion
to this record: one query — `SELECT * FROM car WHERE car_brand =
'Toyota' LIMIT 10;` — followed from the plan to the printed rows,
showing where a WHERE clause lives once compiled (the `Filter` node's
`Predicate`, the compiled tree captured behind
`truthy_value(evaluate(*on, row))`), what the per-row call actually
executes, and where the object's answers — and its lifetime — end.
