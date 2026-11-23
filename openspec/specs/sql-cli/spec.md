# sql-cli

## Purpose

The session front door: a SQL REPL that accumulates semicolon-terminated statements, survives every error, scripts cleanly when piped, keeps the raw row-id commands as escape hatches for inspecting what SQL claimed, and carries p2dbeng's `append` CSV bulk load.

## Requirements

### Requirement: Statement accumulation and dispatch
A line whose first word is an escape-hatch or session command (`scan`, `read`, `seek`, `bulk`, `append`, `checkindex`, `quit`) SHALL execute immediately. Any other input SHALL accumulate across lines until a line ending in `;`, then be parsed and executed as one SQL statement. The prompt SHALL indicate an unfinished statement while accumulating.

#### Scenario: Multi-line statement
- **WHEN** the user types `SELECT *` on one line and `FROM person;` on the next
- **THEN** one statement is formed and executed, producing one result

#### Scenario: Escape hatch runs bare
- **WHEN** the user types `scan person parts` with no semicolon
- **THEN** the raw scan executes immediately without SQL parsing

### Requirement: Errors never end the session
Every failure — parse error, bind error, execution error, unknown command — SHALL print a message naming the problem and return to the prompt. The session SHALL end only on `quit` or end of input, and the exit code SHALL be 0 unless flushing session files at exit fails, in which case it SHALL be 1.

#### Scenario: Bad input continues
- **WHEN** a statement fails to parse and the user then runs a valid SELECT
- **THEN** the parse error is printed, and the valid statement executes normally afterwards

### Requirement: Piped sessions are scriptable
When input is not a terminal the prompt SHALL be suppressed and output SHALL otherwise be identical to the interactive case, so the same session text produces the same result whether typed or piped.

#### Scenario: Piped session
- **WHEN** a session file containing DDL, INSERT, and SELECT is piped into the CLI
- **THEN** execution proceeds without prompts, prints the same statements' results, and exits with the flush-based exit code

### Requirement: Escape-hatch commands keep their row-id semantics
`scan`, `read`, `seek`, and `bulk` SHALL keep their existing behavior: records addressed by `(page, slot)` row id, scan reporting a count, reads showing a `(pool hit)` marker when the page is already resident, bulk continuing generated numbering after existing rows and maintaining the table's indexes. Files bind to the table they were first used with; naming a different table later SHALL be refused.

#### Scenario: Raw read shows the pool hit
- **WHEN** the same row id is read twice in a session with a small pool
- **THEN** the second read prints the `(pool hit)` marker

### Requirement: append bulk-loads CSV without SQL
The session SHALL accept an `append` command of the form `append <table> <csv-file>` that loads the CSV file's rows into the named table's data file (resolving the file path by the same rules as the other commands), creating the data file when missing, through the same index-maintaining insert path as SQL INSERT. Each invalid row SHALL be reported as `line N: reason` on the diagnostics stream and skipped, never fatal; the command SHALL end with counts of appended and skipped rows.

#### Scenario: Bulk load
- **WHEN** the user runs `append person person.csv` against a table whose data file does not yet exist
- **THEN** every valid CSV row is inserted into the table's data file (and into any declared index), and the command reports the appended and skipped counts

#### Scenario: Invalid rows never abort
- **WHEN** the CSV contains rows that violate the table's structure
- **THEN** each is reported with its line number and reason, valid rows are still appended, and the session continues

### Requirement: checkindex verifies a data file's indexes
`checkindex <table> <file>` SHALL run the index verification over every index the catalog declares for the table against that data file, reporting each index's entry count plus any dangling or out-of-order findings, and `OK` per index when clean. An unknown table SHALL fail with a named error.

#### Scenario: Clean index reports OK
- **WHEN** `checkindex person parts` runs and every index entry resolves to an occupied slot
- **THEN** each index reports its count and `OK`

#### Scenario: Dangling entry reported
- **WHEN** an index holds an entry whose slot is free in the data file
- **THEN** `checkindex` names the offending index, key, and row id, and the report is not clean

### Requirement: Escape-hatch paths resolve through data/
A file argument given to the escape-hatch commands (`scan`, `read`, `seek`, `bulk`, `append`, `checkindex`) that contains no `/` SHALL resolve relative to `data/`. A path containing a `/` SHALL be used exactly as given, so absolute and subdirectory paths keep working.

