#include "operators/Limit.h"

#include <cassert>

namespace ops {

Limit::Limit(Operator& child, std::size_t max_rows)
    : child_(&child), max_rows_(max_rows) {}

void Limit::open() {
    assert(!running_ && "Limit::open on an already open limit");
    child_->open();
    emitted_ = 0;
    running_ = true;
}

std::optional<tuple::Tuple> Limit::next() {
    assert(running_ && "Limit::next before open");
    if (emitted_ >= max_rows_) {
        return std::nullopt; // the child is not drained — just not pulled
    }
    const std::optional<tuple::Tuple> row = child_->next();
    if (row) {
        ++emitted_;
    }
    return row;
}

void Limit::close() {
    if (running_) {
        child_->close();
        running_ = false;
    }
}

}
