#pragma once

// JoinedSchema — the FROM clause as one column space. The tables appear
// in FROM order; each carries its table name and optional alias. A joined
// tuple is the concatenation of the tables' tuples in that order, and a
// column's absolute index is its position in the concatenation.
//
// Resolution rules (the spec's):
// - a qualified reference `t.c` resolves against the one table whose name
//   or alias is `t`; any other qualifier is a named error;
// - a bare reference `c` must match exactly one column across all tables:
//   none is an unknown-column error, more than one is an ambiguity error
//   — both raised before any page is pinned.
//
// The single-table binder entry shares this machinery: one table with no
// alias resolves exactly as the old per-schema lookup did.

#include "schema/TableSchema.h"
#include "tuple/Column.h"

#include <optional>
#include <string>
#include <vector>

namespace exec {

class JoinedSchema {
public:
    struct Table {
        std::string name;                 // the table's name
        std::string alias;                // "" when the query gives none
        const schema::TableSchema* schema; // caller-owned; outlives the JoinedSchema
    };

    JoinedSchema() = default;

    // Purpose: append one FROM entry. The order of add() calls is the
    // joined column order.
    void add(std::string name, std::string alias, const schema::TableSchema* schema);

    std::size_t table_count() const { return tables_.size(); }
    std::size_t column_count() const;

    // Purpose: one FROM entry, for callers that re-bind against a single
    // table (the pushdown classifier binds pushed conjuncts with
    // table-local positions).
    const Table& table(std::size_t position) const { return tables_[position]; }

    // Purpose: the joined-tuple position of a table's first column —
    // star projection walks each table's columns from here.
    std::size_t table_first_column(std::size_t position) const {
        return index_of_table(position);
    }

    struct Resolved {
        std::size_t index;            // position in the joined tuple
        tuple::ColumnType type;
        std::size_t table_position;   // which FROM entry owns the column
    };

    // Purpose: resolve one column reference against the joined space.
    // Returns: the absolute index, type, and owning table position.
    // Throws: common::Error by name — "unknown table or alias 't'",
    // "unknown column 'c' in table 't'", "unknown column 'c'", or
    // "ambiguous column 'c' (in 'a' and 'b')".
    Resolved resolve(const std::string& qualifier, const std::string& column) const;

    // Purpose: the FROM entries whose schema declares `column` — the
    // conjunct classifier's table set for a bare reference.
    std::vector<std::size_t> tables_with(const std::string& column) const;

    // Purpose: a readable FROM listing for error messages, e.g.
    // "'weather, cities'".
    std::string describe() const;

private:
    // The joined-tuple position of a table's first column.
    std::size_t index_of_table(std::size_t position) const;

    std::vector<Table> tables_;
};

}
