// ex13 — exec layer checks: bind-time refusals, evaluation semantics,
// access-path selection with its markers, the DDL seam (CREATE/DROP
// TABLE and INDEX through the catalog writer), INSERT all-or-nothing
// with index maintenance, and SELECT projection. Runs inside a sandbox
// directory so catalogs and data files never touch the workspace.

#include "bufpool/BufferManager.h"
#include "check_support.h"
#include "exec/Binder.h"
#include "exec/Executor.h"
#include "exec/JoinedSchema.h"
#include "exec/Pushdown.h"
#include "policy/LRUPolicy.h"
#include "schema/TableGenerator.h"
#include "schema/TableSchemaJson.h"
#include "sql/Ast.h"
#include "sql/Parser.h"

#include <array>
#include <map>
#include <sstream>
#include <optional>
#include <type_traits>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <random>
#include <sys/stat.h>
#include <vector>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cout << "FAIL: " << message << "\n";
        ++failures;
    }
}

using checks::ok;

// Parses one statement and runs it through the executor.
exec::StatementResult execute_one(exec::Executor& executor, const std::string& text) {
    sql::ParseResult parsed = sql::parse(text);
    if (parsed.statements.size() != 1) {
        throw common::Error("expected exactly one statement");
    }
    return executor.execute(*parsed.statements.front());
}

// Runs one statement into `result`; false (with the reason printed) when
// it fails.
bool run_sql(exec::Executor& executor, const std::string& text, exec::StatementResult& result) {
    try {
        result = execute_one(executor, text);
        return true;
    } catch (const common::Error& e) {
        std::cout << "      " << text << ": " << e.what() << "\n";
        return false;
    }
}

// The message one statement fails with ("" when it succeeds).
std::string sql_error(exec::Executor& executor, const std::string& text) {
    return checks::error_of([&] { execute_one(executor, text); });
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Inserts `count` generated rows (ids from 1) through the escape-hatch
// write path, which maintains the table's indexes.
void insert_generated(exec::Executor& executor, const std::string& table, std::size_t count) {
    for (const tuple::Tuple& tuple :
         schema::generate_tuples(executor.table(table).schema, count, 1234, 1)) {
        executor.insert_into(table, "data/" + table + ".tbl", tuple);
    }
}

// --- joined-schema binding (the join suite's bind-level section) ---------
//
// Resolution rules exercised directly against JoinedSchema: qualified
// refs by table name or alias, bare refs across all tables, and the
// named refusals (ambiguity, unknown qualifier, unknown column) — plus
// bind_expr through the joined space, with indices landing in
// FROM-order concatenation positions.

const char* const kWeatherSchemaText = R"({"table_name": "weather",
    "columns": [
      {"column_name": "city",    "column_type": "String",  "size": 12},
      {"column_name": "temp_lo", "column_type": "Integer", "size": 4}]})";

const char* const kCitiesSchemaText = R"({"table_name": "cities",
    "columns": [
      {"column_name": "name",     "column_type": "String", "size": 12},
      {"column_name": "location", "column_type": "String", "size": 3}]})";

// A value-returning try: the body's result, or nullopt on a named error.
template <class F>
auto ok_result(F&& body) -> std::optional<std::invoke_result_t<F&>> {
    try {
        return body();
    } catch (const common::Error&) {
        return std::nullopt;
    }
}

// Parses one expression out of a WHERE clause: `SELECT 1 FROM t WHERE <expr>`.
// The parse lives in a local static holder just long enough for the caller
// to bind against it.
const sql::Expr* where_of(const std::string& expr_text) {
    static std::optional<sql::ParseResult> holder;
    try {
        holder = sql::parse("SELECT 1 FROM t WHERE " + expr_text);
    } catch (const common::Error& e) {
        std::cout << "      where_of parse failed: " << e.what() << "\n";
        holder.reset();
        return nullptr;
    }
    if (holder->statements.size() != 1) {
        return nullptr;
    }
    const auto* select =
        static_cast<const sql::SelectStatement*>(holder->statements[0].get());
    return select->where.get();
}

void joined_bind_checks() {
    const schema::TableSchema weather = schema::parse_table_schema(kWeatherSchemaText);
    const schema::TableSchema cities = schema::parse_table_schema(kCitiesSchemaText);

    exec::JoinedSchema joined;
    joined.add("weather", "", &weather);
    joined.add("cities", "c", &cities);

    // Bare and qualified resolution land in concatenation positions.
    check(joined.resolve("", "city").index == 0 &&
              joined.resolve("", "city").type == tuple::ColumnType::String &&
              joined.resolve("", "temp_lo").index == 1 &&
              joined.resolve("", "name").index == 2 &&
              joined.resolve("", "location").index == 3 &&
              joined.resolve("cities", "name").table_position == 1 &&
              joined.resolve("c", "location").table_position == 1 &&
              joined.resolve("weather", "temp_lo").table_position == 0,
          "JoinedSchema resolves bare and qualified names to concatenated positions");

    // A table name and its alias select the same entry.
    check(joined.resolve("cities", "name").index == joined.resolve("c", "name").index,
          "JoinedSchema resolves qualifiers by table name or alias");

    // The named refusals. The ambiguity case needs two tables that share
    // a column name, so it gets its own fixture.
    const schema::TableSchema named_a = schema::parse_table_schema(
        R"({"table_name": "a",
            "columns": [
              {"column_name": "name", "column_type": "String",  "size": 5},
              {"column_name": "x",    "column_type": "Integer", "size": 4}]})");
    const schema::TableSchema named_b = schema::parse_table_schema(
        R"({"table_name": "b",
            "columns": [
              {"column_name": "name", "column_type": "String",  "size": 5},
              {"column_name": "y",    "column_type": "Integer", "size": 4}]})");
    exec::JoinedSchema both_named;
    both_named.add("a", "", &named_a);
    both_named.add("b", "", &named_b);
    check(contains(checks::error_of([&] { both_named.resolve("", "name"); }), "ambiguous"),
          "a bare name in two tables is refused as ambiguous");
    check(contains(checks::error_of([&] { joined.resolve("towns", "city"); }),
                   "unknown table or alias"),
          "an unknown qualifier is refused by name");
    check(contains(checks::error_of([&] { joined.resolve("", "nosuch"); }), "unknown column"),
          "an unknown bare column is refused by name");
    check(contains(checks::error_of([&] { joined.resolve("cities", "city"); }),
                   "unknown column 'city' in table 'cities'"),
          "an unknown column under a known qualifier is refused by name");

    // bind_expr through the joined space.
    const sql::Expr* undeclared = where_of("w.city = c.name");
    bool refused = false;
    if (undeclared != nullptr) {
        // w is not a declared alias in `joined` — refused by name.
        refused = contains(checks::error_of([&] { exec::bind_expr(*undeclared, joined); }),
                           "unknown table or alias 'w'");
    }
    check(refused, "bind_expr refuses an undeclared qualifier through the join");

    exec::JoinedSchema aliased;
    aliased.add("weather", "w", &weather);
    aliased.add("cities", "c", &cities);
    const sql::Expr* equijoin = where_of("w.city = c.name");
    bool equijoin_binds = false;
    if (equijoin != nullptr) {
        const exec::BoundExprPtr bound = [&]() -> exec::BoundExprPtr {
            try {
                return exec::bind_expr(*equijoin, aliased);
            } catch (const common::Error&) {
                return nullptr;
            }
        }();
        equijoin_binds = bound != nullptr && bound->left->column_index == 0 &&
                         bound->right->column_index == 2;
    }
    check(equijoin_binds, "bind_expr resolves the aliased equijoin to positions 0 and 2");

    const sql::Expr* typed = where_of("city = temp_lo");
    check(typed != nullptr &&
              contains(checks::error_of([&] { exec::bind_expr(*typed, joined); }),
                       "cannot compare"),
          "bind_expr keeps type strictness across the join");

    // The single-table entry: bare names resolve; a qualifier must be the
    // table's own name (the wrapper knows no alias).
    const sql::Expr* single = where_of("city = 'PR'");
    bool single_binds = false;
    if (single != nullptr) {
        single_binds =
            checks::ok([&] { return exec::bind_expr(*single, weather).get() != nullptr; });
    }
    check(single_binds, "the single-table bind entry resolves bare names");
    const sql::Expr* foreign = where_of("c.city = 'PR'");
    check(foreign != nullptr &&
              contains(checks::error_of([&] { exec::bind_expr(*foreign, weather); }),
                       "unknown table or alias"),
          "the single-table bind entry refuses a foreign qualifier");

    // --- pushdown: the WHERE splitter --------------------------------------
    //
    // Conjuncts classify by their referenced-table set: one table →
    // pushed (bound with table-local positions), anything else → residual
    // (bound with joined positions).

    const auto split_of = [&](const std::string& expr_text) {
        const sql::Expr* where = where_of(expr_text);
        return where ? ok_result([&] { return exec::split_where(*where, aliased); })
                     : std::optional<exec::PushdownSplit>{};
    };

    // The lecture's equijoin conjunct stays above the join.
    std::optional<exec::PushdownSplit> equijoin_split = split_of("city = name");
    check(equijoin_split.has_value() && equijoin_split->pushed.empty() &&
              equijoin_split->residual.size() == 1 &&
              equijoin_split->residual[0]->left->column_index == 0 &&
              equijoin_split->residual[0]->right->column_index == 2,
          "split_where keeps the equijoin conjunct residual with joined positions");

    // A single-table conjunct pushes with a table-local index (temp_lo is
    // weather's second column).
    std::optional<exec::PushdownSplit> single_table = split_of("temp_lo > 0");
    check(single_table.has_value() && single_table->residual.empty() &&
              single_table->pushed.size() == 1 &&
              single_table->pushed[0].table_position == 0 &&
              single_table->pushed[0].expr->left->column_index == 1,
          "split_where pushes a single-table conjunct with a table-local index");

    // The lecture example: temp_lo > 0 AND city = name — one pushed, one
    // residual. A second single-table conjunct on the other table pushes
    // to its own scan.
    std::optional<exec::PushdownSplit> mixed =
        split_of("w.temp_lo > 0 AND city = name AND location = 'PR'");
    check(mixed.has_value() && mixed->pushed.size() == 2 && mixed->residual.size() == 1 &&
              mixed->pushed[0].table_position == 0 &&
              mixed->pushed[1].table_position == 1 &&
              mixed->pushed[1].expr->left->column_index == 1,
          "split_where classifies the worked example's conjuncts per table");

    // A column-free conjunct stays residual (no scan to push it into).
    std::optional<exec::PushdownSplit> constant = split_of("1 = 1");
    check(constant.has_value() && constant->pushed.empty() &&
              constant->residual.size() == 1,
          "split_where keeps column-free conjuncts residual");

    // An unknown column inside any conjunct refuses before the split
    // returns.
    const sql::Expr* unknown = where_of("temp_lo > 0 AND nosuch = 1");
    check(unknown != nullptr &&
              contains(checks::error_of([&] { exec::split_where(*unknown, aliased); }),
                       "unknown column"),
          "split_where refuses an unknown column by name");
}

} // namespace

