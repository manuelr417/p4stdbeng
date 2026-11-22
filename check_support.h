#pragma once

// Shared helpers for the check suites. The engine reports a failure by
// throwing common::Error, so a check either runs a step expecting it to
// complete, or expects it to fail with a named message.

#include "common/Error.h"

#include <string>
#include <type_traits>

namespace checks {

// True when `body` completes without a common::Error — and, when it
// returns a value, that value is true.
template <class F>
bool ok(F&& body) {
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
            body();
            return true;
        } else {
            return static_cast<bool>(body());
        }
    } catch (const common::Error&) {
        return false;
    }
}

// The message `body` fails with, or "" when it completes.
template <class F>
std::string error_of(F&& body) {
    try {
        body();
    } catch (const common::Error& e) {
        return e.what();
    }
    return "";
}

// True when `body` fails with a message containing `part`.
template <class F>
bool fails_with(F&& body, const std::string& part = "") {
    try {
        body();
    } catch (const common::Error& e) {
        return std::string(e.what()).find(part) != std::string::npos;
    }
    return false;
}

}
