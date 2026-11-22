#include "Parser.h"

#include "AstPrinter.h"
#include "Lexer.h"
#include "common/Error.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace sql {

namespace {

using StatementPtr = std::unique_ptr<Statement>;

std::string lowered(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// Case-insensitive word comparison against an identifier token — the
// house pattern for words the grammar recognizes without reserving them
// (a column named `primary` or `key` stays a legal column name).
bool word_is(const Token& token, std::string_view word) {
    return token.type == TokenType::Identifier && lowered(token.text) == word;
}

bool int64_from(const std::string& text, std::int64_t& out) {
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

bool double_from(const std::string& text, double& out) {
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

ExprPtr make_literal(LiteralValue value) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Literal;
    expr->value = std::move(value);
    return expr;
}

ExprPtr make_column_ref(std::string table, std::string column) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::ColumnRef;
    expr->table = std::move(table);
    expr->column = std::move(column);
    return expr;
}

ExprPtr make_binary(BinaryOp op, ExprPtr left, ExprPtr right) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Binary;
    expr->bin_op = op;
    expr->left = std::move(left);
    expr->right = std::move(right);
    return expr;
}

ExprPtr make_unary(UnaryOp op, ExprPtr operand) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Unary;
    expr->un_op = op;
    expr->operand = std::move(operand);
    return expr;
}

ExprPtr make_function(FunctionCode fn, ExprPtr arg) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Function;
    expr->fn = fn;
    expr->arg = std::move(arg);
    return expr;
}

// One function per grammar rule (sql.md), consuming tokens through the
// cursor below. A rule that cannot continue throws "line L: message".
class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    ParseResult run() {
        ParseResult result;
        while (peek().type != TokenType::End) {
            while (accept(TokenType::Semicolon)) {
            }
            if (peek().type == TokenType::End) break;
            result.statements.push_back(parse_statement());
            if (!accept(TokenType::Semicolon) && peek().type != TokenType::End) {
                fail("expected ';'");
            }
        }
        return result;
    }

