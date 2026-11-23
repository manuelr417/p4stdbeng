// p4stdbeng_cli — the session CLI: a SQL REPL over the engine
// plus ISAM indexes. SQL statements accumulate across lines until a
// semicolon; the ex row-id commands (scan, read, seek, bulk) run bare
// as escape hatches, and checkindex verifies a data file's indexes.
// A SELECT may pipe its rows to a CSV file — `SELECT ... | out.csv;` —
// the engine's own dialect, header included, written instead of the
// row lines while the report stays on the screen.
// Session configuration comes from conf.json in the working directory:
// the main BufferManager (pool_size frames) and the join BufferManager
// (join_pool_size frames — the hash join's scratch pool) live for the
// whole session and are shared by every heap file and index file; the
// hash_join switch sets the executor's hash availability. This is the
// system boundary: every command's common::Error is caught in the loop
// below, printed, and the session continues; exit code 1 only if the
// config is broken or a flush fails at quit.

#include "bufpool/BufferManager.h"
#include "common/Error.h"
#include "config/Config.h"
#include "exec/Executor.h"
#include "policy/PolicyFactory.h"
#include "policy/ReplacementPolicy.h"
#include "schema/TableGenerator.h"
#include "schema/TableCsv.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "sql/Parser.h"
#include "tuple/Tuple.h"

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {

bool is_escape_command(const std::string& word) {
    return word == "scan" || word == "read" || word == "seek" ||
           word == "bulk" || word == "append" || word == "checkindex" ||
           word == "quit" || word == "help";
}

bool parse_uint64(const std::string& text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_row_id(const std::string& page_text, const std::string& slot_text,
                  heapfile::RowId& row_id) {
    std::uint64_t page = 0;
    std::uint64_t slot = 0;
    if (!parse_uint64(page_text, page) || !parse_uint64(slot_text, slot) ||
        page > 0xFFFFFFFFu || slot > 0xFFFFFFFFu) {
        return false;
    }
    row_id = heapfile::RowId{static_cast<std::uint32_t>(page),
                             static_cast<std::uint32_t>(slot)};
    return true;
}

// Escape-hatch path resolution (option a): a bare file name — no slash —
// lives in the canonical data directory under the canonical name, so it
// gains the `.tbl` extension unless it already carries it; anything
// containing a slash is used exactly as given, so absolute and
// subdirectory paths keep working.
std::string resolve_data_path(const std::string& argument) {
    if (argument.find('/') != std::string::npos) {
        return argument;
    }
    std::string name = argument;
    const std::string extension = ".tbl";
    if (name.size() < extension.size() ||
        name.compare(name.size() - extension.size(), extension.size(),
                     extension) != 0) {
        name += extension;
    }
    return "data/" + name;
}

// The CSV form of the same rule: a bare name resolves relative to data/
// but keeps its own extension (a CSV is not a table file); a path with a
// slash is used exactly as given.
std::string resolve_csv_path(const std::string& argument) {
    if (argument.find('/') != std::string::npos) {
        return argument;
    }
    return "data/" + argument;
}

std::string print_value(const exec::Value& value) {
    if (const int* v = std::get_if<int>(&value)) {
        return std::to_string(*v);
    }
    if (const double* v = std::get_if<double>(&value)) {
        char buffer[64];
        const auto written =
            std::to_chars(buffer, buffer + sizeof(buffer), *v);
        return std::string(buffer, written.ptr);
    }
    return std::get<std::string>(value);
}

[[noreturn]] void fail(const std::string& message) {
    throw common::Error(message);
}

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// The export pipe: the last `|` outside single quotes splits a
// statement's text from its CSV target (`SELECT ... | out.csv;`). Returns
// npos when the text carries no unquoted pipe; a second unquoted pipe
// refuses — one export target per statement, no guessing. A `|`
// inside a string literal is data, not a split (the '' toggle self-
// corrects for doubled quotes inside literals).
std::size_t find_export_pipe(const std::string& text) {
    std::size_t last = std::string::npos;
    std::size_t count = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\'') {
            in_string = !in_string;
        } else if (text[i] == '|' && !in_string) {
            last = i;
            ++count;
        }
    }
    if (count > 1) {
        fail("one export per statement: a second '|' is not supported");
    }
    return last;
}

// The boundary: a failure inside `body` is printed, and the session (or
// the enclosing command's loop) continues.
template <class F>
void report_errors(F&& body) {
    try {
        body();
    } catch (const common::Error& e) {
        std::cerr << e.what() << "\n";
    }
}

