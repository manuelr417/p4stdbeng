// p4stdbeng — the CLI suite: the timing switch's end-to-end contract, driven
// through the real session binary. The config reader's refusals live in
// the config suite; this suite pins what the CLI does with the switches —
// one `time:` line per SQL statement (statements sharing one input line
// timed individually), refused statements timed after their named error,
// escape-hatch commands untimed, the line's shape, and timing false
// keeping the output free of time lines — and the labeled_rows switch's
// rendering contract: labeled rows when true, a header line plus
// value-only rows when false, with the report lines and the escape
// hatches unchanged.

#include "check_support.h"
#include "schema/TableCsv.h"
#include "schema/TableSchemaJson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef P4DBENG_CLI_PATH
#define P4DBENG_CLI_PATH "./p4stdbeng_cli"
#endif

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

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Writes `text` to conf.json in the current (sandbox) directory.
void write_conf(bool timing, bool labeled_rows = true) {
    std::ofstream conf("conf.json");
    conf << "{ \"policy\": \"lru\", \"pool_size\": 10, \"join_pool_size\": 8, "
         << "\"hash_join\": true, \"timing\": " << (timing ? "true" : "false")
         << ", \"labeled_rows\": " << (labeled_rows ? "true" : "false")
         << " }\n";
}

// Runs one scripted session: `script` feeds the CLI's stdin, the merged
// stdout+stderr comes back. conf.json must already be in place; tables the
// script needs must be creatable in the sandbox.
std::string run_cli(const std::string& script) {
    const std::string script_path = "session.sql";
    {
        std::ofstream out(script_path);
        out << script;
    }
    std::string output;
    const std::string command =
        P4DBENG_CLI_PATH " < " + script_path + " 2>&1";
    if (FILE* pipe = popen(command.c_str(), "r")) {
        char buffer[512];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
            output.append(buffer, read);
        }
        pclose(pipe);
    }
    std::remove(script_path.c_str());
    return output;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// The time lines: exactly `time: <seconds> seconds`, the middle a
// nonnegative number.
bool is_time_line(const std::string& line) {
    if (line.rfind("time: ", 0) != 0) return false;
    const std::size_t start = 6;
    const std::size_t end = line.find(" seconds", start);
    if (end == std::string::npos || end == start) return false;
    const std::string number = line.substr(start, end - start);
    char* parsed_end = nullptr;
    const double value = std::strtod(number.c_str(), &parsed_end);
    return parsed_end == number.c_str() + number.size() && value >= 0.0;
}

std::size_t count_time_lines(const std::string& output) {
    std::size_t count = 0;
    for (const std::string& line : split_lines(output)) {
        if (is_time_line(line)) ++count;
    }
    return count;
}

}  // namespace

