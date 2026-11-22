# sql-ddl

## Purpose

Gives the session lifecycle control over tables and indexes: CREATE and DROP statements that persist as catalog entries and on-disk files, through the engine's catalog writer — the DDL mutation seam.

## Requirements

### Requirement: CREATE TABLE registers a table
`CREATE TABLE <name> (<col> <type> [PRIMARY KEY] [REFERENCES <parent>(<col>)], ...)` SHALL create the table's catalog entry — one JSON catalog file per table, holding the table name, the columns in declaration order, the primary-key column's name when any column carries the suffix, and one entry in a `foreign_keys` array for every column carrying a `REFERENCES` suffix — with `INTEGER`/`INT` mapping to a 4-byte integer column, `VARCHAR(n)` to a string column of declared width n, and `DOUBLE` to an 8-byte double column. At most one column per table SHALL carry `PRIMARY KEY`; a second one SHALL be refused with a named error. A column SHALL carry at most one `REFERENCES` suffix. The table-level `PRIMARY KEY (col)` and `FOREIGN KEY (col) REFERENCES ...` forms SHALL be refused with named errors. Declaring a primary key SHALL also create the constraint's backing index over the table's (empty) rows exactly as `CREATE INDEX` would, recorded in the same catalog entry. After success the table is immediately usable by the other statements. Creating a table whose name already exists in the catalog SHALL fail with a named error and leave the existing catalog untouched.

#### Scenario: New table becomes usable
- **WHEN** `CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, city VARCHAR(3))` succeeds
- **THEN** the catalog file `tablecatalog/person.json` exists, and an INSERT into `person` is accepted without further setup

#### Scenario: Duplicate name refused
- **WHEN** `CREATE TABLE person ...` runs while `tablecatalog/person.json` already exists
- **THEN** the statement fails with a named error and the existing catalog file is unchanged

#### Scenario: A primary key is declared and its index built
- **WHEN** `CREATE TABLE person (pid INTEGER PRIMARY KEY, name VARCHAR(10))` succeeds
- **THEN** the catalog entry records `primary_key: "pid"` and lists the backing index on `pid`, and an immediate lookup through that index answers empty (the table is new), not an error

#### Scenario: Two primary keys refused
- **WHEN** `CREATE TABLE t (a INTEGER PRIMARY KEY, b INTEGER PRIMARY KEY)` runs
- **THEN** the statement fails with a named error and no catalog file is created

#### Scenario: Table-level form refused
- **WHEN** `CREATE TABLE t (a INTEGER, b INTEGER, PRIMARY KEY (a))` runs
- **THEN** the statement fails with a named error (the index layer is single-column; composite keys are not supported)

#### Scenario: A foreign key is declared and recorded
- **WHEN** `person.pid` is the primary key and `CREATE TABLE car (cid INTEGER PRIMARY KEY, owner_id INTEGER REFERENCES person(pid))` succeeds
- **THEN** `car`'s catalog records `foreign_keys: [{column: owner_id, parent_table: person, parent_column: pid}]`, and an INSERT into `car` naming an existing person is accepted without further setup

#### Scenario: Two references on one column refused
- **WHEN** a column definition carries two `REFERENCES` suffixes
- **THEN** the statement fails with a named error and no catalog file is created

#### Scenario: Table-level FOREIGN KEY form refused
- **WHEN** `CREATE TABLE t (a INTEGER, b INTEGER, FOREIGN KEY (b) REFERENCES p(k))` runs
- **THEN** the statement fails with a named error (references are declared inline, and composite references are not supported)

### Requirement: DROP TABLE removes a table, its files, and its indexes
`DROP TABLE <name>` SHALL remove the table's catalog entry, erase its data file from disk, and erase every index file belonging to that table. A data file (or index file) the current session holds open SHALL be closed by the session as part of the drop — the engine refuses to erase a file still open through it, so the session releases its own handles first, and a handle held outside the session's control refuses the drop. Dropping a name with no catalog entry SHALL fail with a named error. After success, statements naming the table report it as unknown.

#### Scenario: Table vanishes completely
- **WHEN** table `person` has a data file and an index file, and `DROP TABLE person` succeeds
- **THEN** the catalog entry, the data file, and the index file are all gone from disk

#### Scenario: Drop closes the session's own open file
- **WHEN** a table's data file was opened earlier in the same session and `DROP TABLE` names that table
- **THEN** the session's handle (and its open index files) are closed first, all files are removed, and later statements naming the table report an unknown table

#### Scenario: Unknown table refused
- **WHEN** `DROP TABLE nosuch` runs with no such catalog entry
- **THEN** the statement fails with a named error and nothing on disk changes

### Requirement: CREATE INDEX builds and records an index
`CREATE INDEX <name> ON <table> (<column>) [USING ISAM | BTREE]` SHALL record the index — name, column, and type — in the table's catalog entry and build the index over the table's current rows as a tree of the declared type (`ISAM` when the clause is absent). An unknown type SHALL be refused with a named error. Creating an index on an unknown table or column SHALL fail with a named error. A second index on the same column of the same table SHALL be refused with a named error regardless of type (one index per column, whatever its kind). A failed CREATE INDEX SHALL leave the catalog unchanged.

