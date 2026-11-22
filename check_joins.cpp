// p4stdbeng — join checks: the executor-level behavior of the FROM chain
// and the differential oracle for joins. The behavioral section pins the
// announcement, star and LIMIT over a join, the bind-time refusals
// (which cost no data-file I/O), and session survival across a failed
// join. The oracle inserts known randomized rows, computes every join
// independently in C++ (the reference), and holds the engine to
// identical answers through BOTH grammar forms — each also re-run with
// pushdown classification bypassed. Runs inside a sandbox directory so
// catalogs and data files never touch the workspace.

#include "bufpool/BufferManager.h"
#include "check_support.h"
#include "common/Error.h"
#include "exec/Executor.h"
#include "policy/LRUPolicy.h"
#include "sql/Parser.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << "\n";
    if (!condition) {
        ++failures;
    }
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// One statement through the parser and executor.
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

// --- the oracle's reference row ----------------------------------------------

// One joined row as the projection `(lw.k, lw.a, rw.k, rw.b)` returns it.
using RefRow = std::tuple<int, std::string, int, std::string>;

RefRow ref_row(int lk, std::string la, int rk, std::string rb) {
    return RefRow{lk, std::move(la), rk, std::move(rb)};
}

RefRow as_ref_row(const std::vector<exec::Value>& row) {
    return RefRow{std::get<int>(row[0]), std::get<std::string>(row[1]),
                  std::get<int>(row[2]), std::get<std::string>(row[3])};
}

std::vector<RefRow> sorted_rows(std::vector<RefRow> rows) {
    std::sort(rows.begin(), rows.end());
    return rows;
}

// The engine's answer to one query, as sorted reference rows; nullopt
// (with the reason printed) when the query fails. `access_path`, when
// given, receives the announced path.
std::optional<std::vector<RefRow>> engine_rows(exec::Executor& executor,
                                               const std::string& query,
                                               std::string* access_path = nullptr) {
    exec::StatementResult result;
    if (!run_sql(executor, query, result)) {
        return std::nullopt;
    }
    if (access_path != nullptr) {
        *access_path = result.rows.access_path;
    }
    std::vector<RefRow> rows;
    rows.reserve(result.rows.rows.size());
    for (const std::vector<exec::Value>& row : result.rows.rows) {
        rows.push_back(as_ref_row(row));
    }
    return sorted_rows(std::move(rows));
}

// --- the oracle's data -------------------------------------------------------

// The rows inserted into each table, kept here so the reference join can
// be computed without the engine.
struct Fixture {
    std::vector<std::pair<int, std::string>> left;   // (k, a)
    std::vector<std::pair<int, std::string>> right;  // (k, b)
};

// The reference join: every pair the predicate keeps, computed straight
// from the recorded rows — outer loop over the left table, inner over the
// right, exactly the lecture's algorithm but off the engine's path.
std::vector<RefRow> reference_join(const Fixture& fixture,
                                   const std::function<bool(int, const std::string&, int,
                                                            const std::string&)>& pred) {
    std::vector<RefRow> rows;
    for (const auto& [lk, la] : fixture.left) {
        for (const auto& [rk, rb] : fixture.right) {
            if (pred(lk, la, rk, rb)) {
                rows.push_back(ref_row(lk, la, rk, rb));
            }
        }
    }
    return rows;
}

// One oracle round: a predicate as SQL (over qualified names) and as the
// reference lambda; `cross` rounds have no WHERE in the comma form and a
// constant-true ON in the JOIN form.
struct Round {
    const char* name;
    const char* pred;  // "" for the cross round
    std::function<bool(int, const std::string&, int, const std::string&)> pred_fn;
};

