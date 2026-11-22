#ifndef P3_COMMON_ERROR_H
#define P3_COMMON_ERROR_H

// The engine's one error type. A failure a caller or a file can genuinely
// produce — bad SQL, a missing or foreign file, an exhausted resource —
// throws common::Error with a named message at the failure site; it is
// caught once, at the system boundary (the CLI prints it, the check
// harness compares it). A check that only guards a module's own contract
// — an internal caller misusing an API — is an assert instead.

#include <stdexcept>
#include <string>
#include <utility>

namespace common {

struct Error : std::runtime_error {
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

/// Runs `body`, re-throwing any Error it raises as "<context>: <message>".
template <class F>
decltype(auto) with_context(const std::string& context, F&& body) {
    try {
        return std::forward<F>(body)();
    } catch (const Error& e) {
        throw Error(context + ": " + e.what());
    }
}

}

#endif // P3_COMMON_ERROR_H
