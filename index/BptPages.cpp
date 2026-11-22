#include "index/BptPages.h"

#include "common/Error.h"

#include <cassert>
#include <cstring>

namespace bptree {

namespace {

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, 4);
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, 4);
}

}

// leaf_capacity — the number of data entries one leaf page holds for a
// key width: the 4096-byte block minus the count/prev/next header, one
// entry per (key + rid) slot.
//
// Parameters:
//   key_width: the codec's encoded key width in bytes (4 for Integer, 8
//     for Double, the column's declared size for String).
//
// Returns: the entry capacity; always at least 2d with d = capacity / 2.
std::size_t leaf_capacity(std::size_t key_width) {
    return (kBlockSize - 12) / (key_width + 8);
}

// separator_capacity — the number of (key, child) pairs one separator
// page holds for a key width: the block minus the count and first-child
// fields.
//
// Parameters:
//   key_width: the codec's encoded key width in bytes.
//
// Returns: the pair capacity; always at least 2d with d = capacity / 2.
std::size_t separator_capacity(std::size_t key_width) {
    return (kBlockSize - 8) / (key_width + 4);
}

// --- BptLeafPage -------------------------------------------------------------

// BptLeafPage::bind — attaches the view to a page buffer (a pinned
// frame's memory) at the given key width. Non-owning: the buffer must
// outlive the view.
//
// Parameters:
//   buffer: the page bytes; not zeroed, not owned.
//   key_width: the encoded key width; fixes the entry stride and the
//     capacity.
//
// Returns: nothing.
void BptLeafPage::bind(char* buffer, std::size_t key_width) {
    buffer_ = buffer;
    key_width_ = key_width;
    capacity_ = leaf_capacity(key_width);
}

// BptLeafPage::format — initializes the page as a valid empty leaf.
//
// Parameters:
//   prev_leaf: the left neighbour's block id, or kNoPage.
//   next_leaf: the right neighbour's block id, or kNoPage.
//
// Returns: nothing; the page reads back as count 0 with the given links.
void BptLeafPage::format(std::uint32_t prev_leaf, std::uint32_t next_leaf) {
    std::memset(buffer_, 0, kBlockSize);
    write_u32(buffer_, kPrevOffset, prev_leaf);
    write_u32(buffer_, kNextOffset, next_leaf);
}

// BptLeafPage::attach — validates the stored state: the count field must
// not claim more entries than the page can hold. Corrupted bytes or a
// foreign page format surface here.
//
// Parameters: none.
//
// Returns: nothing. Throws common::Error naming the count and the
// capacity when the count is impossible.
void BptLeafPage::attach() const {
    if (count() > capacity_) {
        throw common::Error("leaf page holds " + std::to_string(count()) +
                            " entries; at most " + std::to_string(capacity_) +
                            " fit a " + std::to_string(kBlockSize) + "-byte page");
    }
}

// BptLeafPage::count — the number of entries on the page.
//
// Parameters: none.
//
// Returns: the stored entry count (the 32-bit field, widened).
std::size_t BptLeafPage::count() const { return read_u32(buffer_, kCountOffset); }

// BptLeafPage::write_count — the single funnel every count mutation goes
// through, so the on-disk width stays 32 bits.
//
// Parameters:
//   count: the new entry count; must fit the capacity.
//
// Returns: nothing.
void BptLeafPage::write_count(std::size_t count) {
    write_u32(buffer_, kCountOffset, static_cast<std::uint32_t>(count));
}

// BptLeafPage::prev_leaf / set_prev_leaf / next_leaf / set_next_leaf —
// the doubly linked chain's links: a neighbour's block id, or kNoPage at
// either end of the chain.
//
// Parameters:
//   page_id (setters): the new neighbour's block id, or kNoPage.
//
// Returns: the stored block id (getters); nothing (setters).
std::uint32_t BptLeafPage::prev_leaf() const { return read_u32(buffer_, kPrevOffset); }
void BptLeafPage::set_prev_leaf(std::uint32_t page_id) { write_u32(buffer_, kPrevOffset, page_id); }
std::uint32_t BptLeafPage::next_leaf() const { return read_u32(buffer_, kNextOffset); }
void BptLeafPage::set_next_leaf(std::uint32_t page_id) { write_u32(buffer_, kNextOffset, page_id); }

