#include "operators/RowIdScan.h"

#include <cassert>
#include <utility>

namespace ops {

RowIdScan::RowIdScan(heapfile::HeapFile& file, RowIdSource source)
    : file_(&file), source_(std::move(source)) {}

void RowIdScan::open() {
    assert(!running_ && "RowIdScan::open on an already open scan");
    assert(file_->is_open() && "RowIdScan::open on a closed heap file");
    row_ids_ = source_();
    position_ = 0;
    running_ = true;
}

std::optional<tuple::Tuple> RowIdScan::next() {
    assert(running_ && "RowIdScan::next before open");
    if (position_ >= row_ids_.size()) {
        return std::nullopt; // sticky: the list is fixed for the run
    }
    // One find() pin inside this call; released before returning.
    return file_->find(row_ids_[position_++]);
}

void RowIdScan::close() {
    row_ids_.clear();
    position_ = 0;
    running_ = false;
}

}
