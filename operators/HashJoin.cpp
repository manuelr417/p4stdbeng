#include "operators/HashJoin.h"

#include <cstdint>
#include <cstring>

namespace ops {

// --- the provided subtleties ------------------------------------------------

JoinKey join_key(const tuple::Column& column) {
    switch (column.column_type()) {
        case tuple::ColumnType::Integer: {
            // Numerics canonicalize to double: the evaluator promotes
            // ints, so 5 = 5.0 is true and both must share one bucket.
            const double value =
                static_cast<double>(std::get<int>(column.column_value()));
            return JoinKey(value == 0.0 ? 0.0 : value);  // -0.0 -> 0.0
        }
        case tuple::ColumnType::Double: {
            const double value = std::get<double>(column.column_value());
            return JoinKey(value == 0.0 ? 0.0 : value);  // -0.0 -> 0.0
        }
        case tuple::ColumnType::String:
            return JoinKey(std::get<std::string>(column.column_value()));
    }
    assert(false && "unreachable: every ColumnType is handled");
    return JoinKey(0.0);
}

std::size_t hash_key(const JoinKey& key) {
    // One hash for both alternatives: the variant's index salts the mix,
    // the value feeds it. Splitmix64's finalizer over the bytes — enough
    // mixing for the teaching datasets, a few lines to read.
    const std::size_t salt =
        key.index() == 0 ? 0x9e3779b97f4a7c15ULL : 0xbf58476d1ce4e5b9ULL;
    std::size_t h = salt;
    if (const double* value = std::get_if<double>(&key)) {
        // Bit-pattern the double: an int 5 and a double 5.0
        // canonicalize to the same key and hash identically; the
        // -0.0 -> 0.0 normalization already happened in join_key.
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(*value));
        std::memcpy(&bits, value, sizeof(bits));
        h ^= bits;
    } else {
        const std::string& text = std::get<std::string>(key);
        for (const char c : text) {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ULL;  // FNV-1a's prime over the bytes
        }
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

// --- the operator -----------------------------------------------------------

HashJoin::HashJoin(Operator& build, Operator& probe, KeyFn build_key, KeyFn probe_key,
                   Predicate predicate, Partitions& build_partitions,
                   Partitions& probe_partitions)
    : build_(&build), probe_(&probe), build_key_(std::move(build_key)),
      probe_key_(std::move(probe_key)), predicate_(std::move(predicate)),
      build_partitions_(&build_partitions), probe_partitions_(&probe_partitions) {
    assert(build_partitions_->bucket_count() == probe_partitions_->bucket_count() &&
           "the two partition sets must agree on the bucket count");
}

// HashJoin::partition_side — Phase I for one side: pull the child to
// exhaustion and route every row into its bucket. One insert per row —
// pins live inside the call.
//
// TODO(student): implement Phase I's routing (the lecture's partition
// phase, slide 25; the class comment in operators/HashJoin.h holds the
// contract, and operators/Partitions.h documents the bucket-file
// interface — create/write/scan_start/scan_next/erase_all). While the
// child yields a row, compute its bucket as
// hash_key(key(row)) % partitions.bucket_count() and write the row
// there (partitions.write). The key function is already
// canonicalized — equal values land in equal buckets.
void HashJoin::partition_side(Operator& child, const KeyFn& key,
                              Partitions& partitions) {
    (void)child;
    (void)key;
    (void)partitions;
}

// HashJoin::load_partition — Phase II's per-pair setup: build the
// in-memory table T3 from build bucket `index` (rows grouped by their
// canonicalized key — the lecture's bucket of matches, drawn in code),
// position the probe scan on the matching bucket, and reset the cursor
// state. Past the last bucket index, mark the exhaustion
// (past_bucket_count_) — the sticky end-of-run flag next() tests.
//
// TODO(student): implement the per-pair setup (slides 26-27; the member
// declarations in operators/HashJoin.h name every piece of cursor
// state). When index is past bucket_count: set past_bucket_count_,
// clear probe_row_, and return. Otherwise clear table_, walk build
// bucket `index` (scan_start/scan_next on build_partitions_) grouping
// each row into table_ under build_key_(row), then scan_start the
// probe side's bucket `index`, and reset probe_row_, match_position_,
// partition_index_, past_bucket_count_.
void HashJoin::load_partition(std::size_t index) {
    (void)index;
}

// HashJoin::open — start one run. This is Phase I (the Volcano
// contract's named exception where an operator materializes its input,
// because the algorithm requires it): create both partition sets'
// bucket files, partition BOTH children's full outputs on the join
// key, then load partition pair 0 for Phase II.
//
// TODO(student): implement open — the whole of Phase I (slide 25; the
// class comment in operators/HashJoin.h holds the contract). Assert the
// join is not already open; open both children; create both partition
// sets (create()); route each side with partition_side (build first,
// then probe); load_partition(0); set running_.
void HashJoin::open() {
}

// HashJoin::next — Phase II, streamed partition pair by partition pair
// (slides 26-27): for the current probe row, look up its canonicalized
// key in the in-memory table and emit every matching build row as one
// combined tuple — probe columns first, build columns appended, the
// join predicate deciding over the pair (the hash narrows, the
// predicate decides). When the probe row's matches run dry, take the
// next probe row of this bucket; when the bucket runs dry, load the
// next partition pair; when the pairs run out, answer nullopt — and
// keep answering nullopt (sticky exhaustion).
//
// TODO(student): implement next (slides 26-27; the member comments in
// operators/HashJoin.h name the cursor state — probe_row_,
// match_position_, partition_index_, past_bucket_count_ — and
// operators/NestedLoopsJoin.cpp, provided, shows the sibling
// emit-a-combination shape). The loop: while a probe row is present,
// find its bucket group (table_.find(probe_key_(*probe_row_))) and walk
// its matches with match_position_, combining and returning the pairs
// the predicate accepts; then advance to the next probe row
// (probe_partitions_->scan_next()) or, when the bucket is dry,
// load_partition(partition_index_ + 1). past_bucket_count_ ends the
// run. Honor the header's contract: assert next() is not called before
// open().
std::optional<tuple::Tuple> HashJoin::next() {
    return std::nullopt;
}

// HashJoin::close — end the run: erase both partition sets' bucket
// files (the query-end erasure — the files exist only while a run is
// live), clear the in-memory state, and close both children.
// Idempotent: a closed join closes as a no-op.
//
// TODO(student): implement close per the header's contract — when the
// run is live: clear table_ and the cursor state, erase_all() both
// partition sets, close both children, drop running_.
void HashJoin::close() {
}

}
