#ifndef EX11_HEAP_FILE_H
#define EX11_HEAP_FILE_H

#include "bufpool/BufferManager.h"
#include "bufpool/PinnedPage.h"
#include "heapfile/SlottedPage.h"
#include "schema/TableSchema.h"
#include "schema/TupleSerializer.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace heapfile {

/// Address of one record in a heap file: the page id (the block number,
/// where block 0 is the header page) and the slot number inside that page.
/// A row id is valid only while its record exists: erasing frees the slot,
/// and a later insert may hand out the same row id again.
struct RowId {
    std::uint32_t page_id = 0;
    std::uint32_t slot = 0;
};

inline bool operator==(const RowId& a, const RowId& b) {
    return a.page_id == b.page_id && a.slot == b.slot;
}

inline bool operator!=(const RowId& a, const RowId& b) {
    return !(a == b);
}

class HeapFile;

/// Scan iterator over the records of one open HeapFile. Each next() call
/// yields the next occupied slot's tuple, walking the free list's pages in
/// list order, then the full list's pages in list order (tail-append keeps
/// each list aligned with allocation order); once both lists are
/// exhausted, every further call returns nullopt. A corrupt page or list
/// throws. The order is unspecified while the file is being mutated.
class HeapFileIterator {
public:
    /// The next tuple, or nullopt once the scan is exhausted.
    std::optional<tuple::Tuple> next();

    /// The row id of the record most recently returned by next().
    const RowId& row_id() const { return current_; }

private:
    friend class HeapFile;
    HeapFileIterator(HeapFile& file, std::uint32_t free_head,
                     std::uint32_t full_head, std::uint32_t allocated);

    void enter_page();
    void leave_page(std::uint32_t following);
    [[noreturn]] void corrupt(const std::string& message);

    HeapFile* file_;        // the owning HeapFile
    std::uint32_t page_id_; // page being read (kNoPage = current list done)
    std::uint32_t next_list_; // head of the list still to walk (kNoPage none)
    std::uint32_t slot_;    // next slot to inspect on page_id_
    // Cycle guard: seen_[p] marks pages already walked away from. A page
    // may be re-pinned while its slots are iterated, but once the walk
    // advances past a page it can never legitimately return.
    std::vector<char> seen_;
    std::uint32_t iterating_ = kNoPage; // page whose slot scan is in progress
    RowId current_;         // row id of the last yielded record
};

/// A file of fixed-width tuple records stored in slotted pages organized
/// as two header-addressed free-space lists: pages with free space, and
/// pages whose every slot is used. The record layout is chosen by the
/// schema handed to create/open: every column occupies a fixed width
/// (natural size for numerics, the catalog-declared column_size for
/// Strings), so one record is always record_size(schema) bytes. Block 0 is
/// the header page: formatted like a SlottedPage, but its slots are never
/// used — its record area carries the magic, the table name, and the list
/// pointers (free_head/free_tail/full_head/full_tail) plus next_page_id,
/// the allocation frontier. A page's list membership is derived from its
/// occupancy — full list iff every slot is used — and never stored
/// separately. Page ids are block numbers; pages are allocated once and
/// never removed (a page emptied by erases stays on the free list; the
/// file never shrinks). Plain operations hold exactly one pinned page at
/// a time; list surgery (a page joining or moving between lists)
/// transiently holds up to five — the moving page plus header, list
/// neighbors, and destination tail, the auxiliary pins taken in ascending
/// page-id order — so the pool must have the frames for that (the
/// session's 8 do). Every operation reaches the disk only through the
/// injected BufferManager. One HeapFile
/// manages one open file at a time and shares the session's buffer
/// manager with other files: it owns only its own file's lifecycle —
/// close() flushes and closes just this file (BufferManager::close_file),
/// leaving every other file the manager holds untouched. The schema must
/// outlive the open file.
class HeapFile {
public:
    explicit HeapFile(bufman::BufferManager& manager);

    bool is_open() const;

