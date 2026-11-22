#pragma once

// IndexNestedLoopsJoin — the join lecture's third algorithm (the Index
// Nested Loops Join) as a Volcano operator: the outer child is scanned
// once, and for each outer tuple the inner side is produced by probing
// an index with that tuple's key — slide 15's
//
//     for each r in R do
//         Iterator T = I.search(r[i])
//         for each s in T do
//             add t = <r,s>
//
// with the probe and the fetches as the caller's seams. Each matching
// inner row is fetched from the inner heap file and combined with the
// outer tuple (outer columns followed by inner columns), an optional
// inner-side predicate filters the fetched row before combining (the
// pushed single-table conjuncts, whose scan-side Filter has no home on
// this path), and the join predicate is evaluated over every combined
// pair — the index narrows, the predicate decides, so the answer never
// depends on the plan choice.
//
// Cursor state: the current outer tuple plus the position in its probe
// result. No page pins are held across next() calls — the probe and each
// fetch pin inside the call and release before returning.

#include "operators/Operator.h"

#include "heapfile/HeapFile.h"

#include <functional>
#include <optional>
#include <vector>

namespace ops {

class IndexNestedLoopsJoin : public Operator {
public:
    // Produces the inner row ids matching one outer tuple — the caller's
    // index probe (the executor closes it over SessionIndex::range with
    // the outer key as both bounds). The outer tuple is the combined row
    // of the outer child's output; the key's position is the caller's to
    // know. Must return row ids valid in the inner file.
    using Probe = std::function<std::vector<heapfile::RowId>(const tuple::Tuple&)>;

    // The join predicate runs over the combined tuple: outer columns at
    // their positions, inner columns appended after. A null predicate
    // accepts every probed combination.
    using Predicate = std::function<bool(const tuple::Tuple&)>;

    // The inner-side predicate runs over one fetched raw inner row (the
    // pushed single-table conjuncts, bound with table-local positions).
    // A null predicate filters nothing.
    using InnerPredicate = std::function<bool(const tuple::Tuple&)>;

    // Purpose: bind the join to its outer child, the inner file, the
    // probe, and the predicates.
    // Contract: `outer` and `inner_file` must outlive the join; the file
    // must be open; `probe` must return row ids valid in `inner_file`.
    //
    // Parameters:
    // - `outer` — the outer (left) child. Scanned exactly once; one
    //   probe per its tuple, and its tuple leads every combined result
    //   (outer columns first).
    // - `inner_file` — the inner table's open heap file. Every row id
    //   the probe returns is fetched from it (one find() pin per row,
    //   released before next() returns); its columns are appended
    //   after the outer's.
    // - `probe` — the caller's index probe: for one outer tuple it
    //   returns the row ids of `inner_file` whose indexed key equals
    //   the outer's (the executor closes it over the index's range
    //   with the outer key as both bounds). Where the key sits in the
    //   outer tuple is the caller's to know. This is the narrowing
    //   half of the algorithm — the operator never re-checks that the
    //   probe was right, it trusts the row ids.
    // - `predicate` — the join test over the combined tuple: outer
    //   columns at their positions, inner columns appended after.
    //   The index narrows, this decides — the answer never depends
    //   on the plan choice. A null predicate accepts every probed
    //   combination.
    // - `inner_predicate` — optional filter over one raw fetched
    //   inner row, before combining: the pushed single-table
    //   conjuncts, bound with table-local positions (their scan-side
    //   Filter has no home on this path — the inner side is a file
    //   and a probe, not a scan pipeline). A rejected row never pays
    //   the combine. A null predicate filters nothing.
    IndexNestedLoopsJoin(Operator& outer, heapfile::HeapFile& inner_file, Probe probe,
                         Predicate predicate, InnerPredicate inner_predicate = nullptr);

    // Purpose: start one run: opens the outer child and pulls its first
    // tuple.
    // Contract: open() on an already-open join asserts.
    void open() override;

    // Purpose: the next matching pair as one combined tuple.
    // Returns: outer columns followed by inner columns for the next
    // probed combination passing the predicates, or nullopt once the
    // outer side is exhausted; further calls keep answering nullopt.
    // Throws: common::Error from the outer child, the probe, a fetch on
    // a closed or foreign row id, or a predicate.
    // Contract: next() before open() asserts.
    std::optional<tuple::Tuple> next() override;

    // Purpose: end the run; closes the outer child and drops the probe
    // state.
    // Returns: void. Idempotent.
    void close() override;

private:
    Operator* outer_;
    heapfile::HeapFile* inner_file_;
    Probe probe_;
    Predicate predicate_;
    InnerPredicate inner_predicate_;
    std::optional<tuple::Tuple> outer_row_;  // current outer tuple; nullopt = done
    std::vector<heapfile::RowId> matches_;   // the current probe's row ids
    std::size_t position_ = 0;               // next index into matches_
    bool running_ = false;
};

}
