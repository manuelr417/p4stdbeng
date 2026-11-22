#include "exec/Evaluator.h"

#include "common/Error.h"

#include <cstdint>
#include <limits>

namespace exec {

namespace {

// p2dbeng's arithmetic contract: an Integer operation whose mathematical
// result is not representable in 32 bits is a named run-time error, never
// a wrapped value.
bool int_in_range(std::int64_t value) {
    return value >= std::numeric_limits<int32_t>::min() &&
           value <= std::numeric_limits<int32_t>::max();
}

Value column_value(const tuple::Column& column) {
    switch (column.column_type()) {
        case tuple::ColumnType::Integer:
            return Value(std::get<int>(column.column_value()));
        case tuple::ColumnType::Double:
            return Value(std::get<double>(column.column_value()));
        case tuple::ColumnType::String:
            return Value(std::get<std::string>(column.column_value()));
    }
    return Value(0);
}

bool truthy(const Value& value) {
    const int* v = std::get_if<int>(&value);
    return v != nullptr && *v != 0;
}

double numeric(const Value& value) {
    if (const double* v = std::get_if<double>(&value)) {
        return *v;
    }
    return static_cast<double>(std::get<int>(value));
}

int compare_op(sql::BinaryOp op, int cmp) {
    switch (op) {
        case sql::BinaryOp::Eq: return cmp == 0;
        case sql::BinaryOp::Ne: return cmp != 0;
        case sql::BinaryOp::Lt: return cmp < 0;
        case sql::BinaryOp::Le: return cmp <= 0;
        case sql::BinaryOp::Gt: return cmp > 0;
        case sql::BinaryOp::Ge: return cmp >= 0;
        default: return 0;
    }
}

// An Integer result checked against the 32-bit range.
Value checked_int(std::int64_t wide) {
    if (!int_in_range(wide)) {
        throw common::Error("integer overflow");
    }
    return Value(static_cast<int>(wide));
}

Value eval_unary(const BoundExpr& expr, const Value& operand) {
    if (expr.un_op == sql::UnaryOp::Not) {
        return Value(truthy(operand) ? 0 : 1);
    }
    if (const int* v = std::get_if<int>(&operand)) {
        return checked_int(-static_cast<std::int64_t>(*v));
    }
    return Value(-std::get<double>(operand));
}

// Arithmetic, with promotion per the binder's static type.
Value eval_arithmetic(const BoundExpr& expr, const Value& left, const Value& right) {
    const sql::BinaryOp op = expr.bin_op;
    if (expr.type == Type::Int) {
        const std::int64_t l = std::get<int>(left);
        const std::int64_t r = std::get<int>(right);
        switch (op) {
            case sql::BinaryOp::Plus: return checked_int(l + r);
            case sql::BinaryOp::Minus: return checked_int(l - r);
            case sql::BinaryOp::Mul: return checked_int(l * r);
            case sql::BinaryOp::Div:
            case sql::BinaryOp::Mod:
                if (r == 0) {
                    throw common::Error(op == sql::BinaryOp::Div ? "division by zero"
                                                                 : "modulo by zero");
                }
                // int/int division truncates toward zero.
                return checked_int(op == sql::BinaryOp::Div ? l / r : l % r);
            default: return Value(0);
        }
    }
    const double l = numeric(left);
    const double r = numeric(right);
    switch (op) {
        case sql::BinaryOp::Plus: return Value(l + r);
        case sql::BinaryOp::Minus: return Value(l - r);
        case sql::BinaryOp::Mul: return Value(l * r);
        case sql::BinaryOp::Div:
            if (r == 0.0) {
                throw common::Error("division by zero");
            }
            return Value(l / r);
        default: return Value(0.0);
    }
}

Value eval_node(const BoundExpr& expr, const tuple::Tuple& row) {
    switch (expr.kind) {
        case BoundExpr::Kind::Literal:
            return expr.literal;
        case BoundExpr::Kind::Column:
            return column_value(row.column(expr.column_index));
        case BoundExpr::Kind::Unary:
            return eval_unary(expr, eval_node(*expr.operand, row));
        case BoundExpr::Kind::Binary:
            break;
    }
    const sql::BinaryOp op = expr.bin_op;
    // AND/OR short-circuit: the right side runs only when it decides.
    if (op == sql::BinaryOp::And || op == sql::BinaryOp::Or) {
        const bool left = truthy(eval_node(*expr.left, row));
        if (left == (op == sql::BinaryOp::Or)) {
            return Value(left ? 1 : 0);
        }
        return Value(truthy(eval_node(*expr.right, row)) ? 1 : 0);
    }
    const Value left = eval_node(*expr.left, row);
    const Value right = eval_node(*expr.right, row);
    switch (op) {
        case sql::BinaryOp::Eq: case sql::BinaryOp::Ne:
        case sql::BinaryOp::Lt: case sql::BinaryOp::Le:
        case sql::BinaryOp::Gt: case sql::BinaryOp::Ge:
            return Value(compare_op(op, compare_values(left, right)));
        default:
            return eval_arithmetic(expr, left, right);
    }
}

}

Value evaluate(const BoundExpr& expr, const tuple::Tuple& row) {
    return eval_node(expr, row);
}

}
