#pragma once

// IsamIndex — a single-column ISAM index over one heap file, stored in
// its own file of 4096-byte blocks. Structure, in two castes:
//
//   index pages    separator pages, built bottom-up from the leaves'
//                  first keys during bulk load, FROZEN from then on —
//                  no insert ever touches one (that is what makes this
//                  ISAM and not a B+tree)
//   data pages     leaf pages holding sorted (key, rid) entries, chained
//                  left to right for range scans, plus per-leaf overflow
//                  chains that absorb post-build inserts
//
// Block 0 is a header page: it identifies the file (magic, key type and
// width, table and column names) and holds the root, first leaf, level
// count, entry count, and highest allocated block. The index never reads
// or writes heap records; callers hand in values and row addresses, and
// verify() judges rids through a caller-supplied occupancy callback.
//
// One encoding: keys move through KeyCodec only, entries through the
// IndexPages views only. Reachable failures (a foreign file, a missing
// entry, I/O) throw common::Error; operating on a closed index is a
// contract violation and asserts.

#include "bufpool/BufferManager.h"
#include "bufpool/PinnedPage.h"
#include "index/IndexPages.h"
#include "index/KeyCodec.h"
#include "schema/TableSchema.h"
#include "tuple/Column.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace isam {

// One end of a range: a key value plus whether the bound includes it.
struct RangeBound {
    tuple::Column value;
    bool inclusive = true;
};

class IsamIndex {
public:
    // Binds the index to a buffer manager, a table's schema, and the one
    // column it orders. The schema must outlive the index.
    IsamIndex(bufman::BufferManager& manager, const schema::TableSchema& schema,
              std::size_t column_index);

    bool is_open() const { return open_; }
    std::size_t entry_count() const { return entry_count_; }
    const KeyCodec& codec() const { return codec_; }

    // Creates a fresh, empty index file at `path` (header block only)
    // and leaves it open. Refuses an existing non-empty file and a column
    // with no record-able key width.
    void create(const std::string& path);

    // Bulk build: a fresh file filled from `entries` in one pass —
    // sorted in memory, leaves filled left to right, separator levels
    // built bottom-up until one page (the root) holds a level. Leaves
    // the index open.
    void build(const std::string& path,
               const std::vector<std::pair<tuple::Column, Rid>>& entries);

    // Opens an existing index file, validating magic, key type and
    // width, and the table and column names against the bound schema.
    void open(const std::string& path);

    // Flushes and closes this index's file through the shared manager.
    void close();

    // All entries whose key equals `key` (duplicates are contiguous by
    // key).
    std::vector<Rid> lookup(const tuple::Column& key);

    // Entries with lo <= key <= hi (bounds optional, inclusivity per
    // bound), in ascending key order, duplicates adjacent. Walks the
    // leaf chain from the first leaf that may hold `lo`.
    std::vector<Rid> range(const std::optional<RangeBound>& lo,
                           const std::optional<RangeBound>& hi);

    // Enters one (key, rid) pair: descends the frozen separators to the
    // owning leaf and inserts there, chaining a new overflow page when
    // the leaf and its overflow are full. Never restructures anything
    // above the leaves.
    void insert(const tuple::Column& key, Rid rid);

    // Removes the one entry equal to (`key`, `rid`): descends to the
    // owning leaf and walks its overflow chain, closing the found
    // position with a within-page tail-shift. Separators and the leaf
    // chain are never touched, and an overflow page emptied by erases
    // stays in its chain. A pair the index does not hold throws with
    // nothing changed.
    void erase(const tuple::Column& key, Rid rid);

    // Walks every entry (leaf chain, then each leaf's overflow chain)
    // and reports findings: entries whose rid fails `occupied`
    // (dangling), pages whose entries are out of order, and a total that
    // disagrees with the header.
    idx::VerifyResult verify(const std::function<bool(const Rid&)>& occupied);

private:
    using Leaf = bufman::PinnedView<LeafPage>;
    using Overflow = bufman::PinnedView<OverflowPage>;
    using Separator = bufman::PinnedView<SeparatorPage>;

    void write_header();

    // Pins a block and attaches the requested view.
    template <class View>
    bufman::PinnedView<View> pin(std::uint32_t block);
    // Allocates the next block id beyond `last_block_`, bound and
    // formatted as the requested view.
    template <class View, class... FormatArgs>
    bufman::PinnedView<View> allocate(FormatArgs... format_args);

    // Descends from the root to the leaf that may hold `key_bytes` and
    // returns its block id (kNoPage for an empty index). The recursive
    // form (design D2) takes one separator per call; `levels_remaining`
    // counts separator pages still to cross.
    std::uint32_t descend(const char* key_bytes);
    std::uint32_t descend_from(std::uint32_t block, std::uint32_t levels_remaining,
                               const char* key_bytes);

    // Lookup walkers (design D3): the leaf chain and, per leaf, its
    // overflow chain. Each worker pins one page, acts, unpins, then
    // recurses — no frame is held across a call.
    void scan_overflow_chain(std::uint32_t block, const std::string& key_bytes,
                             std::vector<Rid>& out);
    void scan_leaf_chain(std::uint32_t block, const std::string& key_bytes,
                         std::vector<Rid>& out);

    // Range-scan workers (design D3): one leaf merged with its overflow
    // runs, then the leaf chain until the upper bound says stop.
    struct RangeWalk {
        const std::string* lo_bytes = nullptr;  // null: no lower bound
        bool lo_inclusive = true;
        const std::string* hi_bytes = nullptr;  // null: no upper bound
        bool hi_inclusive = true;
        std::vector<Rid>* out = nullptr;
    };
    void gather_overflow_chain(std::uint32_t block, std::vector<DataEntry>& out);
    std::uint32_t emit_leaf_range(std::uint32_t block, RangeWalk& walk);
    void scan_leaf_range(std::uint32_t block, RangeWalk& walk);

    // Verify workers (design D7): walk the leaf chain carrying the
    // previous leaf's last key, walking each leaf's overflow chain on
    // the way.
    struct VerifyWalk {
        const std::function<bool(const Rid&)>& occupied;
        idx::VerifyResult& result;
    };
    void verify_overflow_chain(std::uint32_t block, VerifyWalk& walk);
    void verify_leaf_chain(std::uint32_t block, const std::optional<std::string>& previous_last_key,
                           VerifyWalk& walk);

    // Insert worker (design D6): the first overflow page of a chain with
    // room for one entry (kNoPage when all are full).
    std::uint32_t find_overflow_with_room(std::uint32_t block);
    void insert_into_overflow(std::uint32_t leaf_block, std::uint32_t existing_chain,
                              const DataEntry& entry);

    // Build workers (design D5): fill the leaves, link their chain, and
    // grow separator levels from each page's (first key, block).
    std::vector<IndexEntry> build_leaves(const std::vector<DataEntry>& sorted);
    void link_leaf_chain(const std::vector<IndexEntry>& level, std::size_t index);
    void build_separator_level(std::vector<IndexEntry> level);

    bufman::BufferManager& manager_;
    const schema::TableSchema& schema_;
    std::size_t column_index_ = 0;
    KeyCodec codec_;

    bufman::File file_{};
    std::string path_;
    bool open_ = false;

    std::uint32_t root_ = kNoPage;
    std::uint32_t first_leaf_ = kNoPage;
    std::uint32_t levels_ = 1;
    std::uint32_t last_block_ = kNoPage;
    std::size_t entry_count_ = 0;
};

}
