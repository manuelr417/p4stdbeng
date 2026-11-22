# sql-joins

## Purpose

The SQL surface for two-table joins: the two grammar forms the join lecture teaches, their single shared meaning, the name-resolution rules that make multi-table queries unambiguous or refused by name, and the rule-based index-assisted selection that may answer a join by probe instead of rescan — never at the cost of a different answer.

## Requirements

### Requirement: JOIN-ON syntax
A SELECT SHALL accept `FROM <table> JOIN <table> ON <expr>` where each table ref carries an optional alias and the ON expression may reference columns of either table, qualified or bare.

#### Scenario: Equijoin with ON
- **WHEN** the session executes `SELECT city, temp_lo, location FROM weather JOIN cities ON city = name;`
- **THEN** the statement parses and executes, returning the joined rows

#### Scenario: Alias-qualified ON
- **WHEN** the session executes `SELECT w.city, c.name FROM weather w JOIN cities c ON w.city = c.name;`
- **THEN** the statement parses and executes with aliases resolving in the ON expression

### Requirement: Comma-separated FROM syntax
A SELECT SHALL accept `FROM <table> [, <table>]...` — a comma-separated list of table refs — as a join whose predicate is supplied by the WHERE clause.

#### Scenario: Two tables in FROM
- **WHEN** the session executes `SELECT city, temp_lo, location FROM weather, cities WHERE city = name;`
- **THEN** the statement parses and executes, returning the joined rows

#### Scenario: Mixed form is refused or consistent
- **WHEN** a statement mixes the forms (`FROM weather, cities JOIN towns ON ...`)
- **THEN** the behavior is one defined outcome — either it parses with all tables joined in order, or it is refused with a named error naming the unsupported mixture

### Requirement: The two forms share one meaning
The JOIN-ON form and the comma form with the same predicate in WHERE SHALL produce identical answers: the result is every pair of rows (outer, inner) whose combined row satisfies the join predicate together with any residual WHERE conjuncts.

#### Scenario: Identical answers from both forms
- **WHEN** the same tables, projections, and predicate are queried once with JOIN-ON and once with the comma form
- **THEN** the two result sets are identical (same columns, same rows, same order for a deterministic scan)

### Requirement: Qualified column resolution
A column reference qualified by a table name or alias SHALL resolve against that table in the FROM clause; a qualifier naming no table or alias in the query SHALL be a named error raised before any data is read.

#### Scenario: Qualifier by alias
- **WHEN** `w.city` appears in a query whose FROM declares `weather w`
- **THEN** the reference resolves to weather's city column

#### Scenario: Unknown qualifier
- **WHEN** a query references `towns.city` and no FROM table or alias is named `towns`
- **THEN** the statement fails with a named error identifying the unknown table, before any row is produced

### Requirement: Bare column resolution across the join
An unqualified column reference SHALL resolve against the joined schema of every table in the FROM clause; it is an error if the column exists in none of them.

#### Scenario: Bare name found in one table
- **WHEN** a bare column name appears in exactly one of the joined tables
- **THEN** the reference resolves to that table's column

#### Scenario: Bare name in no table
- **WHEN** a bare column name matches no column of any FROM table
- **THEN** the statement fails with a named error identifying the unknown column, before any row is produced

### Requirement: Ambiguous bare column is a named error
An unqualified column reference whose name exists in more than one FROM table SHALL be refused with a named ambiguity error before any page is pinned.

#### Scenario: Name in both tables
- **WHEN** both joined tables have a column named `name` and a bare `name` reference is used
- **THEN** the statement fails with a named error reporting the ambiguity (qualify the reference to resolve it)

### Requirement: Star expansion over a join
`SELECT *` over a join SHALL project every column of every FROM table, ordered as the tables appear in the FROM clause, with each table's columns in schema order.

#### Scenario: Star over two tables
- **WHEN** `SELECT * FROM weather, cities WHERE city = name;` executes
- **THEN** the result carries weather's columns followed by cities' columns

