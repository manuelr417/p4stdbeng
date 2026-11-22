// parts_bench — the parts-index-benchmark executable: one run measures
// the engine's search cost over generated parts rows, first on the heap
// path (no index), then on the index path, and prints the comparison.
//
// The run sequence: argument parsing (record count, default 1,000,000),
// clean slate (drops tolerating absence), CREATE TABLE, deterministic
// generation with a serializer round-trip self-check, load through the
// programmatic insert path, timed heap-scan baselines, one ISAM index
// per searched column with timed builds, timed index-scan re-runs, a
// result-set equality check between the two paths, and a plain-text
// comparison report. The engine is driven exactly as check_exec.cpp
// drives it: one shared BufferManager, one Executor, no engine changes.

#include "bufpool/BufferManager.h"
#include "common/Error.h"
#include "exec/Executor.h"
#include "policy/LRUPolicy.h"
#include "schema/TableSchema.h"
#include "schema/TupleSerializer.h"
#include "sql/Parser.h"
#include "tools/BulkLoader.h"
#include "tuple/Tuple.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kDefaultRows = 1000000;
constexpr std::size_t kMaxRows = 9999999;
constexpr std::size_t kPoolFrames = 8;
constexpr std::uint32_t kSeed = 42;
const char* const kTable = "parts";
const char* const kDataFile = "data/parts.tbl";
const char* const kPartidIndex = "parts_partid";
const char* const kPpriceIndex = "parts_pprice";
// Q1's lookup key: the spec'd 54645 once the table is large enough to
// contain it, otherwise the largest generated partid — either way the
// equality query answers exactly one row.
constexpr std::size_t kLookupKey = 54645;

const char* const kColors[] = {"red", "blue",   "white", "beige",
                               "green", "black", "yellow"};
const char* const kMaterials[] = {"steel", "aluminum", "copper",
                                  "plastic", "rubber", "brass"};

struct Config {
    std::size_t rows = kDefaultRows;
    bool reuse = false;
};

struct QueryTiming {
    double ms = 0.0;
    std::string access_path;
    std::size_t row_count = 0;
};

struct Measurements {
    double load_ms = 0.0;
    double build_partid_ms = 0.0;       // ISAM
    double build_pprice_ms = 0.0;       // ISAM
    double build_btree_partid_ms = 0.0;
    double build_btree_pprice_ms = 0.0;
    QueryTiming q1_heap;
    QueryTiming q1_index;               // ISAM
    QueryTiming q1_btree;
    QueryTiming q2_heap;
    QueryTiming q2_index;               // ISAM
    QueryTiming q2_btree;
    bool q1_agrees = false;
    bool q2_agrees = false;
    bool q1_btree_agrees = false;
    bool q2_btree_agrees = false;
    std::size_t q2_expected = 0;
    std::uint64_t fingerprint = 0;
};

[[noreturn]] void fail(const std::string& message) {
    throw common::Error(message);
}

double now_ms(std::chrono::steady_clock::time_point start) {
    const auto stop = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(stop - start).count();
}

// --- argument parsing ---------------------------------------------------------

// --rows N / --rows=N (default kDefaultRows) and the --reuse flag.
// Validation happens before anything else runs: non-numeric, <= 0, or
// beyond kMaxRows fails with a named error (the ceiling keeps every
// generated pname inside VARCHAR(10)); --rows and --reuse together fail
// (a reuse run's count is the existing data's, not an argument); unknown
// arguments fail the same way.
Config parse_args(int argc, char** argv) {
    Config config;
    bool rows_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--reuse") {
            config.reuse = true;
            continue;
        }
        std::string value;
        if (arg == "--rows") {
            if (i + 1 >= argc) {
                fail("--rows needs a value");
            }
            value = argv[++i];
        } else if (arg.rfind("--rows=", 0) == 0) {
            value = arg.substr(std::string("--rows=").size());
        } else {
            fail("unknown argument '" + arg + "' (expected --rows N or --reuse)");
        }
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
            fail("--rows must be a positive integer, got '" + value + "'");
        }
        const std::size_t parsed = std::strtoull(value.c_str(), nullptr, 10);
        if (parsed < 1 || parsed > kMaxRows) {
            fail("--rows must be between 1 and " + std::to_string(kMaxRows) + ", got " + value);
        }
        config.rows = parsed;
        rows_seen = true;
    }
    if (config.reuse && rows_seen) {
        fail("--reuse and --rows cannot be combined: a reuse run's record count is the "
             "existing data's");
    }
    return config;
}

