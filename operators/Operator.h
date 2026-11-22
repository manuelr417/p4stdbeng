#pragma once

// Operator — the Volcano iterator contract every relational operator in
// the engine obeys. A plan is a tree of operators: the leaves scan heap
// files, the interior nodes (filter, project, join, limit) each pull
// tuples from their children one next() call at a time and emit their own
// output the same way. Execution never materializes a child's full input
// before the first tuple leaves the root — a LIMIT near the root stops
// the whole pipeline pulling.
//
// The lifecycle is open -> next... -> close, each exactly once per run,
// in that order. close() must run even when the consumer abandons the
// pipeline early (a satisfied LIMIT, an error unwinding): it releases
// every resource the operator holds and closes the children.
//
// The pin discipline: an operator holds no buffer-pool pins ACROSS next()
// calls. Each call pins the pages it reads and releases them before it
// returns, so a half-consumed pipeline can be paused indefinitely without
// framing out the session's pool.
//
// Failure discipline (the house rules): a state a caller or a file can
// genuinely produce — a corrupt page, an I/O failure — throws
// common::Error and is caught at the boundary; misusing the contract
// (next() before open(), open() on an open operator) is an assert whose
// message documents the contract.

#include "tuple/Tuple.h"

#include <optional>

namespace ops {

class Operator {
public:
    virtual ~Operator() = default;

    /// Purpose: initialize the operator for one run and open every child.
    /// The first next() of a run must follow open().
    /// Contract: calling open() on an already-open operator is a contract
    /// violation and asserts.
    virtual void open() = 0;

    /// Purpose: produce the operator's next output tuple.
    /// Returns: the tuple, or nullopt once this operator is exhausted;
    /// every call after the first nullopt also returns nullopt until the
    /// operator is closed and re-opened.
    /// Throws: common::Error for reachable failures (I/O, a corrupt page).
    /// Contract: calling next() before open() asserts.
    virtual std::optional<tuple::Tuple> next() = 0;

    /// Purpose: release the operator's resources and close every child.
    /// Returns: void. Idempotent — closing a closed operator is a no-op.
    /// After close() the operator accepts only a fresh open().
    virtual void close() = 0;
};

}
