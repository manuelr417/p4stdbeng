#include "heapfile/HeapFile.h"

#include "common/Error.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace heapfile {

namespace {

// The header page's record area (its slots are never used): magic, the five
// structural fields, then the table name. Fixed-width fields come before
// the variable-length name so every field has a constant offset.
//
//   0   magic "TUP2"
//   4   free_head     first page with a free slot (kNoPage when empty)
//   8   free_tail     last page appended to the free list
//   12  full_head     first page whose every slot is used
//   16  full_tail
//   20  next_page_id  the id the next page allocation will use (starts at 1)
//   24  name_length
//   28  name bytes
struct HeaderFields {
    std::uint32_t free_head = kNoPage;
    std::uint32_t free_tail = kNoPage;
    std::uint32_t full_head = kNoPage;
    std::uint32_t full_tail = kNoPage;
    std::uint32_t next_page_id = 1;
};

constexpr std::uint32_t kHeapMagic = 0x32505554u; // "TUP2"
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kHeaderFreeHeadOffset = 4;
constexpr std::size_t kHeaderFreeTailOffset = 8;
constexpr std::size_t kHeaderFullHeadOffset = 12;
constexpr std::size_t kHeaderFullTailOffset = 16;
constexpr std::size_t kHeaderNextPageIdOffset = 20;
constexpr std::size_t kNameLengthOffset = 24;
constexpr std::size_t kNameOffset = 28;
constexpr std::size_t kMaxTableNameLength = 200;

std::size_t head_offset(bool free_list) {
    return free_list ? kHeaderFreeHeadOffset : kHeaderFullHeadOffset;
}

std::size_t tail_offset(bool free_list) {
    return free_list ? kHeaderFreeTailOffset : kHeaderFullTailOffset;
}

HeaderFields read_header_fields(const char* buffer) {
    HeaderFields fields;
    fields.free_head = page_detail::read_u32(buffer, kHeaderFreeHeadOffset);
    fields.free_tail = page_detail::read_u32(buffer, kHeaderFreeTailOffset);
    fields.full_head = page_detail::read_u32(buffer, kHeaderFullHeadOffset);
    fields.full_tail = page_detail::read_u32(buffer, kHeaderFullTailOffset);
    fields.next_page_id = page_detail::read_u32(buffer, kHeaderNextPageIdOffset);
    return fields;
}

void write_header_fields(char* buffer, const HeaderFields& fields) {
    page_detail::write_u32(buffer, kHeaderFreeHeadOffset, fields.free_head);
    page_detail::write_u32(buffer, kHeaderFreeTailOffset, fields.free_tail);
    page_detail::write_u32(buffer, kHeaderFullHeadOffset, fields.full_head);
    page_detail::write_u32(buffer, kHeaderFullTailOffset, fields.full_tail);
    page_detail::write_u32(buffer, kHeaderNextPageIdOffset, fields.next_page_id);
}

// Writes the identification record into a header-page buffer: magic,
// empty lists, allocation frontier at 1, table name.
void write_identification(char* buffer, const schema::TableSchema& schema) {
    const std::string& name = schema.table_name();
    if (name.size() > kMaxTableNameLength) {
        throw common::Error("table name is " + std::to_string(name.size()) +
                            " bytes; at most " + std::to_string(kMaxTableNameLength) +
                            " fit the header page");
    }
    page_detail::write_u32(buffer, kMagicOffset, kHeapMagic);
    write_header_fields(buffer, HeaderFields{});
    page_detail::write_u32(buffer, kNameLengthOffset,
                           static_cast<std::uint32_t>(name.size()));
    std::memcpy(buffer + kNameOffset, name.data(), name.size());
}

// Checks the identification record against `schema`: the magic, then the
// table name.
void check_identification(const char* buffer, const schema::TableSchema& schema) {
    const std::uint32_t length = page_detail::read_u32(buffer, kNameLengthOffset);
    if (page_detail::read_u32(buffer, kMagicOffset) != kHeapMagic ||
        length > kMaxTableNameLength) {
        throw common::Error("not a tuple heap file");
    }
    const std::string stored(buffer + kNameOffset, length);
    if (stored != schema.table_name()) {
        throw common::Error("heap file is for table '" + stored + "'");
    }
}

// The list pointers must name allocated pages (or be empty). A dirty page
// may not be on disk yet, so next_page_id can legitimately exceed the
// on-disk block count by at most the pool's frame count; anything beyond
// that is a corrupt header, not slack.
void check_header_fields(const HeaderFields& fields, std::uint64_t blocks) {
    constexpr std::uint64_t kDirtySlack = 4096; // >= any pool this engine builds
    const auto allocated = [&](std::uint32_t id) {
        return id == kNoPage || id < fields.next_page_id;
    };
    if (fields.next_page_id < 1 || fields.next_page_id > blocks + kDirtySlack ||
        !allocated(fields.free_head) || !allocated(fields.free_tail) ||
        !allocated(fields.full_head) || !allocated(fields.full_tail)) {
        throw common::Error("header list pointers are out of range");
    }
}

// The record size a schema gives, refused when it has no layout or does
// not fit a page.
std::size_t checked_record_size(const schema::TableSchema& schema) {
    const std::size_t size = schema::TupleSerializer(schema).record_size();
    if (size == 0) {
        throw common::Error("the catalog has no record layout (no columns, or "
                            "String columns without declared width)");
    }
    constexpr std::size_t kMaxRecord = bufman::kPageSize - kPageHeaderSize - 1;
    if (size > kMaxRecord) {
        throw common::Error("a " + std::to_string(bufman::kPageSize) +
                            "-byte page fits records of at most " +
                            std::to_string(kMaxRecord) + " bytes, the catalog needs " +
                            std::to_string(size));
    }
    return size;
}

std::string cannot(const char* action, const std::string& path) {
    return std::string("cannot ") + action + " \"" + path + "\"";
}

}

