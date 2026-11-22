#pragma once

// BptPages — the B+tree's own page views, mirroring the SlottedPage
// discipline the rest of the engine uses: non-owning views bound to a
// pinned frame's buffer, format/attach the only writers of structural
// fields, attach validating stored counts against capacity. The module
// owns its views outright (the tear-out rule): nothing here is borrowed
// from ISAM's IndexPages at the type level.
//
// Two castes, per the lecture:
//
//   BptLeafPage      data entries — sorted (key, rid) arrays, chained
//                    DOUBLY (prev and next, slides 22 and 35)
//   BptSeparatorPage index entries — the |P0| K1 | P1 | ... | Km | Pm |
//                    form (slide 10), with the routing rule in one place

#include "index/Entries.h"
#include "index/KeyCodec.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bptree {

// 4096-byte block size, taken from the buffer pool's frame size.
constexpr std::size_t kBlockSize = 4096;

// One end of a range: a key value plus whether the bound includes it.
struct RangeBound {
    tuple::Column value;
    bool inclusive = true;
};

// Leaf page: count u32 + prev-leaf u32 + next-leaf u32 + entries.
std::size_t leaf_capacity(std::size_t key_width);
// Separator page: count u32 + first-child u32 + (key + child) pairs.
std::size_t separator_capacity(std::size_t key_width);

// Both views follow one contract: indexes passed in are in range, and
// inserts go only into a page that is not full (callers split first).
// Violations assert; attach throws on a stored count that cannot be.

// --- BptLeafPage: a data page of the B+tree ---------------------------------

class BptLeafPage {
public:
    void bind(char* buffer, std::size_t key_width);

    // format: zero entries, chain links as given.
    void format(std::uint32_t prev_leaf, std::uint32_t next_leaf);
    // Stored count must fit the capacity: corrupted or foreign bytes
    // throw, exactly like SlottedPage::attach.
    void attach() const;

    std::size_t count() const;
    bool full() const { return count() >= capacity_; }

    std::uint32_t prev_leaf() const;
    void set_prev_leaf(std::uint32_t page_id);
    std::uint32_t next_leaf() const;
    void set_next_leaf(std::uint32_t page_id);

    // First entry index whose key is >= `key`. `key` is a raw pointer to
    // codec.width() encoded bytes: encoded keys hold embedded NULs, and a
    // char*->string conversion would truncate at the first one.
    std::size_t lower_bound(const char* key, const isam::KeyCodec& codec) const;
    idx::DataEntry get_entry(std::size_t i) const;
    // Sorted insert: memmoves the tail to keep order.
    void insert_entry(const idx::DataEntry& entry, const isam::KeyCodec& codec);
    // Removes entry i; the entries after it shift one slot forward.
    void erase_entry(std::size_t i);
    // Moves entries [from, count()) into a freshly formatted `destination`
    // (a split's right half); this page keeps the first `from`.
    void move_tail_to(std::size_t from, BptLeafPage& destination);

private:
    void write_count(std::size_t count);
    std::size_t entry_offset(std::size_t i) const {
        return kEntriesOffset + i * (key_width_ + 8);
    }

    static constexpr std::size_t kCountOffset = 0;
    static constexpr std::size_t kPrevOffset = 4;
    static constexpr std::size_t kNextOffset = 8;
    static constexpr std::size_t kEntriesOffset = 12;

    char* buffer_ = nullptr;
    std::size_t key_width_ = 0;
    std::size_t capacity_ = 0;
};

// --- BptSeparatorPage: an index page (internal) ------------------------------
//
// Layout |P0| K1 | P1 | ... | Km | Pm|: the first child, then pairs. Pair
// i holds (K_{i+1}, P_{i+1}), so child(i) is the subtree with keys in
// [K_{i+1}, K_{i+2}). Children are addressed by POSITION 0..count():
// position 0 is P0, position p > 0 is child(p - 1).

class BptSeparatorPage {
public:
    void bind(char* buffer, std::size_t key_width);

    // format: zero separators, first child = kNoPage.
    void format();
    void attach() const;

    std::size_t count() const;
    bool full() const { return count() >= capacity_; }

    std::uint32_t child(std::size_t i) const;      // child routed by pair i
    std::uint32_t child_at(std::size_t position) const;
    const char* separator_key(std::size_t i) const;
    void set_separator_key(std::size_t i, const char* key);
    idx::IndexEntry entry(std::size_t i) const;
    std::uint32_t first_child() const;
    void set_first_child(std::uint32_t page_id);

    // Appends one pair after the current last one (keys ascending).
    void append(const idx::IndexEntry& index_entry);

    // The lecture's routing rule in one place (slides 10 and 24): P0
    // leads to keys < K1, Pi to keys in [Ki, Ki+1), Pm to keys >= Km —
    // the position of the child after the LAST separator <= `key`.
    std::size_t routing_child_index(const char* key, const isam::KeyCodec& codec) const;

    // Inserts one pair so its child sits right after the child currently
    // at position `after_child`.
    void insert_pair_after(std::size_t after_child, const idx::IndexEntry& pair);
    // Drops pair i; the caller has already re-homed its child.
    void erase_pair(std::size_t i);
    // Moves pairs [from, count()) into a freshly formatted `destination`;
    // the caller sets the destination's first child.
    void move_tail_to(std::size_t from, BptSeparatorPage& destination);

private:
    void write_count(std::size_t count);
    std::size_t entry_offset(std::size_t i) const {
        return kPairsOffset + i * (key_width_ + 4);
    }

    static constexpr std::size_t kCountOffset = 0;
    static constexpr std::size_t kFirstChildOffset = 4;
    static constexpr std::size_t kPairsOffset = 8;

    char* buffer_ = nullptr;
    std::size_t key_width_ = 0;
    std::size_t capacity_ = 0;
};

}