#### Scenario: Bare name finds the table file
- **WHEN** `scan parts parts` runs for a table stored at `data/parts.tbl`
- **THEN** the scan reads `data/parts.tbl` and reports its records

#### Scenario: Explicit path is honored as given
- **WHEN** `scan parts /tmp/demo.tbl` runs
- **THEN** the scan reads exactly `/tmp/demo.tbl`, not something under `data/`

### Requirement: Session configuration from conf.json
The session SHALL read `conf.json` from the working directory at startup, with six required keys: `policy`, `pool_size`, `join_pool_size`, the `hash_join` switch, the `timing` switch, and the `labeled_rows` switch. A missing or invalid file SHALL end the session before any statement runs — never a silent default.

#### Scenario: A valid config starts the session
- **WHEN** `conf.json` names a registered policy, two positive pool sizes, and boolean hash-join, timing, and labeled-rows switches
- **THEN** the session starts with the pools sized and policed as declared, the hash join available per the hash-join switch, per-statement timing enabled per the timing switch, and SELECT rows rendered per the labeled-rows switch

#### Scenario: A broken config is fatal
- **WHEN** `conf.json` is missing, unparseable, or missing a key
- **THEN** the session prints a named error and exits without running statements

#### Scenario: Invalid values are refused by name
- **WHEN** `conf.json` carries duplicates, trailing garbage, a non-integer pool size, a non-boolean switch value, or an unregistered policy name
- **THEN** the session prints a named error identifying the offending key and exits

#### Scenario: Hash join off leaves the plain paths
- **WHEN** `hash_join` is `false` and a join query executes whose predicate equates the tables' columns
- **THEN** the answer is produced by the index join when an index serves the shape, by the nested loops join otherwise — never announced as a hash join

### Requirement: SELECT row rendering under the labeled_rows switch
When `labeled_rows` is `true`, each SELECT result row SHALL print in the labeled form — every value prefixed by its column name and `=`, pairs comma-separated — exactly as the session has always rendered them. When `labeled_rows` is `false`, a SELECT's report SHALL print one header line of the projected column names separated by `", "`, then one line per row of the values only, separated by `", "` in the same column order. The access-path announcement line, the trailing `N row(s)` count, the timing line, non-SELECT messages, EXPLAIN output, and the escape-hatch commands' `(page,slot) col=value` records SHALL be identical under both settings. The switch is startup configuration: no statement or session command SHALL change it.

#### Scenario: Labeled rows keep the current format
- **WHEN** `labeled_rows` is `true` and a SELECT returns rows
- **THEN** each row prints as `col=value, col=value, ...` — the format before this change, byte for byte

#### Scenario: Unlabeled rows print a header once and values only
- **WHEN** `labeled_rows` is `false` and a SELECT returns rows
- **THEN** the first line of the result is the projected column names joined by `", "`, and each following line is the row's values joined by `", "` with no column names

#### Scenario: The report lines do not move
- **WHEN** `labeled_rows` is `false` and a SELECT announces an access path and returns N rows
- **THEN** the output still opens with `(via <access path>)`, still ends with `N row(s)`, and carries the `time:` line last when timing is on — only the row lines' shape changed

#### Scenario: An empty result still names its columns
- **WHEN** `labeled_rows` is `false` and a SELECT matches no rows
- **THEN** the header line still prints, followed by `0 row(s)`

#### Scenario: Escape hatches keep their record form
- **WHEN** `labeled_rows` is `false` and a `scan` or `read` escape hatch runs
- **THEN** each record still prints as `(page,slot)  col=value, col=value, ...` — the ex row-id semantics are not SELECT reports