// One oracle round, run: both grammar forms against the reference across
// the THREE access paths — index and hash allowed (an `lw.k = rw.k` shape
// announces the index join, a shape no index serves announces the hash
// join, anything else keeps the plain one); indexes suppressed with hash
// allowed (every equated shape announces the hash join); both suppressed
// (the plain join only). Every run's rows must equal the reference: the
// answer never depends on the path.
void run_round(exec::Executor& executor, const Fixture& fixture, const std::string& name,
               const std::string& pred,
               const std::function<bool(int, const std::string&, int, const std::string&)>&
                   pred_fn) {
    // The two grammar forms of the round's query.
    const std::string projection = "SELECT lw.k, lw.a, rw.k, rw.b ";
    const std::string join_form =
        pred.empty() ? projection + "FROM lw JOIN rw ON 1 = 1"
                     : projection + "FROM lw JOIN rw ON " + pred;
    const std::string comma_form =
        pred.empty() ? projection + "FROM lw, rw" : projection + "FROM lw, rw WHERE " + pred;

    const std::vector<RefRow> expected = sorted_rows(reference_join(fixture, pred_fn));
    // The round's shapes: the equated keys where an index serves the
    // inner column (the probe) and where none does (the string equijoin —
    // the hash join's shape). (The cross round's empty predicate and the
    // range shapes have no equated columns at all.)
    const bool probes = pred.find("lw.k = rw.k") != std::string::npos;
    const bool shape = probes || pred.find("lw.a = rw.b") != std::string::npos;
    const std::string index_path =
        probes ? "index nested loops join" : (shape ? "hash join" : "nested loops join");
    const std::string hash_path = shape ? "hash join" : "nested loops join";

    std::string join_path;
    std::string comma_path;
    const std::optional<std::vector<RefRow>> from_join = engine_rows(executor, join_form, &join_path);
    const std::optional<std::vector<RefRow>> from_comma = engine_rows(executor, comma_form, &comma_path);

    const bool ok = from_join.has_value() && from_comma.has_value() &&
                    *from_join == expected && *from_comma == expected &&
                    join_path == index_path && comma_path == index_path;
    check(ok, "oracle: " + name + " — both forms match the reference (" +
                  std::to_string(expected.size()) + " rows)" +
                  (probes ? " [via index probes]" : (shape ? " [via hash probes]" : "")));
    if (!ok && from_join.has_value()) {
        std::cout << "      join form: " << from_join->size() << " rows ("
                  << join_path << "), reference " << expected.size() << "\n";
    }

    // Index suppressed, hash allowed: the same rounds through the hash
    // join (any equated shape) or the plain join — still the reference.
    executor.set_allow_index(false);
    std::string suppressed_path;
    const std::optional<std::vector<RefRow>> hash_join =
        engine_rows(executor, join_form, &suppressed_path);
    const std::optional<std::vector<RefRow>> hash_comma = engine_rows(executor, comma_form);
    const bool hash_ok = hash_join.has_value() && hash_comma.has_value() &&
                         *hash_join == expected && *hash_comma == expected &&
                         suppressed_path == hash_path;
    check(hash_ok, "oracle: " + name + " — index suppressed, both forms still match the reference");

    // Both suppressed: the plain scan-rescan join only.
    executor.set_allow_hash(false);
    std::string plain_path;
    const std::optional<std::vector<RefRow>> plain_join =
        engine_rows(executor, join_form, &plain_path);
    const std::optional<std::vector<RefRow>> plain_comma = engine_rows(executor, comma_form);
    executor.set_allow_index(true);
    executor.set_allow_hash(true);
    const bool plain_ok = plain_join.has_value() && plain_comma.has_value() &&
                          *plain_join == expected && *plain_comma == expected &&
                          plain_path == "nested loops join";
    check(plain_ok, "oracle: " + name + " — both suppressed, both forms still match the reference");

    // No bucket files outlive the round — every path erases them.
    std::error_code ec;
    const bool tmp_clear = !std::filesystem::exists("tmp", ec) ||
                           std::filesystem::is_empty("tmp", ec);
    check(tmp_clear, "oracle: " + name + " — no bucket files outlive the round");
}

