#include "tools/BulkLoader.h"

#include "common/Error.h"
#include "file/BlockFile.h"
#include "heapfile/SlottedPage.h"
#include "schema/TupleSerializer.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace tools {

namespace {

// The TUP2 header page's record area, re-derived from the same layout
// HeapFile encodes: magic "TUP2" at 0, then the five list fields, then
// the name. The differential check in the suite fails loudly if the two
// derivations ever drift.
constexpr std::uint32_t kHeapMagic = 0x32505554u; // "TUP2"
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kFreeHeadOffset = 4;
constexpr std::size_t kFreeTailOffset = 8;
constexpr std::size_t kFullHeadOffset = 12;
constexpr std::size_t kFullTailOffset = 16;
constexpr std::size_t kNextPageIdOffset = 20;
constexpr std::size_t kNameLengthOffset = 24;
constexpr std::size_t kNameOffset = 28;
constexpr std::size_t kMaxTableNameLength = 200;

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

// Closes the block file on every exit path; close() is infallible.
struct FileGuard {
    bufman::File file;

    explicit FileGuard(bufman::File f) : file(f) {}
    ~FileGuard() {
        if (file.fd >= 0) {
            bufman::close(file);
        }
    }
};

// Composes and writes block 0: a SlottedPage-formatted header page whose
// record area carries the TUP2 identification and the two list heads.
// `full_first`/`full_last` are the full list's ends (kNoPage when empty),
// `free_page` the free list's only page; `next_page_id` is the
// allocation frontier the loader reached.
void write_header_page(bufman::File& file, const schema::TableSchema& schema,
                       std::size_t record_size, std::uint32_t full_first,
                       std::uint32_t full_last, std::uint32_t free_page,
                       std::uint32_t next_page_id) {
    const std::string& name = schema.table_name();
    if (name.size() > kMaxTableNameLength) {
        throw common::Error("table name is " + std::to_string(name.size()) +
                            " bytes; at most " + std::to_string(kMaxTableNameLength) +
                            " fit the header page");
    }
    std::array<char, bufman::kBlockSize> block{};
    heapfile::SlottedPage header;
    header.bind(block.data(), record_size);
    header.format(); // the header page is on no list
    write_u32(block.data(), kMagicOffset, kHeapMagic);
    write_u32(block.data(), kFreeHeadOffset, free_page);
    write_u32(block.data(), kFreeTailOffset, free_page);
    write_u32(block.data(), kFullHeadOffset, full_first);
    write_u32(block.data(), kFullTailOffset, full_last);
    write_u32(block.data(), kNextPageIdOffset, next_page_id);
    write_u32(block.data(), kNameLengthOffset, static_cast<std::uint32_t>(name.size()));
    std::memcpy(block.data() + kNameOffset, name.data(), name.size());
    bufman::write_block(file, 0, block.data());
}

// Validates an existing one-block file's header: identification record
// first (a foreign file must be reported as such), then the slotted
// layout, then empty lists (a loader refills only a virgin file).
void check_empty_header(char* block, const schema::TableSchema& schema,
                        std::size_t record_size) {
    const std::uint32_t length = read_u32(block, kNameLengthOffset);
    if (read_u32(block, kMagicOffset) != kHeapMagic || length > kMaxTableNameLength) {
        throw common::Error("not a tuple heap file");
    }
    const std::string stored(block + kNameOffset, length);
    if (stored != schema.table_name()) {
        throw common::Error("heap file is for table '" + stored + "'");
    }
    heapfile::SlottedPage header;
    header.bind(block, record_size);
    header.attach();
    if (read_u32(block, kFreeHeadOffset) != heapfile::kNoPage ||
        read_u32(block, kFullHeadOffset) != heapfile::kNoPage) {
        throw common::Error("the file already holds records");
    }
}

// The record layout decides everything structural; the same bounds
// HeapFile::create enforces apply here.
std::size_t checked_record_size(const schema::TupleSerializer& serializer) {
    const std::size_t record_size = serializer.record_size();
    if (record_size == 0) {
        throw common::Error("the catalog has no record layout (no columns, "
                            "or String columns without declared width)");
    }
    constexpr std::size_t kMaxRecord = bufman::kBlockSize - heapfile::kPageHeaderSize - 1;
    if (record_size > kMaxRecord) {
        throw common::Error("a " + std::to_string(bufman::kBlockSize) +
                            "-byte page fits records of at most " + std::to_string(kMaxRecord) +
                            " bytes, the catalog needs " + std::to_string(record_size));
    }
    return record_size;
}

}  // namespace

