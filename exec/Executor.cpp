#include "exec/Executor.h"

#include "bufpool/DataFrame.h"
#include "common/Error.h"
#include "policy/LRUPolicy.h"
#include "exec/Binder.h"
#include "exec/Evaluator.h"
#include "exec/JoinedSchema.h"
#include "exec/Pushdown.h"
#include "operators/Filter.h"
#include "operators/HashJoin.h"
#include "operators/Limit.h"
#include "operators/NestedLoopsJoin.h"
#include "operators/IndexNestedLoopsJoin.h"
#include "operators/Partitions.h"
#include "operators/Project.h"
#include "operators/RowIdScan.h"
#include "operators/SeqScan.h"
#include "schema/TableSchemaJson.h"

#include <algorithm>
#include <bit>
#include <dirent.h>
#include <filesystem>
#include <functional>
#include <set>
#include <system_error>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

namespace exec {

// --- SELECT plan annotation --------------------------------------------------
//
// The plan text is assembled by the executing code path at its own decision
// sites: one line per operator, built exactly where that operator is built.
// A run ignores the lines; EXPLAIN renders them. Shown == run by
// construction — there is no second planner to drift from.

struct PlanLine {
    std::size_t depth = 0;   // indentation depth; the root prints at 0
    std::string text;
};

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw common::Error(message);
}

// The evaluator's two-valued logic: booleans are ints 0/1.
bool truthy_value(const Value& value) {
    const int* v = std::get_if<int>(&value);
    return v != nullptr && *v != 0;
}

// A bound expression's value as a storable column (the same conversion
// the index bounds use).
tuple::Column value_to_column(const Value& value) {
    if (const int* v = std::get_if<int>(&value)) {
        return tuple::Column(*v);
    }
    if (const double* v = std::get_if<double>(&value)) {
        return tuple::Column(*v);
    }
    return tuple::Column(std::get<std::string>(value));
}

// --- primary key identity ----------------------------------------------------

// The seen-set's identity for one primary-key value: type-tagged, full
// precision, and unambiguous — two values collide in this text exactly
// when the engine calls them the same key. The double goes in as its
// IEEE bits (no decimal-rounding collisions), with -0.0 folded onto 0.0
// so the two spellings of zero are one key.
std::string pk_key_text(const tuple::Column& column) {
    switch (column.column_type()) {
        case tuple::ColumnType::Integer:
            return "i" + std::to_string(std::get<int>(column.column_value()));
        case tuple::ColumnType::Double: {
            double value = std::get<double>(column.column_value());
            if (value == 0.0) value = 0.0;  // folds -0.0 onto 0.0
            const auto bits = std::bit_cast<std::uint64_t>(value);
            std::string text = "d";
            for (int shift = 60; shift >= 0; shift -= 4) {
                text += "0123456789abcdef"[(bits >> shift) & 0xF];
            }
            return text;
        }
        case tuple::ColumnType::String: {
            const std::string& value = std::get<std::string>(column.column_value());
            return "s" + std::to_string(value.size()) + ":" + value;
        }
    }
    return "";
}

// A primary-key value as an error message quotes it.
std::string pk_value_text(const tuple::Column& column) {
    switch (column.column_type()) {
        case tuple::ColumnType::Integer:
            return std::to_string(std::get<int>(column.column_value()));
        case tuple::ColumnType::Double:
            return std::to_string(std::get<double>(column.column_value()));
        case tuple::ColumnType::String:
            return "'" + std::get<std::string>(column.column_value()) + "'";
    }
    return "";
}

// The table names behind every catalog file in the directory — the walk
// DROP INDEX, DROP TABLE, and the parent-side foreign-key checks share.
std::vector<std::string> catalog_names_in(const std::string& directory) {
    std::vector<std::string> names;
    if (DIR* dir = opendir(directory.c_str())) {
        const std::string suffix = ".json";
        while (const dirent* item = readdir(dir)) {
            const std::string file_name = item->d_name;
            if (file_name.size() > suffix.size() && file_name.ends_with(suffix)) {
                names.push_back(file_name.substr(0, file_name.size() - suffix.size()));
            }
        }
        closedir(dir);
    }
    return names;
}


Value column_to_value(const tuple::Column& column) {
    switch (column.column_type()) {
        case tuple::ColumnType::Integer: return Value(std::get<int>(column.column_value()));
        case tuple::ColumnType::Double: return Value(std::get<double>(column.column_value()));
        case tuple::ColumnType::String: break;
    }
    return Value(std::get<std::string>(column.column_value()));
}

idx::Rid to_rid(heapfile::RowId row_id) {
    return idx::Rid{row_id.page_id, row_id.slot};
}

// Removes a file; a file that is already gone is not an error.
void remove_file(const std::string& path, const std::string& what) {
    if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
        fail("cannot erase " + what + " '" + path + "'");
    }
}

// Every (key, rid) pair of one column — the input of an index build.
std::vector<std::pair<tuple::Column, idx::Rid>> index_entries(heapfile::HeapFile& file,
                                                               std::size_t column) {
    std::vector<std::pair<tuple::Column, idx::Rid>> entries;
    heapfile::HeapFileIterator it = file.scan();
    while (const std::optional<tuple::Tuple> row = it.next()) {
        entries.emplace_back(row->column(column), to_rid(it.row_id()));
    }
    return entries;
}

// The catalog names a CREATE TABLE column's type and width by.
std::pair<tuple::ColumnType, std::size_t> column_type_of(const sql::ColumnDef& column) {
    switch (column.type) {
        case sql::DataType::Integer: return {tuple::ColumnType::Integer, 4};
        case sql::DataType::Double: return {tuple::ColumnType::Double, 8};
        case sql::DataType::String: break;
    }
    return {tuple::ColumnType::String, column.size};
}

}

// --- the index-type dispatch (design D6) -------------------------------------

Executor::SessionIndex::Kind Executor::parse_index_type(const std::string& type) {
    if (type == "ISAM") {
        return SessionIndex::Kind::Isam;
    }
    if (type == "BTREE") {
        return SessionIndex::Kind::Btree;
    }
    fail("unknown index type '" + type + "' (the engine supports ISAM and BTREE)");
}

Executor::SessionIndex Executor::make_session_index(SessionIndex::Kind kind,
                                                    const schema::TableSchema& schema,
                                                    std::size_t column_index,
                                                    const std::string& index_name,
                                                    const std::string& column_name) {
    SessionIndex index;
    index.kind = kind;
    index.column_index = column_index;
    index.index_name = index_name;
    index.column_name = column_name;
    if (kind == SessionIndex::Kind::Btree) {
        index.btree = std::make_unique<bptree::BPlusTree>(manager_, schema, column_index);
    } else {
        index.isam = std::make_unique<isam::IsamIndex>(manager_, schema, column_index);
    }
    return index;
}

void Executor::SessionIndex::open(const std::string& path) const {
    kind == Kind::Btree ? btree->open(path) : isam->open(path);
}

void Executor::SessionIndex::build(
    const std::string& path,
    const std::vector<std::pair<tuple::Column, idx::Rid>>& entries) const {
    kind == Kind::Btree ? btree->build(path, entries) : isam->build(path, entries);
}

void Executor::SessionIndex::close() const {
    kind == Kind::Btree ? btree->close() : isam->close();
}

std::vector<idx::Rid> Executor::SessionIndex::range(
    const std::optional<isam::RangeBound>& lo,
    const std::optional<isam::RangeBound>& hi) const {
    if (kind == Kind::Isam) {
        return isam->range(lo, hi);
    }
    // The two RangeBound forms are shape-identical; convert at the seam.
    const auto convert = [](const std::optional<isam::RangeBound>& bound) {
        return bound ? std::optional(bptree::RangeBound{bound->value, bound->inclusive})
                     : std::nullopt;
    };
    return btree->range(convert(lo), convert(hi));
}

void Executor::SessionIndex::insert(const tuple::Column& key, idx::Rid rid) const {
    kind == Kind::Btree ? btree->insert(key, rid) : isam->insert(key, rid);
}

void Executor::SessionIndex::erase(const tuple::Column& key, idx::Rid rid) const {
    kind == Kind::Btree ? btree->erase(key, rid) : isam->erase(key, rid);
}

idx::VerifyResult Executor::SessionIndex::verify(
    const std::function<bool(const idx::Rid&)>& occupied) const {
    return kind == Kind::Btree ? btree->verify(occupied) : isam->verify(occupied);
}

// --- session state -------------------------------------------------------------

Executor::Executor(bufman::BufferManager& manager, bufman::BufferManager* join_pool,
                   const std::string& catalog_directory)
    : manager_(manager), catalog_directory_(catalog_directory) {
    // The canonical homes of table data and catalogs: created on demand,
    // silently tolerated when they already exist (failures surface later,
    // named, at file open).
    ::mkdir("data", 0755);
    ::mkdir(catalog_directory.c_str(), 0755);
    // The join pool: the caller's, or an internal LRU pool so carried
    // call sites keep their single-pool shape.
    if (join_pool != nullptr) {
        join_pool_ = join_pool;
    } else {
        internal_join_pool_.init(kDefaultJoinPoolFrames,
                                 std::make_unique<bufman::LRUPolicy>());
        join_pool_ = &internal_join_pool_;
    }
}

std::string Executor::catalog_path(const std::string& table_name) const {
    return catalog_directory_ + "/" + table_name + ".json";
}

std::string Executor::data_path(const std::string& table_name) const {
    return "data/" + table_name + ".tbl";
}

std::string Executor::index_path(const std::string& data_file,
                                 const std::string& column_name) const {
    return data_file + "." + column_name + ".idx";
}

schema::Catalog& Executor::catalog(const std::string& name) {
    if (const auto found = catalogs_.find(name); found != catalogs_.end()) {
        return found->second;
    }
    try {
        return catalogs_.emplace(name, schema::load_catalog(catalog_path(name))).first->second;
    } catch (const common::Error& e) {
        fail("unknown table '" + name + "' (" + e.what() + ")");
    }
}

void Executor::save_catalog(const std::string& name) {
    const schema::Catalog& entry = catalogs_.at(name);
    schema::save_table_schema(catalog_path(name), entry.schema, entry.indexes, entry.primary_key,
                              entry.foreign_keys);
}

const schema::Catalog& Executor::table(const std::string& name) {
    return catalog(name);
}

