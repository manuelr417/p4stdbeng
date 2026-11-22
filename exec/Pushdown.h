#pragma once

// Pushdown — the WHERE splitter. The join lecture's standing assumption:
// selections on single tables are taken first. This is the bind-time pass
// that makes it real: the WHERE expression splits into AND-conjuncts,
// each is bound, and each is classified by its referenced-table set. A
// conjunct whose columns all live in one FROM entry is pushed to that
// entry's scan (bound with table-local positions); every other conjunct —
// multi-table, or column-free — stays above the join, bound with joined
// positions. The answer never depends on the split; the oracle runs
// queries with the classification deliberately bypassed.

#include "exec/Binder.h"
#include "exec/JoinedSchema.h"
#include "sql/Ast.h"

#include <cstddef>
#include <vector>

namespace exec {

struct PushedConjunct {
    BoundExprPtr expr;           // bound with table-local column positions
    std::size_t table_position;  // which FROM entry's scan evaluates it
};

struct PushdownSplit {
    std::vector<PushedConjunct> pushed;    // one scan-side filter each
    std::vector<BoundExprPtr> residual;    // evaluated over combined rows
};

// Purpose: split, bind, and classify `where` against `schema`.
// Returns: the split. An empty WHERE never reaches here — callers pass
// the expression only when one parsed.
// Throws: common::Error by name from binding (unknown or ambiguous
// columns, type mismatches) — before any classification is returned.
PushdownSplit split_where(const sql::Expr& where, const JoinedSchema& schema);

}