BulkLoadStats bulk_load(const std::string& path, const schema::TableSchema& schema,
                        std::size_t count, const RowGenerator& generator) {
    return common::with_context("bulk load", [&] {
        const schema::TupleSerializer serializer(schema);
        const std::size_t record_size = checked_record_size(serializer);
        const std::size_t capacity = heapfile::slot_capacity(record_size);

        FileGuard guard(bufman::open_for_append(path));
        bufman::File& file = guard.file;

        // Bootstrap decision by block count, refused before any write. A
        // valid one-block file is reused as-is: its header already
        // carries the right identification and empty lists.
        const std::uint64_t blocks = bufman::block_count(file);
        if (blocks > 1) {
            throw common::Error("the file already holds " + std::to_string(blocks) +
                                " block(s)");
        }
        if (blocks == 1) {
            std::array<char, bufman::kBlockSize> block{};
            bufman::read_block(file, 0, block);
            check_empty_header(block.data(), schema, record_size);
        }

        // The fill/link loop: one local page buffer at a time. Every page
        // that fills completely joins the full list in generation order
        // (its trailer links are the list's links); a partial final page
        // joins the free list as its only element. Each block is written
        // exactly once, except the full list's tail, whose optimistic
        // forward link is patched after the loop.
        std::array<char, bufman::kBlockSize> page{};
        std::array<char, bufman::kBlockSize> record{};
        std::size_t record_index = 0;
        std::uint32_t page_id = 1;
        std::uint32_t full_first = heapfile::kNoPage;
        std::uint32_t full_last = heapfile::kNoPage;
        std::uint32_t partial_page = heapfile::kNoPage;
        while (record_index < count) {
            const std::size_t to_fill = std::min(capacity, count - record_index);
            const bool fills_completely = to_fill == capacity;
            page.fill('\0');
            heapfile::SlottedPage view;
            view.bind(page.data(), record_size);
            if (fills_completely) {
                view.format(full_last, page_id + 1); // optimistic list link
            } else {
                view.format(heapfile::kNoPage, heapfile::kNoPage);
            }
            for (std::size_t slot = 0; slot < to_fill; ++slot, ++record_index) {
                const std::string at_row = " at row index " + std::to_string(record_index);
                const tuple::Tuple tuple =
                    common::with_context("generator failed" + at_row, [&] { return generator(record_index); });
                common::with_context("serialize failed" + at_row,
                                     [&] { serializer.serialize(tuple, record.data()); });
                view.put_record(slot, record.data());
            }
            bufman::write_block(file, page_id, page.data());
            if (fills_completely) {
                if (full_first == heapfile::kNoPage) {
                    full_first = page_id;
                }
                full_last = page_id;
            } else {
                partial_page = page_id;
            }
            ++page_id;
        }

        // The full list's tail carried an optimistic forward link; it ends
        // the list (the partial page, when one exists, is on the free list).
        if (full_last != heapfile::kNoPage) {
            std::array<char, bufman::kBlockSize> tail{};
            bufman::read_block(file, full_last, tail);
            write_u32(tail.data(), heapfile::kNextPageIdOffset, heapfile::kNoPage);
            bufman::write_block(file, full_last, tail.data());
        }

        // The header: both lists' ends and the allocation frontier —
        // written in both bootstrap cases.
        write_header_page(file, schema, record_size, full_first, full_last, partial_page,
                          page_id);
        return BulkLoadStats{record_index, page_id - std::size_t{1}, capacity};
    });
}

}
