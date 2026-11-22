#include "operators/Project.h"

#include <cassert>

namespace ops {

Project::Project(Operator& child, std::vector<std::size_t> indices)
    : child_(&child), indices_(std::move(indices)) {}

void Project::open() {
    assert(!running_ && "Project::open on an already open projection");
    child_->open();
    running_ = true;
}

std::optional<tuple::Tuple> Project::next() {
    assert(running_ && "Project::next before open");
    const std::optional<tuple::Tuple> row = child_->next();
    if (!row) {
        return std::nullopt;
    }
    tuple::Tuple out;
    for (const std::size_t index : indices_) {
        assert(index < row->column_count() &&
               "Project index past the child tuple's column count");
        out.add_column(row->column(index));
    }
    return out;
}

void Project::close() {
    if (running_) {
        child_->close();
        running_ = false;
    }
}

}