void oracle_rounds(exec::Executor& executor, const Fixture& fixture) {
    const std::vector<Round> rounds = {
        {"equijoin", "lw.k = rw.k",
         [](int lk, const std::string&, int rk, const std::string&) { return lk == rk; }},
        {"strictly less", "lw.k < rw.k",
         [](int lk, const std::string&, int rk, const std::string&) { return lk < rk; }},
        {"range overlap", "lw.k >= rw.k - 1",
         [](int lk, const std::string&, int rk, const std::string&) { return lk >= rk - 1; }},
        {"equijoin plus left conjunct", "lw.k = rw.k AND lw.a <> 'aa'",
         [](int lk, const std::string& la, int rk, const std::string&) {
             return lk == rk && la != "aa";
         }},
        {"equijoin plus conjuncts on both sides",
         "lw.k = rw.k AND rw.b <> 'cc' AND lw.k > 1",
         [](int lk, const std::string&, int rk, const std::string& rb) {
             return lk == rk && rb != "cc" && lk > 1;
         }},
        {"string equijoin", "lw.a = rw.b",
         [](int, const std::string& la, int, const std::string& rb) { return la == rb; }},
        {"cross join", "",
         [](int, const std::string&, int, const std::string&) { return true; }},
    };

    for (const Round& round : rounds) {
        run_round(executor, fixture, round.name, round.pred, round.pred_fn);
    }

    // Randomized predicates: each round draws a cross-table comparison, an
    // optional left conjunct, and an optional right conjunct — the SQL
    // text and the reference lambda come from the same draw.
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> op_draw(0, 4);
    std::uniform_int_distribution<int> const_draw(0, 5);
    std::uniform_int_distribution<int> word_draw(0, 2);
    std::uniform_int_distribution<int> coin(0, 1);
    const char* ops[] = {"=", "<>", "<", "<=", ">"};
    const char* const words[] = {"aa", "bb", "cc"};
    const auto op_fn = [](int op, int a, int b) {
        switch (op) {
            case 0: return a == b;
            case 1: return a != b;
            case 2: return a < b;
            case 3: return a <= b;
            default: return a > b;
        }
    };
    const auto word_at = [&words](int index) { return words[index]; };
    for (int round_index = 0; round_index < 8; ++round_index) {
        const int op = op_draw(rng);
        const bool left_conjunct = coin(rng) == 1;
        const bool right_conjunct = coin(rng) == 1;
        const int left_k = const_draw(rng);
        const int right_k = const_draw(rng);
        const int left_word = word_draw(rng);
        const int right_word = word_draw(rng);

        std::string pred = "lw.k " + std::string(ops[op]) + " rw.k";
        std::function<bool(int, const std::string&, int, const std::string&)> fn =
            [op_fn, op](int lk, const std::string&, int rk, const std::string&) {
                return op_fn(op, lk, rk);
            };
        if (left_conjunct) {
            pred += " AND lw.a <> '" + std::string(words[left_word]) + "'";
            const std::string w = word_at(left_word);
            fn = [fn, w](int lk, const std::string& la, int rk, const std::string& rb) {
                return fn(lk, la, rk, rb) && la != w;
            };
        }
        if (right_conjunct) {
            const int op2 = op_draw(rng);
            pred += " AND rw.k " + std::string(ops[op2]) + " " + std::to_string(right_k) +
                    " AND rw.b = '" + std::string(words[right_word]) + "'";
            const int rk_bound = right_k;
            const std::string w = word_at(right_word);
            fn = [fn, op_fn, op2, rk_bound, w](int lk, const std::string& la, int rk,
                                               const std::string& rb) {
                return fn(lk, la, rk, rb) && op_fn(op2, rk, rk_bound) && rb == w;
            };
        }
        run_round(executor, fixture,
                  "random round " + std::to_string(round_index) + " (" + pred + ")", pred, fn);
    }
}

