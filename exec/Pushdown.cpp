#include "exec/Pushdown.h"

#include <set>

namespace exec {

namespace {

// The AND spine flattens: `a AND b AND c` parses left-assoc as
// ((a AND b) AND c), and every leaf of the spine is one conjunct.
void collect_conjuncts(const sql::Expr& expr, std::vector<const sql::Expr*>& out) {
    if (expr.kind == sql::Expr::Kind::Binary && expr.bin_op == sql::BinaryOp::And) {
        collect_conjuncts(*expr.left, out);
        collect_conjuncts(*expr.right, out);
        return;
    }
    out.push_back(&expr);
}

// Every table position a conjunct's column references touch. Resolution
// failures throw — the same named errors binding would raise.
std::set<std::size_t> referenced_tables(const sql::Expr& expr, const JoinedSchema& schema) {
    std::set<std::size_t> tables;
    switch (expr.kind) {
        case sql::Expr::Kind::ColumnRef:
            tables.insert(schema.resolve(expr.table, expr.column).table_position);
            break;
        case sql::Expr::Kind::Binary:
            tables.merge(referenced_tables(*expr.left, schema));
            tables.merge(referenced_tables(*expr.right, schema));
            break;
        case sql::Expr::Kind::Unary:
            tables.merge(referenced_tables(*expr.operand, schema));
            break;
        case sql::Expr::Kind::Literal:
        case sql::Expr::Kind::Function:
            break;
    }
    return tables;
}

} // namespace

PushdownSplit split_where(const sql::Expr& where, const JoinedSchema& schema) {
    std::vector<const sql::Expr*> conjuncts;
    collect_conjuncts(where, conjuncts);

    PushdownSplit split;
    for (const sql::Expr* conjunct : conjuncts) {
        const std::set<std::size_t> tables = referenced_tables(*conjunct, schema);
        if (tables.size() == 1) {
            // One table owns every column: bind with table-local
            // positions so the scan-side filter runs on raw table tuples.
            const std::size_t position = *tables.begin();
            const JoinedSchema::Table& table = schema.table(position);
            JoinedSchema one;
            one.add(table.name, table.alias, table.schema);
            split.pushed.push_back(PushedConjunct{bind_expr(*conjunct, one), position});
        } else {
            // Multi-table (or column-free): bound over combined rows.
            split.residual.push_back(bind_expr(*conjunct, schema));
        }
    }
    return split;
}

}