int run_exec_checks() {
    // The bind-level joined-schema checks run outside the sandbox — they
    // touch no files and pin no pages.
    joined_bind_checks();

    // The sandbox: a fresh temp directory becomes the working directory,
    // so `data/<table>.tbl` and `tablecatalog/` land there. Restored on exit.
    char pattern[] = "/tmp/ex13-exec-XXXXXX";
    char* made = mkdtemp(pattern);
    if (made == nullptr) {
        std::cout << "FAIL: exec sandbox could not be created\n";
        return 1;
    }
    const std::string original = [] {
        std::vector<char> buffer;
        buffer.resize(4096);
        return std::string(getcwd(buffer.data(), buffer.size()));
    }();
    chdir(made);
    if (system("mkdir -p tablecatalog data") != 0) {
        std::cout << "FAIL: exec sandbox catalog dir\n";
    }

    bufman::BufferManager manager;
    manager.init(16, std::make_unique<bufman::LRUPolicy>());
    exec::Executor executor(manager);

    exec::StatementResult result;

    // --- 5.4. CREATE TABLE through the catalog writer ---
    {
        check(run_sql(executor, "CREATE TABLE person (pid INTEGER, name VARCHAR(10), "
                      "age INT, city VARCHAR(3))", result) &&
                  result.message == "table 'person' created",
              "CREATE TABLE reports success");
        std::ifstream catalog("tablecatalog/person.json");
        check(catalog.good(), "the catalog file exists after CREATE TABLE");
        check(contains(sql_error(executor, "CREATE TABLE person (a INTEGER)"), "already exists"),
              "a duplicate CREATE TABLE is refused by name");
    }

    // --- 5.6. INSERT: validation, all-or-nothing, index maintenance ---
    {
        check(run_sql(executor, "INSERT INTO person VALUES (1, 'Ruth', 42, 'PR'), "
                      "(2, 'Amy', 30, 'MA'), (3, 'Ivan', 42, 'PR')", result) &&
                  result.message == "inserted 3 row(s)",
              "a three-row INSERT reports its count");
        check(run_sql(executor, "SELECT * FROM person", result) &&
                  result.rows.rows.size() == 3,
              "the inserted rows are visible");
        check(!sql_error(executor, "INSERT INTO person VALUES (4, 'WayTooLongName', 1, 'PR')").empty(),
              "a too-wide string is refused with a named error");
        check(run_sql(executor, "SELECT * FROM person", result) &&
                  result.rows.rows.size() == 3,
              "a refused INSERT stored nothing (all-or-nothing)");
        check(contains(sql_error(executor, "INSERT INTO person VALUES (5, 'X', 1)"), "4"),
              "a wrong-arity row is refused with the column count");
        check(contains(sql_error(executor, "INSERT INTO person VALUES (5, 'X', 'old', 'PR')"), "must be"),
              "a string in an integer column is refused");
    }

    // --- 5.5. CREATE INDEX / DROP INDEX through the catalog ---
    {
        check(run_sql(executor, "CREATE INDEX person_pid ON person (pid)", result) &&
                  contains(result.message, "person_pid"),
              "CREATE INDEX reports success");
        const std::vector<schema::TableIndex>& indexes = executor.table("person").indexes;
        check(indexes.size() == 1 && indexes[0].name == "person_pid" &&
                  indexes[0].column == "pid",
              "the catalog lists the new index");
        check(contains(sql_error(executor, "CREATE INDEX another ON person (pid)"), "one index per column"),
              "a second index on the same column is refused");
        check(executor.table("person").indexes.size() == 1,
              "the refused CREATE INDEX left the catalog unchanged");
        check(contains(sql_error(executor, "CREATE INDEX bad ON person (nosuch)"), "unknown column"),
              "an index on an unknown column is refused");
    }

    // --- 5.3. access paths and their markers ---
    {
        check(run_sql(executor, "SELECT * FROM person WHERE pid = 2", result) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "equality on the indexed column takes the index scan");

        check(run_sql(executor, "SELECT * FROM person WHERE pid > 1 AND pid < 3", result) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "an AND range on the indexed column takes the index scan");

        check(run_sql(executor, "SELECT * FROM person WHERE pid = 1 AND city = 'PR'", result) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1,
              "an indexed equality plus a residual conjunct still uses the index");

        check(run_sql(executor, "SELECT * FROM person WHERE city = 'PR'", result) &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 2,
              "an unindexed column scans the heap");

        check(run_sql(executor, "SELECT * FROM person WHERE pid = 1 OR city = 'MA'", result) &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 2,
              "a top-level OR degrades to the heap scan");

        check(run_sql(executor, "SELECT * FROM person WHERE pid = 99", result) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.empty(),
              "an index-scan miss answers empty");
    }

    // --- the index-join selection rule --------------------------------------
    {
        check(run_sql(executor, "CREATE TABLE dept (did INTEGER, dname VARCHAR(8))", result) &&
                  run_sql(executor, "CREATE TABLE emp (eid INTEGER, dept_id INTEGER, ename VARCHAR(8))",
                          result),
              "the index-join fixture tables created");
        check(run_sql(executor,
                      "INSERT INTO dept VALUES (1, 'eng'), (2, 'ops'), (3, 'art')", result) &&
                  run_sql(executor,
                          "INSERT INTO emp VALUES (10, 1, 'ann'), (11, 2, 'bob'), "
                          "(12, 2, 'cy'), (13, 9, 'noone')",
                          result),
              "the index-join fixture rows inserted");
        check(run_sql(executor, "CREATE INDEX emp_dept ON emp (dept_id)", result),
              "the inner table's index created");

        // The rule fires: an indexed equijoin through both grammar forms
        // announces the index join and answers the matching pairs.
        check(run_sql(executor, "SELECT did, eid FROM dept JOIN emp ON did = dept_id", result) &&
                  result.rows.access_path == "index nested loops join" &&
                  result.rows.rows.size() == 3,
              "an indexed equijoin (JOIN form) takes the index join");
        check(run_sql(executor, "SELECT did, eid FROM dept, emp WHERE did = dept_id", result) &&
                  result.rows.access_path == "index nested loops join" &&
                  result.rows.rows.size() == 3,
              "an indexed equijoin (comma form) takes the index join");
        check(run_sql(executor, "SELECT did, eid FROM dept d JOIN emp e ON d.did = e.dept_id", result) &&
                  result.rows.access_path == "index nested loops join",
              "aliases resolve inside the rule's shape test");

        // The answers match the plain path: suppression flips the
        // announcement, never the rows.
        const auto as_pairs = [&](const std::string& query) {
            std::vector<std::pair<int, int>> rows;
            if (run_sql(executor, query, result)) {
                for (const std::vector<exec::Value>& row : result.rows.rows) {
                    rows.emplace_back(std::get<int>(row[0]), std::get<int>(row[1]));
                }
            }
            std::sort(rows.begin(), rows.end());
            return rows;
        };
        const std::vector<std::pair<int, int>> via_index = as_pairs(
            "SELECT did, eid FROM dept JOIN emp ON did = dept_id");
        executor.set_allow_index(false);
        // (hash-join supersession: with indexes suppressed and the hash
        // join enabled, the equated shape answers through the hash join —
        // the spec's second preference — where it once fell to the plain
        // join.)
        check(as_pairs("SELECT did, eid FROM dept JOIN emp ON did = dept_id") == via_index &&
                  result.rows.access_path == "hash join",
              "suppressing indexes flips the announcement, not the answer");
        executor.set_allow_index(true);

        // Shapes that cannot probe keep the plain join.
        check(run_sql(executor, "SELECT did, eid FROM dept, emp WHERE dept_id > did", result) &&
                  result.rows.access_path == "nested loops join",
              "a range between the join columns does not probe");
        // (hash-join supersession: the outer-side index still never probes —
        // no reorder — but the equated shape now answers through the hash
        // join instead of the plain join.)
        check(run_sql(executor, "SELECT eid, did FROM emp JOIN dept ON dept_id = did", result) &&
                  result.rows.access_path == "hash join",
              "an index only on the outer side never probes (no reorder — hash answers)");

        // An equality among other conjuncts still probes, and the pushed
        // inner conjunct filters the probed rows inside the operator.
        check(run_sql(executor,
                      "SELECT did, eid FROM dept, emp WHERE ename = 'bob' AND did = dept_id", result) &&
                  result.rows.access_path == "index nested loops join" &&
                  result.rows.rows.size() == 1,
              "an indexed equality among other conjuncts still probes");
        check(as_pairs("SELECT did, eid FROM dept, emp WHERE ename <> 'ann' AND did = dept_id") ==
                      (std::vector<std::pair<int, int>>{{2, 11}, {2, 12}}) &&
                  result.rows.access_path == "index nested loops join",
              "a pushed inner conjunct filters probed rows");

        // The self-join: one table, two aliases, one shared index — the
        // inner alias's column resolves to the indexed one.
        check(run_sql(executor,
                      "SELECT e1.eid, e2.eid FROM emp e1 JOIN emp e2 ON e1.dept_id = e2.dept_id",
                      result) &&
                  result.rows.access_path == "index nested loops join" &&
                  result.rows.rows.size() == 6,
              "a self-join probes the shared index");

        // --- the hash join's slot in the rule (spec: sql-joins) ------------
        // No index serves the shape, hash enabled: both grammar forms take
        // the hash join and answer exactly what the index path answers for
        // the same pairs.
        const std::vector<std::pair<int, int>> via_hash =
            as_pairs("SELECT did, eid FROM emp JOIN dept ON dept_id = did");
        check(run_sql(executor, "SELECT did, eid FROM emp JOIN dept ON dept_id = did", result) &&
                  result.rows.access_path == "hash join",
              "an equated shape no index serves takes the hash join (JOIN form)");
        check(run_sql(executor, "SELECT did, eid FROM emp, dept WHERE dept_id = did", result) &&
                  result.rows.access_path == "hash join" &&
                  as_pairs("SELECT did, eid FROM emp, dept WHERE dept_id = did") == via_hash,
              "the comma form takes the hash join too, same answers");
        check(via_hash == via_index,
              "the hash join answers what the index join answers");

        // The preference order: an inner index outranks the hash join with
        // the switch on or off — the hash join answers only the shapes no
        // index serves.
        executor.set_allow_hash(false);
        check(run_sql(executor, "SELECT did, eid FROM dept JOIN emp ON did = dept_id", result) &&
                  result.rows.access_path == "index nested loops join",
              "an inner index outranks the hash join with the switch off");
        // Hash disabled at the executor flag (the config-off session
        // state): the equated shape downgrades to the plain join, the
        // answers unchanged, and the announcement never says hash.
        check(run_sql(executor, "SELECT did, eid FROM emp JOIN dept ON dept_id = did", result) &&
                  result.rows.access_path == "nested loops join" &&
                  as_pairs("SELECT did, eid FROM emp JOIN dept ON dept_id = did") == via_hash,
              "hash disabled downgrades to the plain join, same answers");
        executor.set_allow_hash(true);

        // The memory gate — slide 29's sizing rule: a build side whose
        // estimated footprint does not fit the join pool refuses the hash
        // join to the plain join. A one-frame join pool cannot hold the
        // wide table's partition (its data file alone fills pages, doubled
        // for the deck's fudge factor), while the default pool takes it.
        // The gated session is fully its own animal — its own main pool
        // and a one-frame join pool, the config-off CLI shape — so
        // everything open is flushed first (close_all) and the fresh
        // session reads complete files from disk.
        check(run_sql(executor, "CREATE TABLE wide (k INTEGER, pad VARCHAR(200))", result),
              "the gate fixture's wide table created");
        std::string wide_insert = "INSERT INTO wide VALUES ";
        for (int w = 1; w <= 20; ++w) {
            wide_insert += (w > 1 ? ", " : "") + std::string("(") + std::to_string(w) +
                           ", '" + std::string(200, 'x') + "')";
        }
        check(run_sql(executor, wide_insert, result), "the gate fixture's wide rows inserted");
        executor.close_all();

        bufman::BufferManager gated_main;
        gated_main.init(8, std::make_unique<bufman::LRUPolicy>());
        bufman::BufferManager one_frame_join;
        one_frame_join.init(1, std::make_unique<bufman::LRUPolicy>());
        exec::Executor gated(gated_main, &one_frame_join);
        check(run_sql(gated, "SELECT did, k FROM wide JOIN dept ON k = did", result) &&
                  result.rows.access_path == "nested loops join",
              "an oversized build side fails the gate to the plain join");
        const auto gated_pairs = [&](exec::Executor& which) {
            std::vector<std::pair<int, int>> rows;
            if (run_sql(which, "SELECT did, k FROM wide JOIN dept ON k = did", result)) {
                for (const std::vector<exec::Value>& row : result.rows.rows) {
                    rows.emplace_back(std::get<int>(row[0]), std::get<int>(row[1]));
                }
            }
            std::sort(rows.begin(), rows.end());
            return rows;
        };
        check(gated_pairs(gated) == gated_pairs(executor) &&
                  result.rows.access_path == "hash join",
              "the default pool takes the same join through hash, same answers");
    }

    // --- 5.2. evaluation semantics through WHERE ---
    {
        check(run_sql(executor, "SELECT pid FROM person WHERE age / 2 = 21", result) &&
                  result.rows.rows.size() == 2,
              "integer division truncates (42 / 2 = 21 finds two rows)");
        check(run_sql(executor, "SELECT pid FROM person WHERE age = 42.0", result) &&
                  result.rows.rows.size() == 2,
              "int/double comparison promotes");
        check(run_sql(executor, "SELECT pid FROM person WHERE city > 'PQ' AND city < 'PS'", result) &&
                  result.rows.rows.size() == 2,
              "string comparison is lexicographic");
        check(run_sql(executor, "SELECT pid FROM person WHERE NOT city = 'PR'", result) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "NOT negates");
        check(run_sql(executor, "SELECT pid FROM person WHERE pid = 1 OR pid = 3", result) &&
                  result.rows.rows.size() == 2,
              "OR still answers correctly on the heap path");
        check(contains(sql_error(executor, "SELECT * FROM person WHERE pid / 0 = 1"), "division by zero"),
              "division by zero is a named runtime error");
    }

    // --- 5.1. bind-time refusals (before any page is pinned) ---
    {
        check(contains(sql_error(executor, "SELECT nosuch FROM person"), "unknown column"),
              "an unknown projected column is a bind error");
        check(contains(sql_error(executor, "SELECT * FROM person WHERE nosuch = 1"), "unknown column"),
              "an unknown WHERE column is a bind error");
        check(contains(sql_error(executor, "SELECT * FROM person WHERE name = 5"), "cannot compare"),
              "string-vs-number comparison is a bind error");
        check(contains(sql_error(executor, "SELECT * FROM person WHERE name + 1 = 5"), "arithmetic needs numeric"),
              "arithmetic on strings is a bind error");
        check(contains(sql_error(executor, "SELECT * FROM person WHERE pid AND 1"), "boolean"),
              "non-boolean AND operands are a bind error");
    }

    // --- 5.7. the wider language is refused, naming the feature ---
    // (p3dbeng's surface adds LIMIT, UPDATE, and DELETE — their positive
    // checks live in the sections below; the wider language that remains
    // outside the surface is refused here. Joins left this list in p4:
    // the nested-loops-joins change supports them, and their checks live
    // in the join suite.)
    {
        const std::pair<const char*, const char*> refusals[] = {
            {"SELECT city, COUNT(*) FROM person GROUP BY city", "GROUP BY"},
            {"SELECT * FROM person ORDER BY pid", "ORDER BY"},
            {"SELECT COUNT(*) FROM person", "aggregate"},
        };
        for (const auto& [text, feature] : refusals) {
            check(contains(sql_error(executor, text), feature),
                  std::string("refused and named: ") + feature);
        }
    }

    // --- projection shapes ---
    {
        check(run_sql(executor, "SELECT city, pid FROM person WHERE pid = 1", result) &&
                  result.rows.columns.size() == 2 &&
                  result.rows.columns[0] == "city" &&
                  result.rows.columns[1] == "pid" &&
                  std::get<std::string>(result.rows.rows[0][0]) == "PR",
              "projection narrows and orders the columns");
        check(run_sql(executor, "SELECT pid AS id FROM person WHERE pid = 1", result) &&
                  result.rows.columns[0] == "id",
              "an alias names the projected column");
    }

    // --- 5.5. DROP INDEX, then the heap path ---
    {
        check(run_sql(executor, "DROP INDEX person_pid", result),
              "DROP INDEX succeeds");
        check(run_sql(executor, "SELECT * FROM person WHERE pid = 2", result) &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 1,
              "after the drop, the same predicate scans the heap");
        check(contains(sql_error(executor, "DROP INDEX person_pid"), "unknown index"),
              "dropping a missing index is refused by name");
    }

    // --- 5.5. the missing index file is rebuilt on open ---
    {
        check(run_sql(executor, "CREATE INDEX person_pid ON person (pid)", result),
              "the index is created again");
        // A full session close, then a brand-new executor: the index
        // file was deleted externally, so the reopen must rebuild it.
        check(ok([&] { executor.close_all(); }), "session closed");
        std::remove("data/person.tbl.pid.idx");
        exec::Executor fresh(manager);
        check(run_sql(fresh, "SELECT * FROM person WHERE pid = 3", result) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 3,
              "a deleted index file is rebuilt and used");
        check(run_sql(fresh, "SELECT * FROM person", result) &&
                  result.rows.rows.size() == 3,
              "the rebuilt index agrees with the heap");
        check(ok([&] { fresh.close_all(); }), "fresh session closed");
    }

    // --- 5.4. DROP TABLE removes everything ---
    {
        check(run_sql(executor, "CREATE TABLE junk (a INTEGER)", result) &&
                  run_sql(executor, "INSERT INTO junk VALUES (7)", result) &&
                  run_sql(executor, "CREATE INDEX junk_a ON junk (a)", result),
              "junk table prepared for the drop");
        check(run_sql(executor, "DROP TABLE junk", result) &&
                  result.message == "table 'junk' dropped",
              "DROP TABLE reports success");
        std::ifstream catalog("tablecatalog/junk.json");
        std::ifstream data("data/junk.tbl");
        std::ifstream index("data/junk.tbl.a.idx");
        check(!catalog.good() && !data.good() && !index.good(),
              "catalog, data, and index files are all gone");
        check(contains(sql_error(executor, "DROP TABLE junk"), "unknown table"),
              "dropping a missing table is refused by name");
    }

    // --- primary key: the constraint owns its index (spec: sql-ddl) ---
    {
        check(run_sql(executor, "CREATE TABLE city (cid INTEGER PRIMARY KEY, "
                      "name VARCHAR(10))", result) &&
                  result.message == "table 'city' created",
              "CREATE TABLE with a primary key reports success");
        // The catalog records both the constraint and its owned index.
        check(executor.table("city").primary_key &&
                  *executor.table("city").primary_key == "cid",
              "the catalog records the primary key column");
        const std::vector<schema::TableIndex>& indexes = executor.table("city").indexes;
        check(indexes.size() == 1 && indexes[0].name == "city_cid" &&
                  indexes[0].column == "cid" && indexes[0].type == "ISAM",
              "the primary key's backing index is declared by the ordinary convention");
        {
            std::ifstream catalog("tablecatalog/city.json");
            std::stringstream text;
            text << catalog.rdbuf();
            check(contains(text.str(), "\"primary_key\": \"cid\""),
                  "the catalog file carries the primary_key field");
        }
        // The constraint's index occupies the column's index slot.
        check(contains(sql_error(executor, "CREATE INDEX city_key ON city (cid)"),
                       "one index per column"),
              "CREATE INDEX on the primary-key column is refused");
        check(contains(sql_error(executor, "DROP INDEX city_cid"), "backs the primary key"),
              "DROP INDEX on the constraint's index is refused");
        check(executor.table("city").indexes.size() == 1,
              "the refused drops left the catalog unchanged");
        // The table is immediately usable, and the owned index materializes
        // through the ordinary lazy open — first use builds it over the rows.
        check(run_sql(executor, "INSERT INTO city VALUES (1, 'PR'), (2, 'MA')", result) &&
                  result.message == "inserted 2 row(s)",
              "a primary-key table accepts inserts right away");
        check(run_sql(executor, "SELECT * FROM city WHERE cid = 2", result) &&
                  result.rows.access_path == "index scan",
              "the owned index answers lookups as any declared index would");
        const exec::Executor::IndexCheck report = executor.check_indexes("city", "data/city.tbl");
        check(report.clean, "checkindex verifies the primary key's index clean");
        // The old catalog shape (no primary_key field) loads and behaves.
        check(ok([&] { executor.close_all(); }), "session closed before the old-format check");
        {
            std::ofstream catalog("tablecatalog/old.json");
            catalog << "{\n  \"table_name\": \"old\",\n  \"columns\": [\n"
                       "    { \"column_name\": \"id\", \"column_type\": \"Integer\", \"size\": 4 }\n"
                       "  ],\n  \"indexes\": []\n}\n";
        }
        exec::Executor old_format(manager);
        check(run_sql(old_format, "INSERT INTO old VALUES (5)", result) &&
                  run_sql(old_format, "SELECT * FROM old", result) &&
                  result.rows.rows.size() == 1,
              "a pre-primary-key catalog loads and works unchanged");
        check(!old_format.table("old").primary_key.has_value(),
              "the old-format table records no primary key");
        check(ok([&] { old_format.close_all(); }), "old-format session closed");
    }

    // --- primary key enforcement on INSERT (spec: sql-dml) ---
    {
        // `city` carries rows 1 and 2 from the DDL block above.
        check(contains(sql_error(executor, "INSERT INTO city VALUES (1, 'DUPE')"),
                       "duplicate value 1 for the primary key"),
              "a duplicate against stored rows is refused, naming the key");
        check(executor.count_rows("city", "data/city.tbl") == 2,
              "the refused insert stored nothing");
        check(contains(sql_error(executor, "INSERT INTO city VALUES (3, 'OK'), (4, 'FINE'), (3, 'LATE')"),
                       "row 3: duplicate value 3"),
              "a duplicate within one statement is refused, naming the row");
        check(executor.count_rows("city", "data/city.tbl") == 2,
              "the within-statement refusal stored none of the three rows");
        check(run_sql(executor, "INSERT INTO city VALUES (3, 'PR'), (4, 'MA')", result) &&
                  result.message == "inserted 2 row(s)",
              "a unique multi-row insert stores everything");
        const exec::Executor::IndexCheck report = executor.check_indexes("city", "data/city.tbl");
        check(report.clean, "the index stays clean through refusals and inserts");
        // The bulk escape hatch (append) shares insert_row, so the probe
        // covers it too — the sideways door is shut.
        check(contains(checks::error_of([&] {
                  executor.insert_into("city", "data/city.tbl", [] {
                      tuple::Tuple tuple;
                      tuple.add(1);
                      tuple.add(std::string("SIDE"));
                      return tuple;
                  }());
              }),
              "duplicate value 1"),
              "the bulk insert path refuses a duplicate key");
        check(executor.count_rows("city", "data/city.tbl") == 4, "the bulk refusal stored nothing");
    }

    // --- primary key enforcement on UPDATE (spec: sql-dml) ---
    {
        // `city` carries rows 1..4 under cid.
        check(contains(sql_error(executor, "UPDATE city SET cid = 2 WHERE cid = 1"),
                       "duplicate value 2 for the primary key"),
              "a key move onto an occupied value is refused, naming the key");
        check(run_sql(executor, "SELECT cid FROM city WHERE name = 'PR'", result) &&
                  result.rows.rows.size() == 2 &&
                  std::get<int>(result.rows.rows[0][0]) == 1 &&
                  std::get<int>(result.rows.rows[1][0]) == 3,
              "the refused move left both rows at their old keys");
        // Two collected rows moving onto the same free value: the seen-set
        // catches the pair before anything applies.
        check(contains(sql_error(executor, "UPDATE city SET cid = 9 WHERE cid = 1 OR cid = 3"),
                       "duplicate value 9"),
              "a multi-row collision is refused whole");
        check(run_sql(executor, "SELECT cid FROM city WHERE cid = 9", result) &&
                  result.rows.rows.empty(),
              "the refused collision applied nothing");
        // Assigning the row's own current value passes.
        check(run_sql(executor, "UPDATE city SET cid = 1 WHERE cid = 1", result) &&
                  result.message == "updated 1 row(s)",
              "assigning the row's own key passes");
        // An accepted move tracks the index.
        check(run_sql(executor, "UPDATE city SET cid = 10 WHERE cid = 1", result),
              "a key move to a free value succeeds");
        check(run_sql(executor, "SELECT name FROM city WHERE cid = 10", result) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<std::string>(result.rows.rows[0][0]) == "PR",
              "the moved row answers under its new key through the index");
        check(run_sql(executor, "SELECT cid FROM city WHERE cid = 1", result) &&
                  result.rows.rows.empty(),
              "the old key no longer resolves");
        const exec::Executor::IndexCheck report = executor.check_indexes("city", "data/city.tbl");
        check(report.clean, "the index stays clean through accepted key moves");
        check(run_sql(executor, "UPDATE city SET cid = 1 WHERE cid = 10", result),
              "the move is undone for the later suites");
    }

    // --- the lazy rebuild verifies a primary key's data (spec: sql-ddl) ---
    {
        // The sideways door: a data file that arrived with duplicates (the
        // bulk loader writes data files only), a catalog that declares the
        // primary key and its index, and no index file yet — the first
        // query takes the lazy-rebuild path and must refuse.
        check(run_sql(executor, "CREATE TABLE zip (zid INTEGER, label VARCHAR(6))", result) &&
                  run_sql(executor, "INSERT INTO zip VALUES (1, 'A'), (1, 'B')", result),
              "the duplicate data file is built without a primary key");
        check(ok([&] { executor.close_all(); }), "session closed for the sideways door");
        {
            std::ofstream catalog("tablecatalog/zip.json");
            catalog << "{\n  \"table_name\": \"zip\",\n  \"columns\": [\n"
                       "    { \"column_name\": \"zid\", \"column_type\": \"Integer\", \"size\": 4 },\n"
                       "    { \"column_name\": \"label\", \"column_type\": \"String\", \"size\": 6 }\n"
                       "  ],\n  \"primary_key\": \"zid\",\n"
                       "  \"indexes\": [\n"
                       "    { \"name\": \"zip_zid\", \"column\": \"zid\", \"type\": \"ISAM\" }\n"
                       "  ]\n}\n";
        }
        {
            exec::Executor sideways(manager);
            check(contains(sql_error(sideways, "SELECT * FROM zip"),
                           "duplicate value 1 for the primary key of table 'zip'"),
                  "a duplicate-carrying data file refuses the rebuild by name");
            std::ifstream index_file("data/zip.tbl.zid.idx");
            check(!index_file.good(), "the refused rebuild left no index file behind");
            check(ok([&] { sideways.close_all(); }), "the sideways session closed");
        }
        // The clean path: unique data rebuilds and answers.
        check(run_sql(executor, "CREATE TABLE ok (oid INTEGER PRIMARY KEY)", result) &&
                  run_sql(executor, "INSERT INTO ok VALUES (1), (2)", result),
              "the clean primary-key table is built");
        check(ok([&] { executor.close_all(); }), "session closed for the clean rebuild");
        std::remove("data/ok.tbl.oid.idx");
        {
            exec::Executor clean(manager);
            check(run_sql(clean, "SELECT * FROM ok WHERE oid = 2", result) &&
                      result.rows.access_path == "index scan" &&
                      result.rows.rows.size() == 1,
                  "a clean data file rebuilds the primary key's index and answers");
            check(clean.check_indexes("ok", "data/ok.tbl").clean,
                  "the rebuilt index verifies clean");
            check(ok([&] { clean.close_all(); }), "the clean session closed");
        }
    }

    // --- foreign keys: creation-time validation (spec: sql-ddl) ---
    {
        // `city` carries cid as its primary key (rows 1..4 from above).
        check(run_sql(executor, "CREATE TABLE resident (rid INTEGER PRIMARY KEY, "
                      "city_id INTEGER REFERENCES city(cid))", result) &&
                  result.message == "table 'resident' created",
              "CREATE TABLE with a reference reports success");
        check(executor.table("resident").foreign_keys.size() == 1 &&
                  executor.table("resident").foreign_keys[0].column == "city_id" &&
                  executor.table("resident").foreign_keys[0].parent_table == "city" &&
                  executor.table("resident").foreign_keys[0].parent_column == "cid",
              "the catalog records the foreign key");
        {
            std::ifstream catalog("tablecatalog/resident.json");
            std::stringstream text;
            text << catalog.rdbuf();
            check(contains(text.str(), "\"foreign_keys\"") &&
                      contains(text.str(), "\"parent_table\": \"city\""),
                  "the catalog file carries the foreign_keys array");
        }
        check(contains(sql_error(executor, "CREATE TABLE orphan (x INTEGER REFERENCES nosuch(n))"),
                       "unknown table 'nosuch'"),
              "a reference to a missing parent is refused by name");
        check(contains(sql_error(executor, "CREATE TABLE badref (x INTEGER REFERENCES city(name))"),
                       "is not its primary key"),
              "a reference to a non-primary-key column is refused");
        check(contains(sql_error(executor, "CREATE TABLE badtype (tag VARCHAR(5) REFERENCES city(cid))"),
                       "different type"),
              "a reference with a mismatched type is refused");
        check(!std::ifstream("tablecatalog/orphan.json").good(),
              "the refused creations wrote no catalogs");
        // A self-reference validates against the statement's own primary key.
        check(run_sql(executor, "CREATE TABLE employee (eid INTEGER PRIMARY KEY, "
                      "manager_id INTEGER REFERENCES employee(eid))", result),
              "a self-reference is accepted");
        check(executor.table("employee").foreign_keys.size() == 1 &&
                  executor.table("employee").foreign_keys[0].parent_table == "employee",
              "the catalog records the self-referencing foreign key");
        // The old catalog shape (no foreign_keys array) loads with none.
        {
            exec::Executor old_format(manager);
            check(old_format.table("old").foreign_keys.empty(),
                  "the old-format table records no foreign keys");
            check(ok([&] { old_format.close_all(); }), "the old-format session closed");
        }
    }

    // --- foreign key enforcement on INSERT (spec: sql-dml) ---
    {
        // `city` holds cid 1..4; `resident` references city(cid).
        check(run_sql(executor, "INSERT INTO resident VALUES (1, 1), (2, 2)", result) &&
                  result.message == "inserted 2 row(s)",
              "references to existing parents are accepted");
        check(contains(sql_error(executor, "INSERT INTO resident VALUES (3, 7)"),
                       "value 7 not found in the primary key of table 'city'"),
              "a missing parent is refused, naming the value and the parent");
        check(executor.count_rows("resident", "data/resident.tbl") == 2,
              "the refused insert stored nothing");
        check(contains(sql_error(executor, "INSERT INTO resident VALUES (4, 3), (5, 9)"),
                       "row 2: value 9 not found"),
              "one bad reference in a multi-row statement refuses it whole");
        check(executor.count_rows("resident", "data/resident.tbl") == 2,
              "the whole-statement refusal stored none of the two rows");
        // The parent-provided set: a manager and their report in one
        // statement, the manager provided by the statement itself.
        check(run_sql(executor, "INSERT INTO employee VALUES (1, 1), (2, 1)", result) &&
                  result.message == "inserted 2 row(s)",
              "a parent provided by the same statement passes");
        check(contains(sql_error(executor, "INSERT INTO employee VALUES (3, 9)"),
                       "value 9 not found in the primary key of table 'employee'"),
              "a self-reference to a key nobody provides is refused");
        // The bulk escape hatch shares insert_row, so the probe covers it.
        check(contains(checks::error_of([&] {
                  executor.insert_into("resident", "data/resident.tbl", [] {
                      tuple::Tuple tuple;
                      tuple.add(9);
                      tuple.add(99);
                      return tuple;
                  }());
              }),
              "value 99 not found in the primary key of table 'city'"),
              "the bulk insert path refuses a missing parent");
        check(executor.count_rows("resident", "data/resident.tbl") == 2,
              "the bulk refusal stored nothing");
    }

    // --- foreign key enforcement on UPDATE (spec: sql-dml) ---
    {
        // resident 1 points at city 1.
        check(contains(sql_error(executor, "UPDATE resident SET city_id = 42 WHERE rid = 1"),
                       "value 42 not found in the primary key of table 'city'"),
              "a reference moved onto a missing parent is refused");
        check(run_sql(executor, "SELECT city_id FROM resident WHERE rid = 1", result) &&
                  result.rows.rows.size() == 1 && std::get<int>(result.rows.rows[0][0]) == 1,
              "the refused move left the reference intact");
        check(run_sql(executor, "UPDATE resident SET city_id = city_id WHERE rid = 1", result) &&
                  result.message == "updated 1 row(s)",
              "assigning the row's own reference passes");
        // The self-referencing key move that keeps up with itself: the
        // report row goes first, so nothing else references the old key.
        check(run_sql(executor, "DELETE FROM employee WHERE eid = 2", result),
              "the report row is removed");
        check(run_sql(executor, "UPDATE employee SET eid = 9, manager_id = 9 WHERE eid = 1", result) &&
                  result.message == "updated 1 row(s)",
              "a self-referencing key move that keeps up with itself passes");
        check(run_sql(executor, "SELECT manager_id FROM employee WHERE eid = 9", result) &&
                  result.rows.rows.size() == 1 && std::get<int>(result.rows.rows[0][0]) == 9,
              "the moved row references its new key");
        check(run_sql(executor, "UPDATE employee SET eid = 1, manager_id = 1 WHERE eid = 9", result),
              "the move is undone for the later suites");
    }

    // --- the parent side is protected (spec: sql-ddl, sql-dml) ---
    {
        // resident 1 and 2 reference city 1 and 2.
        check(contains(sql_error(executor, "DELETE FROM city WHERE cid = 1"),
                       "table 'resident' (column 'city_id') still references key 1"),
              "deleting a referenced parent is refused, naming the referencing table");
        check(run_sql(executor, "SELECT * FROM city", result) && result.rows.rows.size() == 4,
              "the refused delete left the parent intact");
        check(contains(sql_error(executor, "UPDATE city SET cid = 9 WHERE cid = 1"),
                       "still references key 1"),
              "moving a referenced parent key is refused");
        check(run_sql(executor, "SELECT cid FROM city WHERE cid = 1", result) &&
                  result.rows.rows.size() == 1,
              "the refused move left the key in place");
        check(contains(sql_error(executor, "DROP TABLE city"), "referenced by 'resident'"),
              "dropping a referenced parent is refused, naming the referencing table");
        check(run_sql(executor, "SELECT * FROM city", result) && result.rows.rows.size() == 4,
              "the refused drop left the parent fully intact");
        // The self-referencing exclusions: a row may delete itself, and a
        // self-referencing table drops with its constraint.
        check(run_sql(executor, "DELETE FROM employee WHERE eid = 1", result) &&
                  result.message == "deleted 1 row(s)",
              "a self-referencing row deletes itself");
        check(run_sql(executor, "DROP TABLE employee", result) &&
                  result.message == "table 'employee' dropped",
              "a self-referencing table drops with its constraint");
        // Once the child is gone, the parent is free again.
        check(run_sql(executor, "DROP TABLE resident", result), "the child table drops freely");
        check(run_sql(executor, "DELETE FROM city WHERE cid = 1", result) &&
                  result.message == "deleted 1 row(s)",
              "with no referencing child, the parent side is unconstrained again");
    }

    // --- EXPLAIN: the rendered plan is the run plan (spec: sql-explain) ---
    {
        // Zero rows, no marker: the plan text is the statement's output.
        check(run_sql(executor, "EXPLAIN SELECT name FROM person WHERE age > 30 AND city = 'PR'",
                      result) &&
                  result.rows.rows.empty() && result.rows.access_path.empty(),
              "explain: no rows and no access-path marker");
        check(result.message ==
                  "Project (name)\n"
                  "  Filter (person.age > 30 AND person.city = 'PR')\n"
                  "    Scan person (heap)",
              "explain: the heap plan names the scan and its residual");

        check(run_sql(executor, "EXPLAIN SELECT name FROM person WHERE pid = 2", result) &&
                  result.message ==
                      "Project (name)\n"
                      "  Scan person (index person_pid, person.pid = 2)",
              "explain: the index plan names the index and its bound");
        check(run_sql(executor,
                      "EXPLAIN SELECT name FROM person WHERE pid > 1 AND pid < 3 AND city = 'PR'",
                      result) &&
                  result.message ==
                      "Project (name)\n"
                      "  Filter (person.city = 'PR')\n"
                      "    Scan person (index person_pid, person.pid > 1 AND person.pid < 3)",
              "explain: the residual rides above an index scan with a range");

        // The conflicting bounds render their note: the execution returns
        // nothing, and the plan says so.
        run_sql(executor, "EXPLAIN SELECT * FROM person WHERE pid > 3 AND pid < 2", result);
        check(contains(result.message,
                       "Scan person (index person_pid, bounds conflict: answers nothing)"),
              "explain: conflicting bounds render their note");

        check(run_sql(executor, "EXPLAIN SELECT pid FROM person WHERE city = 'MA' LIMIT 5",
                      result) &&
                  result.message ==
                      "Limit (5)\n"
                      "  Project (pid)\n"
                      "    Filter (person.city = 'MA')\n"
                      "      Scan person (heap)",
              "explain: the limit and projection sit above the scan");

        // Join plans: the probed entry has no scan line — the probe on
        // the join line replaces it.
        check(run_sql(executor, "EXPLAIN SELECT did, eid FROM dept JOIN emp ON did = dept_id",
                      result) &&
                  result.message ==
                      "Project (did, eid)\n"
                      "  Join: index nested loops (probe emp_dept: emp.dept_id = dept.did)\n"
                      "    Scan dept (heap)",
              "explain: the index join names its probe index and equated columns");
        run_sql(executor, "EXPLAIN SELECT eid, did FROM emp JOIN dept ON dept_id = did", result);
        check(result.message ==
                  "Project (eid, did)\n"
                  "  Join: hash join (dept.did = emp.dept_id)\n"
                  "    Scan dept (heap)\n"
                  "    Scan emp (heap)",
              "explain: the hash join names both scans");
        check(run_sql(executor, "EXPLAIN SELECT did, eid FROM dept, emp WHERE dept_id > did",
                      result) &&
                  result.message ==
                      "Project (did, eid)\n"
                      "  Filter (emp.dept_id > dept.did)\n"
                      "    Join: nested loops (cross)\n"
                      "      Scan emp (heap)\n"
                      "      Scan dept (heap)",
              "explain: a non-equi comma join stays a cross nested loops with its residual");
        check(run_sql(executor,
                      "EXPLAIN SELECT d.did, e.eid FROM dept d JOIN emp e ON d.did = e.dept_id",
                      result) &&
                  result.message ==
                      "Project (did, eid)\n"
                      "  Join: index nested loops (probe emp_dept: e.dept_id = d.did)\n"
                      "    Scan dept AS d (heap)",
              "explain: aliases appear on the scan and in the join's columns");

        // The three-way plan shows every node's algorithm at once — the
        // runtime marker only ever names the root's.
        const std::string three_way =
            "EXPLAIN SELECT d.dname FROM dept d JOIN emp e ON d.did = e.dept_id "
            "JOIN person p ON e.eid = p.pid";
        run_sql(executor, three_way, result);
        check(result.message ==
                  "Project (dname)\n"
                  "  Join: index nested loops (probe person_pid: p.pid = e.eid)\n"
                  "    Join: index nested loops (probe emp_dept: e.dept_id = d.did)\n"
                  "      Scan dept AS d (heap)",
              "explain: the three-way plan shows every node's algorithm");
        executor.set_allow_index(false);
        run_sql(executor, three_way, result);
        check(result.message ==
                  "Project (dname)\n"
                  "  Join: hash join (p.pid = e.eid)\n"
                  "    Join: hash join (e.dept_id = d.did)\n"
                  "    Scan person AS p (heap)\n"
                  "      Scan emp AS e (heap)\n"
                  "      Scan dept AS d (heap)",
              "explain: suppressed indexes render the hash plan the run takes");
        executor.set_allow_index(true);

        // Bind refusals fire as an execution's would, before any plan.
        check(contains(sql_error(executor, "EXPLAIN SELECT nosuchcol FROM person"),
                       "unknown column"),
              "explain: a bind refusal fires before any plan");

        // The differential pin: the rendered root access path is the one
        // the run announces — across the access-path settings.
        const auto plan_matches_run = [](const std::string& plan, const std::string& marker) {
            if (marker == "index scan") return contains(plan, "(index ");
            if (marker == "heap scan") return contains(plan, "(heap)");
            if (marker == "hash join") return contains(plan, "Join: hash join");
            if (marker == "index nested loops join") {
                return contains(plan, "Join: index nested loops");
            }
            if (marker == "nested loops join") {
                return contains(plan, "Join: nested loops (");
            }
            return false;
        };
        const std::vector<std::string> pinned = {
            "SELECT name FROM person WHERE pid = 2",
            "SELECT name FROM person WHERE city = 'PR'",
            "SELECT did, eid FROM dept JOIN emp ON did = dept_id",
            "SELECT eid, did FROM emp JOIN dept ON dept_id = did",
            "SELECT did, eid FROM dept, emp WHERE dept_id > did",
        };
        bool differential = true;
        for (const int allow_index : {1, 0}) {
            executor.set_allow_index(allow_index == 1);
            for (const std::string& select : pinned) {
                std::string plan;
                std::string marker;
                if (run_sql(executor, "EXPLAIN " + select, result)) {
                    plan = result.message;
                } else {
                    differential = false;
                }
                if (run_sql(executor, select, result)) {
                    marker = result.rows.access_path;
                } else {
                    differential = false;
                }
                differential = differential && plan_matches_run(plan, marker);
            }
        }
        executor.set_allow_index(true);
        check(differential,
              "explain: the rendered root access path matches the run's announcement");

        // A deleted index file rebuilds during EXPLAIN — the same honest
        // file effects an execution has — and the plan shows the index.
        check(ok([&] { executor.close_all(); }), "session closed for the explain rebuild");
        std::remove("data/person.tbl.pid.idx");
        exec::Executor fresh(manager);
        check(run_sql(fresh, "EXPLAIN SELECT name FROM person WHERE pid = 1", result) &&
                  contains(result.message, "Scan person (index person_pid"),
              "explain: a deleted index file rebuilds and the plan shows the index");
        std::ifstream index_file("data/person.tbl.pid.idx");
        check(index_file.good(), "explain: the rebuild left the index file behind");
        check(ok([&] { fresh.close_all(); }), "the rebuild session closed");
    }

    // --- LIMIT: caps the projection, keeps the announced path ---
    {
        check(run_sql(executor, "SELECT pid FROM person LIMIT 2", result) &&
                  result.rows.rows.size() == 2 &&
                  result.rows.access_path == "heap scan",
              "LIMIT caps the result at two rows");
        check(run_sql(executor, "SELECT pid FROM person LIMIT 0", result) &&
                  result.rows.rows.empty(),
              "LIMIT 0 yields zero rows");
        check(run_sql(executor, "SELECT pid FROM person LIMIT 100", result) &&
                  result.rows.rows.size() == 3,
              "a LIMIT past the answer returns every row");
        check(run_sql(executor, "SELECT pid FROM person WHERE pid > 1 LIMIT 1", result) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "LIMIT stops a filtered scan early");
        check(contains(sql_error(executor, "SELECT nosuch FROM person LIMIT 2"), "unknown column"),
              "a bind error still refuses before any row is read");
    }

    // --- arithmetic overflow: p2dbeng's named run-time refusal ---
    {
        check(sql_error(executor, "SELECT * FROM person WHERE 2000000000 + 2000000000 = 0") == "integer overflow",
              "an in-range sum that overflows is a named run-time error");
        check(sql_error(executor, "SELECT * FROM person WHERE -(-2147483647 - 1) = 0") == "integer overflow",
              "negating INT_MIN is a named run-time error");
        check(sql_error(executor, "SELECT * FROM person WHERE pid / (pid - pid) = 0") == "division by zero",
              "division by zero is still a named run-time error");
        check(run_sql(executor, "SELECT * FROM person WHERE 2000000000 - 2000000000 = 0", result) &&
                  result.rows.rows.size() == 3,
              "an in-range operation still evaluates");
    }

    // --- UPDATE: collect-then-apply with index maintenance ---
    {
        // Self-referential assignment over the fixture table.
        check(run_sql(executor, "UPDATE person SET age = age + 1 WHERE pid = 2", result) &&
                  result.message == "updated 1 row(s)",
              "UPDATE reports one row updated");
        check(run_sql(executor, "SELECT age FROM person WHERE pid = 2", result) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 31,
              "UPDATE applied age + 1 against the old value");

        // The swap: simultaneous SET semantics.
        check(run_sql(executor, "CREATE TABLE upd_swap (a INTEGER, b INTEGER)", result) &&
                  run_sql(executor, "INSERT INTO upd_swap VALUES (1, 2)", result),
              "swap table prepared");
        check(run_sql(executor, "UPDATE upd_swap SET a = b, b = a", result) &&
                  result.message == "updated 1 row(s)",
              "the swap UPDATE applied");
        check(run_sql(executor, "SELECT a, b FROM upd_swap", result) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2 &&
                  std::get<int>(result.rows.rows[0][1]) == 1,
              "SET a = b, b = a exchanged the values");

        // A key-column move moves the index entry; a non-key update
        // leaves the index untouched.
        check(run_sql(executor, "CREATE TABLE upd_idx (id INTEGER, val INTEGER)", result) &&
                  run_sql(executor, "INSERT INTO upd_idx VALUES (1, 10), (2, 20)", result) &&
                  run_sql(executor, "CREATE INDEX upd_idx_id ON upd_idx (id)", result),
              "indexed table prepared for UPDATE");
        check(ok([&] { return executor.check_indexes("upd_idx", "data/upd_idx.tbl").clean; }),
              "the index verifies clean before the update");
        check(run_sql(executor, "UPDATE upd_idx SET id = 99 WHERE id = 1", result) &&
                  result.message == "updated 1 row(s)",
              "a key-column UPDATE applied");
        check(run_sql(executor, "SELECT val FROM upd_idx WHERE id = 99", result) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 10,
              "the index answers at the new key");
        check(run_sql(executor, "SELECT val FROM upd_idx WHERE id = 1", result) &&
                  result.rows.rows.empty(),
              "the old key answers nothing");
        check(ok([&] {
                  const exec::Executor::IndexCheck verified =
                      executor.check_indexes("upd_idx", "data/upd_idx.tbl");
                  return verified.clean && verified.report.size() == 1 &&
                         contains(verified.report[0], "2");
              }),
              "the moved index verifies clean with both entries");
        check(run_sql(executor, "UPDATE upd_idx SET val = 21 WHERE id = 2", result) &&
                  ok([&] {
                      const exec::Executor::IndexCheck verified =
                          executor.check_indexes("upd_idx", "data/upd_idx.tbl");
                      return verified.clean && contains(verified.report[0], "2");
                  }),
              "a non-key UPDATE leaves the index count alone");

        // No matches: zero rows, nothing changed.
        check(run_sql(executor, "UPDATE upd_idx SET val = 0 WHERE id = 500", result) &&
                  result.message == "updated 0 row(s)",
              "an UPDATE matching nothing changes nothing");

        // Bind-time refusal: an unknown SET column is named, no I/O.
        check(contains(sql_error(executor, "UPDATE upd_idx SET nosuch = 1"), "unknown column 'nosuch' in SET"),
              "an unknown SET column is refused by name");

        // A SET value that breaks the catalog refuses the statement and
        // applies nothing.
        check(run_sql(executor, "CREATE TABLE upd_wide (s VARCHAR(4))", result),
              "the width table is created");
        check(run_sql(executor, "INSERT INTO upd_wide VALUES ('ok')", result),
              "the width row is inserted");
        check(!sql_error(executor, "UPDATE upd_wide SET s = 'much too wide'").empty(),
              "a too-wide SET value refuses the statement");
        check(run_sql(executor, "SELECT s FROM upd_wide", result) &&
                  result.rows.rows.size() == 1 &&
                  std::get<std::string>(result.rows.rows[0][0]) == "ok",
              "the refused UPDATE stored nothing");
    }

    // --- DELETE: collect-then-erase with index maintenance ---
    {
        // Conditional delete on the indexed table.
        check(run_sql(executor, "DELETE FROM upd_idx WHERE id = 2", result) &&
                  result.message == "deleted 1 row(s)",
              "DELETE reports one row deleted");
        check(run_sql(executor, "SELECT * FROM upd_idx WHERE id = 2", result) &&
                  result.rows.rows.empty(),
              "the deleted row answers nothing");
        check(ok([&] { return executor.check_indexes("upd_idx", "data/upd_idx.tbl").clean; }),
              "deleted rows leave the index verify-clean");

        // Freed space is reusable by a later INSERT.
        check(run_sql(executor, "INSERT INTO upd_idx VALUES (3, 30)", result) &&
                  run_sql(executor, "SELECT val FROM upd_idx WHERE id = 3", result) &&
                  result.rows.rows.size() == 1,
              "an insert after deletes reuses freed space");

        // A collection failure applies nothing: damage a record's string
        // padding through the raw block layer, then refuse. The UPDATE
        // and DELETE collection scans must both apply nothing.
        check(run_sql(executor, "CREATE TABLE mangle (id INTEGER, pad VARCHAR(8))", result) &&
                  run_sql(executor, "INSERT INTO mangle VALUES (1, 'a')", result),
              "the corruption table prepared");
        check(ok([&] { executor.close_all(); }), "session closed for the damage step");
        check(ok([] {
                  bufman::File raw = bufman::open_for_append("data/mangle.tbl");
                  std::array<char, bufman::kBlockSize> block{};
                  bufman::read_block(raw, 1, block);
                  block[7] = 'X';  // pad is 'a' + NUL at [4,6); byte 7 is padding
                  bufman::write_block(raw, 1, block.data());
                  bufman::close(raw);
              }),
              "the damaged page written back");
        exec::Executor fresh_dmg(manager);
        check(!sql_error(fresh_dmg, "DELETE FROM mangle").empty(),
              "DELETE fails fast on the unreadable record");
        check(!sql_error(fresh_dmg, "UPDATE mangle SET id = 9").empty(),
              "UPDATE applies nothing when its collection fails");
        check(run_sql(fresh_dmg, "DROP TABLE mangle", result) &&
                  result.message == "table 'mangle' dropped",
              "the corruption table dropped cleanly");
        check(ok([&] { fresh_dmg.close_all(); }), "the damage session closed");
    }

    // --- 7.1. the differential oracle ---
    // Random rows, random predicates; every SELECT runs twice — index
    // allowed and index suppressed — and both paths must return
    // identical rows. The index never changes an answer.
    {
        check(run_sql(executor, "CREATE TABLE oracle (id INTEGER, tag VARCHAR(8), "
                      "score INTEGER, label VARCHAR(6))", result),
              "oracle table created");
        // 500 rows through the generator: id counts 1..500, tag/label
        // are random-width strings, score is random in [0, 1000).
        check(ok([&] { insert_generated(executor, "oracle", 500); }), "500 generated rows inserted");
        check(run_sql(executor, "CREATE INDEX oracle_id ON oracle (id)", result) &&
                  run_sql(executor, "CREATE INDEX oracle_score ON oracle "
                                    "(score)", result),
              "both oracle indexes created");

        // p3dbeng's extension: randomized INSERT/UPDATE/DELETE mutations
        // against a shadow model of expected entries. Every mutation runs
        // through SQL, and after each one both indexes must verify clean
        // and the row count must equal the shadow's.
        {
            std::map<int, tuple::Tuple> shadow;
            for (const tuple::Tuple& tuple :
                 schema::generate_tuples(executor.table("oracle").schema, 500, 1234, 1)) {
                shadow[std::get<int>(tuple.column(0).column_value())] = tuple;
            }
            std::mt19937 mutate_rng(20260214);
            auto mpick = [&](int lo, int hi) {
                return static_cast<int>(
                           mutate_rng() %
                           static_cast<unsigned>(hi - lo + 1)) + lo;
            };
            auto random_word = [&](int width) {
                static const char* letters = "abcdefghij";
                std::string word;
                for (int i = 0; i < width; ++i) {
                    word += letters[mpick(0, 9)];
                }
                return word;
            };
            int inserted_id = 500;
            bool mutations_ok = true;
            std::string mutation_failure;
            std::string last_sql;
            for (int step = 0; step < 250 && mutations_ok; ++step) {
                const int shape = mpick(0, 5);
                if (shape <= 1 || shadow.size() < 50) {
                    ++inserted_id;
                    const std::string tag = random_word(mpick(1, 8));
                    const std::string label = random_word(mpick(1, 6));
                    const std::string sql_text =
                        "INSERT INTO oracle VALUES (" +
                        std::to_string(inserted_id) + ", '" + tag + "', " +
                        std::to_string(mpick(0, 999)) + ", '" + label + "')";
                    last_sql = sql_text;
                    if (const std::string error = sql_error(executor, sql_text); !error.empty()) {
                        mutations_ok = false;
                        mutation_failure = "insert failed: " + error;
                        break;
                    }
                    tuple::Tuple fresh;
                    fresh.add(inserted_id);
                    fresh.add(tag);
                    fresh.add(mpick(0, 999));
                    fresh.add(label);
                    shadow[inserted_id] = std::move(fresh);
                } else if (shape <= 3) {
                    auto it = shadow.begin();
                    std::advance(it, mpick(0, static_cast<int>(shadow.size()) - 1));
                    const int id = it->first;
                    const int new_score = mpick(0, 999);
                    const std::string sql_text =
                        "UPDATE oracle SET score = " +
                        std::to_string(new_score) + " WHERE id = " +
                        std::to_string(id);
                    last_sql = sql_text;
                    if (const std::string error = sql_error(executor, sql_text); !error.empty()) {
                        mutations_ok = false;
                        mutation_failure = "update failed: " + error;
                        break;
                    }
                    tuple::Tuple updated;
                    const tuple::Tuple& old = it->second;
                    for (std::size_t i = 0; i < old.column_count(); ++i) {
                        updated.add_column(i == 2
                                               ? tuple::Column(new_score)
                                               : old.column(i));
                    }
                    it->second = std::move(updated);
                } else {
                    auto it = shadow.begin();
                    std::advance(it, mpick(0, static_cast<int>(shadow.size()) - 1));
                    const int id = it->first;
                    const std::string sql_text =
                        "DELETE FROM oracle WHERE id = " + std::to_string(id);
                    last_sql = sql_text;
                    if (const std::string error = sql_error(executor, sql_text); !error.empty()) {
                        mutations_ok = false;
                        mutation_failure = "delete failed: " + error;
                        break;
                    }
                    shadow.erase(it);
                }
                // The invariants after every mutation: both indexes clean
                // and the row count equal to the shadow's.
                if (!ok([&] {
                        const exec::Executor::IndexCheck verified =
                            executor.check_indexes("oracle", "data/oracle.tbl");
                        return verified.clean && verified.report.size() == 2;
                    })) {
                    mutations_ok = false;
                    mutation_failure = "index not clean after: " + last_sql;
                    break;
                }
                if (!ok([&] { return executor.count_rows("oracle", "data/oracle.tbl") == shadow.size(); })) {
                    mutations_ok = false;
                    mutation_failure = "row count disagrees with the shadow";
                    break;
                }
            }
            check(mutations_ok,
                  "250 random mutations keep both indexes clean and the count true" +
                      (mutations_ok ? std::string()
                                    : " — " + mutation_failure));
        }

        // Random predicates over both indexed columns, with and without
        // residual conjuncts, with and without OR.
        std::mt19937 rng(20260213);
        auto pick = [&](int lo, int hi) {
            return static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)) + lo;
        };
        bool all_match = true;
        std::string first_mismatch;
        for (int case_index = 0; case_index < 300; ++case_index) {
            std::string where;
            const int shape = pick(0, 6);
            switch (shape) {
                case 0: where = "id = " + std::to_string(pick(1, 500)); break;
                case 1: where = "id < " + std::to_string(pick(1, 500)); break;
                case 2: where = "id >= " + std::to_string(pick(1, 500)); break;
                case 3: {
                    const int a = pick(1, 499);
                    where = "id > " + std::to_string(a) + " AND id < " +
                            std::to_string(a + pick(1, 100));
                    break;
                }
                case 4:
                    where = "score = " + std::to_string(pick(0, 999));
                    break;
                case 5:
                    where = "score > " + std::to_string(pick(0, 900)) +
                            " AND score < " + std::to_string(pick(901, 1000));
                    break;
                case 6:
                    where = "id = " + std::to_string(pick(1, 500)) +
                            " OR score < " + std::to_string(pick(0, 200));
                    break;
            }
            const std::string select =
                "SELECT * FROM oracle WHERE " + where;
            exec::StatementResult with_index;
            executor.set_allow_index(true);
            if (!run_sql(executor, select, with_index)) {
                all_match = false;
                first_mismatch = "index path failed: " + select;
                break;
            }
            exec::StatementResult without;
            executor.set_allow_index(false);
            if (!run_sql(executor, select, without)) {
                all_match = false;
                first_mismatch = "heap path failed: " + select;
                break;
            }
            executor.set_allow_index(true);
            // Row order is unspecified without ORDER BY (the index walks
            // keys, the heap scans row ids), so compare as sorted sets.
            std::vector<std::vector<exec::Value>> a = with_index.rows.rows;
            std::vector<std::vector<exec::Value>> b = without.rows.rows;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a != b ||
                with_index.rows.columns != without.rows.columns) {
                all_match = false;
                first_mismatch = select + " (index " +
                                 std::to_string(with_index.rows.rows.size()) +
                                 " rows, heap " +
                                 std::to_string(without.rows.rows.size()) +
                                 " rows)";
                break;
            }
        }
        check(all_match, "300 random predicates: index path == heap path" +
                             (all_match ? std::string() : " — " + first_mismatch));
        executor.set_allow_index(true);
        check(ok([&] { executor.close_all(); }), "oracle session closed");
    }

    // --- the escape-hatch bulk path on a fresh table ---
    // A table with no data file counts as zero rows, so generated
    // numbering starts at 1 — the ex11 semantics.
    {
        check(run_sql(executor, "CREATE TABLE bulkr (id INTEGER, tag VARCHAR(6))", result),
              "bulkr table created with no data file");
        std::size_t existing = 999;  // must be overwritten with 0
        check(ok([&] {
                  existing = executor.count_rows("bulkr", "data/bulkr.tbl", true);
                  return existing == 0;
              }),
              "count_rows on a missing data file yields zero rows");
        const std::vector<tuple::Tuple> generated = schema::generate_tuples(
            executor.table("bulkr").schema, 3, 42, static_cast<int>(existing) + 1);
        check(ok([&] {
                  for (const tuple::Tuple& tuple : generated) {
                      executor.insert_into("bulkr", "data/bulkr.tbl", tuple);
                  }
                  return std::get<int>(generated.front().column(0).column_value()) == 1;
              }),
              "bulk numbering starts at 1 on a fresh table");
    }

    // --- index types: the catalog field, the USING clause, the dispatch ---
    {
        // The catalog records each type; the loader defaults to ISAM.
        check(run_sql(executor, "CREATE TABLE typed (a INTEGER, s VARCHAR(6), v INTEGER)", result) &&
                  run_sql(executor, "INSERT INTO typed VALUES (1, 'x', 10), (2, 'y', 20), "
                          "(3, 'z', 30)", result) &&
                  run_sql(executor, "CREATE INDEX typed_a ON typed (a)", result) &&
                  run_sql(executor, "CREATE INDEX typed_s ON typed (s) USING BTREE", result),
              "both index types created");
        const schema::Catalog& typed = executor.table("typed");
        const std::vector<schema::TableIndex>& typed_indexes = typed.indexes;
        check(typed_indexes.size() == 2 &&
                  typed_indexes[0].type == "ISAM" &&
                  typed_indexes[1].type == "BTREE",
              "the catalog records ISAM (the default) and BTREE");

        // An unknown USING type is refused by name, catalog unchanged.
        check(contains(sql_error(executor, "CREATE INDEX typed_bad ON typed (v) USING HASH"), "HASH"),
              "an unknown USING type is refused by name");
        check(run_sql(executor, "SELECT * FROM typed WHERE a = 2", result) &&
                  result.rows.rows.size() == 1,
              "the refused type left the table working");

        // One index per column holds across types.
        check(contains(sql_error(executor, "CREATE INDEX typed_a2 ON typed (a) USING BTREE"), "already carries an index"),
              "one index per column holds across types");

        // The BTREE index serves SELECTs and survives maintenance.
        check(run_sql(executor, "SELECT s FROM typed WHERE a = 3", result) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<std::string>(result.rows.rows[0][0]) == "z",
              "a BTREE-declared index serves the index-scan path");
        check(ok([&] {
                  const exec::Executor::IndexCheck verified =
                      executor.check_indexes("typed", "data/typed.tbl");
                  return verified.clean && verified.report.size() == 2;
              }),
              "both index types verify clean");
        check(run_sql(executor, "UPDATE typed SET a = 99 WHERE a = 3", result) &&
                  run_sql(executor, "SELECT s FROM typed WHERE a = 99", result) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1,
              "a key-column move rides the BTREE index");
        check(run_sql(executor, "DELETE FROM typed WHERE a = 99", result) &&
                  ok([&] { return executor.check_indexes("typed", "data/typed.tbl").clean; }),
              "a DELETE keeps the BTREE index verify-clean");

        // The lazy rebuild builds the declared type.
        check(run_sql(executor, "CREATE TABLE rebt (a INTEGER, s VARCHAR(6))", result) &&
                  run_sql(executor, "INSERT INTO rebt VALUES (1, 'x'), (2, 'y')", result) &&
                  run_sql(executor, "CREATE INDEX rebt_a ON rebt (a) USING BTREE", result),
              "the rebuild fixture prepared");
        check(ok([&] { executor.close_all(); }), "the session closed for the rebuild");
        std::remove("data/rebt.tbl.a.idx");
        exec::Executor fresh_btree(manager);
        check(run_sql(fresh_btree, "SELECT * FROM rebt WHERE a = 2", result) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1,
              "the missing BTREE file rebuilt lazily and answers");
        check(ok([&] { fresh_btree.close_all(); }), "the rebuild session closed");
        {
            // The rebuilt file is a B+tree, not an ISAM index.
            isam::IsamIndex probe(manager, typed.schema, 0);
            check(checks::fails_with([&] { probe.open("data/rebt.tbl.a.idx"); }),
                  "the rebuilt file is not an ISAM index");
        }

        // A kind mismatch refuses instead of silently rebuilding: swap the
        // two tables' index files and open again.
        check(run_sql(executor, "CREATE TABLE isam_t (a INTEGER)", result) &&
                  run_sql(executor, "INSERT INTO isam_t VALUES (1), (2)", result) &&
                  run_sql(executor, "CREATE INDEX isam_t_a ON isam_t (a)", result) &&
                  run_sql(executor, "CREATE TABLE btree_t (b INTEGER)", result) &&
                  run_sql(executor, "INSERT INTO btree_t VALUES (1), (2)", result) &&
                  run_sql(executor, "CREATE INDEX btree_t_b ON btree_t (b) USING BTREE", result),
              "the mismatch fixture prepared");
        check(ok([&] { executor.close_all(); }), "the session closed for the swap");
        std::rename("data/isam_t.tbl.a.idx", "/tmp/p3-swap.idx");
        std::rename("data/btree_t.tbl.b.idx", "data/isam_t.tbl.a.idx");
        std::rename("/tmp/p3-swap.idx", "data/btree_t.tbl.b.idx");
        {
            exec::Executor fresh_swap(manager);
            check(!sql_error(fresh_swap, "SELECT * FROM isam_t WHERE a = 1").empty(),
                  "a kind-mismatched index file refuses by name");
            check(ok([&] { fresh_swap.close_all(); }), "the swap session closed");
        }
        // Restore the honest files so later sections see a clean sandbox.
        std::remove("data/isam_t.tbl.a.idx");
        std::remove("data/btree_t.tbl.b.idx");
        std::remove("tablecatalog/isam_t.json");
        std::remove("tablecatalog/btree_t.json");
        std::remove("data/isam_t.tbl");
        std::remove("data/btree_t.tbl");
    }

    // --- the differential oracle over the BTREE type (6.3) -------------------
    // The same gauntlet the ISAM oracle runs, with the table's indexes
    // declared BTREE (one) and ISAM (the other) — one index of each type
    // on separate columns of one table.
    {
        check(run_sql(executor, "CREATE TABLE oracle_b (id INTEGER, tag VARCHAR(8), "
                      "score INTEGER, label VARCHAR(6))", result),
              "the btree oracle table created");
        check(ok([&] { insert_generated(executor, "oracle_b", 500); }),
              "500 generated rows inserted (btree oracle)");
        check(run_sql(executor, "CREATE INDEX oracle_b_id ON oracle_b (id) USING BTREE", result) &&
                  run_sql(executor, "CREATE INDEX oracle_b_score ON oracle_b (score)", result),
              "one BTREE and one ISAM index created on separate columns");

        std::mt19937 rng(20260215);
        auto pick = [&](int lo, int hi) {
            return static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)) + lo;
        };
        bool all_match = true;
        std::string first_mismatch;
        for (int case_index = 0; case_index < 300; ++case_index) {
            std::string where;
            const int shape = pick(0, 6);
            switch (shape) {
                case 0: where = "id = " + std::to_string(pick(1, 500)); break;
                case 1: where = "id < " + std::to_string(pick(1, 500)); break;
                case 2: where = "id >= " + std::to_string(pick(1, 500)); break;
                case 3: {
                    const int a = pick(1, 499);
                    where = "id > " + std::to_string(a) + " AND id < " +
                            std::to_string(a + pick(1, 100));
                    break;
                }
                case 4:
                    where = "score = " + std::to_string(pick(0, 999));
                    break;
                case 5:
                    where = "score > " + std::to_string(pick(0, 900)) +
                            " AND score < " + std::to_string(pick(901, 1000));
                    break;
                case 6:
                    where = "id = " + std::to_string(pick(1, 500)) +
                            " OR score < " + std::to_string(pick(0, 200));
                    break;
            }
            const std::string select =
                "SELECT * FROM oracle_b WHERE " + where;
            exec::StatementResult with_index;
            executor.set_allow_index(true);
            if (!run_sql(executor, select, with_index)) {
                all_match = false;
                first_mismatch = "index path failed: " + select;
                break;
            }
            exec::StatementResult without;
            executor.set_allow_index(false);
            if (!run_sql(executor, select, without)) {
                all_match = false;
                first_mismatch = "heap path failed: " + select;
                break;
            }
            executor.set_allow_index(true);
            std::vector<std::vector<exec::Value>> a = with_index.rows.rows;
            std::vector<std::vector<exec::Value>> b = without.rows.rows;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a != b ||
                with_index.rows.columns != without.rows.columns) {
                all_match = false;
                first_mismatch = select + " (index " +
                                 std::to_string(with_index.rows.rows.size()) +
                                 " rows, heap " +
                                 std::to_string(without.rows.rows.size()) +
                                 " rows)";
                break;
            }
        }
        check(all_match, "300 random predicates on the btree oracle: index path == heap path" +
                             (all_match ? std::string() : " — " + first_mismatch));
        executor.set_allow_index(true);
        check(ok([&] { executor.close_all(); }), "the btree oracle session closed");
    }

    chdir(original.c_str());
    std::cout << (failures == 0
                      ? "exec suite: all checks passed."
                      : "exec suite: " + std::to_string(failures) +
                            " check(s) FAILED.")
              << "\n";
    return failures;
}