// The same rounds with pushdown classification bypassed: every conjunct
// stays in one filter above the join. The answers must not move.
void bypass_rounds(exec::Executor& executor, const Fixture& fixture) {
    const std::vector<Round> rounds = {
        {"equijoin", "lw.k = rw.k",
         [](int lk, const std::string&, int rk, const std::string&) { return lk == rk; }},
        {"equijoin plus left conjunct", "lw.k = rw.k AND lw.a <> 'aa'",
         [](int lk, const std::string& la, int rk, const std::string&) {
             return lk == rk && la != "aa";
         }},
        {"equijoin plus conjuncts on both sides",
         "lw.k = rw.k AND rw.b <> 'cc' AND lw.k > 1",
         [](int lk, const std::string&, int rk, const std::string& rb) {
             return lk == rk && rb != "cc" && lk > 1;
         }},
    };
    for (const Round& round : rounds) {
        const std::string query =
            "SELECT lw.k, lw.a, rw.k, rw.b FROM lw JOIN rw ON " + std::string(round.pred);
        const std::vector<RefRow> expected =
            sorted_rows(reference_join(fixture, round.pred_fn));
        // With pushdown bypassed the rule still fires on the ON's
        // equijoin: the whole WHERE stays above the join, the inner
        // predicate is empty, and the probe narrows alone.
        std::string bypassed_path;
        const std::optional<std::vector<RefRow>> bypassed =
            engine_rows(executor, query, &bypassed_path);
        check(bypassed.has_value() && *bypassed == expected &&
                  bypassed_path == "index nested loops join",
              std::string("pushdown bypass: ") + round.name +
                  " answers as the reference (via index probes)");
    }
}

} // namespace