// --- statement plumbing -------------------------------------------------------

// Parses one statement's text.
std::unique_ptr<sql::Statement> parse_one(const std::string& text) {
    sql::ParseResult parsed = sql::parse(text);
    if (parsed.statements.size() != 1) {
        fail("expected exactly one statement, got " + std::to_string(parsed.statements.size()));
    }
    return std::move(parsed.statements.front());
}

// Executes one statement's text through the shared executor. SELECT
// fills the result; the caller times around this for the measured steps.
exec::StatementResult run_sql(exec::Executor& executor, const std::string& text) {
    return executor.execute(*parse_one(text));
}

// The clean-slate phase, split by mode: the index drops always run (a
// genuine heap-scan baseline needs no indexes, and their absence is
// tolerated), while DROP TABLE runs only in default mode — a reuse run
// keeps the catalog and data file.
void clean_slate(exec::Executor& executor, bool keep_table) {
    std::vector<std::string> drops = {
        std::string("DROP INDEX ") + kPartidIndex + ";",
        std::string("DROP INDEX ") + kPpriceIndex + ";",
    };
    if (!keep_table) {
        drops.push_back(std::string("DROP TABLE ") + kTable + ";");
    }
    for (const std::string& text : drops) {
        try {
            const std::string message = run_sql(executor, text).message;
            std::cout << "clean slate: " << message << "\n";
        } catch (const common::Error& e) {
            std::cout << "clean slate: absent (" << e.what() << ")\n";
        }
    }
}

// --- the parts generator ------------------------------------------------------

// "p" + the row number, zero-padded to 6 digits: 7 characters through
// 999,999 and 8 beyond, so pname stays inside VARCHAR(10) for every
// allowed record count.
std::string part_name(std::size_t row_number) {
    std::string digits = std::to_string(row_number);
    while (digits.size() < 6) {
        digits.insert(digits.begin(), '0');
    }
    return "p" + digits;
}

double clamped_normal(std::mt19937& engine, std::normal_distribution<>& dist,
                      double lo, double hi) {
    const double value = dist(engine);
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

// FNV-1a over every row's values: two runs with the same record count
// must print the same fingerprint, which is how determinism is checked
// from outside the process.
std::uint64_t fingerprint(const std::vector<tuple::Tuple>& rows) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const char* bytes, std::size_t length) {
        for (std::size_t i = 0; i < length; ++i) {
            hash ^= static_cast<unsigned char>(bytes[i]);
            hash *= 1099511628211ull;
        }
    };
    for (const tuple::Tuple& tuple : rows) {
        for (std::size_t c = 0; c < tuple.column_count(); ++c) {
            const char column_tag = static_cast<char>('0' + c);
            mix(&column_tag, 1);
            const tuple::Column& column = tuple.column(c);
            switch (column.column_type()) {
                case tuple::ColumnType::Integer: {
                    const int value = std::get<int>(column.column_value());
                    mix(reinterpret_cast<const char*>(&value), sizeof(value));
                    break;
                }
                case tuple::ColumnType::String: {
                    const std::string& value =
                        std::get<std::string>(column.column_value());
                    mix(value.data(), value.size());
                    break;
                }
                case tuple::ColumnType::Double: {
                    const double value =
                        std::get<double>(column.column_value());
                    mix(reinterpret_cast<const char*>(&value), sizeof(value));
                    break;
                }
            }
        }
    }
    return hash;
}

