#pragma once

// Executor — the statement dispatcher and the session state it runs
// against: one buffer manager (shared with every heap file and index
// file), the catalog cache, and the file table. The statement surface
// is the proposal's contract; everything the grammar accepts but the
// surface excludes (joins, GROUP BY, ORDER BY, LIMIT, aggregates,
// UPDATE, DELETE) is refused here with a named error — the subset
// contract lives in the dispatcher, not the grammar.
//
// Physical naming: a table's data file is `data/<table>.tbl` (the
// `data` directory is created on demand); an index file is
// `data/<table>.tbl.<column>.idx` beside it.
// Opening a data file builds any catalog-declared index file that is
// missing (lazy rebuild), and every write through a session file
// maintains that file's indexes.

#include "bufpool/BufferManager.h"
#include "exec/Planner.h"
#include "exec/Value.h"
#include "heapfile/HeapFile.h"
#include "index/BPlusTree.h"
#include "index/IsamIndex.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "sql/Ast.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace exec {

struct PlanLine;  // the SELECT plan annotation's one line (Executor.cpp)

struct ResultSet {
    std::vector<std::string> columns;             // projected names
    std::vector<std::vector<Value>> rows;
    std::string access_path;                      // "index scan"/"heap scan"
};

struct StatementResult {
    std::string message;     // one-line report (DDL, INSERT)
    ResultSet rows;          // filled for SELECT
};

class Executor {
public:
    // The join pool is the hash join's scratch pool (the lecture's `B`
    // buffers): partition-file I/O and the in-memory table's budget flow
    // through it and never evict main-pool table pages. When `join_pool`
    // is null, an internal join pool of kDefaultJoinPoolFrames LRU frames
    // is created, so carried call sites keep their single-pool shape.
    Executor(bufman::BufferManager& manager,
             bufman::BufferManager* join_pool = nullptr,
             const std::string& catalog_directory = "tablecatalog");

    // Dispatches one parsed statement. SELECT fills `rows`; other
    // statements fill `message`. Refusals and failures throw by name;
    // the session continues.
    StatementResult execute(const sql::Statement& statement);

    // The differential-oracle hook: when false, the planner's index path
    // is suppressed and every SELECT scans the heap.
    void set_allow_index(bool allow) { allow_index_ = allow; }

    // The hash-join availability flag — ONE flag for two controls: the
    // session initializes it from conf.json's "hash_join" switch at
    // startup, and the join oracle's set_allow_hash toggles it between
    // rounds. When false, joins run only as nested loops or index joins.
    void set_allow_hash(bool allow) { hash_enabled_ = allow; }

    // The join scratch pool (see the constructor).
    bufman::BufferManager& join_pool() { return *join_pool_; }

    // The join oracle's hook: when false, WHERE conjuncts are never
    // classified — every one stays in a single filter above the join,
    // bound over combined rows. The answers are the same by contract;
    // the join suite holds both runs to that.
    void set_pushdown(bool allow) { pushdown_enabled_ = allow; }

    // The table's schema and catalog indexes (loading the catalog on
    // first use). An unknown table throws.
    const schema::Catalog& table(const std::string& name);

    // The session file for `path`, bound to `table_name`: opened on
    // first use, its catalog indexes opened or lazily built with it.
    // The escape-hatch commands and the SQL path share this.
    heapfile::HeapFile& file_for(const std::string& table_name, const std::string& path,
                                 bool create_when_missing);

    // quit/EOF: close every session file exactly once. Every close is
    // attempted; the failures are re-thrown together afterwards.
    void close_all();

    // The escape-hatch write path: inserts one tuple through the session
    // file, maintaining that file's indexes (heap first, then index).
    heapfile::RowId insert_into(const std::string& table_name, const std::string& path,
                                const tuple::Tuple& tuple);

    // Counts the records of a session file by scanning it. With
    // create_when_missing, a missing data file counts as zero rows (and
    // is created) instead of failing — the bulk path's fresh-table case.
    std::size_t count_rows(const std::string& table_name, const std::string& path,
                           bool create_when_missing = false);

    // checkindex: verifies every catalog-declared index of `path`
    // against the data file; one report line per index, clean or named.
    struct IndexCheck {
        std::vector<std::string> report;
        bool clean = true;
    };
    IndexCheck check_indexes(const std::string& table_name, const std::string& path);

private:
    // The single dispatch point (design D6): one kind-tagged holder whose
    // methods forward to the declared index type, so INSERT/UPDATE/DELETE
    // maintenance, access-path execution, lazy rebuild, and checkindex
    // never branch on the type themselves.
    struct SessionIndex {
        enum class Kind { Isam, Btree };
        Kind kind = Kind::Isam;
        std::size_t column_index = 0;
        std::string index_name;   // catalog name
        std::string column_name;
        std::unique_ptr<isam::IsamIndex> isam;
        std::unique_ptr<bptree::BPlusTree> btree;

