#pragma once

// RowIdScan — the index access path as an operator: emits the rows at
// the row ids a caller-supplied source produces, fetched from the heap
// file one find() per tuple. The source runs once at open() — an index
// range call is eager by nature — and the fetches stream one per next(),
// each pin scoped inside the call, so a half-consumed index scan frames
// nothing.
//
// This is the operator that lets a SELECT announce `(via index scan)`
// while every row still flows through the same pipeline contract as a
// heap scan: the planner's bounds pick the row ids; the operator and the
// nodes above it never know an index was involved.

#include "operators/Operator.h"

#include "heapfile/HeapFile.h"

#include <functional>
#include <optional>
#include <vector>

namespace ops {

class RowIdScan : public Operator {
public:
    // Produces the row ids this scan emits, in order. Called once per
    // open(); may return an empty vector (an index that answers nothing).
    using RowIdSource = std::function<std::vector<heapfile::RowId>()>;

    // Purpose: bind the scan to `file` and the row-id source.
    // Contract: `file` must be open and outlive the operator; the source
    // must return row ids that are valid in `file` at fetch time.
    RowIdScan(heapfile::HeapFile& file, RowIdSource source);

    // Purpose: start one run — invokes the source and positions at its
    // first row id.
    // Contract: open() on an already-open scan asserts.
    void open() override;

    // Purpose: the next row of the source's list.
    // Returns: the tuple at the next row id, or nullopt once the list is
    // done; further calls keep answering nullopt.
    // Throws: common::Error when the file is closed, a row id names no
    // live record (the index promised it), or a page is corrupt.
    // Contract: next() before open() asserts.
    std::optional<tuple::Tuple> next() override;

    // Purpose: end the run — drop the row-id list.
    // Returns: void. Idempotent; the file itself stays open.
    void close() override;

private:
    heapfile::HeapFile* file_;
    RowIdSource source_;
    std::vector<heapfile::RowId> row_ids_;
    std::size_t position_ = 0;
    bool running_ = false;
};

}
