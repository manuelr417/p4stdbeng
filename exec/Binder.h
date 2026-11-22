#pragma once

// Binder — the pass that turns a parsed sql::Expr tree into a resolved,
// statically typed BoundExpr tree against a column space: one table's
// schema, or a JoinedSchema when the FROM clause names several. Column
// names become tuple positions; every node's type is inferred here, so
// type errors (unknown columns, ambiguous names, string-vs-number
// comparison, arithmetic on strings, non-boolean logic) are named bind
// errors BEFORE any page is pinned. Aggregate function nodes are refused
// by name — the surface has no aggregates.

#include "exec/JoinedSchema.h"
#include "exec/Value.h"
#include "schema/TableSchema.h"
#include "sql/Ast.h"

#include <memory>
#include <string>

namespace exec {

struct BoundExpr {
    enum class Kind { Literal, Column, Binary, Unary };

    Kind kind = Kind::Literal;
    Type type = Type::Bool;
    Value literal;                          // Literal
    std::size_t column_index = 0;           // Column
    sql::BinaryOp bin_op = sql::BinaryOp::Eq;   // Binary
    sql::UnaryOp un_op = sql::UnaryOp::Not;     // Unary
    std::unique_ptr<BoundExpr> left;            // Binary
    std::unique_ptr<BoundExpr> right;           // Binary
    std::unique_ptr<BoundExpr> operand;         // Unary
};

using BoundExprPtr = std::unique_ptr<BoundExpr>;

// Binds `expr` against a joined column space; a bind error throws by name.
BoundExprPtr bind_expr(const sql::Expr& expr, const JoinedSchema& schema);

// The single-table entry: one FROM table, resolvable by its name (no
// alias is known at this seam). A bind error throws by name.
BoundExprPtr bind_expr(const sql::Expr& expr, const schema::TableSchema& schema);

}