int run_joins_checks() {
    // The sandbox: a fresh temp directory becomes the working directory,
    // so `data/<table>.tbl` and `tablecatalog/` land there. Restored on exit.
    char pattern[] = "/tmp/p4stdbeng-joins-XXXXXX";
    char* made = mkdtemp(pattern);
    if (made == nullptr) {
        std::cout << "FAIL: join sandbox could not be created\n";
        return 1;
    }
    const std::string original = [] {
        std::vector<char> buffer;
        buffer.resize(4096);
        return std::string(getcwd(buffer.data(), buffer.size()));
    }();
    chdir(made);
    if (system("mkdir -p tablecatalog data") != 0) {
        std::cout << "FAIL: join sandbox catalog dir\n";
    }

    bufman::BufferManager manager;
    manager.init(16, std::make_unique<bufman::LRUPolicy>());
    exec::Executor executor(manager);

    exec::StatementResult result;

    // --- the fixture: two tables, known randomized rows -------------------
    Fixture fixture;
    {
        check(run_sql(executor,
                      "CREATE TABLE lw (k INTEGER, a VARCHAR(6))", result),
              "oracle: table lw created");
        check(run_sql(executor,
                      "CREATE TABLE rw (k INTEGER, b VARCHAR(6))", result),
              "oracle: table rw created");

        std::mt19937 rng(2026);
        std::uniform_int_distribution<int> key(1, 4);   // a small domain: duplicates
        std::uniform_int_distribution<int> word(0, 2);
        const char* words[] = {"aa", "bb", "cc"};

        const auto insert_all = [&](const char* table, const char* word_column,
                                    std::vector<std::pair<int, std::string>>& into,
                                    std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                const int k = key(rng);
                const std::string w = words[word(rng)];
                std::string text = "INSERT INTO " + std::string(table) + " VALUES (" +
                                   std::to_string(k) + ", '" + w + "')";
                if (!run_sql(executor, text, result)) {
                    check(false, std::string("oracle: insert into ") + table);
                    return;
                }
                into.emplace_back(k, w);
            }
        };
        insert_all("lw", "a", fixture.left, 12);
        insert_all("rw", "b", fixture.right, 9);
        check(fixture.left.size() == 12 && fixture.right.size() == 9,
              "oracle: 12 left and 9 right rows inserted");
    }

    // --- the announcement and the shapes ----------------------------------
    {
        // (hash-join supersession: no index exists yet at this point, so
        // the equated shape announces the hash join — the spec's second
        // preference — where it once announced the plain join.)
        check(run_sql(executor,
                      "SELECT lw.k, lw.a, rw.k, rw.b FROM lw JOIN rw ON lw.k = rw.k", result) &&
                  result.rows.access_path == "hash join",
              "a join announces (via hash join)");

        check(run_sql(executor, "SELECT * FROM lw JOIN rw ON lw.k = rw.k", result) &&
                  result.rows.columns.size() == 4 &&
                  result.rows.columns[0] == "k" && result.rows.columns[1] == "a" &&
                  result.rows.columns[2] == "k" && result.rows.columns[3] == "b",
              "star over a join spreads across both tables in FROM order");

        // LIMIT over a join: the pipeline stops early with exactly the
        // asked-for number of rows.
        check(run_sql(executor,
                      "SELECT lw.k, lw.a, rw.k, rw.b FROM lw JOIN rw ON lw.k = rw.k LIMIT 3",
                      result) &&
                  result.rows.rows.size() == 3,
              "LIMIT over a join yields exactly the limit");
    }

    // --- three-way chains: every join pairs the accumulation with its own inner ---
    {
        // A third table with known rows; keys 2 and 3 collide with the
        // fixture's 1..4 domain so the chain's second join has real work.
        check(run_sql(executor, "CREATE TABLE tw (k INTEGER, c VARCHAR(6))", result) &&
                  run_sql(executor,
                          "INSERT INTO tw VALUES (2, 'aa'), (3, 'bb'), (12, 'cc')", result),
              "the third chain table created and filled");

        using Triple = std::tuple<int, std::string, int, std::string, int, std::string>;
        const auto engine_triples = [&](const std::string& query) {
            std::vector<Triple> rows;
            if (run_sql(executor, query, result)) {
                for (const std::vector<exec::Value>& row : result.rows.rows) {
                    rows.emplace_back(std::get<int>(row[0]), std::get<std::string>(row[1]),
                                      std::get<int>(row[2]), std::get<std::string>(row[3]),
                                      std::get<int>(row[4]), std::get<std::string>(row[5]));
                }
            }
            std::sort(rows.begin(), rows.end());
            return rows;
        };

        // The reference: ((lw ⋈ rw) ⋈ tw), computed off the engine.
        std::vector<Triple> expected;
        for (const auto& [lk, la] : fixture.left) {
            for (const auto& [rk, rb] : fixture.right) {
                if (lk != rk) continue;
                for (const auto& [tk, tc] :
                     {std::pair<int, std::string>{2, "aa"}, std::pair<int, std::string>{3, "bb"},
                      std::pair<int, std::string>{12, "cc"}}) {
                    if (rk == tk) {
                        expected.emplace_back(lk, la, rk, rb, tk, tc);
                    }
                }
            }
        }
        std::sort(expected.begin(), expected.end());
        check(!expected.empty(), "the three-way reference is non-empty");

        check(engine_triples("SELECT lw.k, lw.a, rw.k, rw.b, tw.k, tw.c "
                             "FROM lw JOIN rw ON lw.k = rw.k JOIN tw ON rw.k = tw.k") == expected,
              "a three-way JOIN chain answers ((lw JOIN rw) JOIN tw)");
        check(engine_triples("SELECT lw.k, lw.a, rw.k, rw.b, tw.k, tw.c "
                             "FROM lw, rw, tw WHERE lw.k = rw.k AND rw.k = tw.k") == expected,
              "a three-way comma chain answers the same left-deep join");
        check(engine_triples("SELECT lw.k, lw.a, rw.k, rw.b, tw.k, tw.c FROM lw JOIN rw "
                             "ON lw.k = rw.k, tw WHERE rw.k = tw.k") == expected,
              "a mixed three-way chain continues in FROM order");

        // LIMIT caps the chain's output.
        if (expected.size() >= 2) {
            check(run_sql(executor,
                          "SELECT lw.k, lw.a, rw.k, rw.b, tw.k, tw.c FROM lw JOIN rw "
                          "ON lw.k = rw.k JOIN tw ON rw.k = tw.k LIMIT 2",
                          result) &&
                      result.rows.rows.size() == 2,
                  "LIMIT over a three-way chain yields exactly the limit");
        }
    }

    // --- the bind-time refusals, and survival across them ------------------
    {
        check(contains(sql_error(executor, "SELECT k FROM lw, rw WHERE lw.k = rw.k"),
                       "ambiguous column 'k'"),
              "a bare name in both tables is refused as ambiguous");
        check(contains(sql_error(executor, "SELECT x.k FROM lw JOIN rw ON lw.k = rw.k"),
                       "unknown table or alias"),
              "an unknown qualifier is refused by name");
        check(contains(sql_error(executor,
                                 "SELECT lw.nosuch FROM lw JOIN rw ON lw.k = rw.k"),
                       "unknown column"),
              "an unknown column under a qualifier is refused by name");
        check(contains(sql_error(executor,
                                 "SELECT lw.k FROM lw JOIN rw ON lw.k = rw.nosuch"),
                       "unknown column"),
              "an unknown column inside an ON is refused by name");

        // The session survives every failed join: the next statement runs
        // (and answers through the hash join — the equated shape with no
        // index serving it).
        check(run_sql(executor,
                      "SELECT lw.k, lw.a, rw.k, rw.b FROM lw JOIN rw ON lw.k = rw.k",
                      result) &&
                  result.rows.access_path == "hash join",
              "the session survives a failed join");
    }

    // --- the indexes on the join columns ------------------------------------
    // Created after the no-index pins above: from here on every oracle
    // round runs through all three access paths — everything allowed (the
    // index join when an inner index serves the shape, the hash join when
    // only the shape is equated), indexes suppressed (hash or plain), and
    // both suppressed (the plain join only) — all held to the reference.
    {
        check(run_sql(executor, "CREATE INDEX lw_k ON lw (k)", result) &&
                  run_sql(executor, "CREATE INDEX rw_k ON rw (k)", result),
              "the fixture's join columns indexed");
    }

    // --- the oracle: both forms against the reference ----------------------
    oracle_rounds(executor, fixture);

    // --- the pushdown bypass -----------------------------------------------
    executor.set_pushdown(false);
    bypass_rounds(executor, fixture);
    executor.set_pushdown(true);
    check(run_sql(executor,
                  "SELECT lw.k, lw.a, rw.k, rw.b FROM lw JOIN rw ON lw.k = rw.k",
                  result) && result.rows.rows.size() > 0,
          "pushdown restored after the bypass rounds");

    // --- the refusals cost no data-file I/O ---------------------------------
    //
    // A fresh session over catalogs whose data files are gone from disk:
    // a bind-time refusal must name the bind error — reaching the data
    // file would fail with "cannot open" instead.
    {
        check(run_sql(executor, "CREATE TABLE io_lw (k INTEGER, a VARCHAR(6))", result) &&
                  run_sql(executor, "CREATE TABLE io_rw (k INTEGER, b VARCHAR(6))", result),
              "the no-I/O probe's tables created");
        check(run_sql(executor, "INSERT INTO io_lw VALUES (1, 'aa')", result),
              "the no-I/O probe's left row inserted");
        executor.close_all();
        std::remove("data/io_lw.tbl");
        std::remove("data/io_rw.tbl");

        exec::Executor fresh(manager);
        const std::string ambiguous =
            sql_error(fresh, "SELECT k FROM io_lw, io_rw WHERE io_lw.k = io_rw.k");
        check(contains(ambiguous, "ambiguous") && !contains(ambiguous, "cannot open"),
              "an ambiguous name is refused before any data-file I/O");
        const std::string unknown =
            sql_error(fresh, "SELECT z.k FROM io_lw, io_rw WHERE io_lw.k = io_rw.k");
        check(contains(unknown, "unknown table or alias") &&
                  !contains(unknown, "cannot open"),
              "an unknown qualifier is refused before any data-file I/O");
        fresh.close_all();
    }

    executor.close_all();
    chdir(original.c_str());
    std::cout << (failures == 0
                      ? "join suite: all checks passed."
                      : "join suite: " + std::to_string(failures) +
                            " check(s) FAILED.")
              << "\n";
    return failures;
}