// BptLeafPage::lower_bound — the first entry index whose key compares >=
// `key` (binary search over the sorted entries). The parameter is a RAW
// pointer on purpose: encoded keys hold embedded NULs, and a
// char*->string conversion would truncate at the first one.
//
// Parameters:
//   key: the encoded target key, exactly codec.width() bytes.
//   codec: the key codec; supplies the comparison order.
//
// Returns: an index in 0..count() — the insertion point that keeps the
// page sorted; count() when every key compares less.
std::size_t BptLeafPage::lower_bound(const char* key, const isam::KeyCodec& codec) const {
    std::size_t lo = 0;
    std::size_t hi = count();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (codec.compare(buffer_ + entry_offset(mid), key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// BptLeafPage::get_entry — the data entry at slot i in its in-flight form
// (encoded key string + rid).
//
// Parameters:
//   i: the entry position. Contract: i < count().
//
// Returns: the (key, rid) pair.
idx::DataEntry BptLeafPage::get_entry(std::size_t i) const {
    assert(i < count() && "leaf entry index out of range");
    idx::DataEntry entry;
    entry.key.assign(buffer_ + entry_offset(i), key_width_);
    std::memcpy(&entry.rid.page_id, buffer_ + entry_offset(i) + key_width_, 4);
    std::memcpy(&entry.rid.slot, buffer_ + entry_offset(i) + key_width_ + 4, 4);
    return entry;
}

// BptLeafPage::insert_entry — sorted insert: the entry lands at its
// lower_bound position, the tail memmoves to keep order, and the count
// grows by one.
//
// Parameters:
//   entry: the (encoded key, rid) pair; the key is codec.width() bytes.
//   codec: the key codec; supplies the comparison order.
//
// Returns: nothing. Contract: the page is not full — a full leaf is the
// caller's to split first.
void BptLeafPage::insert_entry(const idx::DataEntry& entry, const isam::KeyCodec& codec) {
    const std::size_t n = count();
    assert(n < capacity_ && "insert into a full leaf; the caller splits first");
    const std::size_t pos = lower_bound(entry.key.data(), codec);
    std::memmove(buffer_ + entry_offset(pos + 1), buffer_ + entry_offset(pos),
                 (n - pos) * (key_width_ + 8));
    char* raw = buffer_ + entry_offset(pos);
    std::memcpy(raw, entry.key.data(), key_width_);
    std::memcpy(raw + key_width_, &entry.rid.page_id, 4);
    std::memcpy(raw + key_width_ + 4, &entry.rid.slot, 4);
    write_count(n + 1);
}

// BptLeafPage::erase_entry — removes the entry at position i: the entries
// after it shift one slot forward, so the page stays contiguous and
// sorted. Unlike the ISAM leaf, no gap remains.
//
// Parameters:
//   i: the caller-found position. Contract: i < count().
//
// Returns: nothing.
void BptLeafPage::erase_entry(std::size_t i) {
    const std::size_t n = count();
    assert(i < n && "leaf entry index out of range");
    std::memmove(buffer_ + entry_offset(i), buffer_ + entry_offset(i + 1),
                 (n - i - 1) * (key_width_ + 8));
    write_count(n - 1);
}

// BptLeafPage::move_tail_to — the split's right half: every entry from
// position `from` to the end moves into the destination view, and this
// page's count drops to `from`. The chain links are the caller's to set.
//
// Parameters:
//   from: the first moved position (the split point: d on an insert
//     split — this page keeps d).
//   destination: a bound, freshly formatted leaf view to fill.
//
// Returns: nothing; afterwards both pages are contiguous and sorted.
void BptLeafPage::move_tail_to(std::size_t from, BptLeafPage& destination) {
    const std::size_t moved = count() - from;
    std::memcpy(destination.buffer_ + kEntriesOffset, buffer_ + entry_offset(from),
                moved * (key_width_ + 8));
    destination.write_count(moved);
    write_count(from);
}

// --- BptSeparatorPage --------------------------------------------------------

// BptSeparatorPage::bind — attaches the view to a page buffer at the
// given key width. Non-owning: the buffer must outlive the view.
//
// Parameters:
//   buffer: the page bytes; not zeroed, not owned.
//   key_width: the encoded key width; fixes the pair stride and the
//     capacity.
//
// Returns: nothing.
void BptSeparatorPage::bind(char* buffer, std::size_t key_width) {
    buffer_ = buffer;
    key_width_ = key_width;
    capacity_ = separator_capacity(key_width);
}

// BptSeparatorPage::format — initializes the page as a valid empty
// separator: zero pairs, first child = kNoPage. A separator is never
// stored empty: the caller overwrites both fields.
//
// Parameters: none.
//
// Returns: nothing.
void BptSeparatorPage::format() {
    std::memset(buffer_, 0, kBlockSize);
    write_u32(buffer_, kFirstChildOffset, idx::kNoPage);
}

// BptSeparatorPage::attach — validates the stored state: the count field
// must not claim more pairs than fit. Which caste a page is, the caller
// knows from the level; attach only checks the arithmetic.
//
// Parameters: none.
//
// Returns: nothing. Throws common::Error naming the count and the
// capacity when the count is impossible.
void BptSeparatorPage::attach() const {
    if (count() > capacity_) {
        throw common::Error("separator page holds " + std::to_string(count()) +
                            " separators; at most " + std::to_string(capacity_) +
                            " fit a " + std::to_string(kBlockSize) + "-byte page");
    }
}

// BptSeparatorPage::count — the number of (key, child) pairs; the page
// routes count() + 1 children (the pairs plus the first child).
//
// Parameters: none.
//
// Returns: the stored pair count.
std::size_t BptSeparatorPage::count() const { return read_u32(buffer_, kCountOffset); }

// BptSeparatorPage::write_count — the single funnel every pair-count
// mutation goes through.
//
// Parameters:
//   count: the new pair count; must fit the capacity.
//
// Returns: nothing.
void BptSeparatorPage::write_count(std::size_t count) {
    write_u32(buffer_, kCountOffset, static_cast<std::uint32_t>(count));
}

// BptSeparatorPage::child — the child pointer pair i routes to: pair i =
// (K_{i+1}, P_{i+1}), so child(i) is P_{i+1} — the subtree holding keys
// in [K_{i+1}, K_{i+2}).
//
// Parameters:
//   i: the pair position. Contract: i < count().
//
// Returns: the child block id stored with pair i.
std::uint32_t BptSeparatorPage::child(std::size_t i) const {
    return read_u32(buffer_, entry_offset(i) + key_width_);
}

// BptSeparatorPage::separator_key — pair i's encoded key bytes: the
// separator K_{i+1}, which equals the first key of the child it routes to
// (the exactness invariant). A raw pointer on purpose — encoded keys hold
// embedded NULs.
//
// Parameters:
//   i: the pair position. Contract: i < count().
//
// Returns: a pointer into the page buffer, codec.width() bytes, valid
// while the frame stays pinned.
const char* BptSeparatorPage::separator_key(std::size_t i) const {
    return buffer_ + entry_offset(i);
}

// BptSeparatorPage::set_separator_key — rewrites pair i's key bytes: the
// exactness maintenance after a redistribution or a first-key erase.
//
// Parameters:
//   i: the pair position. Contract: i < count().
//   key: the new encoded separator, exactly codec.width() bytes.
//
// Returns: nothing.
void BptSeparatorPage::set_separator_key(std::size_t i, const char* key) {
    std::memcpy(buffer_ + entry_offset(i), key, key_width_);
}

// BptSeparatorPage::entry — pair i as an IndexEntry: the named form of
// the (encoded key, child) pair, for callers that pass the pair along.
//
// Parameters:
//   i: the pair position. Contract: i < count().
//
// Returns: the (key, child) pair copied out of the page.
idx::IndexEntry BptSeparatorPage::entry(std::size_t i) const {
    idx::IndexEntry entry;
    entry.key.assign(buffer_ + entry_offset(i), key_width_);
    entry.child = child(i);
    return entry;
}

// BptSeparatorPage::first_child / set_first_child — the leftmost child
// P0: the subtree holding keys < K1, the one child no pair names.
//
// Parameters:
//   page_id (setter): the new first child's block id.
//
// Returns: the first child's block id (getter; kNoPage on a formatted
// but empty page); nothing (setter).
std::uint32_t BptSeparatorPage::first_child() const {
    return read_u32(buffer_, kFirstChildOffset);
}

void BptSeparatorPage::set_first_child(std::uint32_t page_id) {
    write_u32(buffer_, kFirstChildOffset, page_id);
}

// BptSeparatorPage::append — adds one (key, child) pair after the
// current last pair, in ascending key order (the bottom-up build feeds
// sorted pairs).
//
// Parameters:
//   index_entry: the (encoded key, child) pair to add.
//
// Returns: nothing. Contract: the page is not full.
void BptSeparatorPage::append(const idx::IndexEntry& index_entry) {
    insert_pair_after(count(), index_entry);
}

// BptSeparatorPage::routing_child_index — the lecture's routing rule in
// one place (slides 10 and 24): the POSITION of the child after the LAST
// separator <= `key` — 0 routes to the first child (key < K1), i to the
// child pair i-1 routes to (K_i <= key < K_{i+1}), count() to the last
// child (key >= Km).
//
// Parameters:
//   key: the encoded target key, exactly codec.width() bytes (raw
//     pointer — embedded NULs).
//   codec: the key codec; supplies the comparison order.
//
// Returns: a child position in 0..count(); child_at resolves it.
std::size_t BptSeparatorPage::routing_child_index(const char* key,
                                                  const isam::KeyCodec& codec) const {
    // The count of separators <= key: always a valid position 0..count().
    std::size_t lo = 0;
    std::size_t hi = count();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (codec.compare(separator_key(mid), key) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// BptSeparatorPage::child_at — resolves a routing position to a block id:
// position 0 is the first child, position p > 0 is child(p - 1).
//
// Parameters:
//   position: a child position. Contract: position <= count().
//
// Returns: the child's block id.
std::uint32_t BptSeparatorPage::child_at(std::size_t position) const {
    return position == 0 ? first_child() : child(position - 1);
}

// BptSeparatorPage::insert_pair_after — seats one (key, child) pair so the
// new child sits right after the child currently at position
// `after_child` (0 pushes every pair right). The tail memmoves to keep
// order; the count grows by one.
//
// Parameters:
//   after_child: the child position the new pair follows (the descent's
//     routing position).
//   pair: the (encoded key, child) pair; the key equals the new child's
//     subtree minimum (copy-up exactness).
//
// Returns: nothing. Contract: the page is not full — a full separator is
// the caller's to split first.
void BptSeparatorPage::insert_pair_after(std::size_t after_child, const idx::IndexEntry& pair) {
    const std::size_t n = count();
    assert(n < capacity_ && "insert into a full separator; the caller splits first");
    // Pair i holds (K_{i+1}, P_{i+1}): the new child at position
    // after_child + 1 means the new pair lands at pair index after_child.
    const std::size_t pos = after_child;
    std::memmove(buffer_ + entry_offset(pos + 1), buffer_ + entry_offset(pos),
                 (n - pos) * (key_width_ + 4));
    char* raw = buffer_ + entry_offset(pos);
    std::memcpy(raw, pair.key.data(), key_width_);
    std::memcpy(raw + key_width_, &pair.child, 4);
    write_count(n + 1);
}

// BptSeparatorPage::erase_pair — drops pair i (the merge's separator
// removal: the pair's key and child pointer both go, the child having
// been re-homed by the caller). The tail memmoves to close the gap.
//
// Parameters:
//   i: the pair position. Contract: i < count().
//
// Returns: nothing.
void BptSeparatorPage::erase_pair(std::size_t i) {
    const std::size_t n = count();
    assert(i < n && "separator pair index out of range");
    std::memmove(buffer_ + entry_offset(i), buffer_ + entry_offset(i + 1),
                 (n - i - 1) * (key_width_ + 4));
    write_count(n - 1);
}

// BptSeparatorPage::move_tail_to — the split's right half: every pair from
// position `from` to the end moves into the destination view, and this
// page's count drops to `from`. The destination's FIRST CHILD is the
// caller's to set (the carried middle pair's child on an insert split).
//
// Parameters:
//   from: the first moved pair position.
//   destination: a bound, freshly formatted separator view to fill.
//
// Returns: nothing; afterwards both pages hold ascending pairs.
void BptSeparatorPage::move_tail_to(std::size_t from, BptSeparatorPage& destination) {
    const std::size_t moved = count() - from;
    std::memcpy(destination.buffer_ + kPairsOffset, buffer_ + entry_offset(from),
                moved * (key_width_ + 4));
    destination.write_count(moved);
    write_count(from);
}

}
