#include "operators/IndexNestedLoopsJoin.h"

#include <cassert>
#include <utility>

namespace ops {

IndexNestedLoopsJoin::IndexNestedLoopsJoin(Operator& outer, heapfile::HeapFile& inner_file,
                                           Probe probe, Predicate predicate,
                                           InnerPredicate inner_predicate)
    : outer_(&outer), inner_file_(&inner_file), probe_(std::move(probe)),
      predicate_(std::move(predicate)), inner_predicate_(std::move(inner_predicate)) {}

// IndexNestedLoopsJoin::open — start one run: open the outer child and
// pull its first tuple; if there is one, probe the index with it and
// position at the probe result's start. This is the "for each r in R"
// initialization: one outer tuple in hand, one probe's row ids waiting.
//
// TODO(student): implement open (the lecture's index nested loops join,
// slide 15; the class comment in operators/IndexNestedLoopsJoin.h holds
// the full contract). Open the outer child, pull its first tuple into
// outer_row_, and — when a tuple came back — fill matches_ by probing
// with it (probe_(*outer_row_)) and reset position_ to 0. Set running_.
// Honor the header's contract: assert (or refuse) an already-open join
// and a closed inner file.
void IndexNestedLoopsJoin::open() {
}

// IndexNestedLoopsJoin::next — the next matching pair as one combined
// tuple: walk the current probe's row ids under the current outer tuple
// (fetch each from the inner file, apply the inner predicate to the raw
// row BEFORE combining, append the inner columns after the outer's,
// then let the join predicate decide over the combination); when the
// probe result runs dry, advance the outer tuple and probe again — one
// probe per outer tuple, the lecture's cost.
//
// TODO(student): implement next (slide 15's
//     for each r in R do
//         Iterator T = I.search(r[i])
//         for each s in T do
//             add t = <r,s>
// ; the class comment in operators/IndexNestedLoopsJoin.h holds the
// full contract, and operators/NestedLoopsJoin.cpp — provided — shows
// the sibling cursor shape). While outer_row_ is present: fetch
// matches_[position_] from inner_file_ (one find() pin, released by the
// call), skip the row when inner_predicate_ is set and rejects it,
// otherwise combine (outer columns first, inner appended) and return it
// when predicate_ is null or accepts. When matches_ run dry, pull the
// next outer tuple and probe again. Return nullopt once the outer side
// is exhausted — and keep answering nullopt afterwards.
std::optional<tuple::Tuple> IndexNestedLoopsJoin::next() {
    return std::nullopt;
}

// IndexNestedLoopsJoin::close — end the run: close the outer child and
// drop the probe state (the matches vector, the cursor, the running
// flag). Idempotent: a closed join closes as a no-op.
//
// TODO(student): implement close per the header's contract.
void IndexNestedLoopsJoin::close() {
}

}
