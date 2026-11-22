#pragma once

// Filter — the selection operator: emits exactly the child tuples its
// predicate accepts, in the child's order. The predicate is a function
// the plan builder binds against the child's schema; the operator itself
// never sees column names or types. Evaluation is one call per candidate
// tuple, inside next() — a throwing predicate fails the pipeline with the
// operator's close() still owed by the consumer.

#include "operators/Operator.h"

#include <functional>
#include <optional>

namespace ops {

class Filter : public Operator {
public:
    // Purpose: bind the filter to `child` (already constructed) and the
    // predicate each tuple must satisfy.
    // Contract: `child` must outlive the filter; the predicate must be
    // callable for every tuple the child emits.
    Filter(Operator& child, std::function<bool(const tuple::Tuple&)> predicate);

    // Purpose: start one run; opens the child.
    // Contract: open() on an already-open filter asserts.
    void open() override;

    // Purpose: the next accepted tuple.
    // Returns: the child's next tuple for which the predicate is true, or
    // nullopt once the child is exhausted; further calls keep answering
    // nullopt.
    // Throws: common::Error from the child, or from the predicate.
    // Contract: next() before open() asserts.
    std::optional<tuple::Tuple> next() override;

    // Purpose: end the run; closes the child.
    // Returns: void. Idempotent.
    void close() override;

private:
    Operator* child_;
    std::function<bool(const tuple::Tuple&)> predicate_;
    bool running_ = false;
};

}
