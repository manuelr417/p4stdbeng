#include "operators/Filter.h"

#include <cassert>

namespace ops {

Filter::Filter(Operator& child, std::function<bool(const tuple::Tuple&)> predicate)
    : child_(&child), predicate_(std::move(predicate)) {}

void Filter::open() {
    assert(!running_ && "Filter::open on an already open filter");
    child_->open();
    running_ = true;
}

std::optional<tuple::Tuple> Filter::next() {
    assert(running_ && "Filter::next before open");
    // Pull until the predicate accepts or the child runs dry; the child's
    // own exhaustion is sticky, so the pull loop terminates.
    while (const std::optional<tuple::Tuple> row = child_->next()) {
        if (predicate_(*row)) {
            return row;
        }
    }
    return std::nullopt;
}

void Filter::close() {
    if (running_) {
        child_->close();
        running_ = false;
    }
}

}