void print_record(const schema::TableSchema& schema, const heapfile::RowId& row_id,
                  const tuple::Tuple& tuple, const char* note = "") {
    std::cout << "(" << row_id.page_id << "," << row_id.slot << ")  " << note;
    for (std::size_t i = 0; i < schema.column_count(); ++i) {
        if (i != 0) {
            std::cout << ", ";
        }
        std::cout << schema.column_info(i).column_name() << "=";
        const tuple::Column& column = tuple.column(i);
        switch (column.column_type()) {
            case tuple::ColumnType::Integer: std::cout << std::get<int>(column.column_value()); break;
            case tuple::ColumnType::Double: std::cout << std::get<double>(column.column_value()); break;
            case tuple::ColumnType::String: std::cout << std::get<std::string>(column.column_value()); break;
        }
    }
    std::cout << "\n";
}

// --- the escape-hatch commands (ex semantics) -----------------------------

void cmd_scan(exec::Executor& executor, const std::vector<std::string>& args) {
    const schema::TableSchema& schema = executor.table(args[1]).schema;
    heapfile::HeapFileIterator it =
        executor.file_for(args[1], resolve_data_path(args[2]), false).scan();
    std::size_t count = 0;
    while (const std::optional<tuple::Tuple> tuple = it.next()) {
        print_record(schema, it.row_id(), *tuple);
        ++count;
    }
    std::cout << "scanned " << count << " record(s)\n";
}

void cmd_read(exec::Executor& executor, const std::vector<std::string>& args) {
    const schema::TableSchema& schema = executor.table(args[1]).schema;
    heapfile::HeapFile& file = executor.file_for(args[1], resolve_data_path(args[2]), false);
    heapfile::RowId row_id;
    if (!parse_row_id(args[3], args[4], row_id)) {
        fail("row id must be two nonnegative integers: page slot");
    }
    const bool hit = file.resident(row_id.page_id);
    print_record(schema, row_id, file.find(row_id), hit ? "(pool hit)  " : "");
}

void cmd_seek(exec::Executor& executor, const std::vector<std::string>& args) {
    for (std::size_t i = 3; i + 1 < args.size(); i += 2) {
        // One read per row id, each reporting its own failure: reuse
        // cmd_read through a sub-argument vector (page, slot at 3).
        report_errors([&] {
            cmd_read(executor, {args[0], args[1], resolve_data_path(args[2]), args[i], args[i + 1]});
        });
    }
}

void cmd_bulk(exec::Executor& executor, const std::vector<std::string>& args) {
    const schema::TableSchema& schema = executor.table(args[1]).schema;
    std::uint64_t count = 0;
    if (!parse_uint64(args[2], count)) {
        fail("count must be a nonnegative integer");
    }
    if (count == 0) {
        std::cout << "nothing to insert\n";
        return;
    }
    // A fresh table has no data file yet: its count is zero and the first
    // generated row numbers at 1 (the ex semantics).
    const std::string data_file = resolve_data_path(args[3]);
    const std::size_t existing = executor.count_rows(args[1], data_file, true);
    const std::vector<tuple::Tuple> generated =
        schema::generate_tuples(schema, static_cast<std::size_t>(count), 42,
                                static_cast<int>(existing) + 1);
    for (const tuple::Tuple& tuple : generated) {
        executor.insert_into(args[1], data_file, tuple);
    }
    std::cout << "appended " << generated.size() << " generated record(s)\n";
}

void cmd_checkindex(exec::Executor& executor, const std::vector<std::string>& args) {
    for (const std::string& line : executor.check_indexes(args[1], resolve_data_path(args[2])).report) {
        std::cout << line << "\n";
    }
}

// --- append: bulk-load CSV rows through the engine's insert path -----------
//
// One row per line, comma-separated. Each line is parsed, validated
// against the catalog, and inserted through insert_into — the same
// index-maintaining route SQL INSERT takes — so every declared index
// tracks the loaded rows. An invalid line is reported as
// "line N: reason" and skipped, never fatal.

void cmd_append(exec::Executor& executor, const std::vector<std::string>& args) {
    const schema::TableSchema& schema = executor.table(args[1]).schema;
    const std::string csv_path = resolve_csv_path(args[2]);
    const std::string data_file = resolve_data_path(args[1] + ".tbl");
    std::ifstream in(csv_path);
    if (!in) {
        fail("cannot open '" + csv_path + "'");
    }
    std::size_t appended = 0;
    std::size_t skipped = 0;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back(); // files written by Windows editors still load
        }
        if (line.empty()) {
            continue; // blank lines are skipped silently
        }
        try {
            const tuple::Tuple tuple = schema::parse_csv_row(schema, line);
            schema.validate(tuple);
            executor.insert_into(args[1], data_file, tuple);
            ++appended;
        } catch (const common::Error& e) {
            std::cerr << "line " << line_number << ": " << e.what() << "\n";
            ++skipped;
        }
    }
    std::cout << "appended " << appended << " row(s), skipped " << skipped << " row(s)\n";
}

