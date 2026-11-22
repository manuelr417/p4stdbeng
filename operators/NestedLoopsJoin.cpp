#include "operators/NestedLoopsJoin.h"

#include <cassert>
#include <utility>

namespace ops {

NestedLoopsJoin::NestedLoopsJoin(Operator& outer, Operator& inner, Predicate predicate)
    : outer_(&outer), inner_(&inner), predicate_(std::move(predicate)) {}

void NestedLoopsJoin::open() {
    assert(!running_ && "NestedLoopsJoin::open on an already open join");
    outer_->open();
    inner_->open();
    outer_row_ = outer_->next();
    running_ = true;
}

std::optional<tuple::Tuple> NestedLoopsJoin::next() {
    assert(running_ && "NestedLoopsJoin::next before open");
    // The cursor: walk the inner child under the current outer tuple;
    // when it runs dry, advance the outer tuple and restart the inner
    // scan. A fresh inner pass per outer tuple — the lecture's cost.
    while (outer_row_) {
        while (const std::optional<tuple::Tuple> inner_row = inner_->next()) {
            tuple::Tuple combined = *outer_row_;
            for (std::size_t i = 0; i < inner_row->column_count(); ++i) {
                combined.add_column(inner_row->column(i));
            }
            if (!predicate_ || predicate_(combined)) {
                return combined;
            }
        }
        outer_row_ = outer_->next();
        if (outer_row_) {
            inner_->close();
            inner_->open();
        }
    }
    return std::nullopt;
}

void NestedLoopsJoin::close() {
    if (running_) {
        outer_->close();
        inner_->close(); // idempotent: an exhausted run already closed it
        running_ = false;
    }
}

}
