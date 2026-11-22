# The Predicate, told by one query

## `SELECT * FROM car WHERE car_brand = 'Toyota' LIMIT 10;`

**The framing:** a WHERE condition is not code that runs. It is a condition that gets **compiled into a value**, and the executor **calls** that value once per row. The Predicate is that value. This presentation follows it — from SQL text to ten printed rows — through this one query, and opens up the one line where it all happens.

---

## The plan — shown once, with EXPLAIN

```
Limit (10)
  Project (cid, car_brand, car_model, car_year, car_price, car_owner)
    Filter (car.car_brand = 'Toyota')
      Scan car (heap)
```

Four nodes. One of them is our subject: **the Filter**. Everything below it manufactures candidate rows; everything above it moves rows the Filter approved. The WHERE clause lives in exactly one place — inside that Filter node — and there it is a Predicate object. The line `Filter (car.car_brand = 'Toyota')` is that object's name plate: the plan text showing where the condition was placed *before* the run begins.

## Act 1 — The condition becomes a value

Three transformations, each visible in code.

**Parse** — a tree that still speaks names:

```cpp
Binary{ Eq, Column{"car_brand"}, Literal{"Toyota"} }
```

**Bind** (once per statement) — names become positions; the catalog is consulted; type errors would refuse here:

```cpp
Binary{ Eq, Column{1}, Literal{"Toyota"} }        // column 1 = car_brand
```

**The Predicate** — the compiled tree captured as a callable value:

```cpp
std::shared_ptr<const BoundExpr> on;
on = bind_expr(where, car_schema);                // compile once

predicate = [on](const tuple::Tuple& row) {
    return truthy_value(evaluate(*on, row));
};
```

### That third snippet, with no magic left

One line, three pieces: `bool ← truthy_value( evaluate( *on , row ) )`.

**`on` — the captured tree.** Declared one line above the lambda (`Executor.cpp:1642`): `std::shared_ptr<const BoundExpr>`. A smart pointer to the tree that `bind_expr` built. Why a pointer? The tree owns its children through `unique_ptr`s — it cannot be sensibly copied, and a `std::function` must be *copyable*. So the executor captures **the handle**: copying the lambda copies the pointer (a refcount bump), never the tree. Why `const`? The tree is finished at bind time — *bind once, evaluate thousands of times* — and the type now promises nobody rewrites it.

**`*on` — dereferencing to the root node.** The expression `*on` *is* the `BoundExpr` struct itself, passed to `evaluate` by const reference (no copy per call). For our query that struct is:

```
predicate (the std::function)
   └── captured: on ──┐
                      ▼
              BoundExpr{Binary, Eq}            ← *on  is THIS struct
                ├── left  → BoundExpr{Column, column_index = 1}
                └── right → BoundExpr{Literal, "Toyota"}
```

**`evaluate` — the one interpreter.** Declared in `exec/Evaluator.h:16`: `Value evaluate(const BoundExpr& expr, const tuple::Tuple& row);`. One function, ~150 lines in `exec/Evaluator.cpp`, shared by every WHERE and every ON in the engine. It recursively walks the tree against the row and returns a `Value` — a `std::variant<int, double, std::string>`, *not* a bool. For our tree its recursion is two branches plus a comparison: the `Column` case reads `row.column(1)` (position 1 was baked in at bind time — no name lookup per row), the `Literal` case returns the carried `"Toyota"`, and the comparison lands through `compare_values` / `compare_op` (`Evaluator.cpp:136`) as **`Value{0}` or `Value{1}`** — SQL's false and true. It returns a `Value` rather than `bool` because a WHERE's top is just another expression: `WHERE car_price / 1000 > 50` runs arithmetic in the same interpreter.

**`truthy_value` — the last inch** (`Executor.cpp:58`):

```cpp
bool truthy_value(const Value& value) {
    const int* v = std::get_if<int>(&value);
    return v != nullptr && *v != 0;
}
```

`evaluate` said `Value{0}`; the Predicate's signature demands a `bool`. Three lines make the conversion: nonzero int → `true`.

**The sentence that replaces the magic:** *"dereference the captured pointer to reach the compiled tree, run the one shared interpreter over it against this row, and convert SQL's `Value{0}`/`Value{1}` into a C++ `bool`."*

And the point that makes the design click: **the lambda body contains no trace of Toyota.** The text `car_brand = 'Toyota'` lives only in the captured tree — data. The identical line evaluates `WHERE car_price > 50000 AND car_owner = 10` without changing a character: the *code* is universal, the *condition* is the captured value.

