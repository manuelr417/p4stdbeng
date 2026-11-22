#include "exec/JoinedSchema.h"

#include "common/Error.h"

namespace exec {

void JoinedSchema::add(std::string name, std::string alias, const schema::TableSchema* schema) {
    tables_.push_back(Table{std::move(name), std::move(alias), schema});
}

std::size_t JoinedSchema::column_count() const {
    std::size_t count = 0;
    for (const Table& table : tables_) {
        count += table.schema->column_count();
    }
    return count;
}

std::size_t JoinedSchema::index_of_table(std::size_t position) const {
    std::size_t index = 0;
    for (std::size_t i = 0; i < position; ++i) {
        index += tables_[i].schema->column_count();
    }
    return index;
}

JoinedSchema::Resolved JoinedSchema::resolve(const std::string& qualifier,
                                             const std::string& column) const {
    // A qualifier selects exactly one FROM entry, by table name or alias.
    if (!qualifier.empty()) {
        for (std::size_t position = 0; position < tables_.size(); ++position) {
            const Table& table = tables_[position];
            if (table.name == qualifier || (!table.alias.empty() && table.alias == qualifier)) {
                const std::optional<std::size_t> index = table.schema->column_index(column);
                if (!index) {
                    throw common::Error("unknown column '" + column + "' in table '" +
                                        qualifier + "'");
                }
                Resolved resolved;
                resolved.table_position = position;
                resolved.index = index_of_table(position) + *index;
                resolved.type = table.schema->column_info(*index).column_type();
                return resolved;
            }
        }
        throw common::Error("unknown table or alias '" + qualifier + "' in FROM " + describe());
    }

    // A bare name must match exactly one column across all tables.
    std::vector<std::size_t> owners = tables_with(column);
    if (owners.empty()) {
        throw common::Error("unknown column '" + column + "' in FROM " + describe());
    }
    if (owners.size() > 1) {
        std::string listing;
        for (const std::size_t position : owners) {
            if (!listing.empty()) {
                listing += "' and '";
            }
            listing += tables_[position].alias.empty() ? tables_[position].name
                                                       : tables_[position].alias;
        }
        throw common::Error("ambiguous column '" + column + "' (in '" + listing + "')");
    }
    const std::size_t position = owners.front();
    const Table& table = tables_[position];
    const std::size_t index = *table.schema->column_index(column);
    Resolved resolved;
    resolved.table_position = position;
    resolved.index = index_of_table(position) + index;
    resolved.type = table.schema->column_info(index).column_type();
    return resolved;
}

std::vector<std::size_t> JoinedSchema::tables_with(const std::string& column) const {
    std::vector<std::size_t> owners;
    std::size_t position = 0;
    for (const Table& table : tables_) {
        if (table.schema->column_index(column)) {
            owners.push_back(position);
        }
        ++position;
    }
    return owners;
}

std::string JoinedSchema::describe() const {
    std::string listing;
    for (const Table& table : tables_) {
        if (!listing.empty()) {
            listing += ", ";
        }
        listing += table.alias.empty() ? table.name
                                       : table.name + " " + table.alias;
    }
    return "'" + listing + "'";
}

}