void print_help() {
    std::cerr << "SQL statements end with ';'. Escape-hatch commands:\n"
              << "  scan <table> <file>\n"
              << "  read <table> <file> <page> <slot>\n"
              << "  seek <table> <file> <page> <slot>...\n"
              << "  bulk <table> <count> <file>\n"
              << "  append <table> <csv-file>\n"
              << "  checkindex <table> <file>\n"
              << "  quit\n"
              << "Tables: the names in tablecatalog/ (e.g. person, car)\n";
}

// The export writer: the engine's CSV dialect — one header line of the
// projected names, then the values only, every field joined by a bare
// ',' (the reader splits without trimming, so no spaces), no quoting (a
// comma cannot appear inside a value by the engine's own rules). The
// format is fixed regardless of the labeled_rows switch: the switch
// styles the screen; the file is data. Called only after the statement
// executed successfully, so a failed query leaves no file.
void write_result_csv(const exec::ResultSet& rows, const std::string& path) {
    std::ofstream out(path);
    if (!out) {
        fail("cannot open '" + path + "'");
    }
    for (std::size_t i = 0; i < rows.columns.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << rows.columns[i];
    }
    out << "\n";
    for (const std::vector<exec::Value>& row : rows.rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            out << print_value(row[i]);
        }
        out << "\n";
    }
}

// Executes one SQL statement's text and prints the result; the first
// error stops the line's statement batch. `labeled_rows` picks the SELECT
// row rendering: true — the labeled teaching form (`col=value, ...` per
// row); false — one header line of the projected column names, then the
// values only. The access-path announcement, the count, and every
// non-SELECT report are identical under both. `export_csv` (empty =
// none) redirects a SELECT's rows to that CSV file instead of the
// screen, keeping the report lines; a piped non-SELECT refuses.
void execute_statement(exec::Executor& executor, const std::string& text,
                       bool labeled_rows, const std::string& export_csv) {
    const std::vector<std::unique_ptr<sql::Statement>> parsed =
        sql::parse(text).statements;
    if (!export_csv.empty() && parsed.size() != 1) {
        fail("one statement per export: the text before '|' must be a "
             "single SQL statement");
    }
    for (const std::unique_ptr<sql::Statement>& statement : parsed) {
        const exec::StatementResult result = executor.execute(*statement);
        if (!result.rows.access_path.empty()) {
            std::cout << "(via " << result.rows.access_path << ")\n";
            if (!export_csv.empty()) {
                // The rows go to the file; the screen keeps the report.
                write_result_csv(result.rows, export_csv);
            } else if (labeled_rows) {
                for (const std::vector<exec::Value>& row : result.rows.rows) {
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        if (i != 0) {
                            std::cout << ", ";
                        }
                        std::cout << result.rows.columns[i] << "=" << print_value(row[i]);
                    }
                    std::cout << "\n";
                }
            } else {
                // The compact form: the projected names once as a header
                // (an empty result still names its columns), then the
                // values only, same separator.
                for (std::size_t i = 0; i < result.rows.columns.size(); ++i) {
                    if (i != 0) {
                        std::cout << ", ";
                    }
                    std::cout << result.rows.columns[i];
                }
                std::cout << "\n";
                for (const std::vector<exec::Value>& row : result.rows.rows) {
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        if (i != 0) {
                            std::cout << ", ";
                        }
                        std::cout << print_value(row[i]);
                    }
                    std::cout << "\n";
                }
            }
            std::cout << result.rows.rows.size() << " row(s)\n";
        } else {
            if (!result.message.empty()) {
                std::cout << result.message << "\n";
            }
            if (!export_csv.empty()) {
                fail("export applies to a SELECT");
            }
        }
    }
}

// Runs one escape-hatch command line.
void run_command(exec::Executor& executor, const std::vector<std::string>& args) {
    static const std::unordered_map<std::string, std::size_t> kArity = {
        {"scan", 3}, {"read", 5}, {"seek", 5}, {"bulk", 4}, {"append", 3}, {"checkindex", 3}};
    const std::string& command = args[0];
    const auto arity = kArity.find(command);
    if (command == "help") {
        print_help();
    } else if (arity == kArity.end() || args.size() < arity->second) {
        fail("usage: " + command + " (see help)");
    } else if (command == "scan") {
        cmd_scan(executor, args);
    } else if (command == "read") {
        cmd_read(executor, args);
    } else if (command == "seek") {
        cmd_seek(executor, args);
    } else if (command == "bulk") {
        cmd_bulk(executor, args);
    } else if (command == "append") {
        cmd_append(executor, args);
    } else if (command == "checkindex") {
        cmd_checkindex(executor, args);
    }
}

}