HeapFile::HeapFile(bufman::BufferManager& manager) : manager_(manager) {}

bool HeapFile::is_open() const {
    return open_;
}

void HeapFile::create(const std::string& path, const schema::TableSchema& schema) {
    assert(!open_ && "create on an open heap file");
    const std::size_t size =
        common::with_context(cannot("create", path), [&] { return checked_record_size(schema); });
    // open_for_append creates the file when missing, so this also covers the
    // fresh-path case; an existing empty path is allowed.
    bufman::File file = manager_.open_file(path);
    try {
        if (const std::uint64_t blocks = bufman::block_count(file); blocks > 0) {
            throw common::Error(cannot("create", path) + ": the file already holds " +
                                std::to_string(blocks) + " block(s)");
        }
        bufman::PinnedPage header = bufman::PinnedPage::fresh(manager_, file, 0);
        // prev/next default to kNoPage: the header page is on no list, and
        // its trailer links are never read again — the list heads live in
        // the record area.
        SlottedPage view;
        view.bind(header.data(), size);
        view.format();
        write_identification(header.data(), schema);
        header.mark_dirty();
    } catch (...) {
        manager_.close_file(file);
        throw;
    }
    // Write the header back and leave a one-block file on disk.
    manager_.close_file(file);
}

void HeapFile::open(const std::string& path, const schema::TableSchema& schema) {
    assert(!open_ && "open on an open heap file");
    common::with_context(cannot("open", path), [&] {
        const std::size_t size = checked_record_size(schema);
        // Probe with a read-only open first: manager_.open_file would
        // silently create a missing file (O_CREAT).
        bufman::File probe = bufman::open_for_read(path);
        bufman::close(probe);

        file_ = manager_.open_file(path);
        try {
            const std::uint64_t blocks = bufman::block_count(file_);
            if (blocks == 0) {
                throw common::Error("the file has no header page");
            }
            bufman::PinnedPage header(manager_, file_, 0);
            // Identification first: a foreign file must be reported as such
            // rather than as a slot-count mismatch from attach.
            check_identification(header.data(), schema);
            SlottedPage view;
            view.bind(header.data(), size);
            view.attach();
            check_header_fields(read_header_fields(header.data()), blocks);
        } catch (...) {
            // Unwind completely: an unusable file must not stay open.
            manager_.close_file(file_);
            file_ = bufman::File{};
            throw;
        }
        record_size_ = size;
    });
    schema_ = &schema;
    serializer_.emplace(schema);
    open_ = true;
}

HeapFileIterator HeapFile::scan() {
    assert(open_ && "scan on a closed heap file");
    // One header visit for the two list heads and the walk bound.
    bufman::PinnedPage header(manager_, file_, 0);
    const HeaderFields fields = read_header_fields(header.data());
    return HeapFileIterator(*this, fields.free_head, fields.full_head,
                            fields.next_page_id);
}

HeapFile::DataPage HeapFile::attach_page(bufman::PinnedPage pin) {
    SlottedPage view;
    view.bind(pin.data(), record_size_);
    view.attach();
    return DataPage{std::move(pin), view};
}