### Requirement: Per-statement timing under the timing switch
When `timing` is `true`, the session SHALL measure each SQL statement's elapsed time — from the moment the complete statement dispatches to the executor until the last line of its report (result rows, a count, or a named error) has printed — and SHALL print it as the statement's final line in the form `time: <seconds> seconds`, seconds with six decimal places, on the standard output. Each semicolon-terminated statement SHALL be timed individually; a statement accumulated over several input lines SHALL be timed from its dispatch (the user's time between lines is not engine time), and a refused statement SHALL be timed as any other. When `timing` is `false`, the session SHALL take no time and print no time line. The escape-hatch commands (`scan`, `read`, `seek`, `bulk`, `append`, `checkindex`) SHALL NOT be timed.

#### Scenario: Each statement reports its own time
- **WHEN** `timing` is `true` and one input line carries `SELECT 1; SELECT 2;`
- **THEN** each statement's report ends with its own `time: <seconds> seconds` line — two time lines for two statements

#### Scenario: Timing off leaves the output unchanged
- **WHEN** `timing` is `false` and statements run
- **THEN** no `time:` line prints and the session's output is otherwise exactly as it would be without the switch

#### Scenario: A refused statement is still timed
- **WHEN** `timing` is `true` and a statement fails with a named error (an unknown table, a constraint refusal)
- **THEN** the error prints as usual and a `time: <seconds> seconds` line follows it — the refusal is that statement's report

#### Scenario: Escape-hatch commands are not timed
- **WHEN** `timing` is `true` and an escape-hatch command such as `bulk` or `append` runs
- **THEN** the command's report prints as usual with no `time:` line

#### Scenario: The time line reads in seconds
- **WHEN** `timing` is `true` and any SQL statement completes
- **THEN** the final line matches `time: <number> seconds`, the number carrying six decimal places of second-resolution elapsed time measured on a monotonic clock

### Requirement: tmp maintained for join scratch
The session SHALL keep a `tmp/` directory beside `data/` for join scratch files: stale contents wiped when the session starts, the directory created when first needed, and files left by an interrupted earlier session not treated as an error.

#### Scenario: Stale scratch does not survive a restart
- **WHEN** a previous session left files in `tmp/` (a crash mid-query) and a new session starts
- **THEN** the new session wipes them before the first statement and runs normally

### Requirement: Export a SELECT result to a CSV file
A SELECT dispatched with a pipe suffix — `<query> | <target>` inside the statement's `;`-terminated text — SHALL write its result set to the target file in the engine's CSV dialect and SHALL NOT print the result rows to the standard output. The dialect SHALL be one header line of the projected column names followed by one line per row of the values only, every field separated by a bare `,` with no spaces, so that the file parses back through the engine's own CSV reader. The split between query and target SHALL be the last `|` outside single-quoted strings; the text before SHALL parse as exactly one statement and the trimmed text after is the target, which SHALL resolve like `append`'s CSV paths: a bare name through `data/`, a path with a slash exactly as given. A failed or refused query SHALL create no file. The access-path announcement and the `N row(s)` count SHALL still print, and the statement SHALL be timed as any other when the timing switch is on — the file write inside the dispatch clock. The file format SHALL NOT depend on the `labeled_rows` switch.

#### Scenario: A piped SELECT writes the file and keeps the report
- **WHEN** a SELECT with a pipe target matches rows and the target opens
- **THEN** the file holds the projected column names on the first line and the values only on each following line, all comma-separated with no spaces; the standard output shows `(via <access path>)` and `N row(s)` but no row lines, and the `time:` line when timing is on

#### Scenario: The file round-trips through the engine's CSV reader
- **WHEN** an exported file's lines are parsed by the engine's CSV row reader against the source table's schema
- **THEN** every line parses to a tuple with zero skips — bare commas, no quoting, header tolerated by the reader's caller

#### Scenario: A bare target resolves through data/
- **WHEN** the pipe target is a bare file name with no slash
- **THEN** the file is written to `data/<name>`, and a target containing a slash is used exactly as given — the same rule `append` applies

#### Scenario: A refused or failed query leaves no file
- **WHEN** the piped statement is a non-SELECT, names no target, carries a second pipe, or fails with a named error
- **THEN** the refusal or error prints by name and no file is created or truncated

#### Scenario: An unopenable target is a named error
- **WHEN** the query succeeds but the target file cannot be opened for writing
- **THEN** the session prints `cannot open '<path>'` and continues — the next statement runs

#### Scenario: An empty result writes the header only
- **WHEN** a piped SELECT matches no rows
- **THEN** the file contains only the header line, and the standard output reports `0 row(s)`

#### Scenario: A pipe inside a string literal is not a split
- **WHEN** a SELECT's predicate contains a `|` inside single quotes
- **THEN** the query parses whole and no export happens — only an unquoted pipe splits the statement