int run_cli_checks() {
    // The sandbox: catalogs and data land here, never in the workspace.
    char pattern[] = "/tmp/ex13-cli-XXXXXX";
    char* made = mkdtemp(pattern);
    if (made == nullptr) {
        std::cout << "FAIL: cli sandbox could not be created\n";
        return 1;
    }
    const std::string original = [] {
        std::vector<char> buffer;
        buffer.resize(4096);
        return std::string(getcwd(buffer.data(), buffer.size()));
    }();
    chdir(made);
    if (system("mkdir -p tablecatalog data") != 0) {
        std::cout << "FAIL: cli sandbox catalog dir\n";
    }

    // --- timing true: one line per statement, in shape, last ---
    write_conf(true);
    const std::string setup =
        "CREATE TABLE person (pid INTEGER, name VARCHAR(10));\n"
        "INSERT INTO person VALUES (1, 'ann'), (2, 'bob');\n"
        "SELECT name FROM person WHERE pid = 2;\nquit\n";
    const std::string output = run_cli(setup);
    check(count_time_lines(output) == 3,
          "timing true: every SQL statement reports its own time");
    check(contains(output, "name=bob") && contains(output, "1 row(s)"),
          "timing true: the statement's results still print");
    {
        // The last statement's report ends with its time line.
        const std::vector<std::string> lines = split_lines(output);
        std::size_t last = lines.size();
        while (last > 0 && lines[last - 1].empty()) --last;
        check(last > 0 && is_time_line(lines[last - 1]),
              "timing true: the time line is the statement's last line");
    }

    // Two statements sharing one input line are timed individually.
    {
        const std::string output =
            run_cli("SELECT pid FROM person; SELECT pid FROM person;\nquit\n");
        check(count_time_lines(output) == 2,
              "timing true: statements sharing one line are timed one by one");
    }

    // A refused statement is timed — the refusal is its report.
    {
        const std::string output =
            run_cli("SELECT nosuch FROM person;\nquit\n");
        check(contains(output, "unknown column") && count_time_lines(output) == 1,
              "timing true: a refused statement's error is followed by its time");
    }

    // Escape-hatch commands are not timed.
    {
        const std::string output = run_cli("bulk person 2 data/person.tbl\nquit\n");
        check(contains(output, "appended") && count_time_lines(output) == 0,
              "timing true: escape-hatch commands print no time line");
    }

    // --- timing false: no clock, no line ---
    write_conf(false);
    {
        const std::string output = run_cli(setup);
        check(count_time_lines(output) == 0 && contains(output, "name=bob"),
              "timing false: no time lines, results unchanged");
    }

    // --- labeled_rows false: the compact rendering ---
    // (a fresh table per session: the sandbox's catalog persists across
    // run_cli calls)
    {
        write_conf(false, false);
        const std::string compact = run_cli(
            "CREATE TABLE fmt (pid INTEGER, name VARCHAR(10));\n"
            "INSERT INTO fmt VALUES (1, 'ann'), (2, 'bob');\n"
            "SELECT pid, name FROM fmt;\nquit\n");
        check(contains(compact, "pid, name\n1, ann\n2, bob\n2 row(s)"),
              "labeled_rows false: one header line, then value-only rows");
        check(!contains(compact, "pid=1") && !contains(compact, "name=ann"),
              "labeled_rows false: no per-row labels anywhere");
        check(contains(compact, "(via heap scan)"),
              "labeled_rows false: the access-path announcement stays");

        const std::string empty = run_cli(
            "CREATE TABLE fmt2 (pid INTEGER, name VARCHAR(10));\n"
            "INSERT INTO fmt2 VALUES (1, 'ann');\n"
            "SELECT pid, name FROM fmt2 WHERE pid = 99;\nquit\n");
        check(contains(empty, "pid, name\n0 row(s)"),
              "labeled_rows false: an empty result still names its columns");

        const std::string hatch = run_cli(
            "CREATE TABLE fmt3 (pid INTEGER, name VARCHAR(10));\n"
            "INSERT INTO fmt3 VALUES (1, 'ann');\n"
            "scan fmt3 data/fmt3.tbl\nquit\n");
        check(contains(hatch, "pid=1, name=ann") &&
                  contains(hatch, "scanned 1 record(s)"),
              "labeled_rows false: escape-hatch records keep their labels");

        write_conf(true, false);
        const std::string timed = run_cli(
            "CREATE TABLE fmt4 (pid INTEGER, name VARCHAR(10));\n"
            "INSERT INTO fmt4 VALUES (1, 'ann');\n"
            "SELECT pid, name FROM fmt4;\nquit\n");
        check(count_time_lines(timed) == 3 &&
                  contains(timed, "pid, name\n1, ann\n1 row(s)"),
              "labeled_rows false: timing wraps the compact report the same");
    }

    // --- the export pipe: SELECT ... | file ---
    // (a fresh table per session: the sandbox's catalog persists across
    // run_cli calls)
    {
        write_conf(true, true);
        const std::string exported = run_cli(
            "CREATE TABLE ex1 (pid INTEGER, name VARCHAR(10), score DOUBLE);\n"
            "INSERT INTO ex1 VALUES (1, 'ann', 9.5), (2, 'bob', 7.25);\n"
            "SELECT pid, name, score FROM ex1 | out1.csv;\n"
            "SELECT pid, name FROM ex1 WHERE pid = 99 | empty.csv;\nquit\n");
        check(contains(exported, "(via heap scan)\n2 row(s)\ntime:"),
              "export: the screen keeps the announcement, count, and time");
        check(!contains(exported, "pid=1") && !contains(exported, "9.5"),
              "export: the row lines do not print");
        check(contains(exported, "0 row(s)"),
              "export: an empty result reports its zero count");

        const std::string csv = [] {
            std::ifstream in("data/out1.csv");
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }();
        check(csv == "pid,name,score\n1,ann,9.5\n2,bob,7.25\n",
              "export: the file is the engine's CSV dialect, header included");
        const std::string empty_csv = [] {
            std::ifstream in("data/empty.csv");
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }();
        check(empty_csv == "pid,name\n",
              "export: an empty result writes the header only");

        // A slashed target is honored as given (not under data/).
        run_cli("SELECT pid FROM ex1 | ./direct.csv;\nquit\n");
        {
            std::ifstream in("direct.csv");
            check(in.good(), "export: a slashed path is used exactly as given");
        }

        // The refusal surface: nothing below may create a file.
        const std::string refused = run_cli(
            "CREATE TABLE ex2 (pid INTEGER);\n"
            "INSERT INTO ex2 VALUES (1);\n"
            "INSERT INTO ex2 VALUES (2) | nope1.csv;\n"
            "CREATE TABLE ex3 (pid INTEGER) | nope2.csv;\n"
            "SELECT nosuch FROM ex2 | nope3.csv;\n"
            "SELECT pid FROM ex2 | ;\n"
            "SELECT pid FROM ex2 | a.csv | b.csv;\n"
            "SELECT pid FROM ex2 | nosuchdir/x.csv;\n"
            "SELECT pid FROM ex2;\nquit\n");
        check(contains(refused, "export applies to a SELECT") &&
                  contains(refused, "export target missing after '|'") &&
                  contains(refused, "one export per statement") &&
                  contains(refused, "cannot open 'nosuchdir/x.csv'"),
              "export: every refusal names itself");
        check(contains(refused, "2 row(s)"),
              "export: the session continues after every refusal");
        bool nope_exists = false;
        for (const char* name :
             {"nope1.csv", "nope2.csv", "nope3.csv", "a.csv", "b.csv",
              "data/nope1.csv", "data/nope2.csv", "data/nope3.csv",
              "data/a.csv", "data/b.csv", "nosuchdir"}) {
            std::ifstream probe(name);
            if (probe.good()) nope_exists = true;
        }
        check(!nope_exists, "export: a refused or failed query leaves no file");

        // A pipe inside a string literal is data, not a split.
        const std::string quoted = run_cli(
            "CREATE TABLE ex4 (pid INTEGER, name VARCHAR(12));\n"
            "INSERT INTO ex4 VALUES (7, 'a|b');\n"
            "SELECT pid, name FROM ex4 WHERE name = 'a|b';\nquit\n");
        check(contains(quoted, "pid=7, name=a|b"),
              "export: a quoted pipe does not split the statement");

        // The round-trip: the written file parses back through the
        // engine's own CSV reader — header skipped by the caller, every
        // data line a valid tuple, zero skips.
        {
            const schema::TableSchema schema =
                schema::load_table_schema("tablecatalog/ex1.json");
            std::ifstream in("data/out1.csv");
            std::string line;
            std::getline(in, line);  // the header
            std::size_t parsed = 0;
            std::size_t skipped = 0;
            while (std::getline(in, line)) {
                try {
                    schema::parse_csv_row(schema, line);
                    ++parsed;
                } catch (const common::Error&) {
                    ++skipped;
                }
            }
            check(parsed == 2 && skipped == 0,
                  "export: the file round-trips through the engine's "
                  "CSV reader");
        }
    }

    chdir(original.c_str());
    std::cout << (failures == 0 ? "cli suite clean\n"
                                : "cli suite: failures above\n");
    return failures;
}