// Deterministic generation (fixed seed): sequential partid, a color from
// the seven-color list, clamped-normal price and weight, reproducible
// strings. Every row is validated as it is built, so a generator
// regression fails the run loudly instead of loading bad data.
std::vector<tuple::Tuple> generate_parts(std::size_t count) {
    std::mt19937 engine(kSeed);
    std::uniform_int_distribution<std::size_t> color_dist(0, 6);
    std::uniform_int_distribution<std::size_t> material_dist(0, 5);
    std::normal_distribution<double> price_dist(1500.25, 2999.5 / 6.0);
    std::normal_distribution<double> weight_dist(25.5, 49.0 / 6.0);

    std::vector<tuple::Tuple> rows;
    rows.reserve(count);
    for (std::size_t i = 1; i <= count; ++i) {
        const double price =
            clamped_normal(engine, price_dist, 0.5, 3000.0);
        const double weight =
            clamped_normal(engine, weight_dist, 1.0, 50.0);
        const std::string name = part_name(i);
        if (price < 0.5 || price > 3000.0 || weight < 1.0 || weight > 50.0 ||
            name.size() > 10) {
            fail("generated row " + std::to_string(i) + " violates its column bounds");
        }
        tuple::Tuple tuple;
        tuple.add(static_cast<int>(i));
        tuple.add(name);
        tuple.add(std::string(kColors[color_dist(engine)]));
        tuple.add(weight);
        tuple.add(price);
        tuple.add(std::string(kMaterials[material_dist(engine)]));
        rows.push_back(std::move(tuple));
    }
    return rows;
}

// The width contract, checked where it bites: a sample of generated rows
// must survive serialize/deserialize against the created schema
// unchanged (first and last row — the last carries the widest pname).
void round_trip_check(const schema::TableSchema& schema, const std::vector<tuple::Tuple>& rows) {
    schema::TupleSerializer serializer(schema);
    std::vector<char> buffer(serializer.record_size());
    for (const std::size_t index : {std::size_t{0}, rows.size() - 1}) {
        serializer.serialize(rows[index], buffer.data());
        if (!(serializer.deserialize(buffer.data()) == rows[index])) {
            fail("round-trip mismatch at row " + std::to_string(index + 1));
        }
    }
}

// --- result-set plumbing ------------------------------------------------------

// One stable key per row: int/double/string tagged and rendered with
// full double precision, so set equality cannot be fooled by display
// rounding. Rows are sorted before comparison — row order is
// unspecified without ORDER BY.
std::string value_key(const exec::Value& value) {
    if (const int* v = std::get_if<int>(&value)) {
        return "i" + std::to_string(*v);
    }
    if (const double* v = std::get_if<double>(&value)) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", *v);
        return std::string("d") + buffer;
    }
    return "s" + std::get<std::string>(value);
}

std::vector<std::string> row_keys(const exec::ResultSet& rows) {
    std::vector<std::string> keys;
    keys.reserve(rows.rows.size());
    for (const std::vector<exec::Value>& row : rows.rows) {
        std::string key;
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i != 0) {
                key += "|";
            }
            key += value_key(row[i]);
        }
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

bool sets_agree(const exec::ResultSet& baseline,
                const exec::ResultSet& indexed) {
    return row_keys(baseline) == row_keys(indexed);
}

// The loaded data, spot-checked through the engine against the rows the
// generator produced: the lookup key answers exactly one row, and
// partid 1, the lookup key, and partid N match the generated tuples
// field for field.
exec::ResultSet spot_checks(exec::Executor& executor, const std::vector<tuple::Tuple>& rows,
                            std::size_t lookup_key) {
    exec::ResultSet lookup_key_result;
    for (const std::size_t partid : {std::size_t{1}, lookup_key, rows.size()}) {
        exec::StatementResult result =
            run_sql(executor, "SELECT * FROM parts WHERE partid = " + std::to_string(partid) + ";");
        const tuple::Tuple& expected = rows[partid - 1];
        const std::string label = "spot check for partid " + std::to_string(partid);
        if (result.rows.rows.size() != 1 || result.rows.rows[0].size() != expected.column_count()) {
            fail(label + " returned " + std::to_string(result.rows.rows.size()) + " rows");
        }
        for (std::size_t c = 0; c < result.rows.rows[0].size(); ++c) {
            const exec::Value& value = result.rows.rows[0][c];
            tuple::Column fetched(0);
            if (const int* v = std::get_if<int>(&value)) {
                fetched = tuple::Column(*v);
            } else if (const double* v = std::get_if<double>(&value)) {
                fetched = tuple::Column(*v);
            } else {
                fetched = tuple::Column(std::get<std::string>(value));
            }
            if (!(fetched == expected.column(c))) {
                fail(label + " column " + std::to_string(c) + " disagrees");
            }
        }
        if (partid == lookup_key) {
            lookup_key_result = std::move(result.rows);
        }
    }
    return lookup_key_result;
}