**The sentence students should hold:** after bind, the WHERE is a *value of type* `Predicate` (`std::function<bool(const Tuple&)>`) — storable, passable, callable. Not executed — *held*.

## Act 2 — The planner places it: inside the Filter

The catalog says no index serves `car_brand`, so nothing can answer the condition early. The executor stores the Predicate in a `Filter` node, between the scan and the projection — built once, before `open()`, before any row exists:

```cpp
nodes.push_back(std::make_unique<ops::Filter>(*scan, predicate));
```

From here on, "the WHERE clause" **is** `predicate_`, a member of the Filter: dormant machinery waiting for rows.

## Act 3 — The use: one pull at a time

Execution starts: `Limit` asks `Project`, `Project` asks `Filter`, `Filter` asks `Scan` — and then **calls the predicate on what comes back**. The actual opening of the run, with real rows (this heap scan starts at page 8):

```
pull 1   (8,0) cid=680, car_brand=Toyota, …   → TRUE   ↑ passes
pull 2   (8,1) cid=681, car_brand=BMW, …      → FALSE  ✗ dies here
pull 3   (8,2) cid=682, car_brand=Kia, …      → FALSE  ✗
pull 4   (8,3) cid=683, car_brand=Subaru, …   → FALSE  ✗
   ⋮      (8,4)…(8,8) Honda, Nissan, Mazda, Ford, Volvo → FALSE ✗
pull 11  (8,10) cid=690, car_brand=Toyota, …  → TRUE   ↑ passes
   ⋮
pull 91  cid=70, car_brand=Toyota, …          ← the 10th TRUE
```

One FALSE call, replayed through the anatomy we just opened:

```
predicate_(row cid=681, BMW, …)
   truthy_value( evaluate(*on, row) )
                     │
          *on ──▶ Binary{Eq}
                    ├── left:  Column{1}  → row.column(1) = "BMW"
                    └── right: Literal    → "Toyota"
          compare_values("BMW","Toyota") → nonzero → Value{0}
          truthy_value(Value{0}) → false          ✗ the row dies at the Filter
```

Same object, new defendant each call — it holds no memory of previous rows. And what it does **not** do is as instructive as what it does: it doesn't fetch pages (Scan's job), doesn't narrow columns (Project's), doesn't count (Limit's). One responsibility: **accept or reject the row it was handed.**

## Act 4 — The answers travel — or don't

- **TRUE** → the row continues up: `Project` copies its six columns in order, `Limit` increments its count. cid 680 is the first printed line.
- **FALSE** → the row stops at the Filter. `Project` never sees it; cid 681 simply ceases to exist for this query.

## Act 5 — The stop, and the object's lifetime

After the 10th TRUE (cid 70), `Limit` stops pulling. The scan is abandoned mid-table — the remaining ~609 rows are never read, and **the predicate is never called again**.

The run's final tally for this one object:

| | |
|---|---|
| Born | at bind time, before `open()` |
| Body | one shared pointer to `Binary{Eq, Column{1}, Literal{"Toyota"}}`, wrapped in a three-token body: `truthy_value(evaluate(*on, row))` |
| Lives in | `Filter::predicate_` — EXPLAIN's `Filter (…)` line is its name plate |
| Called | ≈ 91 times (21 rows on page 8 → 3 TRUE; 70 rows into page 1 → 7 TRUE) |
| Answers | 10 TRUE → the ten printed rows; ~81 FALSE → rows that died at the Filter |
| Dies | with the plan, at `close()` — rebuilt fresh by the next statement |

## The slide they take home

```
"car_brand = 'Toyota'"   ──parse──▶  names tree   ──bind──▶  Binary{Eq, Column{1}, "Toyota"}
      │                                                           │
      │                                    shared_ptr<const BoundExpr> on     (copyable handle,
      │                                                           │               immutable tree)
      │                                     predicate = [on](row){ truthy_value(evaluate(*on, row)); }
      │                                               │            ▲           ▲
      │                                               │            │           └─ Value→bool  (3 lines)
      │                                               │            └─ the ONE interpreter (tree+row in,
      │                                               │                              Value out)
      │                                               ▼
      │                                        Filter::predicate_     (EXPLAIN: Filter (…))
      │                                                           │ called per pulled row
      │                                             10 × TRUE ────┤──── ~81 × FALSE
      │                                          (rows printed)   │   (rows die at the Filter)
      │                                                           ▼
      └───────────────────────────────  10th TRUE  →  Limit stops pulling  →  object discarded
```

One condition, compiled once into an immutable tree, carried by a copyable pointer, wrapped in a three-token universal body, stored in one node, called once per row — silent forever after the tenth yes. That is the Predicate: the WHERE clause, as an object.