Executor::SessionFile& Executor::session(const std::string& table_name, const std::string& path,
                                         bool create_when_missing) {
    if (const auto found = files_.find(path); found != files_.end()) {
        if (found->second.table_name != table_name) {
            fail("'" + path + "' is bound to table '" + found->second.table_name + "'");
        }
        return found->second;
    }
    const schema::Catalog& entry = catalog(table_name);
    const schema::TableSchema& schema = entry.schema;
    SessionFile session_file{table_name, std::make_unique<heapfile::HeapFile>(manager_), {}};
    try {
        session_file.file->open(path, schema);
    } catch (const common::Error&) {
        if (!create_when_missing) {
            throw;
        }
        session_file.file->create(path, schema);
        session_file.file->open(path, schema);
    }

    // Open every catalog-declared index for this data file. A MISSING
    // index file is rebuilt from the data file's rows (lazy rebuild); an
    // EXISTING file must open as the declared kind — a mismatch is a
    // named refusal, never a silent rebuild.
    try {
        for (const schema::TableIndex& declared : entry.indexes) {
            const std::optional<std::size_t> column = schema.column_index(declared.column);
            if (!column) {
                continue;  // the catalog is checked on write; tolerate here
            }
            SessionIndex index = make_session_index(parse_index_type(declared.type), schema,
                                                    *column, declared.name, declared.column);
            const std::string idx_path = index_path(path, declared.column);
            struct stat file_stat;
            if (::stat(idx_path.c_str(), &file_stat) == 0 && file_stat.st_size > 0) {
                index.open(idx_path);
            } else {
                // The lazy rebuild, over the data file's rows. When the
                // rebuilt index backs the table's primary key, the rebuild
                // first verifies the column's uniqueness: a data file that
                // arrived sideways (the bulk loader writes data files
                // only) cannot sneak duplicates in behind the constraint.
                // The refusal names the duplicate and leaves no index
                // file behind — build() below never runs.
                std::vector<std::pair<tuple::Column, idx::Rid>> entries =
                    index_entries(*session_file.file, *column);
                if (entry.primary_key && declared.column == *entry.primary_key) {
                    std::set<std::string> keys;
                    for (const auto& [key, rid] : entries) {
                        if (!keys.insert(pk_key_text(key)).second) {
                            fail("duplicate value " + pk_value_text(key) +
                                 " for the primary key of table '" + table_name +
                                 "' (column '" + declared.column + "')");
                        }
                    }
                }
                index.build(idx_path, entries);
            }
            session_file.indexes.push_back(std::move(index));
        }
    } catch (...) {
        close_session(session_file);  // a half-opened session must not stay registered
        throw;
    }
    return files_.emplace(path, std::move(session_file)).first->second;
}

heapfile::HeapFile& Executor::file_for(const std::string& table_name, const std::string& path,
                                       bool create_when_missing) {
    return *session(table_name, path, create_when_missing).file;
}

// The session's open index files close first: their dirty pages live in
// the shared pool as much as the data file's do.
void Executor::close_session(SessionFile& session_file) {
    for (const SessionIndex& index : session_file.indexes) {
        index.close();
    }
    session_file.file->close();
}

void Executor::close_all() {
    std::string errors;
    const auto attempt = [&](const auto& close) {
        try {
            close();
        } catch (const common::Error& e) {
            errors += (errors.empty() ? "" : "; ") + std::string(e.what());
        }
    };
    for (auto& [path, session_file] : files_) {
        for (const SessionIndex& index : session_file.indexes) {
            attempt([&] { index.close(); });
        }
        attempt([&] { session_file.file->close(); });
    }
    files_.clear();
    if (!errors.empty()) {
        fail(errors);
    }
}

// Every insert lands in the heap first, then in each index of the file
// (the mutation order the proposal pins).
heapfile::RowId Executor::insert_row(SessionFile& session_file, const tuple::Tuple& tuple) {
    // The primary key's last line of defense: every insert of every path
    // (SQL INSERT, the append/bulk escape hatch) probes the constraint's
    // index before anything lands. SQL INSERT has usually already refused
    // in its check phase (all-or-nothing with the statement's seen-set);
    // the bulk path has no such phase, so this probe is what keeps a
    // sideways load from storing a violation. The two sources of truth
    // agree by construction: the index holds the keys already stored, and
    // handle_insert's seen-set only ever holds keys this statement will
    // store.
    const schema::Catalog& entry = catalog(session_file.table_name);
    if (entry.primary_key) {
        for (const SessionIndex& index : session_file.indexes) {
            if (index.column_name == *entry.primary_key) {
                const tuple::Column& key = tuple.column(index.column_index);
                const isam::RangeBound bound{key, true};
                if (!index.range(bound, bound).empty()) {
                    fail("duplicate value " + pk_value_text(key) + " for the primary key of table '" +
                         session_file.table_name + "' (column '" + *entry.primary_key + "')");
                }
                break;
            }
        }
    }
    // The foreign keys' last line of defense: every insert of every path
    // probes each declared reference's parent before anything lands.
    // SQL INSERT has usually already refused in its check phase; the bulk
    // path has no such phase, so this probe is what keeps a sideways load
    // from storing a dangling reference. A self-referencing row provides
    // its own parent — its own primary-key value, which is not yet in the
    // index this probe reads (nothing of this row has landed).
    for (const schema::TableForeignKey& fk : entry.foreign_keys) {
        const tuple::Column& key =
            tuple.column(*entry.schema.column_index(fk.column));
        SessionFile* parent_session = &session_file;
        bool parent_has_data = true;
        if (fk.parent_table != session_file.table_name) {
            try {
                parent_session = &session(fk.parent_table, data_path(fk.parent_table), false);
            } catch (const common::Error&) {
                parent_has_data = false;  // no parent data file: nothing stored there
            }
        }
        bool found = false;
        if (parent_has_data) {
            for (const SessionIndex& index : parent_session->indexes) {
                if (index.column_name == fk.parent_column) {
                    const isam::RangeBound bound{key, true};
                    found = !index.range(bound, bound).empty();
                    break;
                }
            }
        }
        if (!found && fk.parent_table == session_file.table_name && entry.primary_key) {
            const tuple::Column& own_parent =
                tuple.column(*entry.schema.column_index(*entry.primary_key));
            found = key == own_parent;
        }
        if (!found) {
            fail("value " + pk_value_text(key) + " not found in the primary key of table '" +
                 fk.parent_table + "' (column '" + fk.parent_column + "'), referenced by " +
                 session_file.table_name + "." + fk.column);
        }
    }
    const heapfile::RowId row_id = session_file.file->insert(tuple);
    for (const SessionIndex& index : session_file.indexes) {
        index.insert(tuple.column(index.column_index), to_rid(row_id));
    }
    return row_id;
}

heapfile::RowId Executor::insert_into(const std::string& table_name, const std::string& path,
                                      const tuple::Tuple& tuple) {
    return insert_row(session(table_name, path, true), tuple);
}

std::size_t Executor::count_rows(const std::string& table_name, const std::string& path,
                                 bool create_when_missing) {
    heapfile::HeapFileIterator it = file_for(table_name, path, create_when_missing).scan();
    std::size_t count = 0;
    while (it.next()) {
        ++count;
    }
    return count;
}

Executor::IndexCheck Executor::check_indexes(const std::string& table_name,
                                             const std::string& path) {
    IndexCheck check;
    SessionFile& session_file = session(table_name, path, false);
    if (catalog(table_name).indexes.empty()) {
        check.report.push_back("table '" + table_name + "' declares no indexes");
        return check;
    }
    // A slot is occupied iff the heap can fetch its record.
    heapfile::HeapFile& file = *session_file.file;
    const auto occupied = [&file](const idx::Rid& rid) {
        try {
            file.find(heapfile::RowId{rid.page_id, rid.slot});
            return true;
        } catch (const common::Error&) {
            return false;
        }
    };
    for (const SessionIndex& index : session_file.indexes) {
        const idx::VerifyResult result = index.verify(occupied);
        std::string line = index.index_name + ": " + std::to_string(result.entry_count) + " entries";
        if (result.findings.empty()) {
            line += ", OK";
        } else {
            check.clean = false;
            line += ", " + std::to_string(result.findings.size()) + " finding(s)";
            for (const std::string& finding : result.findings) {
                line += "\n  " + finding;
            }
        }
        check.report.push_back(line);
    }
    return check;
}

// Scans for every row `where` accepts (all rows without one), keeping each
// row's id and values. UPDATE and DELETE collect first and apply after —
// never mutating while the scan is alive — so a collection failure
// changes nothing.
std::vector<Executor::Match> Executor::collect_matches(heapfile::HeapFile& file,
                                                       const BoundExpr* where) {
    std::vector<Match> matches;
    heapfile::HeapFileIterator it = file.scan();
    while (std::optional<tuple::Tuple> row = it.next()) {
        if (where == nullptr || truthy_value(evaluate(*where, *row))) {
            matches.push_back(Match{it.row_id(), std::move(*row)});
        }
    }
    return matches;
}

// --- the parent side of the foreign keys -------------------------------------

std::vector<Executor::ReferencingChild> Executor::referencing_children(
    const std::string& parent_table) {
    std::vector<ReferencingChild> children;
    // Every catalog in the directory, loaded or not — the walk DROP INDEX
    // performs, looking for references instead of index names. An
    // unreadable catalog is not this statement's error.
    for (const std::string& name : catalog_names_in(catalog_directory_)) {
        if (name == parent_table) continue;  // a self-reference is handled with exemptions
        try {
            for (const schema::TableForeignKey& fk : catalog(name).foreign_keys) {
                if (fk.parent_table == parent_table) {
                    children.push_back(ReferencingChild{name, fk.column});
                }
            }
        } catch (const common::Error&) {
        }
    }
    // The parent's own catalog may be unloaded (a session that never
    // mentioned it) — its self-reference still matters for DELETE and
    // UPDATE of the parent's own rows.
    try {
        for (const schema::TableForeignKey& fk : catalog(parent_table).foreign_keys) {
            if (fk.parent_table == parent_table) {
                children.push_back(ReferencingChild{parent_table, fk.column});
            }
        }
    } catch (const common::Error&) {
    }
    return children;
}

