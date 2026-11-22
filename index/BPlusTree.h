#pragma once

// BPlusTree — a single-column B+tree index over one heap file, stored in
// its own file of 4096-byte blocks. The lecture's dynamic tree index
// (after Ramakrishna & Gehrke): sorted leaf pages chained DOUBLY hold the
// data entries; separator pages in the |P0| K1 | P1 | ... | Km | Pm| form
// route descents; every node keeps the order-d shape (d <= m <= 2d keys,
// the root exempt). Splits on insert and borrow-or-merge on erase keep
// every leaf at the same depth; height changes only at the root.
//
// Block 0 is a header page: it identifies the file (magic BPT1, key type
// and width, table and column names) and holds the root, first leaf,
// level count, entry count, highest allocated block, and the head of the
// free-block list (pages freed by merges are reused before new blocks
// are allocated). The index never reads or writes heap records; callers
// hand in values and row addresses, and verify() judges rids through a
// caller-supplied occupancy callback.
//
// One encoding: keys move through KeyCodec only, entries through the
// BptPages views only. Reachable failures (a foreign file, a missing
// entry, I/O) throw common::Error; operating on a closed index is a
// contract violation and asserts.
//
// The recursion contract (the lecture's own algorithm shapes): every
// public operation is a thin wrapper; the work happens in the private
// recursive auxiliaries below, whose extra parameters carry state up the
// unwinding exactly as the lecture's (newNode, newKey) out-params do.

#include "bufpool/BufferManager.h"
#include "bufpool/PinnedPage.h"
#include "index/BptPages.h"
#include "index/Entries.h"
#include "index/KeyCodec.h"
#include "schema/TableSchema.h"
#include "tuple/Column.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bptree {

class BPlusTree {
public:
    // Binds the index to a buffer manager, a table's schema, and the one
    // column it orders. The schema must outlive the index.
    BPlusTree(bufman::BufferManager& manager,
              const schema::TableSchema& schema, std::size_t column_index);

    bool is_open() const { return open_; }
    std::size_t entry_count() const { return entry_count_; }
    const isam::KeyCodec& codec() const { return codec_; }

    // Creates a fresh, empty index file at `path` (header block only)
    // and leaves it open. Refuses an existing file that already holds
    // blocks, and a column with no record-able key width.
    void create(const std::string& path);

    // Bulk build: a fresh file filled from `entries` in one pass —
    // sorted in memory, leaves filled left to right linking the doubly
    // linked chain, separator levels built bottom-up until one page (the
    // root) holds a level. Leaves the index open.
    void build(const std::string& path,
               const std::vector<std::pair<tuple::Column, idx::Rid>>& entries);

    // Opens an existing index file, validating the magic, key type and
    // width, and the table and column names against the bound schema. A
    // file of another index kind (an ISAM file, say) is refused by name.
    void open(const std::string& path);

    // Flushes and closes this index's file through the shared manager.
    void close();

    // All entries whose key equals `key` (the order within the run is
    // unspecified). Boundary duplicates left of the landing leaf are
    // gathered through the prev links.
    std::vector<idx::Rid> lookup(const tuple::Column& key);

    // Entries with lo <= key <= hi (bounds optional, inclusivity per
    // bound), in ascending key order, duplicates adjacent.
    std::vector<idx::Rid> range(const std::optional<RangeBound>& lo,
                                const std::optional<RangeBound>& hi);

    // Enters one (key, rid) pair: descends by the lecture's routing,
    // inserts at the leaf, and splits full pages upward — a full leaf
    // keeps d entries and gives d+1 to a new leaf (its smallest key
    // copied up), a full separator page moves its middle key up, and a
    // root split allocates a new root (the height grows by one).
    void insert(const tuple::Column& key, idx::Rid rid);

    // Removes the one entry equal to (`key`, `rid`): the exact entry is
    // located first, then removed; underflowed pages borrow from a
    // sibling or merge with one, cascading upward, and a root left with
    // a single child is dropped. A pair the index does not hold throws
    // with nothing changed.
    void erase(const tuple::Column& key, idx::Rid rid);

    // Walks the whole tree and reports findings: a page reachable twice
    // or not at all, key-order violations within a page or across the
    // leaf chain, an occupancy violation of the order rule (other than
    // the root), a separator that does not equal the first key of the
    // child it routes to, leaves at different depths, entries whose rid
    // fails `occupied` (dangling), and a total that disagrees with the
    // header.
    idx::VerifyResult verify(const std::function<bool(const idx::Rid&)>& occupied);

private:
    using Leaf = bufman::PinnedView<BptLeafPage>;
    using Separator = bufman::PinnedView<BptSeparatorPage>;

    void write_header();

    // Pins a block and attaches the requested view.
    Leaf pin_leaf(std::uint32_t block);
    Separator pin_separator(std::uint32_t block);
    // Claims a block for a new page — the free list's head when a merge
    // freed one, otherwise one past `last_block_` — bound and formatted
    // (a leaf with no chain links).
    Leaf new_leaf();
    Separator new_separator();
    bufman::PinnedPage alloc_next_block();
    // Pushes a merged-away block onto the free list (the caller holds it
    // pinned and dirty through the surgery).
    void chain_freed(std::uint32_t block, char* buffer);