    /// Creates a new one-block heap file (just the header page, carrying
    /// the identification record and the empty lists) at `path`. Refuses a
    /// path that already holds a non-empty file; an existing empty path is
    /// formatted. The new file is left closed.
    void create(const std::string& path, const schema::TableSchema& schema);

    /// Opens an existing heap file read-write, checking that block 0 is a
    /// tuple heap file header for `schema`'s table, that the pages
    /// hold records of the schema's record size, and that the header's
    /// list pointers are in range. The schema must outlive the open file.
    void open(const std::string& path, const schema::TableSchema& schema);

    /// Starts a scan over all records: the free list's pages in list
    /// order, then the full list's pages in list order.
    HeapFileIterator scan();

    /// Validates `tuple` against the schema, packs it, and stores it in
    /// the free list's head page — the first page with a free slot, found
    /// without walking. When the free list is empty, allocates a fresh
    /// page (id taken from the header's next_page_id) and appends it to
    /// the free list. When the target page's last free slot is taken, the
    /// page moves to the full list. Returns the new record's row id.
    RowId insert(const tuple::Tuple& tuple);

    /// The record at `row_id`. Throws when the row id names the header
    /// page, a page beyond the end of the file, an out-of-range slot, a
    /// free slot, or a record that violates the schema.
    tuple::Tuple find(RowId row_id);

    /// Packs `tuple` and overwrites the record at `row_id`. Never moves
    /// the page between lists.
    void update(RowId row_id, const tuple::Tuple& tuple);

    /// Frees `row_id`'s slot; the record bytes stay behind as garbage and
    /// the slot becomes reusable by later inserts. When the erase frees a
    /// slot on a page that was full, the page moves to the free list (at
    /// the tail — the oldest-emptied page is reused first).
    void erase(RowId row_id);

    /// True when `page_id` is currently resident in the file's pool — the
    /// introspection a CLI needs to report pool hits.
    bool resident(std::uint32_t page_id) const;

    /// Walks both free-space lists from the header and verifies the
    /// structure: head/tail pointers agree with the walk, every allocated
    /// page (1 .. next_page_id-1) is reachable exactly once, no page
    /// contradicts its list's occupancy rule, page 0 is on no list, and
    /// every on-disk data page is reachable (dirty pages may not be on
    /// disk yet, so not the other way around). Throws a description of
    /// the first violation. A diagnostic, never called by the storage
    /// operations themselves.
    void check_structure();

    /// Flushes and closes only this file (BufferManager::close_file);
    /// every other file the manager holds stays open.
    void close();

private:
    friend class HeapFileIterator;
    using DataPage = bufman::PinnedView<SlottedPage>;

    DataPage pin_page(std::uint32_t page_id);
    DataPage attach_page(bufman::PinnedPage pin);
    // pin_page for find/update/erase: also checks the row id against the
    // header page, the file's extent, and the slot's occupancy.
    DataPage pin_record_page(RowId row_id);
    DataPage pin_insert_target();

    // --- List surgery (the only writers of list pointers) -------------------
    // Every primitive acquires ALL its pins before its first write, in
    // ascending page-id order, so once the pins are held the surgery is
    // pure memory writes and cannot fail mid-way.

    /// Allocates page `page_id`, formats it, and makes it the head and
    /// tail of the (empty) free list. Returns the new page, pinned.
    bufman::PinnedPage append_fresh_page(std::uint32_t page_id);

    /// Moves `page` (pinned by the caller) from one list to the other:
    /// to_full=true sends a just-filled page from the free list to the
    /// full list, to_full=false sends a just-unfilled page back.
    void move_between_lists(bool to_full, DataPage& page);

    bufman::BufferManager& manager_;
    bufman::File file_;
    const schema::TableSchema* schema_ = nullptr; // caller-owned; outlives open
    // The record codec, bound to *schema_ at open; empty while closed.
    std::optional<schema::TupleSerializer> serializer_;
    std::size_t record_size_ = 0;
    bool open_ = false;
};

/// Deletes the heap file at `path` from disk with the standard file-remove
/// call. Refuses while the file is open through `manager`.
void erase_file(bufman::BufferManager& manager, const std::string& path);

}

#endif // EX11_HEAP_FILE_H