### Requirement: LIMIT applies to the join result
A LIMIT on a join SELECT SHALL stop row production once the limit is reached, exactly as for single-table SELECTs.

#### Scenario: Limit fewer than matches
- **WHEN** a join query with `LIMIT n` has more than n matching pairs
- **THEN** exactly n rows are returned

### Requirement: Index-assisted join selection
When a join's predicate contains an equality conjunct between one column of the tables before the newest FROM entry and one column of the newest entry, and the newest entry has an index on that column, the engine SHALL be able to execute the join by index probe; the answer SHALL be identical to the nested loops scan path's answer for the same statement and data. When no such index applies, or index use is suppressed, the plain nested loops join runs unchanged.

#### Scenario: Index on the inner join column
- **WHEN** the session executes a join whose predicate equates `a.x = b.y`, with an index on b(y), through either grammar form
- **THEN** the answer is identical to the same query executed with index use suppressed, and the announcement names the index join

#### Scenario: Index suppressed
- **WHEN** index use is suppressed and the same join query executes
- **THEN** the answer is identical to the index-assisted run and the announcement names the nested loops join

#### Scenario: Index only on the outer side
- **WHEN** the only applicable index is on a column of the tables before the newest FROM entry
- **THEN** the plain nested loops join runs (no join reorder), producing the same answer

#### Scenario: Predicate shapes that cannot probe
- **WHEN** the join predicate has no equality conjunct naming the newest entry's column (a range, a string match on an unindexed column, or a cross join)
- **THEN** the plain nested loops join runs and the answer is unchanged

### Requirement: Oracle equality with the index path
The join oracle SHALL hold when the index path is allowed: every join SELECT run through both grammar forms, against the reference join, with index use allowed and with index use suppressed, SHALL produce identical answers.

#### Scenario: Oracle over indexed joins
- **WHEN** the check suite runs randomized two-table data and predicates with an index on the inner join column, each query executed with index use allowed and suppressed
- **THEN** every run yields identical answers

### Requirement: Hash-assisted join selection
When a join's predicate contains an equality conjunct between one column of the tables before the newest FROM entry and one column of the newest entry, no index serves that shape on the inner table, the session's configuration enables the hash join, and the estimated build side fits the join pool's memory budget, the engine SHALL be able to execute the join as a hash join; the answer SHALL be identical to the nested loops scan path's answer. Otherwise the plain nested loops join runs unchanged.

#### Scenario: Hash join through both grammar forms
- **WHEN** the session executes a join whose predicate equates `a.x = b.y`, with no index on b(y), hash join enabled, and a build side within budget, through either grammar form
- **THEN** the answer is identical to the same query executed with the plain join, and the announcement names the hash join

#### Scenario: Hash use suppressed
- **WHEN** hash use is suppressed and the same join query executes
- **THEN** the answer is identical and the announcement names the nested loops join

#### Scenario: Hash join disabled by configuration
- **WHEN** the session's configuration turns the hash join off and the same join query executes
- **THEN** the answer is identical and the announcement names the nested loops join or the index join, never the hash join

#### Scenario: Build side over budget
- **WHEN** the newest FROM entry's estimated size exceeds the join pool's memory budget
- **THEN** the plain nested loops join runs and the answer is unchanged

#### Scenario: An index outranks the hash
- **WHEN** the inner table has an index on the equated column, with the hash join on or off
- **THEN** the index join runs and the announcement names the index join, never the hash join

### Requirement: Oracle equality across the three paths
The join oracle SHALL hold with the hash path allowed: every join SELECT, run through both grammar forms against the reference join, SHALL produce identical answers with index and hash use allowed (index path), with indexes suppressed and hash allowed (hash path), and with both suppressed (plain path).

#### Scenario: Oracle over the three paths
- **WHEN** the check suite runs randomized two-table data and predicates with each query executed on all three access paths
- **THEN** every run yields identical answers, and each path's announcement matches the path that ran
