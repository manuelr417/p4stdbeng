# volcano-operators

## Purpose

The execution model: every SELECT evaluates through a pipeline of Volcano-style operators (open / next / close), the nested loops join and its index-probed variant are the join operators, and single-table predicates evaluate at the scan side — with correctness never depending on any of it.

## Requirements

### Requirement: Iterator pipeline execution
Every SELECT SHALL be evaluated as a tree of operators, each implementing the Volcano contract: `open` initializes the operator and its children, `next` produces the next output tuple or reports exhaustion, and `close` releases the operator's resources and closes its children. No operator shall materialize the full input of its child before producing its first tuple, except where the algorithm requires it.

#### Scenario: Single-table SELECT on the pipeline
- **WHEN** any single-table SELECT executes
- **THEN** it produces the same answers, in the same order, as the pre-join engine produced for the same statement and data

#### Scenario: Incremental production
- **WHEN** a SELECT with LIMIT n has its limit satisfied
- **THEN** no further tuples are pulled from the operators below the limit

### Requirement: Nested loops join operator
A two-table join SHALL evaluate as a nested loops join: the outer side is scanned once, and for each outer tuple the inner side is rescanned, emitting every combined tuple that satisfies the join predicate; a cross join (no predicate) emits every combination.

#### Scenario: Equijoin pairs
- **WHEN** a join query pairs rows on an equality predicate
- **THEN** the result contains exactly the matching combinations, including duplicates on both sides

#### Scenario: Cross join
- **WHEN** the comma form is used with no matching predicate in WHERE
- **THEN** every combination of outer and inner rows is produced

#### Scenario: Empty side
- **WHEN** either input of a join has no rows
- **THEN** the join produces no rows and the statement succeeds

### Requirement: Index nested loops join operator
A join whose inner table has an index on a column the join predicate equates with an outer column SHALL be executable as an index nested loops join: the outer side is scanned once, and for each outer tuple the index is probed with that tuple's key, every matching inner row is fetched and combined with the outer tuple.

#### Scenario: Probe answers the equijoin
- **WHEN** the inner table has an index on the join column and the outer tuple's key matches inner rows
- **THEN** the combined pairs are exactly those an inner scan would produce for that outer tuple, duplicates included

#### Scenario: Key with no matches
- **WHEN** the outer tuple's key matches no inner row
- **THEN** the join produces no pair for that tuple and continues with the next

#### Scenario: The predicate still decides
- **WHEN** a pair is produced by a probe
- **THEN** the join predicate is evaluated over the combined pair, so the answer equals the nested loops join's answer regardless of which path produced it

#### Scenario: No pins held across next
- **WHEN** the operator is paused after a next call and other files are scanned through the same buffer pool
- **THEN** the scans succeed and the resumed operator answers exactly what an uninterrupted run answers

### Requirement: Inner-side conjuncts filter probed rows
Single-table WHERE conjuncts pushed to the inner table SHALL be evaluated on each probed inner row before it is combined, so the index path preserves the pushdown semantics of the scan path.

#### Scenario: Pushed conjunct with the index path
- **WHEN** a join query's WHERE contains an inner-table conjunct and the index path executes
- **THEN** the result equals the same query with the conjunct evaluated above the join

### Requirement: Hash join operator
A join SHALL be executable as a hash join: Phase I partitions both inputs on the canonicalized join key into disk-resident bucket files; Phase II answers partition pair by partition pair — the build partition loaded into an in-memory table, the probe partition streamed, every matching combination emitted with the join predicate evaluated over it. The join key SHALL canonicalize values the way the evaluator compares them (numerics as doubles, so `5` and `5.0` meet; strings as bytes).

#### Scenario: Probe answers the equijoin
- **WHEN** both inputs are partitioned on the join key and the probe tuple's key matches build rows
- **THEN** the combined pairs are exactly those a nested loops join would produce for that probe tuple, duplicates on both sides included

#### Scenario: Key with no matches
- **WHEN** a probe tuple's key matches no build row
- **THEN** the join produces no pair for that tuple and continues