private:
    // --- token cursor ---

    const Token& peek(std::size_t ahead = 0) const {
        const std::size_t i = pos_ + ahead;
        return i < tokens_.size() ? tokens_[i] : tokens_.back();
    }

    const Token& advance() {
        if (pos_ < tokens_.size() - 1) return tokens_[pos_++];
        return tokens_.back();  // never step past the End token
    }

    bool accept(TokenType type) {
        if (peek().type != type) return false;
        advance();
        return true;
    }

    void expect(TokenType type, const std::string& what) {
        if (!accept(type)) fail("expected " + what);
    }

    std::string expect_identifier(const std::string& what = "identifier") {
        if (peek().type != TokenType::Identifier) fail("expected " + what);
        return advance().text;
    }

    // An identifier when one follows: `AS alias` or a bare alias.
    std::string optional_alias() {
        if (accept(TokenType::As)) return expect_identifier("alias after AS");
        if (peek().type == TokenType::Identifier) return advance().text;
        return "";
    }

    [[noreturn]] void fail(const std::string& message) const { fail_at(peek().line, message); }

    [[noreturn]] static void fail_at(std::size_t line, const std::string& message) {
        throw common::Error("line " + std::to_string(line) + ": " + message);
    }

    // `item (',' item)*`
    template <class Rule>
    void comma_list(Rule rule) {
        do {
            rule();
        } while (accept(TokenType::Comma));
    }

    // --- statements ---

    StatementPtr parse_statement() {
        switch (peek().type) {
            case TokenType::Create:
                // CREATE TABLE vs CREATE INDEX: the second word decides.
                return peek(1).type == TokenType::Index ? parse_create_index()
                                                        : parse_create_table();
            case TokenType::Drop:
                return peek(1).type == TokenType::Index ? parse_drop_index()
                                                        : parse_drop_table();
            case TokenType::Insert: return parse_insert();
            case TokenType::Update: return parse_update();
            case TokenType::Delete: return parse_delete();
            case TokenType::Select: return parse_select();
            default:
                // The EXPLAIN prefix, matched as a contextual word like the
                // house matches PRIMARY KEY and REFERENCES — the lexer's
                // keyword table stays untouched, so a table named `explain`
                // still parses as a FROM name.
                if (word_is(peek(), "explain")) return parse_explain();
                fail("expected a statement");
        }
    }

    StatementPtr parse_create_table() {
        advance();  // CREATE
        expect(TokenType::Table, "TABLE");
        auto statement = std::make_unique<CreateTableStatement>();
        statement->table = expect_identifier("table name");
        expect(TokenType::LParen, "'('");
        bool seen_primary_key = false;
        comma_list([&] {
            // The table-level `PRIMARY KEY (col)` form: refused by name —
            // composite keys are out of scope (the index layer is
            // single-column), so there is no table-level form at all.
            if (word_is(peek(), "primary") && word_is(peek(1), "key")) {
                fail("the table-level PRIMARY KEY (col) form is not supported: "
                     "declare the primary key inline on its column");
            }
            // The table-level `FOREIGN KEY (col) REFERENCES ...` form:
            // refused for the same reason — references are declared
            // inline, one column at a time.
            if (word_is(peek(), "foreign") && word_is(peek(1), "key")) {
                fail("the table-level FOREIGN KEY (col) REFERENCES ... form is not "
                     "supported: declare the reference inline on its column");
            }
            ColumnDef column;
            column.name = expect_identifier("column name");
            parse_type(column);
            // The inline PRIMARY KEY suffix, matched as a pair of identifier
            // words rather than reserved keywords — a column named `key`
            // still parses as a column. At most one per table.
            if (word_is(peek(), "primary") && word_is(peek(1), "key")) {
                advance();
                advance();
                if (seen_primary_key) {
                    fail("column '" + column.name +
                         "': a table declares at most one primary key");
                }
                seen_primary_key = true;
                column.primary_key = true;
            }
            // The inline REFERENCES parent(col) suffix, the same contextual
            // matching — the referenced table and column are ordinary
            // identifiers, and a column named `references` still parses.
            // At most one per column.
            if (word_is(peek(), "references")) {
                advance();
                ColumnRef ref;
                ref.parent_table = expect_identifier("referenced table name");
                expect(TokenType::LParen, "'(' after REFERENCES");
                ref.parent_column = expect_identifier("referenced column name");
                expect(TokenType::RParen, "')' after REFERENCES");
                column.references = std::move(ref);
                if (word_is(peek(), "references")) {
                    fail("column '" + column.name +
                         "': a column declares at most one reference");
                }
            }
            statement->columns.push_back(std::move(column));
        });
        expect(TokenType::RParen, "')'");
        return statement;
    }

    // INTEGER and DOUBLE carry their catalog widths (4 and 8 bytes) so a
    // CREATE TABLE round-trips to the exact catalog JSON ex13 will write.
    void parse_type(ColumnDef& column) {
        switch (peek().type) {
            case TokenType::Integer:
            case TokenType::Int:
                column.type = DataType::Integer;
                column.size = 4;
                advance();
                return;
            case TokenType::Double:
                column.type = DataType::Double;
                column.size = 8;
                advance();
                return;
            case TokenType::Varchar: {
                advance();
                expect(TokenType::LParen, "'(' after VARCHAR");
                if (peek().type != TokenType::IntLiteral) fail("expected VARCHAR size");
                std::int64_t size = 0;
                if (!int64_from(advance().text, size) || size <= 0) {
                    fail("VARCHAR size must be a positive integer");
                }
                column.type = DataType::String;
                column.size = static_cast<std::size_t>(size);
                expect(TokenType::RParen, "')' after VARCHAR size");
                return;
            }
            default:
                fail("expected a type");
        }
    }

    StatementPtr parse_drop_table() {
        advance();  // DROP
        expect(TokenType::Table, "TABLE");
        auto statement = std::make_unique<DropTableStatement>();
        statement->table = expect_identifier("table name");
        return statement;
    }

    StatementPtr parse_create_index() {
        advance();  // CREATE
        advance();  // INDEX — the dispatch already peeked it
        auto statement = std::make_unique<CreateIndexStatement>();
        statement->index_name = expect_identifier("index name");
        expect(TokenType::On, "ON");
        statement->table = expect_identifier("table name");
        expect(TokenType::LParen, "'('");
        statement->column = expect_identifier("column name");
        expect(TokenType::RParen, "')'");
        // Optional index type: USING ISAM | BTREE (the executor refuses
        // unknown types by name; the grammar records the word).
        if (accept(TokenType::Using)) {
            statement->index_type = expect_identifier("index type");
        }
        return statement;
    }

    StatementPtr parse_drop_index() {
        advance();  // DROP
        advance();  // INDEX — the dispatch already peeked it
        auto statement = std::make_unique<DropIndexStatement>();
        statement->index_name = expect_identifier("index name");
        return statement;
    }

    StatementPtr parse_insert() {
        advance();  // INSERT
        expect(TokenType::Into, "INTO");
        auto statement = std::make_unique<InsertStatement>();
        statement->table = expect_identifier("table name");
        expect(TokenType::Values, "VALUES");
        comma_list([&] {
            expect(TokenType::LParen, "'('");
            std::vector<ExprPtr> row;
            comma_list([&] { row.push_back(parse_literal()); });
            expect(TokenType::RParen, "')'");
            statement->rows.push_back(std::move(row));
        });
        return statement;
    }

    ExprPtr parse_literal() {
        switch (peek().type) {
            case TokenType::IntLiteral:
            case TokenType::DoubleLiteral:
            case TokenType::StringLiteral:
                return parse_primary();
            default:
                fail("expected a literal value");
        }
    }

    StatementPtr parse_update() {
        advance();  // UPDATE
        auto statement = std::make_unique<UpdateStatement>();
        statement->table = expect_identifier("table name");
        expect(TokenType::Set, "SET");
        comma_list([&] {
            SetClause set;
            set.column = expect_identifier("column name");
            expect(TokenType::Eq, "'='");
            set.value = parse_expr();
            statement->sets.push_back(std::move(set));
        });
        if (accept(TokenType::Where)) statement->where = parse_expr();
        return statement;
    }

    StatementPtr parse_delete() {
        advance();  // DELETE
        expect(TokenType::From, "FROM");
        auto statement = std::make_unique<DeleteStatement>();
        statement->table = expect_identifier("table name");
        if (accept(TokenType::Where)) statement->where = parse_expr();
        return statement;
    }

    std::unique_ptr<SelectStatement> parse_select() {
        const std::size_t line = peek().line;
        advance();  // SELECT
        auto statement = std::make_unique<SelectStatement>();
        if (accept(TokenType::Star)) {
            SelectItem star;
            star.star = true;
            statement->items.push_back(std::move(star));
        } else {
            comma_list([&] {
                SelectItem item;
                item.expr = parse_expr();
                item.alias = optional_alias();
                statement->items.push_back(std::move(item));
            });
        }
        expect(TokenType::From, "FROM");
        statement->from = parse_table_ref();
        // The FROM chain: either syntax extends it, left-deep in FROM
        // order. A comma table is a cross join (null ON — its predicate
        // arrives in WHERE); JOIN requires its ON.
        for (;;) {
            if (accept(TokenType::Comma)) {
                JoinClause cross;
                cross.table = parse_table_ref();
                statement->joins.push_back(std::move(cross));
            } else if (accept(TokenType::Join)) {
                JoinClause join;
                join.table = parse_table_ref();
                expect(TokenType::On, "ON");
                join.on = parse_expr();
                statement->joins.push_back(std::move(join));
            } else {
                break;
            }
        }
        if (accept(TokenType::Where)) statement->where = parse_expr();
        if (accept(TokenType::Group)) {
            expect(TokenType::By, "BY");
            comma_list([&] { statement->group_by.push_back(parse_column_ref()); });
        }
        if (accept(TokenType::Order)) {
            expect(TokenType::By, "BY");
            comma_list([&] {
                OrderItem item;
                item.expr = parse_column_ref();
                item.descending = accept(TokenType::Desc);
                if (!item.descending) accept(TokenType::Asc);
                statement->order_by.push_back(std::move(item));
            });
        }
        if (accept(TokenType::Limit)) {
            if (peek().type != TokenType::IntLiteral) fail("expected an integer after LIMIT");
            std::int64_t limit = 0;
            if (!int64_from(advance().text, limit) || limit < 0) fail("invalid LIMIT");
            statement->limit = static_cast<std::uint64_t>(limit);
            statement->has_limit = true;
        }
        check_grouping(*statement, line);
        return statement;
    }

    // The EXPLAIN prefix: wraps a SELECT whose plan renders instead of
    // executing. Everything after EXPLAIN that is not a plain SELECT is
    // refused here, at parse time, by name — including the two spelled
    // forms a user is most likely to reach for from another engine.
    StatementPtr parse_explain() {
        advance();  // EXPLAIN
        if (word_is(peek(), "explain")) {
            fail("EXPLAIN EXPLAIN is not supported: explain a SELECT, not an EXPLAIN");
        }
        if (word_is(peek(), "analyze")) {
            fail("EXPLAIN ANALYZE is not supported: "
                 "EXPLAIN renders the plan and executes nothing");
        }
        if (peek().type != TokenType::Select) {
            fail("EXPLAIN supports SELECT statements");
        }
        auto statement = std::make_unique<ExplainStatement>();
        statement->select = parse_select();
        return statement;
    }

    TableRef parse_table_ref() {
        TableRef ref;
        ref.name = expect_identifier("table name");
        ref.alias = optional_alias();
        return ref;
    }

    // --- expressions ---

    ExprPtr parse_expr() { return parse_or(); }

    ExprPtr parse_or() {
        auto left = parse_and();
        while (accept(TokenType::Or)) {
            left = make_binary(BinaryOp::Or, std::move(left), parse_and());
        }
        return left;
    }

    ExprPtr parse_and() {
        auto left = parse_not();
        while (accept(TokenType::And)) {
            left = make_binary(BinaryOp::And, std::move(left), parse_not());
        }
        return left;
    }

    ExprPtr parse_not() {
        if (accept(TokenType::Not)) return make_unary(UnaryOp::Not, parse_not());
        return parse_comparison();
    }

    ExprPtr parse_comparison() {
        auto left = parse_additive();
        if (!is_comparison(peek().type)) return left;
        const BinaryOp op = comparison_op(advance().type);
        auto node = make_binary(op, std::move(left), parse_additive());
        // Exactly one comparison per level: 'a < b < c' stops here with a
        // named error instead of reassociating silently.
        if (is_comparison(peek().type)) fail("comparison operators cannot be chained");
        return node;
    }

    ExprPtr parse_additive() {
        auto left = parse_multiplicative();
        for (;;) {
            BinaryOp op;
            switch (peek().type) {
                case TokenType::Plus: op = BinaryOp::Plus; break;
                case TokenType::Minus: op = BinaryOp::Minus; break;
                default: return left;
            }
            advance();
            left = make_binary(op, std::move(left), parse_multiplicative());
        }
    }

    ExprPtr parse_multiplicative() {
        auto left = parse_unary();
        for (;;) {
            BinaryOp op;
            switch (peek().type) {
                case TokenType::Star: op = BinaryOp::Mul; break;
                case TokenType::Slash: op = BinaryOp::Div; break;
                case TokenType::Percent: op = BinaryOp::Mod; break;
                default: return left;
            }
            advance();
            left = make_binary(op, std::move(left), parse_unary());
        }
    }

    ExprPtr parse_unary() {
        if (accept(TokenType::Minus)) return make_unary(UnaryOp::Neg, parse_unary());
        return parse_primary();
    }

    ExprPtr parse_primary() {
        LiteralValue value;
        switch (peek().type) {
            case TokenType::IntLiteral:
                if (!int64_from(peek().text, value.i)) fail("integer literal out of range");
                value.tag = LiteralValue::Tag::Int;
                break;
            case TokenType::DoubleLiteral:
                if (!double_from(peek().text, value.d)) fail("double literal out of range");
                value.tag = LiteralValue::Tag::Double;
                break;
            case TokenType::StringLiteral:
                value.tag = LiteralValue::Tag::String;
                value.s = peek().text;
                break;
            case TokenType::LParen: {
                advance();
                auto expr = parse_expr();
                expect(TokenType::RParen, "')'");
                return expr;
            }
            case TokenType::Identifier:
                if (peek(1).type == TokenType::LParen) return parse_function_call();
                return parse_column_ref();
            default:
                fail("expected an expression");
        }
        advance();
        return make_literal(std::move(value));
    }

    ExprPtr parse_function_call() {
        const std::string name = advance().text;
        advance();  // '('
        const FunctionCode code = function_code(name);
        ExprPtr arg;
        if (accept(TokenType::Star)) {
            if (code != FunctionCode::Count) fail("'*' is only allowed in COUNT(*)");
        } else {
            arg = parse_expr();
        }
        expect(TokenType::RParen, "')'");
        return make_function(code, std::move(arg));
    }

    ExprPtr parse_column_ref() {
        if (peek().type != TokenType::Identifier) fail("expected a column");
        std::string table;
        std::string column = advance().text;
        if (accept(TokenType::Dot)) {
            table = column;
            column = expect_identifier("column name after '.'");
        }
        return make_column_ref(std::move(table), std::move(column));
    }

    FunctionCode function_code(const std::string& name) const {
        const std::string word = lowered(name);
        if (word == "count") return FunctionCode::Count;
        if (word == "sum") return FunctionCode::Sum;
        if (word == "avg") return FunctionCode::Avg;
        if (word == "min") return FunctionCode::Min;
        if (word == "max") return FunctionCode::Max;
        fail("unknown function '" + name + "'");
    }

    static bool is_comparison(TokenType type) {
        switch (type) {
            case TokenType::Eq:
            case TokenType::Ne:
            case TokenType::Lt:
            case TokenType::Le:
            case TokenType::Gt:
            case TokenType::Ge:
                return true;
            default: return false;
        }
    }

    static BinaryOp comparison_op(TokenType type) {
        switch (type) {
            case TokenType::Eq: return BinaryOp::Eq;
            case TokenType::Ne: return BinaryOp::Ne;
            case TokenType::Lt: return BinaryOp::Lt;
            case TokenType::Le: return BinaryOp::Le;
            case TokenType::Gt: return BinaryOp::Gt;
            case TokenType::Ge: return BinaryOp::Ge;
            default: return BinaryOp::Eq;  // not reached; is_comparison gates callers
        }
    }

    // --- structural semantics (catalog-free) ---

    // With GROUP BY present, every select item is either verbatim in
    // GROUP BY or an aggregate; ORDER BY items are column refs by the
    // grammar, so they must appear verbatim. Column existence waits for
    // the ex13 binder — the front-end accepts SELECT nope FROM t by design.
    static void check_grouping(const SelectStatement& statement, std::size_t line) {
        if (statement.group_by.empty()) return;
        const auto in_group_by = [&](const Expr& expr) {
            for (const ExprPtr& grouped : statement.group_by) {
                if (expr == *grouped) return true;
            }
            return false;
        };
        for (const SelectItem& item : statement.items) {
            if (!item.star && item.expr->kind != Expr::Kind::Function && !in_group_by(*item.expr)) {
                fail_at(line, "select item '" + print_expr(*item.expr) +
                                  "' must appear in GROUP BY or be an aggregate");
            }
        }
        for (const OrderItem& item : statement.order_by) {
            if (!in_group_by(*item.expr)) {
                fail_at(line, "ORDER BY item '" + print_expr(*item.expr) +
                                  "' must appear in GROUP BY");
            }
        }
    }

    const std::vector<Token>& tokens_;
    std::size_t pos_ = 0;
};

}

ParseResult parse(const std::string& query) {
    const std::vector<Token> tokens = tokenize(query);
    return Parser(tokens).run();
}

}
