#pragma once

// Limit — the stop operator: emits the child's first `max_rows` tuples,
// then reports exhaustion. The point of the operator is what it does NOT
// do: once the limit is reached it pulls nothing further from the child,
// so the whole pipeline below stops working (a scan stops advancing, a
// join stops pairing). A limit of zero yields nothing at all.

#include "operators/Operator.h"

#include <cstddef>
#include <optional>

namespace ops {

class Limit : public Operator {
public:
    // Purpose: bind the limit to `child` and the maximum tuple count.
    // Contract: `child` must outlive the limit.
    Limit(Operator& child, std::size_t max_rows);

    // Purpose: start one run; opens the child.
    // Contract: open() on an already-open limit asserts.
    void open() override;

    // Purpose: the next tuple within the limit.
    // Returns: the child's next tuple while fewer than `max_rows` have
    // been emitted, then nullopt for the rest of the run — with the child
    // left positioned wherever it was, not drained.
    // Contract: next() before open() asserts.
    std::optional<tuple::Tuple> next() override;

    // Purpose: end the run; closes the child.
    // Returns: void. Idempotent.
    void close() override;

private:
    Operator* child_;
    std::size_t max_rows_;
    std::size_t emitted_ = 0;
    bool running_ = false;
};

}