        void open(const std::string& path) const;
        void build(const std::string& path,
                   const std::vector<std::pair<tuple::Column, idx::Rid>>& entries) const;
        void close() const;
        std::vector<idx::Rid> range(const std::optional<isam::RangeBound>& lo,
                                    const std::optional<isam::RangeBound>& hi) const;
        void insert(const tuple::Column& key, idx::Rid rid) const;
        void erase(const tuple::Column& key, idx::Rid rid) const;
        idx::VerifyResult verify(const std::function<bool(const idx::Rid&)>& occupied) const;
    };

    struct SessionFile {
        std::string table_name;
        std::unique_ptr<heapfile::HeapFile> file;
        std::vector<SessionIndex> indexes;
    };

    // The catalog's type word ("ISAM" default) as the dispatch kind.
    static SessionIndex::Kind parse_index_type(const std::string& type);
    SessionIndex make_session_index(SessionIndex::Kind kind, const schema::TableSchema& schema,
                                    std::size_t column_index, const std::string& index_name,
                                    const std::string& column_name);

    StatementResult handle_create_table(const sql::CreateTableStatement& statement);
    StatementResult handle_drop_table(const sql::DropTableStatement& statement);
    StatementResult handle_create_index(const sql::CreateIndexStatement& statement);
    StatementResult handle_drop_index(const sql::DropIndexStatement& statement);
    StatementResult drop_index_in(const std::string& table_name, schema::Catalog& catalog,
                                  std::size_t position);
    StatementResult handle_insert(const sql::InsertStatement& statement);
    StatementResult handle_select(const sql::SelectStatement& statement);
    StatementResult handle_explain(const sql::ExplainStatement& statement);
    // The SELECT body shared by the run and the EXPLAIN: a run passes
    // nullptr for `plan` and drives the pipeline; EXPLAIN passes the
    // collector every decision site appends to, and the drive is skipped.
    StatementResult execute_select(const sql::SelectStatement& statement,
                                   std::vector<PlanLine>* plan);
    StatementResult handle_update(const sql::UpdateStatement& statement);
    StatementResult handle_delete(const sql::DeleteStatement& statement);

    // The row-level seams shared by the statements and escape hatches.
    SessionFile& session(const std::string& table_name, const std::string& path,
                         bool create_when_missing);
    heapfile::RowId insert_row(SessionFile& session, const tuple::Tuple& tuple);
    struct Match {
        heapfile::RowId row_id;
        tuple::Tuple values;
    };
    std::vector<Match> collect_matches(heapfile::HeapFile& file, const BoundExpr* where);
    void close_session(SessionFile& session);

    // The parent side of the foreign keys: the referencing (child table,
    // child column) pairs every catalog in the directory declares against
    // `parent_table` — including the parent's own self-reference.
    struct ReferencingChild {
        std::string table;
        std::string column;
    };
    std::vector<ReferencingChild> referencing_children(const std::string& parent_table);

    // RESTRICT: refuses when a row of a referencing table would still
    // hold one of `removed_keys` (canonical key texts) once this
    // statement completes. `exempt` decides, per child row that hits, whether
    // the statement itself ends that reference — a self-referencing row
    // the statement deletes or re-points does not block it; cross-table
    // children have no exemptions (this statement cannot touch them).
    void check_parent_side(const std::string& parent_table,
                           const std::vector<std::string>& removed_keys,
                           const std::function<bool(heapfile::RowId, const tuple::Column&)>& exempt);

    schema::Catalog& catalog(const std::string& name);
    std::string catalog_path(const std::string& table_name) const;
    std::string data_path(const std::string& table_name) const;
    std::string index_path(const std::string& data_file,
                           const std::string& column_name) const;
    void save_catalog(const std::string& name);

    bufman::BufferManager& manager_;
    bufman::BufferManager internal_join_pool_;
    bufman::BufferManager* join_pool_ = nullptr;
    std::string catalog_directory_;
    std::unordered_map<std::string, schema::Catalog> catalogs_;
    std::unordered_map<std::string, SessionFile> files_;
    bool allow_index_ = true;
    bool pushdown_enabled_ = true;
    bool hash_enabled_ = true;

    // The internal join pool's frames when the caller provides none.
    static constexpr std::size_t kDefaultJoinPoolFrames = 8;
};

}
