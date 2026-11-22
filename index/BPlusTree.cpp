#include "index/BPlusTree.h"

#include "common/Error.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>

namespace bptree {

namespace {

constexpr std::uint32_t kBptMagic = 0x31545042u; // "BPT1"

// Header block field offsets.
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kKeyTypeOffset = 4;
constexpr std::size_t kKeyWidthOffset = 8;
constexpr std::size_t kRootOffset = 12;
constexpr std::size_t kFirstLeafOffset = 16;
constexpr std::size_t kLevelsOffset = 20;
constexpr std::size_t kLastBlockOffset = 24;
constexpr std::size_t kFreedHeadOffset = 28;
constexpr std::size_t kEntryCountOffset = 32;
constexpr std::size_t kNameLengthOffset = 40;
constexpr std::size_t kNameOffset = 44;
constexpr std::size_t kMaxTableNameLength = 200;

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

std::string rid_text(idx::Rid rid) {
    return "(" + std::to_string(rid.page_id) + ", " + std::to_string(rid.slot) + ")";
}

// Splits `total` items into the fewest pages of at most `max_per_page`,
// sized evenly (the first total % pages get one extra) — so a bulk build
// keeps every page within the order rule's d..2d.
std::vector<std::size_t> even_groups(std::size_t total, std::size_t max_per_page) {
    const std::size_t pages = (total + max_per_page - 1) / max_per_page;
    std::vector<std::size_t> sizes(pages, total / pages);
    for (std::size_t p = 0; p < total % pages; ++p) {
        ++sizes[p];
    }
    return sizes;
}

// Every page a borrow or merge touches, pinned up front in ascending
// block order (the TUP2 list-surgery discipline), so the mutation that
// follows is pure memory writes that cannot fail midway. Every page is
// released dirty.
class Surgery {
public:
    Surgery(bufman::BufferManager& manager, bufman::File& file,
            std::vector<std::uint32_t> blocks, std::size_t key_width)
        : key_width_(key_width) {
        std::sort(blocks.begin(), blocks.end());
        blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
        for (const std::uint32_t block : blocks) {
            pins_.emplace(block, bufman::PinnedPage(manager, file, block));
        }
        for (auto& [block, pin] : pins_) {
            pin.mark_dirty();
        }
    }

    template <class View>
    View view(std::uint32_t block) {
        View page;
        page.bind(data(block), key_width_);
        return page;
    }