#### Scenario: Index over existing rows
- **WHEN** table `person` holds 100 rows and `CREATE INDEX person_pid ON person (pid)` succeeds
- **THEN** the catalog entry for `person` lists index `person_pid` on column `pid` with type `ISAM` (the default), and a SELECT filtering on `pid` can use the index path

#### Scenario: Explicit B+tree type recorded
- **WHEN** `CREATE INDEX person_pid ON person (pid) USING BTREE` succeeds
- **THEN** the catalog entry records the index with type `BTREE`, the index file is a B+tree file, and a SELECT filtering on `pid` uses the index path through the B+tree

#### Scenario: Unknown type refused
- **WHEN** `CREATE INDEX person_pid ON person (pid) USING HASH` runs
- **THEN** the statement fails with a named error naming the unsupported type and the catalog is unchanged

#### Scenario: One index per column
- **WHEN** `person (pid)` already carries an ISAM index and `CREATE INDEX other ON person (pid) USING BTREE` runs
- **THEN** the statement fails with a named error and the catalog still lists exactly one index on `pid`, whatever its type

### Requirement: Index files live beside their data file
An index's physical file SHALL be named after the data file and column (`<data-file>.<column>.idx`) and sit beside it, whatever the index's type; the file's own magic SHALL identify which kind was built. When a data file is opened for a table whose catalog declares indexes and an expected index file is missing, it SHALL be built from that data file's current rows as the catalog-declared type rather than reported as an error. Opening an existing index file SHALL verify the file's kind matches the catalog-declared type; a mismatch SHALL be refused with a named error.

#### Scenario: Index discovered in a later session
- **WHEN** an index was created in an earlier session and a new session opens the same data file
- **THEN** the index is used from its existing file without rebuilding — as the type the catalog declares — and lookups answer from it

#### Scenario: Missing index file rebuilt
- **WHEN** a data file's `.pid.idx` was deleted externally and the data file is opened again
- **THEN** the index file is rebuilt from the data file's rows as the catalog-declared type and answers are correct again

#### Scenario: File kind contradicting the catalog refused
- **WHEN** the catalog declares `pid`'s index as a B+tree but `data/person.tbl.pid.idx` on disk is an ISAM file
- **THEN** opening the index fails with a named error and no entries are returned

### Requirement: DROP INDEX removes an index
`DROP INDEX <name>` SHALL remove the index's catalog entry and erase its file(s) from disk. Dropping an unknown index name SHALL fail with a named error. After success, SELECTs on the table no longer report an index path for that column.

#### Scenario: Dropped index leaves heap path only
- **WHEN** index `person_pid` is dropped and a SELECT filters on `pid` afterwards
- **THEN** the SELECT succeeds via the heap path and prints the heap-scan marker

### Requirement: Catalog writes are all-or-nothing
Every DDL statement that alters a catalog SHALL either fully apply or leave the catalog exactly as before; a mid-write failure SHALL never leave a catalog file that fails to load.

#### Scenario: Catalog survives failed DDL
- **WHEN** a DDL statement fails partway (for example an unwritable target)
- **THEN** the catalog file still loads and describes the state before the failed statement

### Requirement: Canonical file layout
A table's data file SHALL be `data/<table>.tbl` in the working directory, and the `data` directory SHALL be created on demand. An index file SHALL sit beside its data file as `data/<table>.tbl.<column>.idx`. Every SQL operation that resolves a table's files — CREATE TABLE and DROP TABLE file lifecycle, INSERT, SELECT, lazy index rebuild, and index verification — SHALL use these paths. The catalog remains at `tablecatalog/<name>.json`; no other data-file naming exists (in particular, no catalog key redirects a table's data file elsewhere).

#### Scenario: Table data lands in data/
- **WHEN** `CREATE TABLE parts ...` succeeds and rows are inserted
- **THEN** the rows live in `data/parts.tbl`, and no `parts.bin` appears in the working directory

#### Scenario: Index beside its table
- **WHEN** `CREATE INDEX parts_pid ON parts (pid)` succeeds
- **THEN** the index file is `data/parts.tbl.pid.idx`

#### Scenario: Drop removes everything it owns
- **WHEN** `DROP TABLE parts` succeeds
- **THEN** `data/parts.tbl` and every `data/parts.tbl.<column>.idx` are gone from disk, and the working directory never held a `parts.bin`

### Requirement: The primary key owns its index
A declared primary key SHALL own exactly one backing index, created by the `CREATE TABLE` itself, named by the ordinary index convention (`<table>_<column>`), of type `ISAM` (the `CREATE INDEX` default), and recorded among the table's catalog indexes. `DROP INDEX` on that index SHALL be refused with a named error — the constraint owns it, and `DROP TABLE` remains the only way to remove it. `CREATE INDEX` on the primary-key column SHALL remain refused (one index per column — the constraint's index occupies the slot). A catalog entry written before primary keys existed (no `primary_key` field) SHALL load and work unchanged.

