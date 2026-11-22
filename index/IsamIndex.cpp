#include "index/IsamIndex.h"

#include "common/Error.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace isam {

namespace {

// The identification record at the fixed offsets of block 0. Magic is
// "ISAM" read little-endian, matching the ex11 header-page pattern
// ("TUP1"): a foreign file is a magic error, not an arithmetic surprise.
constexpr std::uint32_t kIndexMagic = 0x4D415349u;  // "ISAM"
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kKeyTypeOffset = 4;
constexpr std::size_t kKeyWidthOffset = 8;
constexpr std::size_t kTableNameLengthOffset = 12;
constexpr std::size_t kTableNameOffset = 16;
constexpr std::size_t kMaxNameLength = 200;
constexpr std::size_t kColumnNameLengthOffset =
    kTableNameOffset + kMaxNameLength;              // 216
constexpr std::size_t kColumnNameOffset =
    kColumnNameLengthOffset + 4;                    // 220
constexpr std::size_t kRootOffset =
    kColumnNameOffset + kMaxNameLength;             // 420
constexpr std::size_t kFirstLeafOffset = kRootOffset + 4;
constexpr std::size_t kLevelsOffset = kRootOffset + 8;
constexpr std::size_t kEntryCountOffset = kRootOffset + 12;
constexpr std::size_t kLastBlockOffset = kRootOffset + 20;

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

std::uint64_t read_u64(const char* buffer, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u64(char* buffer, std::size_t offset, std::uint64_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

// The equals-scan shared by leaf and overflow pages (D4): appends the rid
// of every entry whose key equals `key_bytes`.
void collect_equals(const EntryPage& page, const KeyCodec& codec,
                    const std::string& key_bytes, std::vector<Rid>& out) {
    for (std::size_t i = page.lower_bound(key_bytes, codec); i < page.count(); ++i) {
        const DataEntry entry = page.get_entry(i);
        if (codec.compare(entry.key.data(), key_bytes.data()) != 0) {
            break;
        }
        out.push_back(entry.rid);
    }
}

// The position of the exact (key_bytes, rid) entry on one entry page:
// the equals-run found by lower_bound, scanned for the matching rid.
std::optional<std::size_t> find_exact(const EntryPage& page, const KeyCodec& codec,
                                      const std::string& key_bytes, Rid rid) {
    for (std::size_t i = page.lower_bound(key_bytes, codec); i < page.count(); ++i) {
        const DataEntry entry = page.get_entry(i);
        if (codec.compare(entry.key.data(), key_bytes.data()) != 0) {
            break;
        }
        if (entry.rid == rid) {
            return i;
        }
    }
    return std::nullopt;
}

// The per-entry integrity checks shared by leaf and overflow pages
// (D7): sortedness within the page and rid occupancy. `label` names the
// page in findings ("leaf 3", "overflow 5").
void check_entries(const EntryPage& page, const KeyCodec& codec, const std::string& label,
                   const std::function<bool(const Rid&)>& occupied,
                   idx::VerifyResult& result) {
    for (std::size_t i = 0; i < page.count(); ++i) {
        const DataEntry entry = page.get_entry(i);
        if (i > 0 && codec.compare(page.get_entry(i - 1).key.data(), entry.key.data()) > 0) {
            result.findings.push_back(label + ": entries out of order near key " +
                                      codec.display(entry.key.data()));
        }
        if (!occupied(entry.rid)) {
            result.findings.push_back("dangling entry: key " + codec.display(entry.key.data()) +
                                      " rid (" + std::to_string(entry.rid.page_id) + "," +
                                      std::to_string(entry.rid.slot) + ")");
        }
    }
    result.entry_count += page.count();
}

}  // namespace

IsamIndex::IsamIndex(bufman::BufferManager& manager,
                     const schema::TableSchema& schema,
                     std::size_t column_index)
    : manager_(manager), schema_(schema), column_index_(column_index) {
    if (column_index < schema.column_count()) {
        const schema::ColumnSchema& column = schema.column_info(column_index);
        codec_ = KeyCodec(column.column_type(),
                          column.column_type() == tuple::ColumnType::String
                              ? column.column_size()
                              : 0);
    }
}

// --- header -----------------------------------------------------------------

void IsamIndex::write_header() {
    const std::string& table_name = schema_.table_name();
    const std::string& column_name = schema_.column_info(column_index_).column_name();
    bufman::PinnedPage header(manager_, file_, 0);
    char* buffer = header.data();
    write_u32(buffer, kMagicOffset, kIndexMagic);
    write_u32(buffer, kKeyTypeOffset, static_cast<std::uint32_t>(codec_.key_type()));
    write_u32(buffer, kKeyWidthOffset, static_cast<std::uint32_t>(codec_.width()));
    write_u32(buffer, kTableNameLengthOffset, static_cast<std::uint32_t>(table_name.size()));
    std::memcpy(buffer + kTableNameOffset, table_name.data(), table_name.size());
    write_u32(buffer, kColumnNameLengthOffset, static_cast<std::uint32_t>(column_name.size()));
    std::memcpy(buffer + kColumnNameOffset, column_name.data(), column_name.size());
    write_u32(buffer, kRootOffset, root_);
    write_u32(buffer, kFirstLeafOffset, first_leaf_);
    write_u32(buffer, kLevelsOffset, levels_);
    write_u64(buffer, kEntryCountOffset, entry_count_);
    write_u32(buffer, kLastBlockOffset, last_block_);
    header.mark_dirty();
}

// --- lifecycle ----------------------------------------------------------------

void IsamIndex::create(const std::string& path) {
    assert(!open_ && "create on an open index");
    const std::string context = "cannot create \"" + path + "\"";
    if (!codec_.valid()) {
        throw common::Error(context + ": the column has no usable key width (undeclared String)");
    }
    // Fresh file only: an existing non-empty path is a refusal, an
    // existing empty path is allowed (open_for_append creates missing).
    file_ = manager_.open_file(path);
    try {
        if (const std::uint64_t blocks = bufman::block_count(file_); blocks > 0) {
            throw common::Error(context + ": the file already holds " +
                                std::to_string(blocks) + " block(s)");
        }
        bufman::PinnedPage::fresh(manager_, file_, 0); // the header block
        path_ = path;
        root_ = kNoPage;
        first_leaf_ = kNoPage;
        levels_ = 1;
        entry_count_ = 0;
        last_block_ = 0;
        write_header();
    } catch (...) {
        manager_.close_file(file_);
        file_ = bufman::File{};
        last_block_ = kNoPage;
        throw;
    }
    open_ = true;
}

// Fills leaves left to right from the sorted entries and returns each
// leaf's (first key, block) for the levels above (D5). Leaves are linked
// afterwards by link_leaf_chain.
std::vector<IndexEntry> IsamIndex::build_leaves(const std::vector<DataEntry>& sorted) {
    std::vector<IndexEntry> level;
    const std::size_t capacity = leaf_capacity(codec_.width());
    for (std::size_t start = 0; start < sorted.size(); start += capacity) {
        Leaf leaf = allocate<LeafPage>(kNoPage, kNoPage);
        for (std::size_t i = start; i < std::min(start + capacity, sorted.size()); ++i) {
            leaf->insert_entry(sorted[i], codec_);
        }
        level.push_back(IndexEntry{sorted[start].key, leaf.id()});
    }
    if (!level.empty()) {
        first_leaf_ = level.front().child;
    }
    return level;
}

// Links the leaf chain forward, one leaf per frame (D5): leaf i's
// next_leaf is leaf i+1's block; the last leaf ends the chain.
void IsamIndex::link_leaf_chain(const std::vector<IndexEntry>& level, std::size_t index) {
    if (index + 1 >= level.size()) {
        return;
    }
    {
        Leaf leaf = pin<LeafPage>(level[index].child);
        leaf->set_next_leaf(level[index + 1].child);
        leaf.mark_dirty();
    }
    link_leaf_chain(level, index + 1);
}

// Builds one separator level from the pages below and recurses until a
// level fits in one page — that page is the root (D5). Separators are
// the first key of each child page except the leftmost; each separator
// page stores the first child of its chunk (written last: the slot moves
// with the entry count) plus one (key, child) entry per remaining child.
void IsamIndex::build_separator_level(std::vector<IndexEntry> level) {
    if (level.size() <= 1) {
        // One page holds this level: it is the root (an empty index has
        // none, and a single leaf is its own root).
        root_ = level.empty() ? kNoPage : level.front().child;
        return;
    }
    ++levels_;
    std::vector<IndexEntry> next_level;
    const std::size_t capacity = separator_capacity(codec_.width());
    for (std::size_t start = 0; start < level.size(); start += capacity) {
        Separator page = allocate<SeparatorPage>();
        for (std::size_t i = start + 1; i < std::min(start + capacity, level.size()); ++i) {
            page->append(level[i]);
        }
        page->set_first_child(level[start].child);
        next_level.push_back(IndexEntry{level[start].key, page.id()});
    }
    build_separator_level(std::move(next_level));
}

// The whole build as named phases (D5): encode and sort, fill the
// leaves, link the chain, grow separator levels. A failure closes the
// half-built index.
void IsamIndex::build(const std::string& path,
                      const std::vector<std::pair<tuple::Column, Rid>>& entries) {
    create(path);
    try {
        std::vector<DataEntry> sorted;
        sorted.reserve(entries.size());
        for (const auto& [value, rid] : entries) {
            sorted.push_back(DataEntry{codec_.encode(value), rid});
        }
        std::sort(sorted.begin(), sorted.end(), [this](const DataEntry& a, const DataEntry& b) {
            return codec_.compare(a.key.data(), b.key.data()) < 0;
        });
        const std::vector<IndexEntry> leaves = build_leaves(sorted);
        link_leaf_chain(leaves, 0);
        build_separator_level(leaves);
        entry_count_ = sorted.size();
        write_header();
    } catch (...) {
        close();
        throw;
    }
}

void IsamIndex::open(const std::string& path) {
    assert(!open_ && "open on an open index");
    common::with_context("cannot open \"" + path + "\"", [&] {
        if (!codec_.valid()) {
            throw common::Error("the column has no usable key width (undeclared String)");
        }
        // Probe read-only first: manager_.open_file would create a
        // missing file, and a missing path must be reported as such.
        bufman::File probe = bufman::open_for_read(path);
        bufman::close(probe);
    });

    file_ = manager_.open_file(path);
    try {
        common::with_context("cannot open \"" + path + "\"", [&] {
            if (bufman::block_count(file_) == 0) {
                throw common::Error("the file has no header block");
            }
            const bufman::PinnedPage header(manager_, file_, 0);
            const char* buffer = header.data();
            const std::uint32_t name_length = read_u32(buffer, kTableNameLengthOffset);
            const std::uint32_t column_length = read_u32(buffer, kColumnNameLengthOffset);
            const std::string table_name =
                name_length <= kMaxNameLength ? std::string(buffer + kTableNameOffset, name_length) : "";
            const std::string column_name =
                column_length <= kMaxNameLength ? std::string(buffer + kColumnNameOffset, column_length) : "";
            const std::uint32_t key_width = read_u32(buffer, kKeyWidthOffset);
            if (read_u32(buffer, kMagicOffset) != kIndexMagic) {
                throw common::Error("not an index file");
            }
            if (read_u32(buffer, kKeyTypeOffset) != static_cast<std::uint32_t>(codec_.key_type())) {
                throw common::Error("key type differs");
            }
            if (key_width != codec_.width()) {
                throw common::Error("key width " + std::to_string(key_width) +
                                    " differs from " + std::to_string(codec_.width()));
            }
            if (table_name != schema_.table_name()) {
                throw common::Error("index file is for table '" + table_name + "'");
            }
            if (column_name != schema_.column_info(column_index_).column_name()) {
                throw common::Error("index file is for column '" + column_name + "'");
            }
            root_ = read_u32(buffer, kRootOffset);
            first_leaf_ = read_u32(buffer, kFirstLeafOffset);
            levels_ = read_u32(buffer, kLevelsOffset);
            last_block_ = read_u32(buffer, kLastBlockOffset);
            entry_count_ = static_cast<std::size_t>(read_u64(buffer, kEntryCountOffset));
        });
    } catch (...) {
        manager_.close_file(file_);
        file_ = bufman::File{};
        throw;
    }
    path_ = path;
    open_ = true;
}

void IsamIndex::close() {
    assert(open_ && "close on a closed index");
    open_ = false;
    root_ = kNoPage;
    first_leaf_ = kNoPage;
    levels_ = 1;
    last_block_ = kNoPage;
    entry_count_ = 0;
    manager_.close_file(file_);
    file_ = bufman::File{};
}

// --- pin helpers ----------------------------------------------------------------

template <class View>
bufman::PinnedView<View> IsamIndex::pin(std::uint32_t block) {
    bufman::PinnedPage pin(manager_, file_, block);
    View view;
    view.bind(pin.data(), codec_.width());
    view.attach();
    return {std::move(pin), view};
}

// Pages are allocated densely and never removed, so the next id is the
// highest allocated plus one — the on-disk block count cannot be used:
// dirty pages reach disk only on eviction/flush.
template <class View, class... FormatArgs>
bufman::PinnedView<View> IsamIndex::allocate(FormatArgs... format_args) {
    const std::uint32_t block = last_block_ + 1;
    if (block == kNoPage) {
        throw common::Error("index would need block id " + std::to_string(block) +
                            ", beyond the 32-bit page id range");
    }
    bufman::PinnedPage pin = bufman::PinnedPage::fresh(manager_, file_, block);
    last_block_ = block;
    View view;
    view.bind(pin.data(), codec_.width());
    view.format(format_args...);
    return {std::move(pin), view};
}

// --- descent ---------------------------------------------------------------------

std::uint32_t IsamIndex::descend(const char* key_bytes) {
    if (root_ == kNoPage) {
        return kNoPage;  // empty index: no leaf exists
    }
    // levels_ counts the leaf level too: cross levels_ - 1 separator
    // pages, then land on a leaf. levels_ == 1 means the root IS a leaf.
    return descend_from(root_, levels_ - 1, key_bytes);
}

// The recursive descent (D2): take the child after the last separator
// strictly below the key, unpin, recurse. The base case is the leaf.
std::uint32_t IsamIndex::descend_from(std::uint32_t block, std::uint32_t levels_remaining,
                                      const char* key_bytes) {
    if (levels_remaining == 0) {
        return block;  // the landing leaf
    }
    const std::uint32_t child = pin<SeparatorPage>(block)->descend_step(key_bytes, codec_);
    return descend_from(child, levels_remaining - 1, key_bytes);
}

// --- lookups ----------------------------------------------------------------------

void IsamIndex::scan_overflow_chain(std::uint32_t block, const std::string& key_bytes,
                                    std::vector<Rid>& out) {
    if (block == kNoPage) {
        return;
    }
    std::uint32_t next = kNoPage;
    {
        const Overflow page = pin<OverflowPage>(block);
        collect_equals(page.view, codec_, key_bytes, out);
        next = page->next_overflow();
    }
    scan_overflow_chain(next, key_bytes, out);
}

// Scan the landing leaf and its overflow chain, then continue into the
// next leaf while its first key is still <= the target — duplicates are
// contiguous but can span leaves, and the landing leaf may end below the
// key.
void IsamIndex::scan_leaf_chain(std::uint32_t block, const std::string& key_bytes,
                                std::vector<Rid>& out) {
    if (block == kNoPage) {
        return;
    }
    std::uint32_t next_leaf = kNoPage;
    std::uint32_t overflow = kNoPage;
    {
        const Leaf leaf = pin<LeafPage>(block);
        collect_equals(leaf.view, codec_, key_bytes, out);
        next_leaf = leaf->next_leaf();
        overflow = leaf->first_overflow();
    }
    scan_overflow_chain(overflow, key_bytes, out);
    if (next_leaf == kNoPage) {
        return;
    }
    bool may_continue = false;
    {
        const Leaf next = pin<LeafPage>(next_leaf);
        may_continue = next->count() > 0 &&
                       codec_.compare(next->get_entry(0).key.data(), key_bytes.data()) <= 0;
    }
    if (may_continue) {
        scan_leaf_chain(next_leaf, key_bytes, out);
    }
}

std::vector<Rid> IsamIndex::lookup(const tuple::Column& key) {
    assert(open_ && "lookup on a closed index");
    const std::string key_bytes = codec_.encode(key);
    std::vector<Rid> out;
    scan_leaf_chain(descend(key_bytes.data()), key_bytes, out);
    return out;
}

// One overflow chain as sorted runs, in chain order (D3); the caller
// stable-sorts the merge.
void IsamIndex::gather_overflow_chain(std::uint32_t block, std::vector<DataEntry>& out) {
    if (block == kNoPage) {
        return;
    }
    std::uint32_t next = kNoPage;
    {
        const Overflow page = pin<OverflowPage>(block);
        for (std::size_t i = 0; i < page->count(); ++i) {
            out.push_back(page->get_entry(i));
        }
        next = page->next_overflow();
    }
    gather_overflow_chain(next, out);
}

// One leaf of a range scan (D3): the leaf's entries merged with its
// overflow runs, stable-sorted, filtered by the bounds, emitted in key
// order. Returns the leaf's chain successor.
std::uint32_t IsamIndex::emit_leaf_range(std::uint32_t block, RangeWalk& walk) {
    std::vector<DataEntry> merged;
    std::uint32_t next_leaf = kNoPage;
    std::uint32_t overflow = kNoPage;
    {
        const Leaf leaf = pin<LeafPage>(block);
        for (std::size_t i = 0; i < leaf->count(); ++i) {
            merged.push_back(leaf->get_entry(i));
        }
        next_leaf = leaf->next_leaf();
        overflow = leaf->first_overflow();
    }
    gather_overflow_chain(overflow, merged);
    std::stable_sort(merged.begin(), merged.end(), [this](const DataEntry& a, const DataEntry& b) {
        return codec_.compare(a.key.data(), b.key.data()) < 0;
    });
    for (const DataEntry& entry : merged) {
        if (walk.lo_bytes) {
            const int cmp = codec_.compare(entry.key.data(), walk.lo_bytes->data());
            if (cmp < 0 || (cmp == 0 && !walk.lo_inclusive)) {
                continue;
            }
        }
        if (walk.hi_bytes) {
            const int cmp = codec_.compare(entry.key.data(), walk.hi_bytes->data());
            if (cmp > 0 || (cmp == 0 && !walk.hi_inclusive)) {
                continue;
            }
        }
        walk.out->push_back(entry.rid);
    }
    return next_leaf;
}

// Emit each leaf, then recurse while the chain continues and the upper
// bound admits the next leaf's first key. Without an upper bound the
// walk runs to the end of the chain.
void IsamIndex::scan_leaf_range(std::uint32_t block, RangeWalk& walk) {
    if (block == kNoPage) {
        return;
    }
    const std::uint32_t next_leaf = emit_leaf_range(block, walk);
    if (next_leaf == kNoPage) {
        return;
    }
    if (walk.hi_bytes) {
        bool beyond = true;
        {
            const Leaf next = pin<LeafPage>(next_leaf);
            if (next->count() > 0) {
                const int cmp = codec_.compare(next->get_entry(0).key.data(), walk.hi_bytes->data());
                beyond = cmp > 0 || (cmp == 0 && !walk.hi_inclusive);
            }
        }
        if (beyond) {
            return;
        }
    }
    scan_leaf_range(next_leaf, walk);
}

std::vector<Rid> IsamIndex::range(const std::optional<RangeBound>& lo,
                                  const std::optional<RangeBound>& hi) {
    assert(open_ && "range on a closed index");
    std::vector<Rid> out;
    if (first_leaf_ == kNoPage) {
        return out;  // empty index: an empty range
    }
    const std::string lo_bytes = lo ? codec_.encode(lo->value) : std::string();
    const std::string hi_bytes = hi ? codec_.encode(hi->value) : std::string();
    RangeWalk walk;
    walk.lo_bytes = lo ? &lo_bytes : nullptr;
    walk.lo_inclusive = lo && lo->inclusive;
    walk.hi_bytes = hi ? &hi_bytes : nullptr;
    walk.hi_inclusive = hi && hi->inclusive;
    walk.out = &out;
    // Start: the leftmost leaf that may hold the lower bound, or the
    // first leaf for an open lower bound.
    scan_leaf_range(lo ? descend(lo_bytes.data()) : first_leaf_, walk);
    return out;
}

// --- insert -----------------------------------------------------------------------

// Tail-recursive: pin, test, unpin, recurse on the chain link (D6).
std::uint32_t IsamIndex::find_overflow_with_room(std::uint32_t block) {
    if (block == kNoPage) {
        return kNoPage;  // no page in the chain has room
    }
    std::uint32_t next = kNoPage;
    {
        const Overflow page = pin<OverflowPage>(block);
        if (!page->full()) {
            return block;
        }
        next = page->next_overflow();
    }
    return find_overflow_with_room(next);
}

// An empty index grows its first leaf; it is simultaneously the root
// (levels_ == 1). Leaves are otherwise frozen: the descent lands on the
// owning leaf, and a full leaf spills into its overflow chain.
void IsamIndex::insert(const tuple::Column& key, Rid rid) {
    assert(open_ && "insert on a closed index");
    const DataEntry entry{codec_.encode(key), rid};
    if (root_ == kNoPage) {
        Leaf leaf = allocate<LeafPage>(kNoPage, kNoPage);
        leaf->insert_entry(entry, codec_);
        root_ = leaf.id();
        first_leaf_ = leaf.id();
        levels_ = 1;
    } else {
        const std::uint32_t leaf_block = descend(entry.key.data());
        std::optional<std::uint32_t> spill_chain;
        {
            Leaf leaf = pin<LeafPage>(leaf_block);
            if (leaf->full()) {
                spill_chain = leaf->first_overflow();
            } else {
                leaf->insert_entry(entry, codec_);
                leaf.mark_dirty();
            }
        }
        if (spill_chain) {
            insert_into_overflow(leaf_block, *spill_chain, entry);
        }
    }
    entry_count_ += 1;
    write_header();
}

// The full-leaf spill (D6): insert into the first overflow page with
// room; when none has room, a new page takes the FRONT of the leaf's
// chain and the leaf is re-pointed at it. Entries stay sorted per page,
// so each leaf's answer remains a merge of sorted runs.
void IsamIndex::insert_into_overflow(std::uint32_t leaf_block, std::uint32_t existing_chain,
                                     const DataEntry& entry) {
    if (const std::uint32_t target = find_overflow_with_room(existing_chain); target != kNoPage) {
        Overflow page = pin<OverflowPage>(target);
        page->insert_entry(entry, codec_);
        page.mark_dirty();
        return;
    }
    std::uint32_t new_block = kNoPage;
    {
        Overflow page = allocate<OverflowPage>(existing_chain); // next = the old front
        page->insert_entry(entry, codec_);
        new_block = page.id();
    }
    Leaf leaf = pin<LeafPage>(leaf_block);
    leaf->set_first_overflow(new_block);
    leaf.mark_dirty();
}

// --- erase -----------------------------------------------------------------------

// Walk the leaf, then its whole overflow chain: spill pages hold keys out
// of leaf order, so the exact entry can sit on any page of the chain.
void IsamIndex::erase(const tuple::Column& key, Rid rid) {
    assert(open_ && "erase on a closed index");
    const std::string key_bytes = codec_.encode(key);
    if (root_ == kNoPage) {
        throw common::Error("index is empty; no entry for key " + codec_.display(key_bytes.data()));
    }
    const auto erase_from = [&](auto& page) {
        const std::optional<std::size_t> pos = find_exact(page.view, codec_, key_bytes, rid);
        if (pos) {
            page->erase_entry(*pos);
            page.mark_dirty();
        }
        return pos.has_value();
    };
    bool erased = false;
    std::uint32_t block = kNoPage;
    {
        Leaf leaf = pin<LeafPage>(descend(key_bytes.data()));
        block = leaf->first_overflow();
        erased = erase_from(leaf);
    }
    while (!erased && block != kNoPage) {
        Overflow page = pin<OverflowPage>(block);
        block = page->next_overflow();
        erased = erase_from(page);
    }
    if (!erased) {
        throw common::Error("no index entry for key " + codec_.display(key_bytes.data()) +
                            " and rid (" + std::to_string(rid.page_id) + ", " +
                            std::to_string(rid.slot) + ")");
    }
    entry_count_ -= 1;
    write_header();
}

// --- verify -----------------------------------------------------------------------

void IsamIndex::verify_overflow_chain(std::uint32_t block, VerifyWalk& walk) {
    if (block == kNoPage) {
        return;
    }
    std::uint32_t next = kNoPage;
    {
        const Overflow page = pin<OverflowPage>(block);
        check_entries(page.view, codec_, "overflow " + std::to_string(block), walk.occupied,
                      walk.result);
        next = page->next_overflow();
    }
    verify_overflow_chain(next, walk);
}

// Per-entry checks on the leaf, the chain-order finding against the
// previous leaf's last key, then the overflow chain and the next leaf.
// An empty leaf carries the previous key forward.
void IsamIndex::verify_leaf_chain(std::uint32_t block,
                                  const std::optional<std::string>& previous_last_key,
                                  VerifyWalk& walk) {
    if (block == kNoPage) {
        return;
    }
    std::optional<std::string> last_key = previous_last_key;
    std::uint32_t overflow = kNoPage;
    std::uint32_t next_leaf = kNoPage;
    {
        const Leaf leaf = pin<LeafPage>(block);
        check_entries(leaf.view, codec_, "leaf " + std::to_string(block), walk.occupied,
                      walk.result);
        for (std::size_t i = 0; previous_last_key && i < leaf->count(); ++i) {
            const DataEntry entry = leaf->get_entry(i);
            if (codec_.compare(previous_last_key->data(), entry.key.data()) > 0) {
                walk.result.findings.push_back("leaf chain out of key order at leaf " +
                                               std::to_string(block) + ", key " +
                                               codec_.display(entry.key.data()));
            }
        }
        if (leaf->count() > 0) {
            last_key = leaf->get_entry(leaf->count() - 1).key;
        }
        overflow = leaf->first_overflow();
        next_leaf = leaf->next_leaf();
    }
    verify_overflow_chain(overflow, walk);
    verify_leaf_chain(next_leaf, last_key, walk);
}

// Order checks: entries sorted within each page, and the leaf chain's
// keys non-decreasing across leaves. Overflow keys are deliberately NOT
// ordered against their leaf — an insert into a full leaf lands in
// overflow regardless of value (6 can sit behind a leaf ending in 9), so
// a cross-structure check would flag legal data. Dangling rids are
// checked everywhere.
idx::VerifyResult IsamIndex::verify(const std::function<bool(const Rid&)>& occupied) {
    assert(open_ && "verify on a closed index");
    idx::VerifyResult result;
    VerifyWalk walk{occupied, result};
    verify_leaf_chain(first_leaf_, std::nullopt, walk);
    if (result.entry_count != entry_count_) {
        result.findings.push_back("walked " + std::to_string(result.entry_count) +
                                  " entries, the header claims " + std::to_string(entry_count_));
    }
    return result;
}

}