    char* data(std::uint32_t block) { return pins_.at(block).data(); }

private:
    std::size_t key_width_;
    std::map<std::uint32_t, bufman::PinnedPage> pins_;
};

}

// BPlusTree::BPlusTree — binds the index to a buffer manager, a table's
// schema, and the one column it orders, deriving the key codec from the
// column's type and width.
//
// Parameters:
//   manager: the session's buffer manager; must outlive the index.
//   schema: the table's schema; must outlive the index (the header stores
//     its name, and open re-checks the column).
//   column_index: the ordered column's position in the schema.
//
// Returns: nothing; an out-of-range column leaves the codec invalid and
// create refuses it by name.
BPlusTree::BPlusTree(bufman::BufferManager& manager,
                     const schema::TableSchema& schema,
                     std::size_t column_index)
    : manager_(manager), schema_(schema), column_index_(column_index) {
    // An out-of-range column leaves the codec invalid; create refuses it.
    if (column_index < schema.column_count()) {
        const schema::ColumnSchema& column = schema.column_info(column_index);
        const std::size_t string_width =
            column.column_type() == tuple::ColumnType::String ? column.column_size() : 0;
        codec_ = isam::KeyCodec(column.column_type(), string_width);
    }
}

// --- header ------------------------------------------------------------------

// BPlusTree::write_header — persists the in-memory header state (magic,
// key identification, root, first leaf, levels, last block, free-list
// head, entry count, table and column names) into block 0.
//
// Parameters: none.
//
// Returns: nothing (block 0 dirty in the pool). Throws common::Error when
// the table name exceeds the header's capacity or block 0 cannot be
// pinned.
void BPlusTree::write_header() {
    const std::string& name = schema_.table_name();
    if (name.size() > kMaxTableNameLength) {
        throw common::Error("table name is " + std::to_string(name.size()) +
                            " bytes; at most " + std::to_string(kMaxTableNameLength) +
                            " fit the header page");
    }
    const std::string& column_name = schema_.column_info(column_index_).column_name();
    bufman::PinnedPage header(manager_, file_, 0);
    char* buffer = header.data();
    write_u32(buffer, kMagicOffset, kBptMagic);
    write_u32(buffer, kKeyTypeOffset, static_cast<std::uint32_t>(codec_.key_type()));
    write_u32(buffer, kKeyWidthOffset, static_cast<std::uint32_t>(codec_.width()));
    write_u32(buffer, kRootOffset, root_);
    write_u32(buffer, kFirstLeafOffset, first_leaf_);
    write_u32(buffer, kLevelsOffset, levels_);
    write_u32(buffer, kLastBlockOffset, last_block_);
    write_u32(buffer, kFreedHeadOffset, freed_head_);
    const std::uint64_t count = entry_count_;
    std::memcpy(buffer + kEntryCountOffset, &count, 8);
    write_u32(buffer, kNameLengthOffset, static_cast<std::uint32_t>(name.size()));
    std::memcpy(buffer + kNameOffset, name.data(), name.size());
    const std::size_t column_offset = kNameOffset + name.size();
    write_u32(buffer, column_offset, static_cast<std::uint32_t>(column_name.size()));
    std::memcpy(buffer + column_offset + 4, column_name.data(), column_name.size());
    header.mark_dirty();
}

// --- create / open / close ---------------------------------------------------

// BPlusTree::create — creates a fresh, empty index file at `path` (a
// single header block, root kNoPage, level count 1) and leaves it open.
// The first insert grows the first leaf. An existing empty file is
// refused too (unlike ISAM — the B+tree keeps one bootstrap path).
//
// Parameters:
//   path: the file to create.
//
// Returns: nothing. Throws common::Error when the codec is invalid or the
// file already holds blocks. Contract: the index is not open.
void BPlusTree::create(const std::string& path) {
    assert(!open_ && "create on an open index");
    const std::string context = "cannot create \"" + path + "\"";
    if (!codec_.valid()) {
        throw common::Error(context + ": the column has no record-able key width");
    }
    file_ = manager_.open_file(path);
    try {
        if (const std::uint64_t blocks = bufman::block_count(file_); blocks > 0) {
            throw common::Error(context + ": the file already holds " +
                                std::to_string(blocks) + " block(s)");
        }
        bufman::PinnedPage::fresh(manager_, file_, 0); // the header block
        root_ = idx::kNoPage;
        first_leaf_ = idx::kNoPage;
        levels_ = 1;
        entry_count_ = 0;
        last_block_ = 0;
        freed_head_ = idx::kNoPage;
        path_ = path;
        write_header();
    } catch (...) {
        manager_.close_file(file_);
        throw;
    }
    open_ = true;
}

// BPlusTree::open — opens an existing index file, validating the kind
// first (a foreign file — an ISAM index, a heap file — is refused by
// name), then the key type and width, then the table and column names
// against the bound schema. Loads the header state into memory.
//
// Parameters:
//   path: the file to open.
//
// Returns: nothing. Throws common::Error naming the mismatch ("not a
// b+tree index file", the key width, "index file is for
// <table>.<column>"). Contract: the index is not open.
void BPlusTree::open(const std::string& path) {
    assert(!open_ && "open on an open index");
    file_ = manager_.open_file(path);
    try {
        const bufman::PinnedPage header(manager_, file_, 0);
        const char* buffer = header.data();
        const std::uint32_t name_length = read_u32(buffer, kNameLengthOffset);
        const std::size_t column_offset = kNameOffset + name_length;
        if (name_length > kMaxTableNameLength ||
            read_u32(buffer, column_offset) > kMaxTableNameLength ||
            read_u32(buffer, kMagicOffset) != kBptMagic) {
            throw common::Error("not a b+tree index file");
        }
        const std::uint32_t key_width = read_u32(buffer, kKeyWidthOffset);
        if (read_u32(buffer, kKeyTypeOffset) != static_cast<std::uint32_t>(codec_.key_type()) ||
            key_width != codec_.width()) {
            throw common::Error("index file holds " + std::to_string(key_width) +
                                "-byte keys of another type; the column needs " +
                                std::to_string(codec_.width()));
        }
        const std::string table_name(buffer + kNameOffset, name_length);
        const std::string column_name(buffer + column_offset + 4,
                                      read_u32(buffer, column_offset));
        if (table_name != schema_.table_name() ||
            column_name != schema_.column_info(column_index_).column_name()) {
            throw common::Error("index file is for " + table_name + "." + column_name);
        }
        root_ = read_u32(buffer, kRootOffset);
        first_leaf_ = read_u32(buffer, kFirstLeafOffset);
        levels_ = read_u32(buffer, kLevelsOffset);
        last_block_ = read_u32(buffer, kLastBlockOffset);
        freed_head_ = read_u32(buffer, kFreedHeadOffset);
        std::uint64_t count = 0;
        std::memcpy(&count, buffer + kEntryCountOffset, 8);
        entry_count_ = static_cast<std::size_t>(count);
    } catch (...) {
        manager_.close_file(file_);
        throw;
    }
    path_ = path;
    open_ = true;
}

// BPlusTree::close — flushes and closes this index's file through the
// shared buffer manager (the dirty pages reach disk here).
//
// Parameters: none.
//
// Returns: nothing. Throws common::Error when the flush fails. Contract:
// the index is open.
void BPlusTree::close() {
    assert(open_ && "close on a closed index");
    manager_.close_file(file_);
    open_ = false;
}

// --- pin and allocation helpers -----------------------------------------------

// BPlusTree::pin_leaf / pin_separator — pins a block and binds the
// requested view over it, validating on attach. The pin is released when
// the returned guard leaves scope.
//
// Parameters:
//   block: the block id to pin.
//
// Returns: the pinned page with its attached view. Throws common::Error
// when the pin or the attach fails.
BPlusTree::Leaf BPlusTree::pin_leaf(std::uint32_t block) {
    bufman::PinnedPage pin(manager_, file_, block);
    BptLeafPage page;
    page.bind(pin.data(), codec_.width());
    page.attach();
    return Leaf{std::move(pin), page};
}

BPlusTree::Separator BPlusTree::pin_separator(std::uint32_t block) {
    bufman::PinnedPage pin(manager_, file_, block);
    BptSeparatorPage page;
    page.bind(pin.data(), codec_.width());
    page.attach();
    return Separator{std::move(pin), page};
}

// BPlusTree::alloc_next_block — claims the next data block: a block freed
// by a merge is reused first (its own first word chains the free list),
// otherwise one past `last_block_` is allocated fresh.
//
// Parameters: none.
//
// Returns: the claimed block, pinned and dirty; the caller formats it.
// Throws common::Error when the block cannot be pinned or allocated.
bufman::PinnedPage BPlusTree::alloc_next_block() {
    // A block freed by a merge is reused before anything new is
    // allocated; its own first word chains the free list.
    if (freed_head_ != idx::kNoPage) {
        bufman::PinnedPage pin(manager_, file_, freed_head_);
        freed_head_ = read_u32(pin.data(), 0);
        pin.mark_dirty();
        return pin;
    }
    const std::uint32_t block = last_block_ == idx::kNoPage ? 1 : last_block_ + 1;
    bufman::PinnedPage pin = bufman::PinnedPage::fresh(manager_, file_, block);
    last_block_ = block;
    return pin;
}

// BPlusTree::new_leaf / new_separator — alloc_next_block plus the view,
// formatted: a leaf with no chain links, or a separator with no pairs.
//
// Parameters: none.
//
// Returns: the new page, pinned and dirty. Throws as alloc_next_block.
BPlusTree::Leaf BPlusTree::new_leaf() {
    bufman::PinnedPage pin = alloc_next_block();
    BptLeafPage page;
    page.bind(pin.data(), codec_.width());
    page.format(idx::kNoPage, idx::kNoPage);
    return Leaf{std::move(pin), page};
}

BPlusTree::Separator BPlusTree::new_separator() {
    bufman::PinnedPage pin = alloc_next_block();
    BptSeparatorPage page;
    page.bind(pin.data(), codec_.width());
    page.format();
    return Separator{std::move(pin), page};
}

// BPlusTree::chain_freed — pushes a merged-away block onto the free list:
// the block's own first word is rewritten to the current head and the
// head moves to this block.
//
// Parameters:
//   block: the vanishing block's id.
//   buffer: its pinned frame's memory; the caller holds the pin, dirty,
//     through the surgery.
//
// Returns: nothing; freed_head_ advances to `block`.
void BPlusTree::chain_freed(std::uint32_t block, char* buffer) {
    write_u32(buffer, 0, freed_head_);
    freed_head_ = block;
}

// BPlusTree::min_leaf_entries / max_leaf_entries / min_separator_keys /
// max_separator_keys — the order-d bounds (design D3): d is half the
// page capacity, rounded down, and a full node holds exactly 2d — so a
// full node plus one arrival splits at named boundaries (slides 35, 37).
// The root is exempt from the minimum.
//
// Parameters: none.
//
// Returns: d (the min forms) or 2d (the max forms) for that caste.
std::size_t BPlusTree::min_leaf_entries() const {
    return leaf_capacity(codec_.width()) / 2;
}

std::size_t BPlusTree::max_leaf_entries() const {
    return min_leaf_entries() * 2;
}

std::size_t BPlusTree::min_separator_keys() const {
    return separator_capacity(codec_.width()) / 2;
}

std::size_t BPlusTree::max_separator_keys() const {
    return min_separator_keys() * 2;
}

// BPlusTree::no_entry — the one refusal for a (key, rid) pair the index
// does not hold.
//
// Parameters:
//   key_bytes: the encoded key.
//   rid: the row address that was not found.
//
// Returns: never. Throws common::Error("no index entry for key ... and
// rid (...)").
void BPlusTree::no_entry(const std::string& key_bytes, idx::Rid rid) const {
    throw common::Error("no index entry for key " + codec_.display(key_bytes.data()) +
                        " and rid " + rid_text(rid));
}

// --- build (design D7) ---------------------------------------------------------

// BPlusTree::build — bulk build (design D7): one in-memory sort, then
// leaves sized evenly (every page within d..2d by construction) filling
// left to right and linking the doubly linked chain, then separator
// levels built bottom-up from each page's first key until one page — the
// root — holds a level. An empty entry list is a valid empty index.
//
// Parameters:
//   path: the file to create and fill (must not exist non-empty).
//   entries: the (value, rid) pairs; unsorted input is fine.
//
// Returns: nothing (the index is open, entry_count_ = the pair count).
// Throws common::Error as create does, or when a value cannot be encoded.
void BPlusTree::build(const std::string& path,
                      const std::vector<std::pair<tuple::Column, idx::Rid>>& entries) {
    create(path);
    if (entries.empty()) {
        return; // the first insert grows the first leaf
    }

    // The codec's byte order IS the value order.
    std::vector<idx::DataEntry> sorted;
    sorted.reserve(entries.size());
    for (const auto& [value, rid] : entries) {
        sorted.push_back(idx::DataEntry{encode(value), rid});
    }
    std::sort(sorted.begin(), sorted.end(),
              [this](const idx::DataEntry& a, const idx::DataEntry& b) {
                  return codec_.compare(a.key.data(), b.key.data()) < 0;
              });

    // The leaf level. A fresh file allocates blocks consecutively, so
    // each leaf's next link is its own id + 1. Each leaf's (first key,
    // block) feeds the first separator level.
    std::vector<idx::IndexEntry> level;
    const std::vector<std::size_t> leaf_sizes = even_groups(sorted.size(), max_leaf_entries());
    std::size_t index = 0;
    for (std::size_t p = 0; p < leaf_sizes.size(); ++p) {
        Leaf leaf = new_leaf();
        leaf->set_prev_leaf(level.empty() ? idx::kNoPage : level.back().child);
        leaf->set_next_leaf(p + 1 < leaf_sizes.size() ? leaf.id() + 1 : idx::kNoPage);
        for (std::size_t i = 0; i < leaf_sizes[p]; ++i) {
            leaf->insert_entry(sorted[index + i], codec_);
        }
        level.push_back(idx::IndexEntry{sorted[index].key, leaf.id()});
        index += leaf_sizes[p];
    }
    first_leaf_ = level.front().child;

    // Each separator level groups the pages below it: a group's first
    // page is the separator's first child, every later page contributes
    // its (first key, block) pair.
    std::uint32_t separator_levels = 0;
    while (level.size() > 1) {
        std::vector<idx::IndexEntry> next_level;
        std::size_t position = 0;
        for (const std::size_t group : even_groups(level.size(), max_separator_keys())) {
            Separator page = new_separator();
            page->set_first_child(level[position].child);
            for (std::size_t i = position + 1; i < position + group; ++i) {
                page->append(level[i]);
            }
            next_level.push_back(idx::IndexEntry{level[position].key, page.id()});
            position += group;
        }
        level = std::move(next_level);
        ++separator_levels;
    }
    root_ = level.front().child;
    levels_ = 1 + separator_levels;
    entry_count_ = sorted.size();
    write_header();
}

// --- search --------------------------------------------------------------------

// BPlusTree::tree_search — the lecture's treeSearch (slides 17, 24): pin
// one separator, take the child after the LAST separator <= the key,
// recurse; `levels_remaining == 1` means `block` IS the landing leaf.
//
// Parameters:
//   block: the page to route from (the root on the first call).
//   levels_remaining: the levels from here down, the leaf level included.
//   key_bytes: the encoded target key, exactly codec.width() bytes.
//
// Returns: the landing leaf's block id (kNoPage for an empty tree).
// Throws common::Error when a page cannot be pinned.
std::uint32_t BPlusTree::tree_search(std::uint32_t block, std::uint32_t levels_remaining,
                                     const std::string& key_bytes) {
    if (levels_remaining <= 1 || block == idx::kNoPage) {
        return block;
    }
    std::uint32_t child = idx::kNoPage;
    {
        const Separator page = pin_separator(block);
        child = page->child_at(page->routing_child_index(key_bytes.data(), codec_));
    }
    return tree_search(child, levels_remaining - 1, key_bytes);
}

// BPlusTree::run_start — the equal-key run's leftmost leaf: walks left
// from the landing leaf while the previous leaf's LAST key EQUALS the
// target — the copy-up convention can leave boundary duplicates in the
// left sibling's tail (design D4). Leaves are globally ordered, so
// equality is the exact test; >= would over-walk.
//
// Parameters:
//   landing: the tree_search landing leaf.
//   key_bytes: the encoded target key.
//
// Returns: the run's first leaf (the landing itself when no left
// neighbour's tail equals the target). Throws on a pin failure.
std::uint32_t BPlusTree::run_start(std::uint32_t landing, const std::string& key_bytes) {
    std::uint32_t current = landing;
    while (current != idx::kNoPage) {
        const std::uint32_t prev = pin_leaf(current)->prev_leaf();
        if (prev == idx::kNoPage) {
            return current;
        }
        const Leaf prev_leaf = pin_leaf(prev);
        const std::size_t n = prev_leaf->count();
        if (n == 0 ||
            codec_.compare(prev_leaf->get_entry(n - 1).key.data(), key_bytes.data()) != 0) {
            return current;
        }
        current = prev;
    }
    return current;
}

// BPlusTree::gather_run — collects every entry whose key equals the
// target, scanning leaves left to right from `start`: the run continues
// while a leaf's last key still compares equal. Lookup's answer walk.
//
// Parameters:
//   key_bytes: the encoded target key.
//   start: the run's leftmost leaf (run_start's result).
//   out: receives the matching rids, appended in leaf order.
//
// Returns: nothing. Throws on a pin failure.
void BPlusTree::gather_run(const std::string& key_bytes, std::uint32_t start,
                           std::vector<idx::Rid>& out) {
    for (std::uint32_t current = start; current != idx::kNoPage;) {
        const Leaf leaf = pin_leaf(current);
        const std::size_t n = leaf->count();
        if (n == 0) {
            return;
        }
        for (std::size_t i = leaf->lower_bound(key_bytes.data(), codec_); i < n; ++i) {
            const idx::DataEntry entry = leaf->get_entry(i);
            if (codec_.compare(entry.key.data(), key_bytes.data()) != 0) {
                break;
            }
            out.push_back(entry.rid);
        }
        // The run continues into the next leaf only while this one ends
        // on the target key.
        if (codec_.compare(leaf->get_entry(n - 1).key.data(), key_bytes.data()) != 0) {
            return;
        }
        current = leaf->next_leaf();
    }
}

// BPlusTree::locate_in_run — finds the exact (key, rid) entry within the
// equal-key run: the target can sit left of the landing leaf (boundary
// duplicates) or right of it (a run spanning leaves).
//
// Parameters:
//   key_bytes: the encoded target key.
//   rid: the exact row address to find.
//   start: the run's leftmost leaf.
//
// Returns: the holding leaf and the entry's position in it. Throws
// common::Error ("no index entry for key ... and rid ...") when the run
// does not hold the pair.
BPlusTree::RunPosition BPlusTree::locate_in_run(const std::string& key_bytes, idx::Rid rid,
                                                std::uint32_t start) {
    for (std::uint32_t current = start; current != idx::kNoPage;) {
        const Leaf leaf = pin_leaf(current);
        const std::size_t n = leaf->count();
        if (n == 0) {
            break;
        }
        for (std::size_t i = leaf->lower_bound(key_bytes.data(), codec_); i < n; ++i) {
            const idx::DataEntry entry = leaf->get_entry(i);
            if (codec_.compare(entry.key.data(), key_bytes.data()) != 0) {
                break;
            }
            if (entry.rid == rid) {
                return RunPosition{current, i};
            }
        }
        if (codec_.compare(leaf->get_entry(n - 1).key.data(), key_bytes.data()) != 0) {
            break;
        }
        current = leaf->next_leaf();
    }
    no_entry(key_bytes, rid);
}

// BPlusTree::lookup — all entries whose key equals `key`: descend by the
// lecture's routing, step left to the run's start, gather forward.
//
// Parameters:
//   key: the value to encode and search for.
//
// Returns: the matching rids (empty for no match or an empty index).
// Throws common::Error when the value cannot be encoded or a pin fails.
// Contract: the index is open.
std::vector<idx::Rid> BPlusTree::lookup(const tuple::Column& key) {
    assert(open_ && "lookup on a closed index");
    std::vector<idx::Rid> out;
    if (root_ == idx::kNoPage) {
        return out;
    }
    const std::string key_bytes = encode(key);
    gather_run(key_bytes, run_start(tree_search(root_, levels_, key_bytes), key_bytes), out);
    return out;
}

// BPlusTree::range — entries with lo <= key <= hi (bounds optional,
// inclusivity per bound), ascending with duplicates adjacent: the walk
// starts at the lower bound's run start (or the chain's first leaf) and
// stops at the first key past the upper bound.
//
// Parameters:
//   lo: the optional lower bound (value + inclusive).
//   hi: the optional upper bound (value + inclusive).
//
// Returns: the matching rids in key order. Throws common::Error when a
// bound cannot be encoded or a pin fails. Contract: the index is open.
std::vector<idx::Rid> BPlusTree::range(const std::optional<RangeBound>& lo,
                                       const std::optional<RangeBound>& hi) {
    assert(open_ && "range on a closed index");
    std::vector<idx::Rid> out;
    if (root_ == idx::kNoPage) {
        return out;
    }
    const std::string lo_bytes = lo ? encode(lo->value) : std::string();
    const std::string hi_bytes = hi ? encode(hi->value) : std::string();
    const auto below_lo = [&](const std::string& key) {
        const int cmp = codec_.compare(key.data(), lo_bytes.data());
        return cmp < 0 || (cmp == 0 && !lo->inclusive);
    };
    const auto above_hi = [&](const std::string& key) {
        const int cmp = codec_.compare(key.data(), hi_bytes.data());
        return cmp > 0 || (cmp == 0 && !hi->inclusive);
    };

    std::uint32_t current =
        lo ? run_start(tree_search(root_, levels_, lo_bytes), lo_bytes) : first_leaf_;
    while (current != idx::kNoPage) {
        const Leaf leaf = pin_leaf(current);
        for (std::size_t i = 0; i < leaf->count(); ++i) {
            const idx::DataEntry entry = leaf->get_entry(i);
            if (lo && below_lo(entry.key)) {
                continue;
            }
            if (hi && above_hi(entry.key)) {
                return out;
            }
            out.push_back(entry.rid);
        }
        current = leaf->next_leaf();
    }
    return out;
}

// --- insert --------------------------------------------------------------------

// BPlusTree::insert — the lecture's insert (slide 34) as a wrapper: an
// empty index grows its first leaf; otherwise insert_aux descends, and a
// split of the old root puts a new root above both halves — the height
// grows by exactly one. entry_count_ and the header follow.
//
// Parameters:
//   key: the value to encode and insert.
//   rid: the row address the entry maps to.
//
// Returns: nothing; the entry is findable by lookup and range
// immediately. Throws common::Error when the value cannot be encoded or a
// pin/allocation fails. Contract: the index is open.
void BPlusTree::insert(const tuple::Column& key, idx::Rid rid) {
    assert(open_ && "insert on a closed index");
    const std::string key_bytes = encode(key);
    if (root_ == idx::kNoPage) {
        Leaf leaf = new_leaf();
        leaf->insert_entry(idx::DataEntry{key_bytes, rid}, codec_);
        root_ = leaf.id();
        first_leaf_ = leaf.id();
        levels_ = 1;
    } else if (const Split split = insert_aux(root_, levels_, key_bytes, rid);
               split.new_block != idx::kNoPage) {
        Separator new_root = new_separator();
        new_root->set_first_child(root_);
        new_root->append(idx::IndexEntry{split.separator_key, split.new_block});
        root_ = new_root.id();
        levels_ += 1;
    }
    entry_count_ += 1;
    write_header();
}

// BPlusTree::insert_aux — the lecture's insertAux (slides 34-37): at the
// leaf, insert or split; at a separator, route, recurse, then seat the
// carried (separator, new child) pair after the descended child —
// splitting this page too when it is full (the middle key moves up).
//
// Parameters:
//   block: the page to act on (the root on the first call).
//   levels_remaining: the levels from here down, the leaf level included.
//   key_bytes: the encoded key to insert.
//   rid: the row address.
//
// Returns: the upward carrier — empty when nothing split, otherwise the
// new right page and the separator that routes to it. Throws on a pin or
// allocation failure.
BPlusTree::Split BPlusTree::insert_aux(std::uint32_t block, std::uint32_t levels_remaining,
                                       const std::string& key_bytes, idx::Rid rid) {
    if (levels_remaining <= 1) {
        return insert_leaf(block, key_bytes, rid);
    }
    std::size_t child_index = 0;
    std::uint32_t child_block = idx::kNoPage;
    {
        const Separator page = pin_separator(block);
        child_index = page->routing_child_index(key_bytes.data(), codec_);
        child_block = page->child_at(child_index);
    }
    const Split child_split = insert_aux(child_block, levels_remaining - 1, key_bytes, rid);
    if (child_split.new_block == idx::kNoPage) {
        return Split{};
    }

    Separator page = pin_separator(block);
    page.mark_dirty();
    const idx::IndexEntry arriving{child_split.separator_key, child_split.new_block};
    if (page->count() < max_separator_keys()) {
        page->insert_pair_after(child_index, arriving);
        return Split{};
    }

    // Full (2d pairs): with the arriving pair there are 2d + 1. This page
    // keeps the first d, the middle pair moves up (slide 37) — its child
    // becomes the new page's first child — and the last d move over.
    std::vector<idx::IndexEntry> pairs;
    for (std::size_t i = 0; i < page->count(); ++i) {
        pairs.push_back(page->entry(i));
    }
    pairs.insert(pairs.begin() + static_cast<long>(child_index), arriving);
    const std::size_t middle_index = pairs.size() / 2;
    while (page->count() > 0) {
        page->erase_pair(page->count() - 1);
    }
    for (std::size_t i = 0; i < middle_index; ++i) {
        page->append(pairs[i]);
    }
    Separator fresh = new_separator();
    fresh->set_first_child(pairs[middle_index].child);
    for (std::size_t i = middle_index + 1; i < pairs.size(); ++i) {
        fresh->append(pairs[i]);
    }
    return Split{pairs[middle_index].key, fresh.id()};
}

// BPlusTree::insert_leaf — the leaf phase of insert_aux: room within 2d
// means a plain sorted insert; a full leaf (2d entries) splits per slide
// 35 — this page keeps d, the new leaf takes d+1, the chain links both
// ways, and the separator copied up is the new leaf's smallest key. The
// arriving entry takes its place in the correct half (equal keys go
// right, where the routing sends their duplicates).
//
// Parameters:
//   block: the landing leaf's block id.
//   key_bytes: the encoded key to insert.
//   rid: the row address.
//
// Returns: the upward carrier — filled when the leaf split. Throws on a
// pin or allocation failure.
BPlusTree::Split BPlusTree::insert_leaf(std::uint32_t block, const std::string& key_bytes,
                                        idx::Rid rid) {
    const idx::DataEntry arriving{key_bytes, rid};
    std::uint32_t old_next = idx::kNoPage;
    {
        Leaf leaf = pin_leaf(block);
        if (leaf->count() < max_leaf_entries()) {
            leaf->insert_entry(arriving, codec_);
            leaf.mark_dirty();
            return Split{};
        }
        old_next = leaf->next_leaf();
    }

    // The split's pages — the new leaf, this leaf, and the old next leaf
    // (its prev link changes) — are all pinned before the first write.
    Leaf fresh = new_leaf();
    Leaf leaf = pin_leaf(block);
    std::optional<Leaf> next;
    if (old_next != idx::kNoPage) {
        next = pin_leaf(old_next);
        next->mark_dirty();
        (*next)->set_prev_leaf(fresh.id());
    }
    leaf.mark_dirty();
    leaf->move_tail_to(min_leaf_entries(), fresh.view);
    fresh->set_prev_leaf(block);
    fresh->set_next_leaf(old_next);
    leaf->set_next_leaf(fresh.id());
    // Equal keys go right, where the routing sends their duplicates.
    Leaf& half = codec_.compare(key_bytes.data(), fresh->get_entry(0).key.data()) >= 0 ? fresh : leaf;
    half->insert_entry(arriving, codec_);
    return Split{fresh->get_entry(0).key, fresh.id()};
}

// --- erase -----------------------------------------------------------------------

// BPlusTree::erase — removes the one entry equal to (key, rid): locate
// first (a miss throws with nothing changed), then either the recursive
// path (the leaf erases, the parents rebalance, a single-child root is
// dropped) or — when the equal-key run crosses into another subtree —
// erase in place and let the leaf's own state decide whether the rebuild
// repair runs (the design's trade-off).
//
// Parameters:
//   key: the value to encode and erase.
//   rid: the exact row address to remove.
//
// Returns: nothing; lookups and ranges no longer return the rid and the
// count dropped by one. Throws common::Error when the index is empty or
// the pair is absent. Contract: the index is open.
void BPlusTree::erase(const tuple::Column& key, idx::Rid rid) {
    assert(open_ && "erase on a closed index");
    if (root_ == idx::kNoPage) {
        throw common::Error("index is empty; no entry to erase");
    }
    const std::string key_bytes = encode(key);
    const std::uint32_t landing = tree_search(root_, levels_, key_bytes);
    const RunPosition target = locate_in_run(key_bytes, rid, run_start(landing, key_bytes));
    if (target.leaf == landing) {
        erase_rec(root_, levels_, key_bytes, rid);
        drop_single_child_roots();
    } else {
        bool repair = false;
        {
            Leaf leaf = pin_leaf(target.leaf);
            leaf->erase_entry(target.pos);
            leaf.mark_dirty();
            repair = leaf->count() < min_leaf_entries() || (target.pos == 0 && leaf->count() > 0);
        }
        if (repair) {
            rebuild_from_leaves();
        }
    }
    entry_count_ -= 1;
    write_header();
}

// BPlusTree::drop_single_child_roots — a root left with a single child is
// dropped (slides 48-49); repeats while it applies, the height falling by
// one each time.
//
// Parameters: none.
//
// Returns: nothing. Throws on a pin failure.
void BPlusTree::drop_single_child_roots() {
    while (levels_ > 1) {
        Separator root = pin_separator(root_);
        if (root->count() != 0) {
            return;
        }
        const std::uint32_t child = root->first_child();
        chain_freed(root_, root.pin.data());
        root.mark_dirty();
        root_ = child;
        levels_ -= 1;
    }
}

// BPlusTree::erase_rec — the erase cascade (slides 38-49) as a carrier
// recursion: at the leaf, remove the exact entry; at a separator, route,
// recurse, then repair the child — redistribute or merge on underflow,
// rewrite the routing separator when the child's smallest key moved.
//
// Parameters:
//   block: the page to act on (the root on the first call).
//   levels_remaining: the levels from here down, the leaf level included.
//   key_bytes: the encoded key of the entry being erased.
//   rid: the exact row address.
//
// Returns: this page's own state for its parent — underflow, and whether
// its subtree's smallest key changed. Throws when the pre-located entry
// is missing (real corruption) or a pin fails.
BPlusTree::EraseUp BPlusTree::erase_rec(std::uint32_t block, std::uint32_t levels_remaining,
                                        const std::string& key_bytes, idx::Rid rid) {
    if (levels_remaining <= 1) {
        return erase_leaf(block, key_bytes, rid);
    }
    std::size_t child_index = 0;
    std::uint32_t child_block = idx::kNoPage;
    {
        const Separator page = pin_separator(block);
        child_index = page->routing_child_index(key_bytes.data(), codec_);
        child_block = page->child_at(child_index);
    }
    const EraseUp child_up = erase_rec(child_block, levels_remaining - 1, key_bytes, rid);
    if (child_up.underflow) {
        return fix_child_underflow(block, levels_remaining, child_index);
    }
    EraseUp up;
    if (child_up.first_changed) {
        if (child_index == 0) {
            // This page's own subtree minimum moved: propagate up.
            up.first_changed = true;
            up.new_first = child_up.new_first;
        } else {
            // The separator routing to the child must equal its minimum.
            Separator page = pin_separator(block);
            page->set_separator_key(child_index - 1, child_up.new_first.data());
            page.mark_dirty();
        }
    }
    return up;
}

// BPlusTree::erase_leaf — the leaf phase of erase_rec: find the exact
// (key, rid) entry in this leaf's equal-key run, erase it (the
// tail-shift closes the slot), and report the leaf's new state upward.
//
// Parameters:
//   block: the landing leaf's block id.
//   key_bytes: the encoded key.
//   rid: the exact row address.
//
// Returns: the leaf's state — underflow (count < d), and whether the
// first key moved (the parent's separator must follow the subtree
// minimum). Throws common::Error when the pair is not in this leaf.
BPlusTree::EraseUp BPlusTree::erase_leaf(std::uint32_t block, const std::string& key_bytes,
                                         idx::Rid rid) {
    Leaf leaf = pin_leaf(block);
    const std::size_t n = leaf->count();
    std::size_t found = n;
    for (std::size_t i = leaf->lower_bound(key_bytes.data(), codec_); i < n; ++i) {
        const idx::DataEntry entry = leaf->get_entry(i);
        if (codec_.compare(entry.key.data(), key_bytes.data()) != 0) {
            break;
        }
        if (entry.rid == rid) {
            found = i;
            break;
        }
    }
    if (found == n) {
        no_entry(key_bytes, rid);
    }
    const std::string old_first = leaf->get_entry(0).key;
    leaf->erase_entry(found);
    leaf.mark_dirty();

    EraseUp up;
    up.underflow = leaf->count() < min_leaf_entries();
    if (found == 0 && leaf->count() > 0) {
        const std::string new_first = leaf->get_entry(0).key;
        if (new_first != old_first) {
            up.first_changed = true;
            up.new_first = new_first;
        }
    } else if (leaf->count() == 0) {
        // An emptied leaf: the underflow repair decides the subtree's new
        // minimum; signal the change upward.
        up.first_changed = true;
        up.new_first = old_first;
    }
    return up;
}

// BPlusTree::fix_child_underflow — repairs a child holding fewer than d
// entries: redistribute with a sibling that can lend (left preferred,
// then right), else merge with a sibling and drop the separator. The
// surgery's pins are all taken before the first write.
//
// Parameters:
//   block: the parent page's block id.
//   levels_remaining: the levels from the parent down — the children are
//     leaves when this is 2.
//   child_index: the underflowed child's routing position in the parent.
//
// Returns: the parent's own post-repair state for its parent. Throws on a
// pin failure.
BPlusTree::EraseUp BPlusTree::fix_child_underflow(std::uint32_t block,
                                                  std::uint32_t levels_remaining,
                                                  std::size_t child_index) {
    Family family;
    family.parent = block;
    family.child_index = child_index;
    family.leaves = levels_remaining <= 2;
    {
        const Separator parent = pin_separator(block);
        family.child = parent->child_at(child_index);
        family.left = child_index > 0 ? parent->child_at(child_index - 1) : idx::kNoPage;
        family.right = child_index < parent->count() ? parent->child_at(child_index + 1)
                                                     : idx::kNoPage;
    }
    family.child_count = quick_count(family.child, family.leaves);

    const std::size_t d = family.leaves ? min_leaf_entries() : min_separator_keys();
    if (family.left != idx::kNoPage && quick_count(family.left, family.leaves) > d) {
        return borrow_from_left(family);
    }
    if (family.right != idx::kNoPage && quick_count(family.right, family.leaves) > d) {
        return borrow_from_right(family);
    }
    return merge_child(family);
}

// BPlusTree::quick_count — a page's occupancy for the repair decision
// (can the sibling lend?), read through a short-lived pin.
//
// Parameters:
//   block: the page to count.
//   leaf: which caste to bind (true = BptLeafPage, false =
//     BptSeparatorPage) — the caller knows it from the level.
//
// Returns: the page's count. Throws on a pin failure.
std::size_t BPlusTree::quick_count(std::uint32_t block, bool leaf) {
    return leaf ? pin_leaf(block)->count() : pin_separator(block)->count();
}

// BPlusTree::borrow_from_left — redistribution from the left sibling
// (slides 40-41 mirrored). A leaf child gains the sibling's last entry at
// its front. A separator child rotates through the parent: the sibling's
// last child becomes the child's first child, and the sibling's last
// separator — that child's subtree minimum — becomes both the child's
// new first key and the parent's separator.
//
// Parameters:
//   family: the parent, the underflowed child (child_index > 0), and its
//     left sibling, plus which caste the children are.
//
// Returns: the parent's state — underflow cleared. Throws on a pin
// failure (nothing written).
BPlusTree::EraseUp BPlusTree::borrow_from_left(const Family& family) {
    assert(family.child_index > 0 && "a left borrow needs a left sibling");
    Surgery surgery(manager_, file_, {family.parent, family.left, family.child}, codec_.width());
    auto parent = surgery.view<BptSeparatorPage>(family.parent);
    std::string boundary;
    if (family.leaves) {
        auto left = surgery.view<BptLeafPage>(family.left);
        auto child = surgery.view<BptLeafPage>(family.child);
        const idx::DataEntry moved = left.get_entry(left.count() - 1);
        left.erase_entry(left.count() - 1);
        child.insert_entry(moved, codec_);
        boundary = moved.key;
    } else {
        auto left = surgery.view<BptSeparatorPage>(family.left);
        auto child = surgery.view<BptSeparatorPage>(family.child);
        const idx::IndexEntry moved = left.entry(left.count() - 1);
        left.erase_pair(left.count() - 1);
        const std::uint32_t old_first_child = child.first_child();
        child.set_first_child(moved.child);
        child.insert_pair_after(0, idx::IndexEntry{moved.key, old_first_child});
        boundary = moved.key;
    }
    parent.set_separator_key(family.child_index - 1, boundary.data());
    return EraseUp{};
}

// BPlusTree::borrow_from_right — redistribution from the right sibling
// (slide 41). A leaf child gains the sibling's first entry at its end,
// and the parent's separator becomes the sibling's new minimum — "copy up
// 67 which is min key on remaining child"; an emptied child also rewrites
// (or propagates) its LEFT boundary. A separator child rotates: the
// sibling's first child moves over under the boundary separator's value.
//
// Parameters:
//   family: the parent, the underflowed child, its right sibling, the
//     caste, and the child's occupancy (0 makes the left boundary matter).
//
// Returns: the parent's state — underflow cleared, possibly a new first
// key to propagate. Throws on a pin failure (nothing written).
BPlusTree::EraseUp BPlusTree::borrow_from_right(const Family& family) {
    Surgery surgery(manager_, file_, {family.parent, family.child, family.right}, codec_.width());
    auto parent = surgery.view<BptSeparatorPage>(family.parent);
    EraseUp up;
    if (family.leaves) {
        auto child = surgery.view<BptLeafPage>(family.child);
        auto right = surgery.view<BptLeafPage>(family.right);
        const idx::DataEntry moved = right.get_entry(0);
        right.erase_entry(0);
        child.insert_entry(moved, codec_);
        parent.set_separator_key(family.child_index, right.get_entry(0).key.data());
        if (family.child_count == 0) {
            // The child was empty: its minimum is now the moved key.
            if (family.child_index > 0) {
                parent.set_separator_key(family.child_index - 1, moved.key.data());
            } else {
                up.first_changed = true;
                up.new_first = moved.key;
            }
        }
    } else {
        auto child = surgery.view<BptSeparatorPage>(family.child);
        auto right = surgery.view<BptSeparatorPage>(family.right);
        const std::string boundary_key = parent.entry(family.child_index).key;
        const std::uint32_t moved_child = right.first_child();
        right.set_first_child(right.entry(0).child);
        right.erase_pair(0);
        child.append(idx::IndexEntry{boundary_key, moved_child});
        if (right.count() > 0) {
            parent.set_separator_key(family.child_index, right.entry(0).key.data());
        }
    }
    return up;
}

// BPlusTree::merge_child — the merge (slides 42-43, 47): the underflowed
// child folds into its left sibling (preferred) or its right sibling
// folds into it; the parent drops the separator between them and the
// vanished page joins the free list.
//
// Parameters:
//   family: the parent, the underflowed child, its siblings (kNoPage
//     when absent), the caste, and the child's occupancy.
//
// Returns: the parent's post-merge state — its own underflow, and any
// first-key change. Throws on a pin failure (nothing written).
BPlusTree::EraseUp BPlusTree::merge_child(const Family& family) {
    const bool merge_left = family.left != idx::kNoPage;
    const std::uint32_t survivor = merge_left ? family.left : family.child;
    const std::uint32_t vanished = merge_left ? family.child : family.right;
    const std::size_t dropped_pair = merge_left ? family.child_index - 1 : family.child_index;

    // A leaf merge closes the chain around the vanished page, so its next
    // neighbour joins the surgery.
    const std::uint32_t chain_neighbour =
        family.leaves ? pin_leaf(vanished)->next_leaf() : idx::kNoPage;
    std::vector<std::uint32_t> blocks = {family.parent, survivor, vanished};
    if (chain_neighbour != idx::kNoPage) {
        blocks.push_back(chain_neighbour);
    }
    Surgery surgery(manager_, file_, blocks, codec_.width());
    auto parent = surgery.view<BptSeparatorPage>(family.parent);
    EraseUp up;
    if (family.leaves) {
        auto into = surgery.view<BptLeafPage>(survivor);
        auto from = surgery.view<BptLeafPage>(vanished);
        const bool survivor_was_empty = into.count() == 0;
        for (std::size_t i = 0; i < from.count(); ++i) {
            into.insert_entry(from.get_entry(i), codec_);
        }
        into.set_next_leaf(from.next_leaf());
        if (chain_neighbour != idx::kNoPage) {
            surgery.view<BptLeafPage>(chain_neighbour).set_prev_leaf(survivor);
        }
        // An emptied survivor that received entries has a new minimum.
        if (survivor_was_empty && into.count() > 0) {
            const std::string new_first = into.get_entry(0).key;
            if (family.child_index > 0) {
                parent.set_separator_key(family.child_index - 1, new_first.data());
            } else {
                up.first_changed = true;
                up.new_first = new_first;
            }
        }
    } else {
        // The boundary separator's value — the vanished page's subtree
        // minimum — routes the vanished page's first child; its pairs
        // follow.
        auto into = surgery.view<BptSeparatorPage>(survivor);
        auto from = surgery.view<BptSeparatorPage>(vanished);
        into.append(idx::IndexEntry{parent.entry(dropped_pair).key, from.first_child()});
        for (std::size_t i = 0; i < from.count(); ++i) {
            into.append(from.entry(i));
        }
    }
    parent.erase_pair(dropped_pair);
    chain_freed(vanished, surgery.data(vanished));
    up.underflow = parent.count() < min_separator_keys();
    return up;
}

// BPlusTree::rebuild_from_leaves — the crossed-subtree erase's simple
// correct repair: collect every entry by walking the leaf chain, close
// and remove the file, and re-run build. The rare case is traded for
// simplicity — see the design's trade-off.
//
// Parameters: none.
//
// Returns: nothing (the index is open and balanced again). Throws when
// the chain cannot be walked, the file cannot be reset, or the rebuild
// fails.
void BPlusTree::rebuild_from_leaves() {
    std::vector<std::pair<tuple::Column, idx::Rid>> entries;
    for (std::uint32_t current = first_leaf_; current != idx::kNoPage;) {
        const Leaf leaf = pin_leaf(current);
        for (std::size_t i = 0; i < leaf->count(); ++i) {
            const idx::DataEntry entry = leaf->get_entry(i);
            entries.emplace_back(codec_.decode(entry.key.data()), entry.rid);
        }
        current = leaf->next_leaf();
    }
    close();
    if (std::remove(path_.c_str()) != 0) {
        throw common::Error("cannot reset the index file for the rebuild");
    }
    const std::string path = path_;
    build(path, entries);
}

// --- verify ---------------------------------------------------------------------

// BPlusTree::verify — the full walk (design D1's verify contract): the
// tree (reachability, occupancy, order, separator exactness, leaf-depth
// uniformity, dangling rids), the free list (no cycles, no overlap),
// block coverage, the leaf chain (global order, total), and the header
// total. A clean tree reports no findings.
//
// Parameters:
//   occupied: the caller's rid judge — true when the rid resolves to an
//     occupied heap slot.
//
// Returns: the walked total and the named findings. Throws when a pin
// fails mid-walk. Contract: the index is open.
idx::VerifyResult BPlusTree::verify(const std::function<bool(const idx::Rid&)>& occupied) {
    assert(open_ && "verify on a closed index");
    idx::VerifyResult result;
    VerifyWalk walk{occupied, result};
    const std::uint32_t allocated = last_block_ == idx::kNoPage ? 0 : last_block_;
    walk.seen.assign(static_cast<std::size_t>(allocated) + 1, 0);
    auto& findings = result.findings;

    if (root_ != idx::kNoPage) {
        verify_rec(root_, 1, true, walk);
    }
    verify_free_list(walk);
    // Coverage: every allocated block is the header, tree-reachable, or
    // freed.
    for (std::uint32_t block = 1; block <= allocated; ++block) {
        if (walk.seen[block] == 0) {
            findings.push_back("block " + std::to_string(block) + " is reachable from nowhere");
        }
    }
    verify_leaf_chain(walk);
    if (walk.min_depth != walk.max_depth && walk.min_depth != 0) {
        findings.push_back("leaves sit at depths " + std::to_string(walk.min_depth) + " and " +
                           std::to_string(walk.max_depth));
    }
    if (result.entry_count != entry_count_) {
        findings.push_back("the walked total (" + std::to_string(result.entry_count) +
                           ") disagrees with the header (" + std::to_string(entry_count_) + ")");
    }
    return result;
}

// BPlusTree::verify_free_list — marks every freed block seen; a block
// beyond the file or reached twice is a finding.
//
// Parameters:
//   walk: the shared walk state (seen marks, findings).
//
// Returns: nothing. Throws on a pin failure.
void BPlusTree::verify_free_list(VerifyWalk& walk) {
    for (std::uint32_t current = freed_head_; current != idx::kNoPage;) {
        if (current >= walk.seen.size()) {
            walk.result.findings.push_back("free list names block " + std::to_string(current) +
                                           " beyond the file");
            return;
        }
        if (walk.seen[current] != 0) {
            walk.result.findings.push_back("free list reaches block " + std::to_string(current) +
                                           " twice");
            return;
        }
        walk.seen[current] = 1;
        current = read_u32(bufman::PinnedPage(manager_, file_, current).data(), 0);
    }
}

// BPlusTree::verify_leaf_chain — the leaf chain: order within and across
// leaves, no loops, and a total that matches the tree walk.
//
// Parameters:
//   walk: the shared walk state (seen marks, findings, the tree total).
//
// Returns: nothing. Throws on a pin failure.
void BPlusTree::verify_leaf_chain(VerifyWalk& walk) {
    auto& findings = walk.result.findings;
    std::size_t chain_total = 0;
    std::optional<std::string> previous_last;
    std::vector<char> on_chain(walk.seen.size(), 0);
    for (std::uint32_t current = first_leaf_; current != idx::kNoPage;) {
        if (current >= on_chain.size()) {
            findings.push_back("leaf chain names block " + std::to_string(current) +
                               " beyond the file");
            break;
        }
        if (on_chain[current] != 0) {
            findings.push_back("leaf chain loops at block " + std::to_string(current));
            break;
        }
        on_chain[current] = 1;
        const Leaf leaf = pin_leaf(current);
        const std::size_t n = leaf->count();
        for (std::size_t i = 1; i < n; ++i) {
            if (codec_.compare(leaf->get_entry(i - 1).key.data(),
                               leaf->get_entry(i).key.data()) > 0) {
                findings.push_back("leaf " + std::to_string(current) +
                                   " is out of order at entry " + std::to_string(i));
            }
        }
        if (previous_last && n > 0 &&
            codec_.compare(previous_last->data(), leaf->get_entry(0).key.data()) > 0) {
            findings.push_back("the leaf chain's key order breaks at " + std::to_string(current));
        }
        if (n > 0) {
            previous_last = leaf->get_entry(n - 1).key;
        }
        chain_total += n;
        current = leaf->next_leaf();
    }
    if (chain_total != walk.result.entry_count) {
        findings.push_back("the leaf chain's total (" + std::to_string(chain_total) +
                           ") disagrees with the tree walk (" +
                           std::to_string(walk.result.entry_count) + ")");
    }
}

// BPlusTree::verify_rec — one verify step: marks the page seen (twice is a
// finding), pins it, and at a separator recurses into every child — each
// separator must equal its child subtree's minimum (exactness) — while
// checking occupancy and order; at a leaf, checks order, judges every
// rid through `occupied`, and feeds the depth range. The header's level
// count, not the page bytes, decides which caste a page is.
//
// Parameters:
//   block: the page to verify.
//   depth: this page's depth (1 at the root; leaves sit at levels_).
//   is_root: the root's occupancy exemption.
//   walk: the shared walk state (seen marks, findings, totals, depths).
//
// Returns: the subtree's smallest key, or nullopt for an empty subtree.
// Throws on a pin failure.
std::optional<std::string> BPlusTree::verify_rec(std::uint32_t block, std::uint32_t depth,
                                                 bool is_root, VerifyWalk& walk) {
    auto& findings = walk.result.findings;
    const std::string id = std::to_string(block);
    if (block == idx::kNoPage) {
        return std::nullopt;
    }
    if (block >= walk.seen.size()) {
        findings.push_back("the tree names block " + id + " beyond the file");
        return std::nullopt;
    }
    if (walk.seen[block] != 0) {
        findings.push_back("block " + id + " is reachable twice");
        return std::nullopt;
    }
    walk.seen[block] = 1;

    if (depth < levels_) {
        const Separator page = pin_separator(block);
        const std::size_t n = page->count();
        if (!is_root && (n < min_separator_keys() || n > max_separator_keys())) {
            findings.push_back("separator " + id + " holds " + std::to_string(n) +
                               " keys, outside the order rule");
        }
        for (std::size_t i = 1; i < n; ++i) {
            if (codec_.compare(page->separator_key(i - 1), page->separator_key(i)) > 0) {
                findings.push_back("separator " + id + " is out of order at " + std::to_string(i));
            }
        }
        const std::optional<std::string> first_min =
            verify_rec(page->first_child(), depth + 1, false, walk);
        for (std::size_t i = 0; i < n; ++i) {
            const idx::IndexEntry pair = page->entry(i);
            const std::optional<std::string> child_min = verify_rec(pair.child, depth + 1, false, walk);
            if (!child_min) {
                findings.push_back("separator " + id + " routes to an empty subtree at " +
                                   std::to_string(i));
            } else if (codec_.compare(pair.key.data(), child_min->data()) != 0) {
                findings.push_back("separator " + id + " entry " + std::to_string(i) +
                                   " does not equal its child's first key");
            }
        }
        return first_min;
    }

    const Leaf leaf = pin_leaf(block);
    const std::size_t n = leaf->count();
    if (!is_root && (n < min_leaf_entries() || n > max_leaf_entries())) {
        findings.push_back("leaf " + id + " holds " + std::to_string(n) +
                           " entries, outside the order rule");
    }
    for (std::size_t i = 1; i < n; ++i) {
        if (codec_.compare(leaf->get_entry(i - 1).key.data(), leaf->get_entry(i).key.data()) > 0) {
            findings.push_back("leaf " + id + " is out of order at entry " + std::to_string(i));
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const idx::DataEntry entry = leaf->get_entry(i);
        if (!walk.occupied(entry.rid)) {
            findings.push_back("dangling entry: key " + codec_.display(entry.key.data()) +
                               " rid " + rid_text(entry.rid));
        }
    }
    walk.result.entry_count += n;
    if (walk.min_depth == 0 || depth < walk.min_depth) {
        walk.min_depth = depth;
    }
    walk.max_depth = std::max(walk.max_depth, depth);
    if (n == 0) {
        return std::nullopt;
    }
    return leaf->get_entry(0).key;
}

}
