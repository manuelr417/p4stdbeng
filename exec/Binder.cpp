#include "exec/Binder.h"

#include "common/Error.h"

#include <utility>

#include <optional>

namespace exec {

namespace {

bool is_numeric(Type type) {
    return type == Type::Int || type == Type::Double;
}

bool is_comparison(sql::BinaryOp op) {
    return op == sql::BinaryOp::Eq || op == sql::BinaryOp::Ne || op == sql::BinaryOp::Lt ||
           op == sql::BinaryOp::Le || op == sql::BinaryOp::Gt || op == sql::BinaryOp::Ge;
}

[[noreturn]] void fail(const std::string& message) {
    throw common::Error(message);
}

std::string both(Type left, Type right) {
    return std::string(type_name(left)) + " and " + type_name(right);
}

BoundExprPtr bind_literal(const sql::LiteralValue& value) {
    auto node = std::make_unique<BoundExpr>();
    node->kind = BoundExpr::Kind::Literal;
    switch (value.tag) {
        case sql::LiteralValue::Tag::Int:
            node->type = Type::Int;
            node->literal = Value(static_cast<int>(value.i));
            break;
        case sql::LiteralValue::Tag::Double:
            node->type = Type::Double;
            node->literal = Value(value.d);
            break;
        case sql::LiteralValue::Tag::String:
            node->type = Type::String;
            node->literal = Value(value.s);
            break;
    }
    return node;
}

BoundExprPtr bind_column(const sql::Expr& expr, const JoinedSchema& schema) {
    const JoinedSchema::Resolved resolved = schema.resolve(expr.table, expr.column);
    auto node = std::make_unique<BoundExpr>();
    node->kind = BoundExpr::Kind::Column;
    node->column_index = resolved.index;
    switch (resolved.type) {
        case tuple::ColumnType::Integer: node->type = Type::Int; break;
        case tuple::ColumnType::Double: node->type = Type::Double; break;
        case tuple::ColumnType::String: node->type = Type::String; break;
    }
    return node;
}

BoundExprPtr bind_node(const sql::Expr& expr, const JoinedSchema& schema);

BoundExprPtr bind_unary(const sql::Expr& expr, const JoinedSchema& schema) {
    auto node = std::make_unique<BoundExpr>();
    node->kind = BoundExpr::Kind::Unary;
    node->un_op = expr.un_op;
    node->operand = bind_node(*expr.operand, schema);
    const Type operand = node->operand->type;
    if (expr.un_op == sql::UnaryOp::Neg) {
        if (!is_numeric(operand)) {
            fail(std::string("unary '-' needs a numeric operand, got ") + type_name(operand));
        }
        node->type = operand;
    } else {
        if (operand != Type::Bool) {
            fail(std::string("NOT needs a boolean operand, got ") + type_name(operand));
        }
        node->type = Type::Bool;
    }
    return node;
}

BoundExprPtr bind_binary(const sql::Expr& expr, const JoinedSchema& schema) {
    auto node = std::make_unique<BoundExpr>();
    node->kind = BoundExpr::Kind::Binary;
    node->bin_op = expr.bin_op;
    node->left = bind_node(*expr.left, schema);
    node->right = bind_node(*expr.right, schema);
    const sql::BinaryOp op = expr.bin_op;
    const Type left = node->left->type;
    const Type right = node->right->type;
    if (op == sql::BinaryOp::And || op == sql::BinaryOp::Or) {
        if (left != Type::Bool || right != Type::Bool) {
            fail(std::string(op == sql::BinaryOp::And ? "AND" : "OR") +
                 " needs boolean operands, got " + both(left, right));
        }
        node->type = Type::Bool;
    } else if (is_comparison(op)) {
        const bool both_numeric = is_numeric(left) && is_numeric(right);
        const bool both_string = left == Type::String && right == Type::String;
        if (!both_numeric && !both_string) {
            fail(std::string("cannot compare ") + type_name(left) + " with " + type_name(right));
        }
        node->type = Type::Bool;
    } else {  // Plus, Minus, Mul, Div, Mod
        if (!is_numeric(left) || !is_numeric(right)) {
            fail("arithmetic needs numeric operands, got " + both(left, right));
        }
        const bool integers = left == Type::Int && right == Type::Int;
        if (op == sql::BinaryOp::Mod && !integers) {
            fail("MOD needs integer operands, got " + both(left, right));
        }
        node->type = integers ? Type::Int : Type::Double;
    }
    return node;
}

BoundExprPtr bind_node(const sql::Expr& expr, const JoinedSchema& schema) {
    switch (expr.kind) {
        case sql::Expr::Kind::Literal: return bind_literal(expr.value);
        case sql::Expr::Kind::ColumnRef: return bind_column(expr, schema);
        case sql::Expr::Kind::Unary: return bind_unary(expr, schema);
        case sql::Expr::Kind::Binary: return bind_binary(expr, schema);
        case sql::Expr::Kind::Function: break;
    }
    fail("aggregate functions are not supported in ex13");
}

}

BoundExprPtr bind_expr(const sql::Expr& expr, const JoinedSchema& schema) {
    return bind_node(expr, schema);
}

BoundExprPtr bind_expr(const sql::Expr& expr, const schema::TableSchema& schema) {
    // One FROM table, resolvable by name: the joined machinery with a
    // single entry resolves exactly as the per-schema lookup did.
    JoinedSchema one;
    one.add(schema.table_name(), "", &schema);
    return bind_expr(expr, one);
}

}
