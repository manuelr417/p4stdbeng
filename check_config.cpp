// p4stdbeng — the config suite: the recouped conf.json reader's acceptance
// of a valid file, its named refusals (missing keys, duplicates, trailing
// garbage and commas, bad sizes, non-boolean switch values), the unknown
// key warning, and the PolicyFactory's resolution and error naming.

#include "check_support.h"
#include "config/Config.h"
#include "policy/PolicyFactory.h"
#include "policy/ReplacementPolicy.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

using checks::fails_with;
using checks::ok;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cout << "FAIL: " << message << "\n";
        ++failures;
    }
}

// The scratch path every case writes and removes; the suite runs from the
// source directory (ctest's working directory), so the name is suite-
// specific and cleaned up on every path.
const char* kScratch = "conf_check_tmp.json";

void write_config(const std::string& text) {
    std::ofstream out(kScratch);
    out << text;
}

void remove_config() { std::remove(kScratch); }

// Loads `text` from the scratch file. Returns success; `error` and the
// diagnostics follow the house convention.
bool load_text(const std::string& text, bufman::Config& out,
               std::string& error, std::string& diagnostics) {
    write_config(text);
    std::ostringstream log;
    const bool good =
        bufman::load_config(kScratch, out, log, error);
    diagnostics = log.str();
    return good;
}