HeapFile::DataPage HeapFile::pin_page(std::uint32_t page_id) {
    return attach_page(bufman::PinnedPage(manager_, file_, page_id));
}

// --- List surgery -----------------------------------------------------------

bufman::PinnedPage HeapFile::append_fresh_page(std::uint32_t page_id) {
    if (page_id >= kNoPage) {
        throw common::Error("insert would need page id " + std::to_string(page_id) +
                            ", beyond the 32-bit row id range");
    }
    // The allocation is the step that can fail, and it comes before any
    // list write; everything after it is memory writes.
    bufman::PinnedPage fresh = bufman::PinnedPage::fresh(manager_, file_, page_id);
    SlottedPage view;
    view.bind(fresh.data(), record_size_);
    view.format();

    bufman::PinnedPage header(manager_, file_, 0);
    assert(read_header_fields(header.data()).free_head == kNoPage &&
           "fresh page appended to a non-empty free list");
    page_detail::write_u32(header.data(), kHeaderFreeHeadOffset, page_id);
    page_detail::write_u32(header.data(), kHeaderFreeTailOffset, page_id);
    page_detail::write_u32(header.data(), kHeaderNextPageIdOffset, page_id + 1);
    header.mark_dirty();
    return fresh;
}

void HeapFile::move_between_lists(bool to_full, DataPage& page) {
    // The moving page's prev/next links name its neighbors in the list it
    // is leaving. Moving TO the full list means moving FROM the free list.
    const std::uint32_t self = page.id();
    const std::uint32_t prev = page.view.prev_page();
    const std::uint32_t next = page.view.next_page();
    const bool src_is_free = to_full;
    const bool dest_is_free = !to_full;

    // 1. Header first (id 0), then the source-list neighbors and the
    //    destination tail in ascending page-id order. They are distinct:
    //    the neighbors and the destination tail sit on different lists.
    bufman::PinnedPage header(manager_, file_, 0);
    const HeaderFields fields = read_header_fields(header.data());
    assert(self != 0 && (src_is_free ? fields.free_head : fields.full_head) != kNoPage &&
           "moving a page off an empty list");
    const std::uint32_t dest_tail = to_full ? fields.full_tail : fields.free_tail;

    std::vector<std::uint32_t> ids;
    for (const std::uint32_t id : {prev, next, dest_tail}) {
        if (id != kNoPage) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    std::vector<DataPage> aux;
    for (const std::uint32_t id : ids) {
        aux.push_back(pin_page(id));
        aux.back().pin.mark_dirty();
    }
    const auto neighbor = [&](std::uint32_t id) -> SlottedPage& {
        return std::find_if(aux.begin(), aux.end(),
                            [&](const DataPage& p) { return p.id() == id; })->view;
    };
    const auto set_header = [&](std::size_t offset, std::uint32_t value) {
        page_detail::write_u32(header.data(), offset, value);
    };

    // 2. All pins held — detach from the source list...
    if (prev == kNoPage) set_header(head_offset(src_is_free), next);
    else neighbor(prev).set_next_page(next);
    if (next == kNoPage) set_header(tail_offset(src_is_free), prev);
    else neighbor(next).set_prev_page(prev);

    // 3. ...and append at the destination list's tail.
    if (dest_tail == kNoPage) set_header(head_offset(dest_is_free), self);
    else neighbor(dest_tail).set_next_page(self);
    set_header(tail_offset(dest_is_free), self);
    page.view.set_prev_page(dest_tail);
    page.view.set_next_page(kNoPage);
    header.mark_dirty();
    page.pin.mark_dirty();
}

// --- Row operations ---------------------------------------------------------

// The free list's head — the oldest page with a free slot — or a fresh
// page when the free list is empty.
HeapFile::DataPage HeapFile::pin_insert_target() {
    HeaderFields fields;
    {
        bufman::PinnedPage header(manager_, file_, 0);
        fields = read_header_fields(header.data());
    }
    if (fields.free_head == kNoPage) {
        return attach_page(append_fresh_page(fields.next_page_id));
    }
    return pin_page(fields.free_head);
}

RowId HeapFile::insert(const tuple::Tuple& tuple) {
    assert(open_ && "insert on a closed heap file");
    // serialize validates the tuple first and throws before any page is
    // pinned.
    std::array<char, bufman::kPageSize> record{};
    serializer_->serialize(tuple, record.data());

    DataPage page = pin_insert_target();
    const std::optional<std::size_t> slot = page.view.first_free_slot();
    if (!slot) {
        // Free list means "has a free slot" by derivation; check_structure
        // is the tool for exactly this drift.
        throw common::Error("free list holds page " + std::to_string(page.id()) +
                            " with no free slot; the lists are inconsistent");
    }
    page.view.put_record(*slot, record.data());
    page.pin.mark_dirty();
    if (page.view.is_full()) {
        move_between_lists(true, page);
    }
    return RowId{page.id(), static_cast<std::uint32_t>(*slot)};
}

HeapFile::DataPage HeapFile::pin_record_page(RowId row_id) {
    if (row_id.page_id == 0) {
        throw common::Error("page 0 is the header page; it never stores records");
    }
    // A page exists iff it is resident in the pool or already written back:
    // dirty pages reach disk only on eviction/flush, so the on-disk block
    // count alone would lag behind the pool and reject live pages.
    if (row_id.page_id >= bufman::block_count(file_) &&
        !manager_.resident(file_, row_id.page_id)) {
        throw common::Error("page " + std::to_string(row_id.page_id) +
                            " is beyond the end of the file");
    }
    DataPage page = pin_page(row_id.page_id);
    if (row_id.slot >= page.view.capacity()) {
        throw common::Error("slot " + std::to_string(row_id.slot) +
                            " is out of range (capacity " +
                            std::to_string(page.view.capacity()) + ")");
    }
    if (!page.view.is_occupied(row_id.slot)) {
        throw common::Error("no record at row id (page " + std::to_string(row_id.page_id) +
                            ", slot " + std::to_string(row_id.slot) + "): the slot is free");
    }
    return page;
}

tuple::Tuple HeapFile::find(RowId row_id) {
    assert(open_ && "find on a closed heap file");
    std::array<char, bufman::kPageSize> record{};
    pin_record_page(row_id).view.get_record(row_id.slot, record.data());
    return serializer_->deserialize(record.data());
}

void HeapFile::update(RowId row_id, const tuple::Tuple& tuple) {
    assert(open_ && "update on a closed heap file");
    std::array<char, bufman::kPageSize> record{};
    serializer_->serialize(tuple, record.data());
    DataPage page = pin_record_page(row_id);
    page.view.put_record(row_id.slot, record.data());
    page.pin.mark_dirty();
}

void HeapFile::erase(RowId row_id) {
    assert(open_ && "erase on a closed heap file");
    DataPage page = pin_record_page(row_id);
    // "Was full" must be read before the slot is cleared: it is what sends
    // the page back to the free list.
    const bool was_full = page.view.is_full();
    page.view.erase(row_id.slot);
    page.pin.mark_dirty();
    if (was_full) {
        move_between_lists(false, page);
    }
}

// --- Structure self-check ---------------------------------------------------

void HeapFile::check_structure() {
    assert(open_ && "check_structure on a closed heap file");
    const std::uint64_t blocks = bufman::block_count(file_);
    HeaderFields fields;
    {
        bufman::PinnedPage header(manager_, file_, 0);
        fields = read_header_fields(header.data());
    }
    check_header_fields(fields, blocks);
    const auto fail = [](const std::string& message) { throw common::Error(message); };

    // Walk both lists, one pin at a time. `seen` indexes page ids; the
    // range check above bounds next_page_id, so the vector is small.
    std::vector<char> seen(fields.next_page_id, 0);
    const std::uint32_t heads[2] = {fields.free_head, fields.full_head};
    const std::uint32_t tails[2] = {fields.free_tail, fields.full_tail};
    for (int list = 0; list < 2; ++list) {
        const std::string list_name = list == 0 ? "free" : "full";
        const std::string page_on_list = " on the " + list_name + " list";
        std::uint32_t last = kNoPage;
        for (std::uint32_t current = heads[list]; current != kNoPage;) {
            const std::string page = "page " + std::to_string(current);
            if (current == 0) fail("the header page appears" + page_on_list);
            if (current >= fields.next_page_id) fail(page + page_on_list + " is beyond next_page_id");
            if (seen[current] != 0) fail(page + " is reachable twice; the lists are cyclic or share a page");
            seen[current] = 1;
            const DataPage data = pin_page(current);
            // Membership ⇔ occupancy: the free list holds exactly the
            // pages with a free slot, the full list exactly the full ones.
            const bool full = data.view.is_full();
            if (full != (list == 1)) {
                fail(page + (full ? " is full but sits on the free list"
                                  : " has free space but sits on the full list"));
            }
            last = current;
            current = data.view.next_page();
        }
        if ((heads[list] == kNoPage) != (tails[list] == kNoPage)) {
            fail("the " + list_name + " list's head and tail disagree about emptiness");
        }
        if (tails[list] != last) {
            fail("the " + list_name + " list's tail pointer does not match the walk");
        }
    }

    // The union of both lists is exactly the allocated pages.
    const std::size_t reachable = std::count(seen.begin() + 1, seen.end(), 1);
    if (reachable != fields.next_page_id - 1) {
        fail(std::to_string(fields.next_page_id - 1 - reachable) +
             " allocated page(s) are on no list");
    }

    // Every on-disk data page must be reachable (dirty pages may not be on
    // disk yet, so not the other way around). A block beyond next_page_id
    // cannot be allocated and is corruption in its own right.
    for (std::uint64_t block = 1; block < blocks; ++block) {
        const std::string page = "on-disk page " + std::to_string(block);
        if (block >= seen.size()) fail(page + " is beyond next_page_id");
        if (seen[block] == 0) fail(page + " is on no list");
    }
}

// --- Lifecycle --------------------------------------------------------------

bool HeapFile::resident(std::uint32_t page_id) const {
    return manager_.resident(file_, page_id);
}

void HeapFile::close() {
    assert(open_ && "close on a closed heap file");
    manager_.close_file(file_);
    open_ = false;
    schema_ = nullptr;
    serializer_.reset();
    record_size_ = 0;
    file_ = bufman::File{};
}

// --- HeapFileIterator -------------------------------------------------------

HeapFileIterator::HeapFileIterator(HeapFile& file, std::uint32_t free_head,
                                   std::uint32_t full_head,
                                   std::uint32_t allocated)
    : file_(&file),
      page_id_(free_head),
      next_list_(full_head),
      slot_(0),
      seen_(allocated, 0) {}

void HeapFileIterator::corrupt(const std::string& message) {
    // A corrupt walk ends the scan: every later call reports exhaustion.
    page_id_ = kNoPage;
    next_list_ = kNoPage;
    throw common::Error(message);
}

void HeapFileIterator::enter_page() {
    if (page_id_ >= seen_.size()) {
        corrupt("list walk reached page " + std::to_string(page_id_) +
                ", beyond the allocated page range");
    }
    if (page_id_ == iterating_) {
        return; // still scanning this page's slots
    }
    // Entering a page the walk already left: the lists are cyclic.
    if (seen_[page_id_] != 0) {
        corrupt("list walk revisited page " + std::to_string(page_id_) +
                "; the lists are cyclic");
    }
    seen_[page_id_] = 1;
    iterating_ = page_id_;
}

void HeapFileIterator::leave_page(std::uint32_t following) {
    page_id_ = following;
    slot_ = 0;
    iterating_ = kNoPage;
}

std::optional<tuple::Tuple> HeapFileIterator::next() {
    assert(file_->is_open() && "scan of a closed heap file");
    std::array<char, bufman::kPageSize> record{};
    for (;;) {
        if (page_id_ == kNoPage) {
            // The current list is done; promote the remaining one.
            if (next_list_ == kNoPage) {
                return std::nullopt;
            }
            leave_page(std::exchange(next_list_, kNoPage));
        }
        enter_page();
        std::optional<std::uint32_t> found;
        std::uint32_t following = kNoPage;
        std::size_t capacity = 0;
        {
            // Copy the record out; it is decoded after the pin is gone.
            const HeapFile::DataPage page = file_->pin_page(page_id_);
            following = page.view.next_page();
            capacity = page.view.capacity();
            for (std::uint32_t slot = slot_; slot < capacity && !found; ++slot) {
                if (page.view.is_occupied(slot)) {
                    page.view.get_record(slot, record.data());
                    found = slot;
                }
            }
        }
        if (!found) {
            leave_page(following);
            continue;
        }
        tuple::Tuple tuple = file_->serializer_->deserialize(record.data());
        current_ = RowId{page_id_, *found};
        if (*found + 1 < capacity) {
            slot_ = *found + 1; // stay on this page
        } else {
            leave_page(following);
        }
        return tuple;
    }
}

// --- Free functions ---------------------------------------------------------

void erase_file(bufman::BufferManager& manager, const std::string& path) {
    if (manager.is_open(path)) {
        throw common::Error(cannot("erase", path) + ": the file is open");
    }
    if (std::remove(path.c_str()) != 0) {
        throw common::Error(cannot("erase", path) + ": " + std::strerror(errno));
    }
}

}