// Every index the catalog currently declares must exist beside the data
// file, and the session's index verification must walk a nonzero entry
// count per index cleanly.
void check_index_files(exec::Executor& executor) {
    for (const schema::TableIndex& index : executor.table(kTable).indexes) {
        const std::string file = std::string(kDataFile) + "." + index.column + ".idx";
        if (!std::filesystem::exists(file)) {
            fail("missing index file " + file);
        }
    }
    const exec::Executor::IndexCheck check = executor.check_indexes(kTable, kDataFile);
    for (const std::string& line : check.report) {
        std::cout << "index check: " << line << "\n";
        const std::size_t colon = line.find(": ");
        const std::size_t entries = line.find(" entries");
        if (colon == std::string::npos || entries == std::string::npos) {
            continue;
        }
        const std::size_t count =
            std::strtoull(line.substr(colon + 2, entries - colon - 2).c_str(), nullptr, 10);
        if (count == 0) {
            fail("index walked zero entries: " + line);
        }
    }
    if (!check.clean) {
        fail("index verification reported findings");
    }
}

void print_report(const Config& config, std::size_t n, const Measurements& m,
                  std::size_t lookup_key) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "== parts_bench report ==\n";
    if (config.reuse) {
        std::cout << "data:                   reused (" << n
                  << " rows, load skipped)\n";
    } else {
        std::cout << "rows loaded:            " << n << "  (load "
                  << m.load_ms << " ms)\n";
    }
    std::cout << "data fingerprint:       0x" << std::hex << m.fingerprint
              << std::dec << "\n";
    std::cout << "index build partid:     " << m.build_partid_ms << " ms (ISAM), "
              << m.build_btree_partid_ms << " ms (BTREE)\n";
    std::cout << "index build pprice:     " << m.build_pprice_ms << " ms (ISAM), "
              << m.build_btree_pprice_ms << " ms (BTREE)\n";
    const QueryTiming* triples[][3] = {
        {&m.q1_heap, &m.q1_index, &m.q1_btree},
        {&m.q2_heap, &m.q2_index, &m.q2_btree},
    };
    const char* const names[] = {
        "Q1 partid = <key>:",
        "Q2 pprice in [1.0, 10.0]:",
    };
    const bool agreements[] = {m.q1_agrees && m.q1_btree_agrees,
                               m.q2_agrees && m.q2_btree_agrees};
    for (std::size_t q = 0; q < 2; ++q) {
        const QueryTiming& heap = *triples[q][0];
        const QueryTiming& isam = *triples[q][1];
        const QueryTiming& btree = *triples[q][2];
        std::cout << names[q] << "  (key " << lookup_key << ")\n";
        std::cout << "  heap scan : " << heap.ms << " ms   rows: "
                  << heap.row_count << "  [" << heap.access_path << "]\n";
        std::cout << "  isam scan : " << isam.ms << " ms   rows: "
                  << isam.row_count << "  [" << isam.access_path << "]\n";
        std::cout << "  btree scan: " << btree.ms << " ms   rows: "
                  << btree.row_count << "  [" << btree.access_path << "]\n";
        if (isam.ms > 0.0) {
            std::cout << "  speedup   : " << heap.ms / isam.ms << "x (ISAM), ";
        } else {
            std::cout << "  speedup   : n/a (ISAM), ";
        }
        if (btree.ms > 0.0) {
            std::cout << heap.ms / btree.ms << "x (BTREE)\n";
        } else {
            std::cout << "n/a (BTREE)\n";
        }
        std::cout << "  answers identical on all three paths: "
                  << (agreements[q] ? "yes" : "NO") << "\n";
    }
}

// --- the run ------------------------------------------------------------------