    // The order-d bounds (design D3): d = floor(capacity / 2), so a full
    // node holds exactly 2d entries.
    std::size_t min_leaf_entries() const;
    std::size_t max_leaf_entries() const;
    std::size_t min_separator_keys() const;
    std::size_t max_separator_keys() const;

    std::string encode(const tuple::Column& key) const { return codec_.encode(key); }
    [[noreturn]] void no_entry(const std::string& key_bytes, idx::Rid rid) const;

    // --- the recursion contract (design D1) --------------------------------

    // The lecture's treeSearch (slide 24): route by one separator's keys,
    // recurse; one level left is the landing leaf.
    std::uint32_t tree_search(std::uint32_t block, std::uint32_t levels_remaining,
                              const std::string& key_bytes);

    // The upward carrier of the lecture's insertAux (slides 34-37): the
    // new right page and the separator that routes to it.
    struct Split {
        std::string separator_key;
        std::uint32_t new_block = idx::kNoPage;
    };
    Split insert_aux(std::uint32_t block, std::uint32_t levels_remaining,
                     const std::string& key_bytes, idx::Rid rid);
    Split insert_leaf(std::uint32_t block, const std::string& key_bytes, idx::Rid rid);

    // The upward carrier of the erase cascade (slides 38-49): whether
    // this page now holds fewer than d entries, and whether its
    // subtree's smallest key changed (so a parent separator needs
    // rewriting).
    struct EraseUp {
        bool underflow = false;
        bool first_changed = false;
        std::string new_first;
    };
    EraseUp erase_rec(std::uint32_t block, std::uint32_t levels_remaining,
                      const std::string& key_bytes, idx::Rid rid);
    EraseUp erase_leaf(std::uint32_t block, const std::string& key_bytes, idx::Rid rid);
    void drop_single_child_roots();

    // The underflow repair at a parent: which children are involved.
    struct Family {
        std::uint32_t parent = idx::kNoPage;
        std::size_t child_index = 0;           // the underflowed child's position
        std::uint32_t child = idx::kNoPage;
        std::uint32_t left = idx::kNoPage;     // kNoPage when absent
        std::uint32_t right = idx::kNoPage;    // kNoPage when absent
        bool leaves = false;                   // the children are leaves
        std::size_t child_count = 0;
    };
    EraseUp fix_child_underflow(std::uint32_t block, std::uint32_t levels_remaining,
                                std::size_t child_index);
    std::size_t quick_count(std::uint32_t block, bool leaf);
    EraseUp borrow_from_left(const Family& family);
    EraseUp borrow_from_right(const Family& family);
    EraseUp merge_child(const Family& family);
    // The crossed-subtree erase's simple correct repair: rebuild from
    // the leaf chain.
    void rebuild_from_leaves();

    // Walks the leaf chain leftward from `landing` while the previous
    // leaf's last key can still equal the target (the prev-step that
    // closes boundary duplicates); returns the run's leftmost leaf.
    std::uint32_t run_start(std::uint32_t landing, const std::string& key_bytes);
    // Scans the equal-key run left to right from `start`, appending every
    // entry whose key equals the target.
    void gather_run(const std::string& key_bytes, std::uint32_t start,
                    std::vector<idx::Rid>& out);
    // Locates the exact (key, rid) within the run: the holding leaf and
    // the entry's position in it.
    struct RunPosition {
        std::uint32_t leaf = idx::kNoPage;
        std::size_t pos = 0;
    };
    RunPosition locate_in_run(const std::string& key_bytes, idx::Rid rid, std::uint32_t start);

    // One verify walk: structure, occupancy, separator exactness, depth
    // uniformity, per-page order. Returns the subtree's smallest key
    // (nullopt for an empty subtree).
    struct VerifyWalk {
        const std::function<bool(const idx::Rid&)>& occupied;
        idx::VerifyResult& result;
        std::uint32_t min_depth = 0;
        std::uint32_t max_depth = 0;
        std::vector<char> seen = {};
    };
    std::optional<std::string> verify_rec(std::uint32_t block, std::uint32_t depth,
                                          bool is_root, VerifyWalk& walk);
    void verify_free_list(VerifyWalk& walk);
    void verify_leaf_chain(VerifyWalk& walk);

    bufman::BufferManager& manager_;
    const schema::TableSchema& schema_;
    std::size_t column_index_ = 0;
    isam::KeyCodec codec_;

    bufman::File file_{};
    std::string path_;
    bool open_ = false;

    std::uint32_t root_ = idx::kNoPage;
    std::uint32_t first_leaf_ = idx::kNoPage;
    std::uint32_t levels_ = 1;
    std::uint32_t last_block_ = idx::kNoPage;
    std::uint32_t freed_head_ = idx::kNoPage;
    std::size_t entry_count_ = 0;
};

}