void Executor::check_parent_side(
    const std::string& parent_table, const std::vector<std::string>& removed_keys,
    const std::function<bool(heapfile::RowId, const tuple::Column&)>& exempt) {
    if (removed_keys.empty()) return;
    for (const ReferencingChild& child : referencing_children(parent_table)) {
        // A child with no data file holds no referencing rows. Only the
        // open is guarded — the scan's refusal must escape, not be
        // swallowed by the open's catch.
        SessionFile* child_session = nullptr;
        try {
            child_session = &session(child.table, data_path(child.table), false);
        } catch (const common::Error&) {
            continue;
        }
        const std::size_t position = *catalog(child.table).schema.column_index(child.column);
        heapfile::HeapFileIterator it = child_session->file->scan();
        while (std::optional<tuple::Tuple> row = it.next()) {
            const tuple::Column& value = row->column(position);
            if (std::find(removed_keys.begin(), removed_keys.end(),
                          pk_key_text(value)) == removed_keys.end()) {
                continue;
            }
            if (child.table == parent_table && exempt(it.row_id(), value)) {
                continue;  // this statement ends that reference itself
            }
            fail("table '" + child.table + "' (column '" + child.column +
                 "') still references key " + pk_value_text(value) + " of table '" +
                 parent_table + "'");
        }
    }
}


// --- dispatch ----------------------------------------------------------------

StatementResult Executor::execute(const sql::Statement& statement) {
    using Kind = sql::Statement::Kind;
    switch (statement.kind) {
        case Kind::CreateTable:
            return handle_create_table(static_cast<const sql::CreateTableStatement&>(statement));
        case Kind::DropTable:
            return handle_drop_table(static_cast<const sql::DropTableStatement&>(statement));
        case Kind::CreateIndex:
            return handle_create_index(static_cast<const sql::CreateIndexStatement&>(statement));
        case Kind::DropIndex:
            return handle_drop_index(static_cast<const sql::DropIndexStatement&>(statement));
        case Kind::Insert:
            return handle_insert(static_cast<const sql::InsertStatement&>(statement));
        case Kind::Select:
            return handle_select(static_cast<const sql::SelectStatement&>(statement));
        case Kind::Explain:
            return handle_explain(static_cast<const sql::ExplainStatement&>(statement));
        case Kind::Update:
            return handle_update(static_cast<const sql::UpdateStatement&>(statement));
        case Kind::Delete:
            return handle_delete(static_cast<const sql::DeleteStatement&>(statement));
    }
    fail("unknown statement kind");
}

// --- DDL ---------------------------------------------------------------------

StatementResult Executor::handle_create_table(const sql::CreateTableStatement& statement) {
    bool exists = true;
    try {
        catalog(statement.table);
    } catch (const common::Error&) {
        exists = false;
    }
    if (exists) {
        fail("table '" + statement.table + "' already exists");
    }
    if (statement.columns.empty()) {
        fail("table '" + statement.table + "' declares no columns");
    }
    // add_column refuses duplicate and empty names.
    schema::Catalog entry{schema::TableSchema(statement.table), {}};
    for (const sql::ColumnDef& column : statement.columns) {
        const auto [type, size] = column_type_of(column);
        entry.schema.add_column(column.name, type, size);
    }
    // The declared primary key: record the constraint, and let it own a
    // backing index declared exactly as CREATE INDEX would (ISAM — the
    // default type — named by the ordinary convention). The parser
    // already refuses a second PRIMARY KEY; this loop is the executor's
    // own guard, because a silently-dropped constraint would be a
    // stored-data lie. The index file itself is materialized by the
    // ordinary lazy open: first use builds it over the (empty) data file.
    for (const sql::ColumnDef& column : statement.columns) {
        if (!column.primary_key) continue;
        if (entry.primary_key) {
            fail("table '" + statement.table +
                 "' declares more than one primary key");
        }
        entry.primary_key = column.name;
        entry.indexes.push_back(
            schema::TableIndex{statement.table + "_" + column.name, column.name, "ISAM"});
    }
    // The declared foreign keys, validated before anything is written:
    // each reference names an existing parent table (parent before child
    // — no forward references, which is what keeps the reference graph
    // acyclic and RESTRICT simple) and that table's declared primary key.
    // A self-reference is validated against the primary key this same
    // statement declares. The referenced column's type must match the
    // referencing column's — the probe compares encoded keys, and a
    // mismatched pair would only ever refuse, misleadingly.
    for (const sql::ColumnDef& column : statement.columns) {
        if (!column.references) continue;
        const std::string& parent_table = column.references->parent_table;
        const std::string& parent_column = column.references->parent_column;
        std::optional<std::string> parent_primary_key;
        if (parent_table == statement.table) {
            parent_primary_key = entry.primary_key;  // validated just above
        } else {
            try {
                parent_primary_key = catalog(parent_table).primary_key;
            } catch (const common::Error&) {
                fail("column '" + column.name + "' references unknown table '" +
                     parent_table + "' (parent tables are created first)");
            }
        }
        if (!parent_primary_key || *parent_primary_key != parent_column) {
            fail("column '" + column.name + "' references '" + parent_table + "(" +
                 parent_column + ")', which is not its primary key");
        }
        const auto [referencing_type, referencing_size] = column_type_of(column);
        const schema::TableSchema& parent_schema = parent_table == statement.table
                                                       ? entry.schema
                                                       : catalog(parent_table).schema;
        const std::size_t parent_position = *parent_schema.column_index(parent_column);
        const schema::ColumnSchema& referenced = parent_schema.column_info(parent_position);
        if (referenced.column_type() != referencing_type) {
            fail("column '" + column.name + "' references '" + parent_table + "(" +
                 parent_column + ")' with a different type");
        }
        entry.foreign_keys.push_back(
            schema::TableForeignKey{column.name, parent_table, parent_column});
    }
    // The catalog writer is the DDL seam: one atomic install.
    schema::save_table_schema(catalog_path(statement.table), entry.schema, entry.indexes,
                              entry.primary_key, entry.foreign_keys);
    catalogs_.emplace(statement.table, std::move(entry));
    return StatementResult{"table '" + statement.table + "' created", {}};
}

// Close the session's own handles first — the data file and every index
// file the session opened — then erase from disk (erase_file refuses
// while a file is open through the manager). Missing files are not
// errors — a table with no inserts has no data file.
StatementResult Executor::handle_drop_table(const sql::DropTableStatement& statement) {
    // The parent side: a referenced table cannot be dropped while
    // another catalog's foreign key names it — the constraint would
    // silently die with the table. A self-reference dies with it, which
    // is why the table's own catalog is skipped here. The walk is the
    // one DROP INDEX performs.
    for (const std::string& name : catalog_names_in(catalog_directory_)) {
        if (name == statement.table) continue;
        // Only the load is guarded — an unreadable catalog is not this
        // statement's error, but a found reference must refuse, and the
        // refusal must escape (a fail inside this try would be swallowed
        // by its own catch).
        std::optional<schema::Catalog> other;
        try {
            other = catalog(name);
        } catch (const common::Error&) {
            continue;
        }
        for (const schema::TableForeignKey& fk : other->foreign_keys) {
            if (fk.parent_table == statement.table) {
                fail("table '" + statement.table + "' is referenced by '" + name +
                     "' (column '" + fk.column + "') and cannot be dropped");
            }
        }
    }
    const std::vector<schema::TableIndex> indexes = catalog(statement.table).indexes;
    const std::string data_file = data_path(statement.table);
    if (const auto found = files_.find(data_file); found != files_.end()) {
        close_session(found->second);
        files_.erase(found);
    }
    remove_file(data_file, "data file");
    for (const schema::TableIndex& declared : indexes) {
        remove_file(index_path(data_file, declared.column), "index file");
    }
    remove_file(catalog_path(statement.table), "catalog");
    catalogs_.erase(statement.table);
    return StatementResult{"table '" + statement.table + "' dropped", {}};
}

StatementResult Executor::handle_create_index(const sql::CreateIndexStatement& statement) {
    schema::Catalog& entry = catalog(statement.table);
    const std::optional<std::size_t> column = entry.schema.column_index(statement.column);
    if (!column) {
        fail("unknown column '" + statement.column + "' in table '" + statement.table + "'");
    }
    for (const schema::TableIndex& declared : entry.indexes) {
        if (declared.column == statement.column) {
            fail("column '" + statement.column + "' already carries an index (one index per column)");
        }
        if (declared.name == statement.index_name) {
            fail("index '" + statement.index_name + "' already exists");
        }
    }
    // The declared type: the USING word (uppercased) or the ISAM default.
    std::string wanted_type = statement.index_type.empty() ? "ISAM" : statement.index_type;
    std::transform(wanted_type.begin(), wanted_type.end(), wanted_type.begin(), ::toupper);
    const SessionIndex::Kind kind = parse_index_type(wanted_type);

    // Record first (the catalog write is all-or-nothing), then build.
    entry.indexes.push_back(schema::TableIndex{statement.index_name, statement.column, wanted_type});
    try {
        save_catalog(statement.table);
    } catch (...) {
        entry.indexes.pop_back();
        throw;
    }
    // If the data file is already open in this session, build the index
    // into it now (otherwise the session opens it lazily on next use). A
    // failed build takes the declaration back out.
    const std::string data_file = data_path(statement.table);
    if (const auto found = files_.find(data_file); found != files_.end()) {
        SessionIndex index = make_session_index(kind, entry.schema, *column,
                                                statement.index_name, statement.column);
        try {
            index.build(index_path(data_file, statement.column),
                        index_entries(*found->second.file, *column));
        } catch (...) {
            entry.indexes.pop_back();
            save_catalog(statement.table);
            throw;
        }
        found->second.indexes.push_back(std::move(index));
    }
    return StatementResult{"index '" + statement.index_name + "' created on '" +
                               statement.table + "(" + statement.column + ")'",
                           {}};
}

// The index name is unique per table: search the loaded catalogs, then
// every catalog file in the directory, so a DROP works even in a session
// that never mentioned the table before.
StatementResult Executor::handle_drop_index(const sql::DropIndexStatement& statement) {
    const auto position_in = [&](const schema::Catalog& entry) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < entry.indexes.size(); ++i) {
            if (entry.indexes[i].name == statement.index_name) {
                return i;
            }
        }
        return std::nullopt;
    };
    for (auto& [name, entry] : catalogs_) {
        if (const auto position = position_in(entry)) {
            return drop_index_in(name, entry, *position);
        }
    }
    std::vector<std::string> names = catalog_names_in(catalog_directory_);
    for (const std::string& name : names) {
        schema::Catalog* entry = nullptr;
        try {
            entry = &catalog(name);
        } catch (const common::Error&) {
            continue;  // an unreadable catalog is not a DROP error
        }
        if (const auto position = position_in(*entry)) {
            return drop_index_in(name, *entry, *position);
        }
    }
    fail("unknown index '" + statement.index_name + "'");
}

