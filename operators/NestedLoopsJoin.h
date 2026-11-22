#pragma once

// NestedLoopsJoin — the lecture's tuple-at-a-time nested loops join as a
// Volcano operator: the outer child is scanned once, and for each outer
// tuple the inner child is rescanned, emitting every combined tuple
// (outer columns followed by inner columns) whose predicate over the
// combination holds. A null predicate is a cross join: every combination.
//
// The cost model is the lecture's, made real: one inner pass per outer
// tuple, each pass a fresh scan through the buffer pool. The inner child
// is restarted — close() then open() — between outer tuples, which any
// operator honoring the iterator contract supports (SeqScan restarts from
// its beginning; the pass-through operators forward the restart).
//
// Cursor state: the current outer tuple plus the inner child's own
// position. No page pins are held across next() calls — each fetch pins
// inside the call and releases before returning.

#include "operators/Operator.h"

#include <functional>
#include <optional>

namespace ops {

class NestedLoopsJoin : public Operator {
public:
    // The predicate runs over the combined tuple: outer columns at their
    // positions, inner columns appended after. A null predicate accepts
    // every combination (cross join).
    using Predicate = std::function<bool(const tuple::Tuple&)>;

    // Purpose: bind the join to its two children and the predicate.
    // Contract: both children must outlive the join.
    //
    // Parameters:
    // - `outer` — the outer (left) child. Scanned exactly once; its
    //   current tuple drives the loop and leads every combined tuple
    //   (outer columns first).
    // - `inner` — the inner (right) child. Restarted — close() then
    //   open() — for every outer tuple, so any iterator-honoring
    //   operator works; each fresh pass re-reads it through the pool.
    //   Its columns are appended after the outer's.
    // - `predicate` — the join test over the combined tuple: outer
    //   columns at their positions, inner columns appended after. The
    //   caller (the executor) binds it over the joined schema; the
    //   operator only calls it. A null predicate accepts every
    //   combination — the cross join.
    NestedLoopsJoin(Operator& outer, Operator& inner, Predicate predicate);

    // Purpose: start one run: opens both children and pulls the first
    // outer tuple.
    // Contract: open() on an already-open join asserts.
    void open() override;

    // Purpose: the next matching pair as one combined tuple.
    // Returns: outer columns followed by inner columns for the next
    // combination satisfying the predicate, or nullopt once the outer
    // side is exhausted; further calls keep answering nullopt.
    // Throws: common::Error from either child or the predicate.
    // Contract: next() before open() asserts.
    std::optional<tuple::Tuple> next() override;

    // Purpose: end the run; closes both children (the inner may already
    // be closed — a run that ended by outer exhaustion restarts it only
    // for another outer tuple).
    // Returns: void. Idempotent.
    void close() override;

private:
    Operator* outer_;
    Operator* inner_;
    Predicate predicate_;
    std::optional<tuple::Tuple> outer_row_; // current outer tuple; nullopt = done
    bool running_ = false;
};

}
