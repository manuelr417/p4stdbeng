#include "operators/SeqScan.h"

#include <cassert>

namespace ops {

SeqScan::SeqScan(heapfile::HeapFile& file) : file_(&file) {}

void SeqScan::open() {
    assert(!running_ && "SeqScan::open on an already open scan");
    assert(file_->is_open() && "SeqScan::open on a closed heap file");
    scan_.emplace(file_->scan());
    running_ = true;
}

std::optional<tuple::Tuple> SeqScan::next() {
    assert(running_ && "SeqScan::next before open");
    // One pin inside the iterator's next(); released before the call
    // returns. The iterator's own exhaustion is sticky — it keeps
    // answering nullopt for the rest of the run.
    return scan_->next();
}

void SeqScan::close() {
    scan_.reset();
    running_ = false;
}

}