// Close the session's handles so the index file is flushed and unpinned
// before erasure: the index itself, then the data file (the whole session
// file goes — its indexes re-open lazily).
StatementResult Executor::drop_index_in(const std::string& table_name, schema::Catalog& entry,
                                        std::size_t position) {
    const std::string index_name = entry.indexes[position].name;
    // The constraint owns its index: dropping it would silently drop the
    // primary key with it, so it is refused — DROP TABLE is the only way
    // the index (and the whole table) goes away.
    if (entry.primary_key && entry.indexes[position].column == *entry.primary_key) {
        fail("index '" + index_name + "' backs the primary key of table '" + table_name +
             "' (column '" + *entry.primary_key + "') and cannot be dropped");
    }
    const std::string data_file = data_path(table_name);
    if (const auto found = files_.find(data_file); found != files_.end()) {
        for (const SessionIndex& index : found->second.indexes) {
            if (index.index_name == index_name) {
                index.close();
            }
        }
        found->second.file->close();
        files_.erase(found);
    }
    remove_file(index_path(data_file, entry.indexes[position].column), "index file");
    entry.indexes.erase(entry.indexes.begin() + static_cast<long>(position));
    save_catalog(table_name);
    return StatementResult{"index '" + index_name + "' dropped", {}};
}

// --- INSERT ------------------------------------------------------------------

StatementResult Executor::handle_insert(const sql::InsertStatement& statement) {
    const schema::Catalog& entry = catalog(statement.table);
    const schema::TableSchema& schema = entry.schema;

    // Validate every row before any page is pinned: an INSERT is
    // all-or-nothing per statement.
    std::vector<tuple::Tuple> tuples;
    for (std::size_t r = 0; r < statement.rows.size(); ++r) {
        const std::vector<sql::ExprPtr>& row = statement.rows[r];
        const std::string row_label = "row " + std::to_string(r + 1);
        if (row.size() != schema.column_count()) {
            fail(row_label + " has " + std::to_string(row.size()) + " value(s), table '" +
                 statement.table + "' has " + std::to_string(schema.column_count()) + " columns");
        }
        tuple::Tuple tuple;
        for (std::size_t c = 0; c < row.size(); ++c) {
            const schema::ColumnSchema& meta = schema.column_info(c);
            const std::string cell_label = row_label + ", column '" + meta.column_name() + "': ";
            if (row[c]->kind != sql::Expr::Kind::Literal) {
                fail(cell_label + "only literal values are supported");
            }
            const sql::LiteralValue& literal = row[c]->value;
            using Tag = sql::LiteralValue::Tag;
            switch (meta.column_type()) {
                case tuple::ColumnType::Integer:
                    if (literal.tag != Tag::Int) fail(cell_label + "value must be an integer");
                    tuple.add(static_cast<int>(literal.i));
                    break;
                case tuple::ColumnType::Double:
                    if (literal.tag == Tag::String) fail(cell_label + "value must be a number");
                    // An integer literal widens.
                    tuple.add(literal.tag == Tag::Double ? literal.d : static_cast<double>(literal.i));
                    break;
                case tuple::ColumnType::String:
                    if (literal.tag != Tag::String) fail(cell_label + "value must be a string");
                    tuple.add(literal.s);
                    break;
            }
        }
        schema.validate(tuple);  // width/NUL rules surface here, named
        tuples.push_back(std::move(tuple));
    }

    // The primary key: check everything, then mutate — the house's
    // two-phase discipline taken one constraint further. Every row's key
    // probes the constraint's index (the keys already stored) AND the
    // statement's seen-set (the keys this statement itself will store):
    // the index alone would pass rows 1 and 3 of one statement holding
    // the same new key, and both would land. Any duplicate refuses the
    // whole statement here, before any row is stored.
    if (entry.primary_key) {
        const std::size_t pk_position = *schema.column_index(*entry.primary_key);
        SessionFile& pk_session = session(statement.table, data_path(statement.table), true);
        const SessionIndex* pk_index = nullptr;
        for (const SessionIndex& index : pk_session.indexes) {
            if (index.column_name == *entry.primary_key) pk_index = &index;
        }
        if (pk_index == nullptr) {
            fail("table '" + statement.table +
                 "' declares a primary key but carries no index over its column");
        }
        std::set<std::string> seen;
        for (std::size_t r = 0; r < tuples.size(); ++r) {
            const tuple::Column& key = tuples[r].column(pk_position);
            const isam::RangeBound bound{key, true};
            if (!pk_index->range(bound, bound).empty() || !seen.insert(pk_key_text(key)).second) {
                fail("row " + std::to_string(r + 1) + ": duplicate value " + pk_value_text(key) +
                     " for the primary key of table '" + statement.table +
                     "' (column '" + *entry.primary_key + "')");
            }
        }
    }

    // The foreign keys: the same check phase, one table over. Each row's
    // referencing value must exist in the parent's primary-key index —
    // the keys already stored there — or in the statement's own
    // parent-provided set: the keys this statement inserts into the
    // referenced column of a self-referenced parent (a manager and their
    // report can arrive in one INSERT, whichever row comes first, because
    // nothing lands until every check passes). Any miss refuses the whole
    // statement here, before any row is stored.
    if (!entry.foreign_keys.empty()) {
        struct ResolvedKey {
            std::size_t child_position;      // the referencing column, here
            const SessionIndex* parent_index = nullptr;  // null: no stored parent keys
            bool self_referencing = false;
        };
        std::vector<ResolvedKey> resolved;
        resolved.reserve(entry.foreign_keys.size());
        for (const schema::TableForeignKey& fk : entry.foreign_keys) {
            ResolvedKey r;
            r.child_position = *schema.column_index(fk.column);
            r.self_referencing = fk.parent_table == statement.table;
            if (r.self_referencing) {
                // The parent is this table: its PK index is the child
                // session's own (the session opens now — the inserts need
                // it anyway).
                SessionFile& fk_session =
                    session(statement.table, data_path(statement.table), true);
                for (const SessionIndex& index : fk_session.indexes) {
                    if (index.column_name == fk.parent_column) r.parent_index = &index;
                }
            } else {
                // A cross-table parent: read-only probe. A parent with no
                // data file yet has stored no keys.
                try {
                    SessionFile& parent =
                        session(fk.parent_table, data_path(fk.parent_table), false);
                    for (const SessionIndex& index : parent.indexes) {
                        if (index.column_name == fk.parent_column) r.parent_index = &index;
                    }
                } catch (const common::Error&) {
                }
            }
            resolved.push_back(r);
        }
        // The parent-provided set: for a self-reference, the statement's
        // own primary-key values.
        std::set<std::string> provided;
        if (entry.primary_key) {
            const std::size_t pk_position = *schema.column_index(*entry.primary_key);
            for (const ResolvedKey& r : resolved) {
                if (!r.self_referencing) continue;
                for (const tuple::Tuple& tuple : tuples) {
                    provided.insert(pk_key_text(tuple.column(pk_position)));
                }
            }
        }
        for (std::size_t r = 0; r < tuples.size(); ++r) {
            for (std::size_t f = 0; f < entry.foreign_keys.size(); ++f) {
                const schema::TableForeignKey& fk = entry.foreign_keys[f];
                const tuple::Column& key = tuples[r].column(resolved[f].child_position);
                const isam::RangeBound bound{key, true};
                const bool found =
                    provided.count(pk_key_text(key)) > 0 ||
                    (resolved[f].parent_index != nullptr &&
                     !resolved[f].parent_index->range(bound, bound).empty());
                if (!found) {
                    fail("row " + std::to_string(r + 1) + ": value " + pk_value_text(key) +
                         " not found in the primary key of table '" + fk.parent_table +
                         "' (column '" + fk.parent_column + "'), referenced by " +
                         statement.table + "." + fk.column);
                }
            }
        }
    }

    SessionFile& session_file = session(statement.table, data_path(statement.table), true);
    for (const tuple::Tuple& tuple : tuples) {
        insert_row(session_file, tuple);
    }
    return StatementResult{"inserted " + std::to_string(tuples.size()) + " row(s)", {}};
}

// --- SELECT ------------------------------------------------------------------
//
// Every SELECT builds an operator tree (the Volcano pipeline) and drives
// it: single-table queries are Scan → Filter → Project (→ Limit), with
// the planner's index bounds turning the scan into a RowIdScan; join
// chains fold left-deep into NestedLoopsJoin nodes, each ON bound over
// its prefix, single-table WHERE conjuncts pushed into scan-side
// filters, the rest filtered above the join. Correctness never depends
// on the shape: the oracle holds every path to identical answers.

