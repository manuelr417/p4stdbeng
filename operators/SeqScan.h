#pragma once

// SeqScan — the leaf of every plan: emits one heap file's records in scan
// order, the order HeapFileIterator defines (the free list's pages in
// list order, then the full list's). The operator holds one scan iterator
// while running; the iterator pins a page only inside a next() call — it
// copies the record out and releases the pin before returning — so no pin
// is held across calls and a paused scan frames nothing.
//
// The scanned HeapFile is caller-owned: it must stay open and outlive the
// operator. The operator never mutates the file and never closes it —
// close() drops the iterator, nothing more.

#include "operators/Operator.h"

#include "heapfile/HeapFile.h"

#include <optional>

namespace ops {

class SeqScan : public Operator {
public:
    // Purpose: bind the scan to `file`, whose records it will emit.
    // Contract: `file` must be open and outlive the operator.
    explicit SeqScan(heapfile::HeapFile& file);

    // Purpose: start one run — a fresh scan from the file's beginning.
    // Contract: open() on an already-open scan asserts.
    void open() override;

    // Purpose: the next record of the scan, in scan order.
    // Returns: the tuple, or nullopt once the file's records are done;
    // further calls in the same run keep answering nullopt.
    // Throws: common::Error when the file is closed or a page is corrupt
    // (the iterator's own failures).
    // Contract: next() before open() asserts.
    std::optional<tuple::Tuple> next() override;

    // Purpose: end the run — drop the scan iterator (its page pin, if the
    // walk is mid-page, went home inside the last next()).
    // Returns: void. Idempotent; the file itself stays open.
    void close() override;

private:
    heapfile::HeapFile* file_;
    std::optional<heapfile::HeapFileIterator> scan_;
    bool running_ = false; // open() called and close() not yet
};

}
