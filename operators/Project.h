#pragma once

// Project — the projection operator: emits each child tuple trimmed to
// the columns at `indices`, in that order. Duplicated indices duplicate
// columns; an index at or past the child's column count is a contract
// violation caught per tuple. Projection never reorders or merges rows —
// one child tuple in, one output tuple out.

#include "operators/Operator.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace ops {

class Project : public Operator {
public:
    // Purpose: bind the projection to `child` and the output column
    // indices (positions in the child tuple, 0-based, output order).
    // Contract: `child` must outlive the projection.
    Project(Operator& child, std::vector<std::size_t> indices);

    // Purpose: start one run; opens the child.
    // Contract: open() on an already-open projection asserts.
    void open() override;

    // Purpose: the next projected tuple.
    // Returns: the child's next tuple trimmed to `indices`, or nullopt
    // once the child is exhausted; further calls keep answering nullopt.
    // Contract: next() before open() asserts; an index past the child
    // tuple's column count asserts.
    std::optional<tuple::Tuple> next() override;

    // Purpose: end the run; closes the child.
    // Returns: void. Idempotent.
    void close() override;

private:
    Operator* child_;
    std::vector<std::size_t> indices_;
    bool running_ = false;
};

}