namespace {

// Conjunction as a predicate: every conjunct must hold over the row.
// The conjuncts live in a shared vector — std::function needs a copyable
// callable, and bound expressions are move-only.
std::function<bool(const tuple::Tuple&)> all_of_conjuncts(std::vector<BoundExprPtr> conjuncts) {
    const std::shared_ptr<const std::vector<BoundExprPtr>> shared =
        std::make_shared<std::vector<BoundExprPtr>>(std::move(conjuncts));
    return [shared](const tuple::Tuple& row) {
        return std::all_of(shared->begin(), shared->end(),
                           [&](const BoundExprPtr& conjunct) {
                               return truthy_value(evaluate(*conjunct, row));
                           });
    };
}

// Drives one pipeline to exhaustion. The tree's Project node has already
// narrowed each row to the projected columns in order, so the driver
// copies whole rows.
void run_pipeline(ops::Operator& root, std::vector<std::vector<Value>>& out) {
    root.open();
    while (const std::optional<tuple::Tuple> row = root.next()) {
        std::vector<Value> values;
        values.reserve(row->column_count());
        for (std::size_t index = 0; index < row->column_count(); ++index) {
            values.push_back(column_to_value(row->column(index)));
        }
        out.push_back(std::move(values));
    }
    root.close();
}

// AND-conjunct flattening for the index-join selection rule: the same
// walk the pushdown classifier does, kept here because the rule reads
// the join's own predicate at the AST level.
void flatten_and(const sql::Expr& expr, std::vector<const sql::Expr*>& out) {
    if (expr.kind == sql::Expr::Kind::Binary && expr.bin_op == sql::BinaryOp::And) {
        flatten_and(*expr.left, out);
        flatten_and(*expr.right, out);
    } else {
        out.push_back(&expr);
    }
}

// The index-join selection rule's shape test (the lecture's rule that
// the index's search key must match the join predicate): one AND-conjunct
// of the shape prefix_col = inner_col, either orientation, with the inner
// column resolving to FROM entry `inner` and the prefix column to an
// entry before it. Resolution cannot fail here — every scanned conjunct
// was already bound (the ON over its prefix, the WHERE over the joined
// space), so ambiguous or unknown names were refused before the fold.
struct EquiJoinKey {
    std::size_t key_position;  // prefix column's joined position — the outer child's layout
    std::size_t inner_column;  // the equated column, table-local within entry `inner`
};

std::optional<EquiJoinKey> equi_join_key(const sql::Expr& source, const JoinedSchema& joined,
                                         std::size_t inner) {
    std::vector<const sql::Expr*> conjuncts;
    flatten_and(source, conjuncts);
    for (const sql::Expr* conjunct : conjuncts) {
        if (conjunct->kind != sql::Expr::Kind::Binary || conjunct->bin_op != sql::BinaryOp::Eq ||
            conjunct->left->kind != sql::Expr::Kind::ColumnRef ||
            conjunct->right->kind != sql::Expr::Kind::ColumnRef) {
            continue;  // ranges, literals, arithmetic: no probe shape
        }
        const JoinedSchema::Resolved lhs = joined.resolve(conjunct->left->table, conjunct->left->column);
        const JoinedSchema::Resolved rhs = joined.resolve(conjunct->right->table, conjunct->right->column);
        if (lhs.table_position == inner && rhs.table_position < inner) {
            return EquiJoinKey{rhs.index, lhs.index - joined.table_first_column(inner)};
        }
        if (rhs.table_position == inner && lhs.table_position < inner) {
            return EquiJoinKey{lhs.index, rhs.index - joined.table_first_column(inner)};
        }
    }
    return std::nullopt;
}

// The per-node join algorithm — one rule per line, the lecture's order of
// preference: the index join whenever an inner index serves the equality
// shape (its announcement is pinned by the sql-joins spec, with the hash
// switch on or off); otherwise the hash join when the session enables it
// and the memory gate passes; otherwise the nested loops join.
enum class JoinAlgorithm { Index, Hash, NestedLoops };

JoinAlgorithm select_join_algorithm(bool index_applies, bool hash_enabled,
                                    bool gate_passes) {
    if (index_applies) return JoinAlgorithm::Index;
    if (hash_enabled && gate_passes) return JoinAlgorithm::Hash;
    return JoinAlgorithm::NestedLoops;
}

// The hash join's memory gate — slide 29's sizing rule as a heuristic:
// the build side's estimated bytes (the inner entry's data file size,
// the stand-in for its partition's footprint) doubled for hash-table
// overhead (the deck's fudge factor f) must fit the join pool. The
// estimate can read low while dirty pages wait in the pool; the failure
// mode is a hash join where the nested loops join was the safer plan —
// a downgrade, never a different answer.
bool hash_gate_passes(const std::string& data_file, std::size_t join_pool_frames) {
    std::error_code ec;
    const std::uintmax_t bytes = std::filesystem::file_size(data_file, ec);
    const std::size_t estimated = ec ? 0 : static_cast<std::size_t>(bytes);
    return estimated * 2 <= join_pool_frames * bufman::kPageSize;
}

// --- SELECT plan annotation --------------------------------------------------
//
// The plan text is assembled by the executing code path at its own decision
// sites: one line per operator, built exactly where that operator is built.
// A run ignores the lines; EXPLAIN renders them. Shown == run by
// construction — there is no second planner to drift from.

// A value the way the error messages quote them: strings in single quotes.
std::string plan_value_text(const Value& value) {
    if (const std::string* text = std::get_if<std::string>(&value)) {
        return "'" + *text + "'";
    }
    return display(value);
}

std::string plan_op_text(sql::BinaryOp op) {
    switch (op) {
        case sql::BinaryOp::Eq: return "=";
        case sql::BinaryOp::Ne: return "<>";
        case sql::BinaryOp::Lt: return "<";
        case sql::BinaryOp::Le: return "<=";
        case sql::BinaryOp::Gt: return ">";
        case sql::BinaryOp::Ge: return ">=";
        case sql::BinaryOp::And: return "AND";
        case sql::BinaryOp::Or: return "OR";
        case sql::BinaryOp::Plus: return "+";
        case sql::BinaryOp::Minus: return "-";
        case sql::BinaryOp::Mul: return "*";
        case sql::BinaryOp::Div: return "/";
        case sql::BinaryOp::Mod: return "%";
    }
    return "";
}

// One bound expression's text. `column_name` maps a bound column position
// to its readable name — joined-qualified for residual conjuncts,
// table-local for a pushed conjunct's own scan. Binary and unary
// sub-expressions render parenthesized (the top level does not):
// unambiguous, no precedence table.
std::string plan_expr_text(const BoundExpr& expr,
                           const std::function<std::string(std::size_t)>& column_name,
                           bool top = true) {
    switch (expr.kind) {
        case BoundExpr::Kind::Literal:
            return plan_value_text(expr.literal);
        case BoundExpr::Kind::Column:
            return column_name(expr.column_index);
        case BoundExpr::Kind::Binary: {
            std::string text = plan_expr_text(*expr.left, column_name, false) + " " +
                               plan_op_text(expr.bin_op) + " " +
                               plan_expr_text(*expr.right, column_name, false);
            return top ? text : "(" + text + ")";
        }
        case BoundExpr::Kind::Unary: {
            std::string text = std::string(expr.un_op == sql::UnaryOp::Not ? "NOT " : "-") +
                               plan_expr_text(*expr.operand, column_name, false);
            return top ? text : "(" + text + ")";
        }
    }
    return "";
}

// The conjuncts' text, AND-joined — a Filter's or a join's inner-filter
// content. Rendered before the conjuncts are moved into their operator.
std::string plan_conjuncts_text(const std::vector<BoundExprPtr>& conjuncts,
                                const std::function<std::string(std::size_t)>& column_name) {
    std::string text;
    for (const BoundExprPtr& conjunct : conjuncts) {
        if (!text.empty()) text += " AND ";
        text += plan_expr_text(*conjunct, column_name);
    }
    return text;
}

std::string plan_conjuncts_text(const std::vector<PushedConjunct>& conjuncts,
                                const std::function<std::string(std::size_t)>& column_name) {
    std::string text;
    for (const PushedConjunct& conjunct : conjuncts) {
        if (!text.empty()) text += " AND ";
        text += plan_expr_text(*conjunct.expr, column_name);
    }
    return text;
}

// The joined-space namer: `qualifier.column`, the FROM qualifier being the
// alias when declared and the table name otherwise — the same qualifier
// the SQL itself resolves against.
std::function<std::string(std::size_t)> joined_column_namer(const JoinedSchema& joined) {
    return [&joined](std::size_t position) -> std::string {
        for (std::size_t t = 0; t < joined.table_count(); ++t) {
            const std::size_t first = joined.table_first_column(t);
            const std::size_t columns = joined.table(t).schema->column_count();
            if (position >= first && position < first + columns) {
                const JoinedSchema::Table& table = joined.table(t);
                const std::string qualifier = table.alias.empty() ? table.name : table.alias;
                return qualifier + "." +
                       table.schema->column_info(position - first).column_name();
            }
        }
        return "#" + std::to_string(position);  // unreachable for bound positions
    };
}

// The pinned key bounds as comparisons: `did = 5`, or `age >= 30 AND
// age < 40`. The lo == hi, both-inclusive shape reads as equality.
std::string plan_bounds_text(const IndexBounds& bounds,
                             const std::function<std::string(std::size_t)>& column_name) {
    const std::string column = column_name(bounds.column_index);
    if (bounds.lo.has_value() && bounds.hi.has_value() && bounds.lo_inclusive &&
        bounds.hi_inclusive && compare_values(*bounds.lo, *bounds.hi) == 0) {
        return column + " = " + plan_value_text(*bounds.lo);
    }
    std::string text;
    if (bounds.lo.has_value()) {
        text += column + (bounds.lo_inclusive ? " >= " : " > ") + plan_value_text(*bounds.lo);
    }
    if (bounds.hi.has_value()) {
        if (!text.empty()) text += " AND ";
        text += column + (bounds.hi_inclusive ? " <= " : " < ") + plan_value_text(*bounds.hi);
    }
    return text;
}

// The rendered plan: the lines were built leaves-first, so the print
// reverses them — the root's line first, the tree reading top-down like
// Postgres's.
std::string render_plan(const std::vector<PlanLine>& lines) {
    std::string text;
    for (std::size_t i = lines.size(); i-- > 0;) {
        if (!text.empty()) text += "\n";
        text.append(lines[i].depth * 2, ' ');
        text += lines[i].text;
    }
    return text;
}


} // namespace

StatementResult Executor::handle_explain(const sql::ExplainStatement& statement) {
    // The full prefix of a run — bind refusals, catalog loads, session
    // opens, plan decisions — executes inside; execute_select skips only
    // the drive. The accumulated lines render as the statement's report.
    std::vector<PlanLine> lines;
    StatementResult result = execute_select(*statement.select, &lines);
    result.message = render_plan(lines);
    return result;
}

StatementResult Executor::handle_select(const sql::SelectStatement& statement) {
    return execute_select(statement, nullptr);
}