QueryTiming timed(exec::Executor& executor, const sql::Statement& query, exec::ResultSet& rows) {
    const auto start = std::chrono::steady_clock::now();
    exec::StatementResult result = executor.execute(query);
    const QueryTiming timing{now_ms(start), result.rows.access_path, result.rows.rows.size()};
    rows = std::move(result.rows);
    return timing;
}

// Fills `m`; returns the row count N (the argument's, or in reuse mode
// the existing table's) and sets `lookup_key`.
std::size_t measure(exec::Executor& executor, const Config& config, Measurements& m,
                    std::size_t& lookup_key) {
    // Reuse mode: the data must already be there. The precondition runs
    // before anything else — no measurement, no generation — and N is a
    // fact of the existing table, not an argument.
    std::size_t n = config.rows;
    if (config.reuse) {
        common::with_context("--reuse: no existing parts table", [&] { executor.table(kTable); });
        n = common::with_context("--reuse: cannot read the existing parts data",
                                 [&] { return executor.count_rows(kTable, kDataFile); });
        if (n == 0) {
            fail("--reuse: the existing parts table is empty (run once without --reuse to "
                 "create it)");
        }
    } else {
        // Create the table (the requested CHAR(n) columns map to the
        // engine's only string type, VARCHAR(n)).
        run_sql(executor,
                "CREATE TABLE parts (partid INTEGER, pname VARCHAR(10), "
                "pcolor VARCHAR(10), pweight DOUBLE, pprice DOUBLE, "
                "pmaterial VARCHAR(20));");
    }
    const schema::TableSchema& schema = executor.table(kTable).schema;

    // Generate, fingerprint, and prove the width contract — both modes.
    // In reuse mode this is verification data only: nothing is written.
    const std::vector<tuple::Tuple> rows = generate_parts(n);
    m.fingerprint = fingerprint(rows);
    round_trip_check(schema, rows);

    // Default mode loads through the bulk loader: raw block writes, no
    // buffer manager (the session has the file closed at this point —
    // the bootstrap-before-open contract), timed. Reuse mode skips the
    // load entirely; its spot checks below verify the existing file
    // against the regenerated rows.
    if (!config.reuse) {
        const auto load_start = std::chrono::steady_clock::now();
        const tools::BulkLoadStats load_stats =
            tools::bulk_load(kDataFile, schema, n, [&rows](std::size_t index) { return rows[index]; });
        m.load_ms = now_ms(load_start);
        std::cout << "  bulk-loaded " << load_stats.records << " rows onto " << load_stats.pages
                  << " pages (capacity " << load_stats.per_page_capacity << ")\n";
    }
    if (const std::size_t counted = executor.count_rows(kTable, kDataFile); counted != n) {
        fail("count_rows returned " + std::to_string(counted) + ", expected " + std::to_string(n));
    }

    // Spot checks against the generated data — in reuse mode these are
    // the verification that the reused file matches the generator.
    lookup_key = n >= kLookupKey ? kLookupKey : n;
    spot_checks(executor, rows, lookup_key);

    // The two benchmark queries, parsed once: the same AST runs on both
    // access paths, so both timers measure identical work.
    const std::unique_ptr<sql::Statement> q1 =
        parse_one("SELECT * FROM parts WHERE partid = " + std::to_string(lookup_key) + ";");
    const std::unique_ptr<sql::Statement> q2 =
        parse_one("SELECT * FROM parts WHERE pprice >= 1.0 AND pprice <= 10.0;");

    // Baselines: no index exists, so both must announce heap scans.
    // Both result sets are retained for the differential check.
    exec::ResultSet q1_baseline;
    exec::ResultSet q2_baseline;
    m.q1_heap = timed(executor, *q1, q1_baseline);
    m.q2_heap = timed(executor, *q2, q2_baseline);
    if (m.q1_heap.access_path != "heap scan" || m.q2_heap.access_path != "heap scan") {
        fail("baseline announced '" + m.q1_heap.access_path + "' / '" + m.q2_heap.access_path +
             "', expected heap scan");
    }
    if (m.q1_heap.row_count != 1) {
        fail("Q1 baseline returned " + std::to_string(m.q1_heap.row_count) + " rows, expected 1");
    }
    m.q2_expected = std::count_if(rows.begin(), rows.end(), [](const tuple::Tuple& tuple) {
        const double price = std::get<double>(tuple.column(4).column_value());
        return price >= 1.0 && price <= 10.0;
    });
    if (m.q2_heap.row_count != m.q2_expected) {
        fail("Q2 baseline returned " + std::to_string(m.q2_heap.row_count) +
             " rows, generator says " + std::to_string(m.q2_expected));
    }

    // The index phase, per query in order: build an ISAM index on the
    // searched column, measure, drop it; build a BTREE on the same
    // column, measure, drop it. The drops reuse the engine's own DROP
    // INDEX, so each query's next baseline runs with no index present.
    struct IndexPass {
        const char* query_label;
        const sql::Statement* query;
        const char* column;
        const char* index_name;
        QueryTiming* isam_out;
        QueryTiming* btree_out;
        double* isam_build_ms;
        double* btree_build_ms;
        bool* isam_agrees;
        bool* btree_agrees;
        const exec::ResultSet* baseline;
    };
    const IndexPass passes[] = {
        {"Q1", q1.get(), "partid", kPartidIndex, &m.q1_index, &m.q1_btree, &m.build_partid_ms,
         &m.build_btree_partid_ms, &m.q1_agrees, &m.q1_btree_agrees, &q1_baseline},
        {"Q2", q2.get(), "pprice", kPpriceIndex, &m.q2_index, &m.q2_btree, &m.build_pprice_ms,
         &m.build_btree_pprice_ms, &m.q2_agrees, &m.q2_btree_agrees, &q2_baseline},
    };
    for (const IndexPass& pass : passes) {
        for (const bool btree : {false, true}) {
            const std::string label =
                std::string(pass.query_label) + " (" + (btree ? "BTREE" : "ISAM") + ")";
            QueryTiming& timing = btree ? *pass.btree_out : *pass.isam_out;

            const auto build_start = std::chrono::steady_clock::now();
            run_sql(executor, std::string("CREATE INDEX ") + pass.index_name + " ON " + kTable +
                                  " (" + pass.column + ")" + (btree ? " USING BTREE" : "") + ";");
            (btree ? *pass.btree_build_ms : *pass.isam_build_ms) = now_ms(build_start);

            exec::ResultSet indexed;
            timing = timed(executor, *pass.query, indexed);
            if (timing.access_path != "index scan") {
                fail(label + " announced '" + timing.access_path + "', expected index scan");
            }
            if (timing.row_count != pass.baseline->rows.size()) {
                fail(label + " row count differs from baseline");
            }
            // The index verifies cleanly and is nonempty before the drop.
            check_index_files(executor);
            bool& agrees = btree ? *pass.btree_agrees : *pass.isam_agrees;
            agrees = sets_agree(*pass.baseline, indexed);
            if (!agrees) {
                fail(std::string(pass.query_label) + ": the " + (btree ? "BTREE" : "ISAM") +
                     " index scan answered a different row set than the heap scan");
            }
            run_sql(executor, std::string("DROP INDEX ") + pass.index_name + ";");
        }
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    Config config;
    try {
        config = parse_args(argc, argv);
    } catch (const common::Error& e) {
        std::cerr << "parts_bench: " << e.what() << "\n";
        return 1;
    }
    std::cout << (config.reuse ? "parts_bench: reusing existing parts data\n"
                               : "parts_bench: loading " + std::to_string(config.rows) + " rows\n");

    bufman::BufferManager manager;
    manager.init(kPoolFrames, std::make_unique<bufman::LRUPolicy>());
    exec::Executor executor(manager);
    clean_slate(executor, config.reuse);

    int status = 0;
    try {
        Measurements measurements;
        std::size_t lookup_key = 0;
        const std::size_t rows = measure(executor, config, measurements, lookup_key);
        print_report(config, rows, measurements, lookup_key);
    } catch (const common::Error& e) {
        std::cerr << "parts_bench: " << e.what() << "\n";
        status = 1;
    }
    try {
        executor.close_all();
    } catch (const common::Error& e) {
        std::cerr << "parts_bench: " << (status == 0 ? "" : "close failed: ") << e.what() << "\n";
        status = 1;
    }
    return status;
}
