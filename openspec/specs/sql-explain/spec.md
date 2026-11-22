# sql-explain Specification

## Purpose

The EXPLAIN statement: ask for a SELECT's query plan and receive it as text, before anything runs. It makes the planner's per-node decisions — access paths, pinned index bounds, and each join's algorithm — visible without executing, and guarantees the shown plan is the plan a run takes.

## Requirements

### Requirement: EXPLAIN renders the plan of a SELECT
`EXPLAIN <select-statement>` SHALL parse as a statement that renders the SELECT's query plan as text and executes nothing: no rows are produced, no index entries are read, and no access-path marker is printed. The rendered plan SHALL name, per operator: the scan's table (and alias, when declared) with its access path — heap, or index by name with the pinned key bounds — the residual conjuncts a scan's rows still pass through, each join's algorithm with the probe index and the equated columns when an index join applies, the projection's columns, and the limit when present. `EXPLAIN` of a statement that is not a SELECT SHALL be refused with a named error, and so shall `EXPLAIN EXPLAIN` and `EXPLAIN ANALYZE`.

#### Scenario: An index-backed query shows the index scan with bounds
- **WHEN** an index exists on `person.pid` and `EXPLAIN SELECT name FROM person WHERE pid = 42` runs
- **THEN** the rendered plan names `person` scanned through that index with the pinned bound `pid = 42`, then the projection of `name` — and no rows are returned

#### Scenario: A join plan names every node's algorithm
- **WHEN** `car` is joined with `Driver` on `car_owner = did` and `Driver` carries an index on `did`
- **THEN** the rendered plan shows the join node's algorithm (index nested loops) with the probe index named, and each scan's own access path — not just the root join's, as the runtime marker reports

#### Scenario: A heap-backed query shows the heap and the residual
- **WHEN** `EXPLAIN SELECT name FROM person WHERE age > 30 AND city = 'PR'` runs with no index on either column
- **THEN** the rendered plan shows `person` scanned from the heap and the residual conjuncts `age > 30 AND city = 'PR'` as a filter after the scan

#### Scenario: EXPLAIN produces no rows
- **WHEN** `EXPLAIN SELECT * FROM person` runs against a table holding rows
- **THEN** the plan text is the statement's whole output, zero rows are reported, and the table's data is not read

#### Scenario: EXPLAIN of a non-SELECT refused
- **WHEN** `EXPLAIN INSERT INTO person VALUES (1, 'Ruth')` or `EXPLAIN UPDATE person SET age = 1` or `EXPLAIN DELETE FROM person` runs
- **THEN** the statement fails with a named error saying EXPLAIN supports SELECT, and nothing executes

#### Scenario: EXPLAIN EXPLAIN and EXPLAIN ANALYZE refused
- **WHEN** `EXPLAIN EXPLAIN SELECT * FROM person` or `EXPLAIN ANALYZE SELECT * FROM person` runs
- **THEN** the statement fails with a named error naming the unsupported form

### Requirement: The shown plan is the run plan
The plan EXPLAIN renders SHALL be built from the same decisions that execute the statement, in the same order: bind-time refusals SHALL fire exactly as they would for the bare SELECT (an unknown column or a type mismatch refuses the EXPLAIN before any plan exists), the session's files SHALL open exactly as an execution would open them (a missing index file SHALL rebuild lazily during EXPLAIN, as it would before a run), and the rendered access paths SHALL be the ones a subsequent execution of the same statement takes under the same session state — including the access-path settings (indexes suppressed, the plan renders the heap path). The plan text SHALL be produced by the executing code path itself, not by a separate planner.

#### Scenario: A bind refusal still refuses
- **WHEN** `EXPLAIN SELECT nosuch FROM person` runs
- **THEN** the statement fails with the same named bind error the bare SELECT gives, and no plan is rendered

#### Scenario: A deleted index file rebuilds during EXPLAIN
- **WHEN** a table's declared index file is deleted externally and `EXPLAIN SELECT * FROM <table> WHERE <indexed column> = 1` runs
- **THEN** the index file is rebuilt exactly as an execution would rebuild it, and the rendered plan shows the index path — the plan of the statement that would now run

#### Scenario: Suppressed indexes render the heap plan
- **WHEN** the session suppresses index usage and `EXPLAIN SELECT * FROM person WHERE pid = 1` runs over an indexed table
- **THEN** the rendered plan shows the heap scan with the residual — the plan the suppressed run takes

#### Scenario: The rendered plan matches the run's announcement
- **WHEN** `EXPLAIN SELECT ...` renders a scan through a named index, and the same SELECT then runs in the same session
- **THEN** the run announces the access path the plan showed (`(via index scan)` for a plan's index scan, `(via hash join)` for a plan's hash-join node at the root)