StatementResult Executor::execute_select(const sql::SelectStatement& statement,
                                         std::vector<PlanLine>* plan) {
    // The annotation flag: a run passes nullptr and never builds plan
    // text; EXPLAIN passes the collector every decision site appends to.
    const bool annotate = plan != nullptr;
    auto note = [plan](std::size_t depth, std::string text) {
        if (plan != nullptr) plan->push_back(PlanLine{depth, std::move(text)});
    };

    // The surface refusals: the grammar accepts these, the executor does
    // not — each names its feature. (Joins stopped being one of these in
    // p4: the FROM chain below builds their plans.)
    if (!statement.group_by.empty()) fail("GROUP BY is not supported");
    if (!statement.order_by.empty()) fail("ORDER BY is not supported");

    // The FROM chain: one catalog entry per table, in FROM order. Only
    // catalogs load here — the bind-time refusals below must cost no
    // data-file I/O.
    struct FromEntry {
        std::string name;
        std::string alias;
        const schema::Catalog* entry;
    };
    std::vector<FromEntry> entries;
    entries.push_back({statement.from.name, statement.from.alias, &catalog(statement.from.name)});
    for (const sql::JoinClause& join : statement.joins) {
        entries.push_back({join.table.name, join.table.alias, &catalog(join.table.name)});
    }

    // A FROM entry as the plan names it: the table, with its alias when
    // the query declares one.
    auto from_qualifier = [](const FromEntry& entry) {
        return entry.alias.empty() ? entry.name : entry.name + " AS " + entry.alias;
    };
    // A FROM entry as the SQL resolves its columns: the alias when
    // declared, else the table name.
    auto sql_qualifier = [](const FromEntry& entry) {
        return entry.alias.empty() ? entry.name : entry.alias;
    };

    JoinedSchema joined;
    for (const FromEntry& entry : entries) {
        joined.add(entry.name, entry.alias, &entry.entry->schema);
    }

    StatementResult result;
    ResultSet& out = result.rows;

    // Projection over the joined space: star spreads across every table
    // in FROM order; plain columns resolve (qualified or bare) to one
    // position. Duplicate header names are kept — the result is positional.
    std::vector<std::size_t> projected;
    for (const sql::SelectItem& item : statement.items) {
        if (item.star) {
            for (std::size_t position = 0; position < joined.table_count(); ++position) {
                const schema::TableSchema& table = *joined.table(position).schema;
                const std::size_t first = joined.table_first_column(position);
                for (std::size_t i = 0; i < table.column_count(); ++i) {
                    projected.push_back(first + i);
                    out.columns.push_back(table.column_info(i).column_name());
                }
            }
            continue;
        }
        if (item.expr->kind == sql::Expr::Kind::Function) {
            fail("aggregate functions are not supported");
        }
        if (item.expr->kind != sql::Expr::Kind::ColumnRef) {
            fail("expressions in the projection are not supported (plain columns only)");
        }
        const sql::Expr& column_expr = *item.expr;
        const JoinedSchema::Resolved resolved =
            joined.resolve(column_expr.table, column_expr.column);
        projected.push_back(resolved.index);
        out.columns.push_back(item.alias.empty() ? column_expr.column : item.alias);
    }

    // Bind the WHERE before any page pin: conjuncts classify by their
    // referenced-table set (pushed conjuncts re-bind with table-local
    // positions), and the residual conjuncts bind over the joined row.
    // Oracle mode bypasses the classification: the whole WHERE stays in
    // one filter above the join — same answers, different plan.
    PushdownSplit split;
    if (statement.where) {
        if (pushdown_enabled_ || statement.joins.empty()) {
            split = split_where(*statement.where, joined);
        } else {
            split.residual.push_back(bind_expr(*statement.where, joined));
        }
    }

    // The single-table access path: the planner sees the bound WHERE and
    // the catalog's indexes. The WHERE binds against the joined schema —
    // for one table the joined positions are the table-local positions,
    // and the table's alias resolves. In oracle mode the planner sees no
    // indexes, so every conjunct stays residual and the heap path runs —
    // the suppression gates the planner, not its output.
    ScanPlan scan_plan;
    if (statement.joins.empty()) {
        const schema::Catalog& entry = *entries.front().entry;
        const schema::TableSchema& schema = entry.schema;
        BoundExprPtr where;
        if (statement.where) {
            where = bind_expr(*statement.where, joined);
        }
        std::vector<CatalogIndex> planner_indexes;
        for (const schema::TableIndex& declared : entry.indexes) {
            const std::optional<std::size_t> column = schema.column_index(declared.column);
            if (allow_index_ && column) {
                planner_indexes.push_back(CatalogIndex{declared.name, *column});
            }
        }
        scan_plan = plan_where(std::move(where), planner_indexes);
    }

    // Open the session files — after every bind-time refusal.
    std::vector<SessionFile*> files;
    for (const FromEntry& entry : entries) {
        files.push_back(&session(entry.name, data_path(entry.name), false));
    }

    // The owning pipeline. Nodes reference their children, so every node
    // lives in this vector and outlives the drive. The hash join's bucket
    // sets and their synthetic schemas live here too — function scope, so
    // they outlive every node that references them and die after the
    // drive (the RAII erasure runs as the statement ends).
    std::vector<std::unique_ptr<ops::Operator>> nodes;
    std::vector<std::unique_ptr<ops::Partitions>> scratch;
    std::vector<std::unique_ptr<schema::TableSchema>> scratch_schemas;

    if (statement.joins.empty()) {
        // Single table: scan (heap or index), filter the residual
        // conjuncts, project, limit.
        SessionFile& session_file = *files.front();
        const SessionIndex* index = nullptr;
        if (scan_plan.use_index) {
            for (const SessionIndex& candidate : session_file.indexes) {
                if (candidate.column_index == scan_plan.bounds.column_index) {
                    index = &candidate;
                    break;
                }
            }
        }
        if (index != nullptr) {
            if (!annotate) out.access_path = "index scan";
            // The scan's line carries the index by name and the pinned
            // bounds; a bounds conflict renders its note — the execution
            // returns nothing, and the plan says so.
            if (annotate) {
                const std::size_t project_depth = statement.has_limit ? 1 : 0;
                const std::size_t filter_depth = project_depth + 1;
                const std::size_t scan_depth =
                    filter_depth + (scan_plan.residual.empty() ? 0 : 1);
                std::string line = "Scan " + from_qualifier(entries.front()) + " (index " +
                                   scan_plan.bounds.index_name;
                if (!scan_plan.bounds.possible()) {
                    line += ", bounds conflict: answers nothing";
                } else {
                    const std::string bounds =
                        plan_bounds_text(scan_plan.bounds, joined_column_namer(joined));
                    if (!bounds.empty()) line += ", " + bounds;
                }
                note(scan_depth, line + ")");
            }
            if (!scan_plan.bounds.possible()) {
                return result; // conflicting bounds: the index answers nothing
            }
            const SessionIndex* bound_index = index;
            const auto bound = [](const std::optional<Value>& value, bool inclusive) {
                return value ? std::optional(isam::RangeBound{value_to_column(*value), inclusive})
                             : std::nullopt;
            };
            nodes.push_back(std::make_unique<ops::RowIdScan>(
                *session_file.file, [bound_index, &scan_plan, &bound] {
                    std::vector<heapfile::RowId> rows;
                    for (const idx::Rid& rid :
                         bound_index->range(bound(scan_plan.bounds.lo, scan_plan.bounds.lo_inclusive),
                                            bound(scan_plan.bounds.hi, scan_plan.bounds.hi_inclusive))) {
                        rows.push_back(heapfile::RowId{rid.page_id, rid.slot});
                    }
                    return rows;
                }));
        } else {
            // No index, or the planned column's index is not open: the
            // heap path answers the same rows.
            if (!annotate) out.access_path = "heap scan";
            if (annotate) {
                const std::size_t project_depth = statement.has_limit ? 1 : 0;
                const std::size_t scan_depth =
                    project_depth + 1 + (scan_plan.residual.empty() ? 0 : 1);
                note(scan_depth, "Scan " + from_qualifier(entries.front()) + " (heap)");
            }
            nodes.push_back(std::make_unique<ops::SeqScan>(*session_file.file));
        }
        if (!scan_plan.residual.empty()) {
            if (annotate) {
                note((statement.has_limit ? 1 : 0) + 1,
                     "Filter (" + plan_conjuncts_text(scan_plan.residual,
                                                      joined_column_namer(joined)) + ")");
            }
            nodes.push_back(std::make_unique<ops::Filter>(
                *nodes.back(), all_of_conjuncts(std::move(scan_plan.residual))));
        }
    } else {
        // The join chain folds left-deep: entry i's (filtered) scan joins
        // everything before it, its ON bound over the entries 0..i
        // prefix. A null ON is a cross join. `left_top` tracks the
        // accumulated subtree's root in `nodes`; `top_of_entry` records
        // each entry's top node so every join pairs the accumulation with
        // its own inner entry (pairing with nodes.back() broke at three
        // tables — the accumulation became its own inner child).
        //
        // The join-algorithm selection rule decides per node before any
        // node is built (the lecture's order of preference): when entry i
        // carries an index on a column the join's own predicate equates
        // with a prefix column, the inner scan is replaced by one probe
        // per outer tuple; when no index applies and the hash join is
        // enabled within the memory gate, both sides partition into
        // buckets; the plain scan-rescan join answers the rest.
        // Rule-based, never a reorder — an index on an earlier entry
        // changes nothing.
        if (!annotate) out.access_path = "nested loops join";
        std::vector<std::vector<PushedConjunct>> pushed_by_table(joined.table_count());
        for (PushedConjunct& conjunct : split.pushed) {
            pushed_by_table[conjunct.table_position].push_back(std::move(conjunct));
        }

        // The per-node algorithm decision. The join's own predicate is
        // the ON for the JOIN form; for the comma form the ON is null and
        // the predicate lives in the WHERE. The index probe needs the
        // index open in the session file — a catalog index that never
        // opened answers through the heap path, exactly as single-table
        // queries do. One selection rule per node (the lecture's order):
        // index join when an open inner index serves the shape; hash join
        // when none does, the session enables it, and the gate passes;
        // nested loops otherwise.
        struct NodeDecision {
            const SessionIndex* index = nullptr;  // the index join's probe; null = none
            bool hash = false;                    // the hash join when no index applies
            std::size_t key_position = 0;         // prefix column, in the outer child's layout
            std::size_t inner_column = 0;         // table-local; the equated column
        };
        std::vector<NodeDecision> decisions(entries.size());
        for (std::size_t i = 1; i < entries.size(); ++i) {
            const sql::JoinClause& join = statement.joins[i - 1];
            const sql::Expr* source =
                join.on != nullptr ? join.on.get()
                                   : (statement.where ? statement.where.get() : nullptr);
            if (source == nullptr) continue;
            const std::optional<EquiJoinKey> key = equi_join_key(*source, joined, i);
            if (!key.has_value()) continue;
            const SessionIndex* index = nullptr;
            if (allow_index_) {
                for (const SessionIndex& candidate : files[i]->indexes) {
                    if (candidate.column_index == key->inner_column) {
                        index = &candidate;
                        break;
                    }
                }
            }
            switch (select_join_algorithm(
                    index != nullptr, hash_enabled_,
                    hash_gate_passes(data_path(entries[i].name), join_pool().pool_size()))) {
                case JoinAlgorithm::Index:
                    decisions[i] = {index, false, key->key_position, key->inner_column};
                    break;
                case JoinAlgorithm::Hash:
                    decisions[i] = {nullptr, true, key->key_position, key->inner_column};
                    break;
                case JoinAlgorithm::NestedLoops:
                    break;
            }
        }

        std::vector<std::size_t> top_of_entry(entries.size(), 0);
        // Tree depths for the annotation: the root chain is limit ->
        // project -> [residual filter] -> top join, and each join hangs
        // the pipeline of the entry it consumes one level below itself.
        const std::size_t table_count = entries.size();
        const std::size_t project_depth = statement.has_limit ? 1 : 0;
        const std::size_t top_join_depth =
            project_depth + 1 + (split.residual.empty() ? 0 : 1);
        auto join_depth = [&](std::size_t i) {
            return top_join_depth + (table_count - 1 - i);
        };
        auto pipeline_depth = [&](std::size_t i) { return join_depth(i) + 1; };
        for (std::size_t position = 0; position < joined.table_count(); ++position) {
            if (decisions[position].index != nullptr) {
                continue;  // the probe replaces this entry's inner scan
            }
            // Entry 0's pipeline hangs under join_1 — the same depth as
            // entry 1's (they are siblings); entry i's under its own join.
            const std::size_t pipeline_top =
                pipeline_depth(position == 0 ? 1 : position);
            if (annotate) {
                // The pipeline's top is its filter when one exists, the
                // bare scan otherwise.
                const std::size_t scan_depth =
                    pipeline_top + (pushed_by_table[position].empty() ? 0 : 1);
                note(scan_depth,
                     "Scan " + from_qualifier(entries[position]) + " (heap)");
            }
            nodes.push_back(std::make_unique<ops::SeqScan>(*files[position]->file));
            if (!pushed_by_table[position].empty()) {
                if (annotate) {
                    const schema::TableSchema& table = entries[position].entry->schema;
                    auto local_name = [&table](std::size_t p) {
                        return table.column_info(p).column_name();
                    };
                    note(pipeline_top, "Filter (" +
                                           plan_conjuncts_text(pushed_by_table[position],
                                                               local_name) + ")");
                }
                std::vector<BoundExprPtr> conjuncts;
                for (PushedConjunct& pushed : pushed_by_table[position]) {
                    conjuncts.push_back(std::move(pushed.expr));
                }
                nodes.push_back(std::make_unique<ops::Filter>(
                    *nodes.back(), all_of_conjuncts(std::move(conjuncts))));
            }
            top_of_entry[position] = nodes.size() - 1;
        }
        std::size_t left_top = top_of_entry[0]; // entry 0's top node
        bool probed = false;
        bool hashed = false;
        // The lecture's B-1 (slide 25): one bucket per join-pool frame
        // except the input's, floored for the smallest pools.
        const std::size_t bucket_count =
            std::max<std::size_t>(1, join_pool().pool_size() - 1);
        int hash_seq = 0;  // statement-unique bucket-file labels
        for (std::size_t i = 1; i < entries.size(); ++i) {
            const sql::JoinClause& join = statement.joins[i - 1];
            // The ON binds over the prefix 0..i, before any data flows.
            JoinedSchema prefix;
            for (std::size_t p = 0; p <= i; ++p) {
                const FromEntry& entry = entries[p];
                prefix.add(entry.name, entry.alias, &entry.entry->schema);
            }
            std::function<bool(const tuple::Tuple&)> predicate;
            std::shared_ptr<const BoundExpr> on;
            if (join.on != nullptr) {
                on = bind_expr(*join.on, prefix);
                predicate = [on](const tuple::Tuple& combined) {
                    return truthy_value(evaluate(*on, combined));
                };
            }
            if (decisions[i].index != nullptr) {
                // The lecture's algorithm: the outer child is scanned once;
                // for each outer tuple the index is probed with that
                // tuple's key, every matching row is fetched from the
                // inner file, and the pushed inner conjuncts filter the
                // fetched row before combining. The ON still evaluates
                // over every combined pair — the index narrows, the
                // predicate decides.
                probed = true;
                std::vector<BoundExprPtr> inner_conjuncts;
                for (PushedConjunct& pushed : pushed_by_table[i]) {
                    inner_conjuncts.push_back(std::move(pushed.expr));
                }
                const SessionIndex* probe_index = decisions[i].index;
                const std::size_t key_position = decisions[i].key_position;
                if (annotate) {
                    const schema::TableSchema& inner = entries[i].entry->schema;
                    const std::string inner_column =
                        inner.column_info(decisions[i].inner_column).column_name();
                    std::string line =
                        "Join: index nested loops (probe " + probe_index->index_name +
                        ": " + sql_qualifier(entries[i]) + "." + inner_column + " = " +
                        joined_column_namer(joined)(key_position) + ")";
                    // The probed entry's pushed conjuncts filter the
                    // fetched rows inside this node — its line says so.
                    const std::string inner_filter =
                        plan_conjuncts_text(inner_conjuncts,
                                            [&inner](std::size_t p) {
                                                return inner.column_info(p).column_name();
                                            });
                    if (!inner_filter.empty()) {
                        line = line.substr(0, line.size() - 1) + "; inner filter " +
                               inner_filter + ")";
                    }
                    note(join_depth(i), std::move(line));
                }
                std::function<bool(const tuple::Tuple&)> inner_predicate;
                if (!inner_conjuncts.empty()) {
                    inner_predicate = all_of_conjuncts(std::move(inner_conjuncts));
                }
                nodes.push_back(std::make_unique<ops::IndexNestedLoopsJoin>(
                    *nodes[left_top], *files[i]->file,
                    [probe_index, key_position](const tuple::Tuple& outer) {
                        std::vector<heapfile::RowId> rows;
                        const isam::RangeBound key{outer.column(key_position), true};
                        for (const idx::Rid& rid : probe_index->range(key, key)) {
                            rows.push_back(heapfile::RowId{rid.page_id, rid.slot});
                        }
                        return rows;
                    },
                    std::move(predicate), std::move(inner_predicate)));
            } else if (decisions[i].hash) {
                // The lecture's fifth algorithm (slides 24-27): Phase I
                // partitions both sides on the join key into bucket files
                // under tmp/ (heap files over the join pool); Phase II
                // probes an in-memory table per partition pair. The build
                // side is the newest entry — the side whose size the gate
                // estimated; the probe side the accumulated prefix,
                // preserving the nested loops orientation. The pushed
                // inner conjuncts keep their scan-side Filter (the build
                // child is a real scan pipeline). The predicate still
                // decides.
                hashed = true;
                ++hash_seq;
                // The probe side's bucket schema: the prefix's joined row
                // as one synthetic table — the columns in prefix order,
                // each named by its table so same-named columns stay
                // distinct (bucket rows are positional; the names are
                // never looked up). Statement-owned: the partition set
                // keeps a pointer to it.
                scratch_schemas.push_back(
                    std::make_unique<schema::TableSchema>("prefix"));
                schema::TableSchema& prefix_schema = *scratch_schemas.back();
                for (std::size_t p = 0; p < i; ++p) {
                    const schema::TableSchema& table = entries[p].entry->schema;
                    const std::string qualifier =
                        entries[p].alias.empty() ? entries[p].name : entries[p].alias;
                    for (std::size_t c = 0; c < table.column_count(); ++c) {
                        const schema::ColumnSchema& column = table.column_info(c);
                        prefix_schema.add_column(qualifier + "." + column.column_name(),
                                                 column.column_type(), column.column_size());
                    }
                }
                scratch.push_back(std::make_unique<ops::Partitions>(
                    join_pool(), "tmp", "join" + std::to_string(hash_seq) + "_build",
                    entries[i].entry->schema, bucket_count));
                scratch.push_back(std::make_unique<ops::Partitions>(
                    join_pool(), "tmp", "join" + std::to_string(hash_seq) + "_probe",
                    prefix_schema, bucket_count));
                const std::size_t inner_column = decisions[i].inner_column;
                const std::size_t key_position = decisions[i].key_position;
                if (annotate) {
                    const schema::TableSchema& inner = entries[i].entry->schema;
                    note(join_depth(i),
                         "Join: hash join (" + sql_qualifier(entries[i]) + "." +
                             inner.column_info(inner_column).column_name() + " = " +
                             joined_column_namer(joined)(key_position) + ")");
                }
                nodes.push_back(std::make_unique<ops::HashJoin>(
                    *nodes[top_of_entry[i]], *nodes[left_top],
                    [inner_column](const tuple::Tuple& row) {
                        return ops::join_key(row.column(inner_column));
                    },
                    [key_position](const tuple::Tuple& row) {
                        return ops::join_key(row.column(key_position));
                    },
                    std::move(predicate), *scratch[scratch.size() - 2], *scratch.back()));
            } else {
                if (annotate) {
                    if (join.on != nullptr) {
                        note(join_depth(i),
                             "Join: nested loops (on " +
                                 plan_expr_text(*on, joined_column_namer(joined)) + ")");
                    } else {
                        note(join_depth(i), "Join: nested loops (cross)");
                    }
                }
                nodes.push_back(std::make_unique<ops::NestedLoopsJoin>(
                    *nodes[left_top], *nodes[top_of_entry[i]], std::move(predicate)));
            }
            left_top = nodes.size() - 1;
        }
        if (hashed) {
            if (!annotate) out.access_path = "hash join";
        } else if (probed) {
            if (!annotate) out.access_path = "index nested loops join";
        }
        if (!split.residual.empty()) {
            if (annotate) {
                note(project_depth + 1,
                     "Filter (" + plan_conjuncts_text(split.residual,
                                                      joined_column_namer(joined)) + ")");
            }
            nodes.push_back(std::make_unique<ops::Filter>(
                *nodes.back(), all_of_conjuncts(std::move(split.residual))));
        }
    }

    // Project (and limit), then drive.
    if (annotate) {
        std::string columns;
        for (std::size_t i = 0; i < out.columns.size(); ++i) {
            if (i != 0) columns += ", ";
            columns += out.columns[i];
        }
        note(statement.has_limit ? 1 : 0, "Project (" + columns + ")");
        if (statement.has_limit) {
            note(0, "Limit (" + std::to_string(statement.limit) + ")");
        }
    }
    nodes.push_back(std::make_unique<ops::Project>(*nodes.back(), projected));
    if (statement.has_limit) {
        nodes.push_back(std::make_unique<ops::Limit>(*nodes.back(),
                                                     static_cast<std::size_t>(statement.limit)));
    }
    if (!annotate) {
        run_pipeline(*nodes.back(), out.rows);
    }
    return result;
}