int main() {
    // Session configuration comes from conf.json in the working directory:
    // the replacement policy is resolved through the factory, both pool
    // sizes must be >= 1, and the hash_join switch is the executor's hash
    // availability. A missing or invalid file is fatal — the session would
    // otherwise silently run with different behavior than declared.
    bufman::Config config;
    std::string error;
    if (!bufman::load_config("conf.json", config, std::cerr, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::unique_ptr<bufman::ReplacementPolicy> policy =
        bufman::PolicyFactory::instance().create(config.policy, error);
    if (!policy) {
        std::cerr << error << '\n';
        return 1;
    }
    std::unique_ptr<bufman::ReplacementPolicy> join_policy =
        bufman::PolicyFactory::instance().create(config.policy, error);
    if (!join_policy) {
        std::cerr << error << '\n';
        return 1;
    }

    // Two pools for the whole session, both policed by the configured
    // policy: the main pool brings table and index pages in; the join
    // pool is the hash join's scratch pool, so join work never evicts
    // the main pool's table pages.
    bufman::BufferManager manager;
    manager.init(config.pool_size, std::move(policy));
    bufman::BufferManager join_manager;
    join_manager.init(config.join_pool_size, std::move(join_policy));

    // tmp/ is the join scratch directory: stale contents (an interrupted
    // earlier session) are wiped before the first statement, best effort —
    // leftovers are not an error, but they must not survive a restart.
    // The directory itself is recreated when a join first needs it.
    std::error_code fs_error;
    std::filesystem::remove_all("tmp", fs_error);
    if (fs_error) {
        std::cerr << "warning: could not wipe tmp/: " << fs_error.message()
                  << '\n';
    }

    exec::Executor executor(manager, &join_manager);
    executor.set_allow_hash(config.hash_join);

    const bool interactive = isatty(fileno(stdin)) != 0;
    std::string pending;  // the accumulating SQL statement text
    while (true) {
        if (interactive) {
            std::cout << (pending.empty() ? "> " : "  ... ") << std::flush;
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
            if (interactive && pending.empty()) {
                std::cout << "\n";
            }
            break;
        }

        // Escape hatches run immediately — but only when nothing is
        // being accumulated (mid-statement text is statement text).
        if (pending.empty()) {
            std::istringstream words(line);
            std::vector<std::string> args;
            for (std::string token; words >> token;) {
                args.push_back(token);
            }
            if (!args.empty() && args[0] == "quit") {
                break;
            }
            if (!args.empty() && is_escape_command(args[0])) {
                report_errors([&] { run_command(executor, args); });
                continue;
            }
        }

        // SQL: accumulate until the statement's semicolon, then execute
        // everything up to it (several statements may share one line).
        pending += line + "\n";
        for (std::size_t semicolon = pending.find(';'); semicolon != std::string::npos;
             semicolon = pending.find(';')) {
            const std::string statement_text = pending.substr(0, semicolon);
            pending.erase(0, semicolon + 1);
            // The timing switch: one clock per dispatched statement, from
            // here (the complete statement hands off) through the last
            // line of its report — result rows, a count, or a named error.
            // The user's time between continuation lines never enters the
            // clock, and the escape-hatch dispatch above stays untimed.
            const auto dispatch_start = std::chrono::steady_clock::now();
            report_errors([&] {
                // The export pipe: a SELECT may name a CSV target — the
                // last unquoted '|' splits query from target. The split
                // sits inside the timed dispatch, so its refusals are
                // timed like any other statement's.
                std::string query = statement_text;
                std::string export_target;
                const std::size_t pipe = find_export_pipe(statement_text);
                if (pipe != std::string::npos) {
                    query = statement_text.substr(0, pipe);
                    export_target = trim(statement_text.substr(pipe + 1));
                    if (export_target.empty()) {
                        fail("export target missing after '|'");
                    }
                    if (trim(query).empty()) {
                        fail("no statement before '|'");
                    }
                    // The same path rule as `append`: a bare name lives
                    // in data/, a slashed path is used as given.
                    export_target = resolve_csv_path(export_target);
                }
                execute_statement(executor, query, config.labeled_rows,
                                  export_target);
            });
            if (config.timing) {
                const std::chrono::duration<double> elapsed =
                    std::chrono::steady_clock::now() - dispatch_start;
                // Formatted through a buffer: std::fixed/setprecision on
                // cout would silently leave the stream printing every
                // later tuple value with six decimals.
                char report[64];
                std::snprintf(report, sizeof(report), "time: %.6f seconds",
                              elapsed.count());
                std::cout << report << "\n";
            }
        }
        // A statement's trailing newline must not count as an unfinished
        // statement: pure-whitespace leftovers are cleared so escape
        // commands dispatch again.
        if (pending.find_first_not_of(" \t\r\n") == std::string::npos) {
            pending.clear();
        }
    }

    // Flush every session file exactly once; exit code 1 on failure.
    try {
        executor.close_all();
    } catch (const common::Error& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