#### Scenario: The backing index behaves like a declared index
- **WHEN** a table with `pid INTEGER PRIMARY KEY` holds rows and `checkindex` runs over it
- **THEN** the primary key's index is listed and verifies clean, exactly as a user-created index would

#### Scenario: DROP INDEX refused on the constraint's index
- **WHEN** `DROP INDEX person_pid` names the primary key's backing index
- **THEN** the statement fails with a named error saying the index backs the primary key, and the catalog is unchanged

#### Scenario: CREATE INDEX refused on the primary-key column
- **WHEN** `CREATE INDEX other ON person (pid)` runs on a table whose primary key is `pid`
- **THEN** the statement fails with the one-index-per-column refusal and the catalog is unchanged

#### Scenario: A pre-existing catalog loads unchanged
- **WHEN** a session opens a table whose catalog JSON carries no `primary_key` field (written before this capability)
- **THEN** the table loads, and every statement over it behaves as before

### Requirement: A rebuilt primary-key index refuses duplicate data
When a primary-key index's file is missing and the lazy rebuild rebuilds it from the data file's rows, the rebuild SHALL first verify the column's uniqueness over the data file: duplicate values SHALL be refused with a named error, and no rebuilt index file SHALL be left behind. A data file with unique values rebuilds normally.

#### Scenario: Duplicate data refuses the rebuild
- **WHEN** a primary-key table's index file is deleted and its data file is replaced with one holding two rows of the same key, then a query opens the table
- **THEN** the rebuild fails with a named error naming the duplicate, no index file is created, and the session continues

#### Scenario: Clean data rebuilds normally
- **WHEN** a primary-key table's index file is deleted and its data file holds unique keys, then a query opens the table
- **THEN** the rebuild succeeds and the index answers lookups exactly as before

### Requirement: A foreign key references the parent's primary key
A `REFERENCES <parent>(<col>)` suffix SHALL be validated at `CREATE TABLE` time: the parent table SHALL already exist in the catalog (parent before child — a reference to a not-yet-created table is refused with a named error), and the named column SHALL be the parent's declared primary key. A table SHALL be allowed to reference itself, validated against the primary key being declared in the same statement. The refusals SHALL be named and SHALL create no catalog file. The catalog loader SHALL accept a `foreign_keys` array as written and SHALL NOT cross-check it against other catalogs' files at load time; catalogs written before foreign keys existed (no `foreign_keys` array) SHALL load and work unchanged.

#### Scenario: Parent must exist first
- **WHEN** `CREATE TABLE car (cid INTEGER PRIMARY KEY, owner_id INTEGER REFERENCES person(pid))` runs while `tablecatalog/person.json` does not exist
- **THEN** the statement fails with a named error naming the missing parent, and no catalog file is created

#### Scenario: The referenced column must be the parent's primary key
- **WHEN** `person.pid` is the primary key and `CREATE TABLE car (owner_id INTEGER REFERENCES person(city))` runs
- **THEN** the statement fails with a named error saying the referenced column is not the parent's primary key

#### Scenario: Self-reference allowed
- **WHEN** `CREATE TABLE employee (eid INTEGER PRIMARY KEY, manager_id INTEGER REFERENCES employee(eid))` runs
- **THEN** the statement succeeds and the catalog records the self-referencing foreign key

#### Scenario: A pre-existing catalog loads unchanged
- **WHEN** a session opens a table whose catalog JSON carries no `foreign_keys` array
- **THEN** the table loads, and every statement over it behaves as before

### Requirement: The parent side is protected
For every foreign key in the catalog that names a table and column, the parent side SHALL be protected with immediate RESTRICT semantics: a DELETE removing a parent key, an UPDATE changing a parent key, and `DROP TABLE` of a referenced parent SHALL be refused with a named error naming the referencing table and column — unless no row will still reference the removed key when the statement completes. A row deleted by the same statement, and a collected row whose own computed new values no longer reference the removed key, SHALL NOT block the statement (a self-referencing row may delete itself; a self-referencing row may move its own key together with its reference). A refused statement SHALL change nothing.

#### Scenario: Deleting a referenced parent refused
- **WHEN** `car.owner_id` references `person(pid)`, person 1 exists, and a car references owner_id 1; `DELETE FROM person WHERE pid = 1` runs
- **THEN** the statement fails with a named error naming `car`, the person row remains, and the car still resolves to it

#### Scenario: Dropping a referenced parent refused
- **WHEN** `car` references `person(pid)` and `DROP TABLE person` runs
- **THEN** the statement fails with a named error naming the referencing table, and `person` remains fully intact

#### Scenario: Self-referencing row may delete itself
- **WHEN** `employee.manager_id` references `employee(eid)` and employee 1's manager is employee 1 itself; `DELETE FROM employee WHERE eid = 1` runs
- **THEN** the statement succeeds — the only referencing row is the row being removed

#### Scenario: Parent key move refused while a child still points at the old key
- **WHEN** `car.owner_id` references `person(pid)` with a car pointing at person 1, and `UPDATE person SET pid = 9 WHERE pid = 1` runs
- **THEN** the statement fails with a named error naming `car`, and person 1 still resolves at its old key
