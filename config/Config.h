#pragma once

// The session configuration reader, recouped from p1dbeng and extended:
// conf.json in the working directory names the replacement policy, sizes
// both buffer pools (the main pool and the join pool), and carries the
// hash-join availability switch. The reader is deliberately strict — the
// session must never silently run with different behavior than declared.

#include <cstddef>
#include <iosfwd>
#include <string>

namespace bufman {

struct Config {
    std::string policy;             // resolved through the PolicyFactory
    std::size_t pool_size = 0;      // main pool frames, validated to be >= 1
    std::size_t join_pool_size = 0; // join pool frames, validated to be >= 1
    bool hash_join = true;          // whether joins may run as hash joins
    bool timing = false;            // whether the CLI reports per-statement time
    bool labeled_rows = true;       // whether SELECT rows print labeled (col=value)
                                    // or as a header line plus bare values
};

// Reads a flat JSON object with six entries from `path`: "policy" (a
// quoted string), "pool_size" and "join_pool_size" (integers >= 1), and
// "hash_join", "timing", and "labeled_rows" (the bare literals true or
// false). All six entries are required; duplicate keys, trailing commas,
// trailing garbage, non-integer pool sizes, and non-boolean switch values
// are rejected.
// Unknown keys are skipped with a warning on the diagnostics stream.
// Follows the house error convention: `error` is cleared on entry and
// filled only on failure.
bool load_config(const std::string& path, Config& out,
                 std::ostream& diagnostics, std::string& error);

}
