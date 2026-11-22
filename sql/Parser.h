#pragma once

#include "Ast.h"

#include <string>

namespace sql {

struct ParseResult {
    std::vector<std::unique_ptr<Statement>> statements;
};

// Parses a ';'-separated statement list. A syntax error throws
// common::Error("line L: message"). The trailing ';' is optional and
// empty statements between semicolons are ignored.
ParseResult parse(const std::string& query);

}