// --- UPDATE ------------------------------------------------------------------
//
// Collect-then-apply (the p2dbeng semantics, index-aware): each matching
// row is overwritten with a new row computed from that row's OLD values —
// so SET assignments never observe each other's results (the swap comes
// free) — and only the indexes whose column a row's value changed are
// rewritten (erase the old entry, insert the new).

StatementResult Executor::handle_update(const sql::UpdateStatement& statement) {
    const schema::TableSchema& schema = catalog(statement.table).schema;

    // Bind every SET target and expression, and the WHERE, before any
    // page is pinned — the bind-time refusals cost no I/O.
    std::vector<BoundExprPtr> assigned(schema.column_count());
    for (const sql::SetClause& clause : statement.sets) {
        const std::optional<std::size_t> index = schema.column_index(clause.column);
        if (!index) {
            fail("unknown column '" + clause.column + "' in SET");
        }
        BoundExprPtr value = bind_expr(*clause.value, schema);
        if (!assigned[*index]) {
            assigned[*index] = std::move(value);  // the first SET of a column wins
        }
    }
    const BoundExprPtr where = statement.where ? bind_expr(*statement.where, schema) : nullptr;

    SessionFile& session_file = session(statement.table, data_path(statement.table), false);
    const std::vector<Match> matches = collect_matches(*session_file.file, where.get());

    // Plan every new row before any page is dirtied: the computed rows
    // are validated (width rules, primary key) across the whole
    // statement, and any refusal applies nothing — the all-or-nothing
    // contract holds for every check, not just the ones that fit before
    // the first apply.
    struct PlannedUpdate {
        heapfile::RowId row_id;
        tuple::Tuple old_values;  // the row's pre-update values (for the index moves)
        tuple::Tuple fresh;
    };
    std::vector<PlannedUpdate> planned;
    planned.reserve(matches.size());
    std::size_t updated = 0;

    // The primary key's check state: the constraint's index (the keys
    // already stored) plus a seen-set over the keys this statement will
    // store — the index alone would pass two collected rows moving onto
    // the same free value. A row assigning its own current value skips
    // the probe entirely (its "new" key is its old one), and in the
    // changed branch an index hit cannot be the row's own entry — the
    // row's own rid sits under its old key — so any hit is a refusal.
    const schema::Catalog& entry = catalog(statement.table);
    const SessionIndex* pk_index = nullptr;
    std::size_t pk_position = 0;
    if (entry.primary_key) {
        pk_position = *schema.column_index(*entry.primary_key);
        for (const SessionIndex& index : session_file.indexes) {
            if (index.column_name == *entry.primary_key) pk_index = &index;
        }
        if (pk_index == nullptr) {
            fail("table '" + statement.table +
                 "' declares a primary key but carries no index over its column");
        }
    }
    std::set<std::string> seen_pk;

    for (const Match& match : matches) {
        tuple::Tuple fresh;
        for (std::size_t i = 0; i < match.values.column_count(); ++i) {
            fresh.add_column(assigned[i] ? value_to_column(evaluate(*assigned[i], match.values))
                                         : match.values.column(i));
        }
        schema.validate(fresh);  // the new row must still fit the catalog
        if (pk_index != nullptr) {
            const tuple::Column& old_key = match.values.column(pk_position);
            const tuple::Column& new_key = fresh.column(pk_position);
            if (!(old_key == new_key)) {
                const isam::RangeBound bound{new_key, true};
                if (!pk_index->range(bound, bound).empty() ||
                    !seen_pk.insert(pk_key_text(new_key)).second) {
                    fail("duplicate value " + pk_value_text(new_key) +
                         " for the primary key of table '" + statement.table +
                         "' (column '" + *entry.primary_key + "')");
                }
            }
        }
        planned.push_back({match.row_id, match.values, std::move(fresh)});
    }

    // The foreign keys, child side: a planned row whose referencing
    // column changes must point at a key that exists — in the parent's
    // primary-key index, or among the parent keys this statement itself
    // provides (a self-reference's planned primary-key values). An
    // unchanged referencing value is not re-probed here: it was valid
    // when stored, and if the parent row it names moves away in this same
    // statement, the parent-side check below refuses that instead —
    // which is the honest refusal, naming the constraint that breaks.
    if (!entry.foreign_keys.empty()) {
        struct ResolvedKey {
            std::size_t child_position;
            const SessionIndex* parent_index = nullptr;
            bool self_referencing = false;
        };
        std::vector<ResolvedKey> resolved;
        resolved.reserve(entry.foreign_keys.size());
        for (const schema::TableForeignKey& fk : entry.foreign_keys) {
            ResolvedKey r;
            r.child_position = *schema.column_index(fk.column);
            r.self_referencing = fk.parent_table == statement.table;
            if (r.self_referencing) {
                for (const SessionIndex& index : session_file.indexes) {
                    if (index.column_name == fk.parent_column) r.parent_index = &index;
                }
            } else {
                try {
                    SessionFile& parent =
                        session(fk.parent_table, data_path(fk.parent_table), false);
                    for (const SessionIndex& index : parent.indexes) {
                        if (index.column_name == fk.parent_column) r.parent_index = &index;
                    }
                } catch (const common::Error&) {
                }
            }
            resolved.push_back(r);
        }
        // The parent-provided set: a self-reference's planned primary-key
        // values (the statement keeps those keys alive by writing them).
        std::set<std::string> provided;
        if (entry.primary_key) {
            const std::size_t pk_position = *schema.column_index(*entry.primary_key);
            for (const ResolvedKey& r : resolved) {
                if (!r.self_referencing) continue;
                for (const PlannedUpdate& plan : planned) {
                    provided.insert(pk_key_text(plan.fresh.column(pk_position)));
                }
            }
        }
        for (const PlannedUpdate& plan : planned) {
            for (std::size_t f = 0; f < entry.foreign_keys.size(); ++f) {
                const schema::TableForeignKey& fk = entry.foreign_keys[f];
                const tuple::Column& old_key = plan.old_values.column(resolved[f].child_position);
                const tuple::Column& new_key = plan.fresh.column(resolved[f].child_position);
                if (old_key == new_key) continue;
                const isam::RangeBound bound{new_key, true};
                const bool found =
                    provided.count(pk_key_text(new_key)) > 0 ||
                    (resolved[f].parent_index != nullptr &&
                     !resolved[f].parent_index->range(bound, bound).empty());
                if (!found) {
                    fail("value " + pk_value_text(new_key) + " not found in the primary key of table '" +
                         fk.parent_table + "' (column '" + fk.parent_column + "'), referenced by " +
                         statement.table + "." + fk.column);
                }
            }
        }
    }

    // The parent side: planned rows whose primary key moves take the old
    // key away, and a referencing row that would still hold it after the
    // statement blocks. The exemption is precise: a self-referencing
    // child row ends its own reference when its computed new values stop
    // naming the removed key (the `SET eid = 9, manager_id = 9` self-move)
    // — any other referencing row, here or in another table, refuses.
    if (entry.primary_key) {
        const std::size_t pk_position = *schema.column_index(*entry.primary_key);
        std::vector<std::string> removed;
        for (const PlannedUpdate& plan : planned) {
            const tuple::Column& old_pk = plan.old_values.column(pk_position);
            const tuple::Column& new_pk = plan.fresh.column(pk_position);
            if (!(old_pk == new_pk)) removed.push_back(pk_key_text(old_pk));
        }
        if (!removed.empty()) {
            check_parent_side(
                statement.table, removed,
                [&](heapfile::RowId rid, const tuple::Column& value) {
                    for (const PlannedUpdate& plan : planned) {
                        if (!(plan.row_id == rid)) continue;
                        // The referencing column is this table's own (a
                        // self-reference); the row is exempt exactly when
                        // its new values stop naming the removed key.
                        for (const schema::TableForeignKey& fk : entry.foreign_keys) {
                            if (fk.parent_table != statement.table) continue;
                            const tuple::Column& fresh_ref =
                                plan.fresh.column(*schema.column_index(fk.column));
                            if (!(fresh_ref == value)) return true;
                        }
                        return false;
                    }
                    return false;
                });
        }
    }

    for (const PlannedUpdate& plan : planned) {
        session_file.file->update(plan.row_id, plan.fresh);
        for (const SessionIndex& index : session_file.indexes) {
            const tuple::Column& old_key = plan.old_values.column(index.column_index);
            const tuple::Column& new_key = plan.fresh.column(index.column_index);
            if (!(old_key == new_key)) {
                index.erase(old_key, to_rid(plan.row_id));
                index.insert(new_key, to_rid(plan.row_id));
            }
        }
        ++updated;
    }
    return StatementResult{"updated " + std::to_string(updated) + " row(s)", {}};
}