#### Scenario: The predicate still decides
- **WHEN** a pair is produced by a hash lookup
- **THEN** the join predicate is evaluated over the combined pair, so the answer equals the nested loops join's answer regardless of which path produced it

#### Scenario: Mixed numeric keys meet
- **WHEN** the join equates an Integer column with a Double column and a row pair holds `5` and `5.0`
- **THEN** the pair is emitted, exactly as the nested loops join emits it

### Requirement: Partitions are provided support
The bucket layer SHALL be provided as its own class: `B−1` disk-resident bucket files under `tmp/`, written and scanned one row at a time through the join buffer pool, erasable as a set. The hash join operator SHALL not manage file paths, schemas, or erasure itself.

#### Scenario: Buckets live in tmp
- **WHEN** a hash join partitions its inputs
- **THEN** the bucket files appear under `tmp/` and nowhere else

#### Scenario: Buckets erased as the query ends
- **WHEN** a hash join statement finishes — normally or through an error
- **THEN** its bucket files are gone from `tmp/`

### Requirement: The join pool is separate
Join scratch I/O — partition writes and reads — SHALL flow through a join buffer pool distinct from the main pool that brings table and index pages in. The join pool's size SHALL come from configuration. Table-page caching SHALL be unaffected by join work: eviction in one pool never evicts the other.

#### Scenario: Join work spares table pages
- **WHEN** a hash join partitions a table while the main pool holds that table's hot pages
- **THEN** the table's pages remain cached in the main pool across the join

### Requirement: Announced join access path
A join SELECT SHALL announce the algorithm that produced its answer, as single-table SELECTs announce theirs: `(via nested loops join)` for the scan path, `(via index nested loops join)` when the answer was produced through index probes, `(via hash join)` when it was produced through partitioned hash probes — preserving `(via index scan)` and `(via heap scan)` for single-table queries.

#### Scenario: Join announcement
- **WHEN** a join SELECT executes with neither index probes nor hash probes available
- **THEN** the result announces `(via nested loops join)`

#### Scenario: Index join announcement
- **WHEN** a join SELECT executes and at least one of its joins probed an index
- **THEN** the result announces `(via index nested loops join)`

#### Scenario: Hash join announcement
- **WHEN** a join SELECT executes and at least one of its joins ran as a hash join
- **THEN** the result announces `(via hash join)`

### Requirement: Single-table predicate pushdown
WHERE conjuncts that reference exactly one FROM table SHALL be evaluated at that table's scan, before any join work; conjuncts referencing more than one table SHALL be evaluated above the join. The answer SHALL not depend on the split.

#### Scenario: Pushed conjunct filters early
- **WHEN** a join query's WHERE contains a single-table conjunct (e.g. `temp_lo > 0` over weather) alongside the join predicate
- **THEN** the result equals the same query with all conjuncts evaluated above the join

#### Scenario: Multi-table conjunct stays above the join
- **WHEN** a WHERE conjunct references columns of both tables (e.g. `city = name`)
- **THEN** the conjunct is evaluated over combined rows, and the result is the matching pairs

### Requirement: Oracle equality across forms and plans
Join answers SHALL hold under a differential oracle: every join SELECT run through both grammar forms, and against a reference join computed independently, SHALL produce identical answers; the carried single-table oracle continues to hold with the pipeline executor.

#### Scenario: Oracle over randomized joins
- **WHEN** the check suite runs randomized two-table data and predicates through both grammar forms and the reference computation
- **THEN** every path yields identical answers

### Requirement: Error discipline over joins
Bind-time refusals for join statements — ambiguous names, unknown tables or columns — SHALL be raised before any page is pinned, and run-time failures during a join SHALL surface as named errors with the session surviving.

#### Scenario: Bind refusal costs no I/O
- **WHEN** a join statement contains an ambiguous bare column name
- **THEN** the named error is raised and no data file is opened for the statement

#### Scenario: Session survives a failing join
- **WHEN** a join statement fails at run time and further statements follow in the session
- **THEN** the error prints and the session continues
