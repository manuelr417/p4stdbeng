// p4stdbeng — the CLI suite: the timing switch's end-to-end contract, driven
// through the real session binary. The config reader's refusals live in
// the config suite; this suite pins what the CLI does with the switch —
// one `time:` line per SQL statement (statements sharing one input line
// timed individually), refused statements timed after their named error,
// escape-hatch commands untimed, the line's shape, and timing false
// keeping the output free of time lines.

#include "check_support.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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
void write_conf(bool timing) {
    std::ofstream conf("conf.json");
    conf << "{ \"policy\": \"lru\", \"pool_size\": 10, \"join_pool_size\": 8, "
         << "\"hash_join\": true, \"timing\": " << (timing ? "true" : "false")
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

    chdir(original.c_str());
    std::cout << (failures == 0 ? "cli suite clean\n"
                                : "cli suite: failures above\n");
    return failures;
}