// --- DELETE ------------------------------------------------------------------
//
// Collect-then-erase with the same discipline: matches are removed from
// the heap and from every declared index, so no lookup or range ever
// returns a deleted row again.

StatementResult Executor::handle_delete(const sql::DeleteStatement& statement) {
    const schema::Catalog& entry = catalog(statement.table);
    const schema::TableSchema& schema = entry.schema;
    const BoundExprPtr where = statement.where ? bind_expr(*statement.where, schema) : nullptr;

    SessionFile& session_file = session(statement.table, data_path(statement.table), false);
    const std::vector<Match> matches = collect_matches(*session_file.file, where.get());

    // The parent side of the foreign keys: the collected rows' primary
    // keys leave the table, and a referencing row that would still hold
    // one of them blocks the statement. The exemption is the collected
    // rows themselves — a self-referencing row's reference ends with its
    // own deletion (the reference dies with the row).
    if (entry.primary_key) {
        const std::size_t pk_position = *schema.column_index(*entry.primary_key);
        std::vector<std::string> removed;
        for (const Match& match : matches) {
            removed.push_back(pk_key_text(match.values.column(pk_position)));
        }
        check_parent_side(statement.table, removed, [&](heapfile::RowId rid, const tuple::Column&) {
            return std::any_of(matches.begin(), matches.end(),
                               [&](const Match& match) { return match.row_id == rid; });
        });
    }

    std::size_t deleted = 0;
    for (const Match& match : matches) {
        session_file.file->erase(match.row_id);
        for (const SessionIndex& index : session_file.indexes) {
            index.erase(match.values.column(index.column_index), to_rid(match.row_id));
        }
        ++deleted;
    }
    return StatementResult{"deleted " + std::to_string(deleted) + " row(s)", {}};
}

}