void run() {
    bufman::Config config;
    std::string error;
    std::string diagnostics;

    // The valid file: every key, whitespace variety, boolean false.
    check(load_text(R"({
        "policy": "lru",
        "pool_size": 10,
        "join_pool_size": 8,
        "hash_join": false,
        "timing": false
    })", config, error, diagnostics) && config.policy == "lru" &&
              config.pool_size == 10 && config.join_pool_size == 8 &&
              config.hash_join == false && config.timing == false &&
              error.empty(),
          "a valid file loads with every field parsed");

    // hash_join true round-trips too.
    check(load_text(R"({"policy": "lru", "pool_size": 2, "join_pool_size": 1,
                     "hash_join": true, "timing": false})",
                    config, error, diagnostics) && config.hash_join,
          "hash_join true parses");

    // timing true round-trips too.
    check(load_text(R"({"policy": "lru", "pool_size": 2, "join_pool_size": 1,
                     "hash_join": true, "timing": true})",
                    config, error, diagnostics) && config.timing,
          "timing true parses");

    // A missing file is refused by name.
    remove_config();
    check(!bufman::load_config(kScratch, config, std::cerr, error) &&
              error.find("cannot open config file") != std::string::npos,
          "a missing file is refused with a named error");

    // Each of the four keys is required.
    struct MissingCase {
        const char* json;
        const char* key;
    };
    const MissingCase missing[] = {
        {R"({"pool_size": 1, "join_pool_size": 1, "hash_join": true})",
         "policy"},
        {R"({"policy": "lru", "join_pool_size": 1, "hash_join": true})",
         "pool_size"},
        {R"({"policy": "lru", "pool_size": 1, "hash_join": true})",
         "join_pool_size"},
        {R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1})",
         "hash_join"},
        {R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
            "hash_join": true})",
         "timing"},
    };
    for (const MissingCase& c : missing) {
        check(!load_text(c.json, config, error, diagnostics) &&
                  error.find("missing '" + std::string(c.key) +
                             "' entry") != std::string::npos,
              std::string("a missing '") + c.key + "' key is refused by name");
    }

    // Duplicates are refused.
    check(!load_text(R"({"policy": "lru", "policy": "lru", "pool_size": 1,
                     "join_pool_size": 1, "hash_join": true})",
                    config, error, diagnostics) &&
              error.find("duplicate key 'policy'") != std::string::npos,
          "a duplicate 'policy' key is refused");
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "pool_size": 2,
                     "join_pool_size": 1, "hash_join": true})",
                    config, error, diagnostics) &&
              error.find("duplicate key 'pool_size'") != std::string::npos,
          "a duplicate 'pool_size' key is refused");
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
                     "hash_join": true, "hash_join": false})",
                    config, error, diagnostics) &&
              error.find("duplicate key 'hash_join'") != std::string::npos,
          "a duplicate 'hash_join' key is refused");
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
                     "hash_join": true, "timing": false, "timing": true})",
                    config, error, diagnostics) &&
              error.find("duplicate key 'timing'") != std::string::npos,
          "a duplicate 'timing' key is refused");

    // Structural garbage.
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
                     "hash_join": true} oops)",
                    config, error, diagnostics) &&
              error.find("unexpected content after the closing '}'") !=
                  std::string::npos,
          "trailing garbage is refused");
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
                     "hash_join": true,})",
                    config, error, diagnostics) &&
              error.find("trailing comma") != std::string::npos,
          "a trailing comma is refused");
    check(!load_text("{}", config, error, diagnostics) &&
              error.find("config object has no entries") !=
                  std::string::npos,
          "an empty object is refused");

    // Value shape errors name the offending key.
    check(!load_text(R"({"policy": "lru", "pool_size": 8x, "join_pool_size": 1,
                     "hash_join": true})",
                    config, error, diagnostics) &&
              error.find("'pool_size' must be a nonnegative integer") !=
                  std::string::npos,
          "a non-integer pool size is refused by name");
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1.5,
                     "hash_join": true})",
                    config, error, diagnostics) &&
              error.find("'join_pool_size' must be a nonnegative integer") !=
                  std::string::npos,
          "a fractional join pool size is refused by name");
    check(!load_text(R"({"policy": "lru", "pool_size": 0, "join_pool_size": 1,
                     "hash_join": true, "timing": false})",
                    config, error, diagnostics) &&
              error.find("'pool_size' must be >= 1") != std::string::npos,
          "pool_size 0 is refused");
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 0,
                     "hash_join": true, "timing": false})",
                    config, error, diagnostics) &&
              error.find("'join_pool_size' must be >= 1") != std::string::npos,
          "join_pool_size 0 is refused");

    // The switch accepts only the bare literals.
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
                     "hash_join": 1})",
                    config, error, diagnostics) &&
              error.find("'hash_join' must be true or false") !=
                  std::string::npos,
          "a numeric hash_join value is refused by name");
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
                     "hash_join": "true"})",
                    config, error, diagnostics) &&
              error.find("'hash_join' must be true or false") !=
                  std::string::npos,
          "a quoted hash_join value is refused by name");
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
                     "hash_join": truex})",
                    config, error, diagnostics) &&
              error.find("'hash_join' must be true or false") !=
                  std::string::npos,
          "a truncated literal hash_join value is refused by name");
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
                     "hash_join": true, "timing": 1})",
                    config, error, diagnostics) &&
              error.find("'timing' must be true or false") != std::string::npos,
          "a numeric timing value is refused by name");
    check(!load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
                     "hash_join": true, "timing": "false"})",
                    config, error, diagnostics) &&
              error.find("'timing' must be true or false") != std::string::npos,
          "a quoted timing value is refused by name");

    // Unknown keys are skipped with a warning — strings and booleans both.
    check(load_text(R"({"policy": "lru", "pool_size": 1, "join_pool_size": 1,
                     "hash_join": true, "timing": false,
                     "comment": "hello", "flag": false})",
                    config, error, diagnostics) &&
              config.pool_size == 1 &&
              diagnostics.find("ignoring unknown key 'comment'") !=
                  std::string::npos &&
              diagnostics.find("ignoring unknown key 'flag'") !=
                  std::string::npos,
          "unknown keys are skipped with a warning");

    // The factory resolves lru and names the available policies otherwise.
    std::string factory_error;
    std::unique_ptr<bufman::ReplacementPolicy> policy =
        bufman::PolicyFactory::instance().create("lru", factory_error);
    check(policy != nullptr && factory_error.empty(),
          "the factory creates the registered lru policy");
    const std::vector<std::string> known =
        bufman::PolicyFactory::instance().known();
    check(std::find(known.begin(), known.end(), "lru") != known.end(),
          "the factory lists its registered names");
    policy = bufman::PolicyFactory::instance().create("bogus", factory_error);
    check(policy == nullptr && factory_error.find("unknown replacement policy "
                                                  "'bogus'") !=
                                     std::string::npos &&
              factory_error.find("available: lru") != std::string::npos,
          "an unknown policy name is refused with the available list");

    remove_config();
}

}

int run_config_checks() {
    try {
        run();
    } catch (const std::exception& e) {
        check(false, std::string("unexpected error: ") + e.what());
    }
    std::cout << (failures == 0 ? "config suite: all checks passed."
                                : "config suite: " + std::to_string(failures) +
                                      " check(s) FAILED.")
              << "\n";
    return failures;
}
