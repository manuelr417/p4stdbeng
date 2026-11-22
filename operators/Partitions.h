#pragma once

// Partitions — the provided support for the disk-based join algorithms:
// B disk-resident bucket files under the session's scratch directory,
// written and read one row at a time through the join buffer pool.
//
// This class is GIVEN support, not student work: the assignment is the
// join algorithm, not the files. The hash join operator (and, later, any
// algorithm that needs partitioned runs) holds two of these — one per
// input side, each with that side's schema — and sees only rows going in
// and coming out. File paths, schemas, and erasure live here.
//
// Every bucket is a plain heap file over the join pool, so the deck's
// "flush a page to its disk-resident bucket only when full" is the pool's
// eviction working naturally: append-only buckets, written once in the
// partition phase and read once in the probing phase. Erasure is the
// query-end contract: erase_all() on close() and in the destructor (the
// RAII guard), so an error unwinding erases too.

#include "bufpool/BufferManager.h"
#include "common/Error.h"
#include "heapfile/HeapFile.h"
#include "schema/TableSchema.h"
#include "tuple/Tuple.h"

#include <cassert>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ops {

class Partitions {
public:
    // Purpose: bind a partition set to the join pool, the scratch
    // directory, a statement-unique label, one input side's schema, and
    // the bucket count (the lecture's B-1).
    // Contract: `pool` and `schema` must outlive the set and every file
    // it creates; `label` must be unique among the sets of one statement
    // (the executor's per-statement counter guarantees it), so the bucket
    // file names never collide.
    Partitions(bufman::BufferManager& pool, std::filesystem::path directory,
               std::string label, const schema::TableSchema& schema,
               std::size_t bucket_count);

    // The query-end erasure: the destructor erases whatever a failed run
    // left behind. Idempotent, like erase_all().
    ~Partitions();

    Partitions(const Partitions&) = delete;
    Partitions& operator=(const Partitions&) = delete;

    // The lecture's B-1: how many buckets the inputs hash into.
    std::size_t bucket_count() const { return bucket_count_; }

    /// Purpose: format the bucket files, creating the scratch directory
    /// when it is missing (the session wipes stale contents at startup;
    /// the directory is created here when the first join needs it).
    /// Called once per run, before the first write; erase_all() in
    /// between allows a fresh run on the same object.
    /// Throws: common::Error when the directory cannot be created.
    /// Contract: create() on an already-created set asserts.
    void create();

    /// Purpose: append one row to bucket `index` — one heap-file insert,
    /// its page pinned and released inside the call (no pins held across
    /// the caller's next()).
    /// Throws: common::Error from the heap file.
    /// Contract: `index` must be below bucket_count(); create() must have
    /// run; both are asserts.
    void write(std::size_t index, const tuple::Tuple& row);

    /// Purpose: start scanning bucket `index` from its first record.
    /// Contract: `index` must be below bucket_count() and create() must
    /// have run — asserts.
    void scan_start(std::size_t index);

    /// Purpose: the next record of the scan started by the last
    /// scan_start, or nullopt at the bucket's end; further calls keep
    /// answering nullopt until a fresh scan_start restarts.
    /// Throws: common::Error from a corrupt bucket page.
    std::optional<tuple::Tuple> scan_next();

    /// Purpose: erase every bucket file — the query ended, normally or
    /// through an error. Closes each open file, then removes it.
    /// Idempotent.
    void erase_all();

private:
    std::string path(std::size_t index) const;

    bufman::BufferManager* pool_;
    std::filesystem::path directory_;
    std::string label_;
    const schema::TableSchema* schema_;
    std::size_t bucket_count_;
    std::vector<std::unique_ptr<heapfile::HeapFile>> files_; // create()'s buckets
    std::optional<heapfile::HeapFileIterator> scan_;         // scan_start's cursor
    bool created_ = false;
};

}
