#pragma once

// HashJoin — the join lecture's fifth algorithm (the Hash Join) as a
// Volcano operator, in the deck's two phases:
//
//   Phase I   (slide 25): partition BOTH inputs on the join key into
//             B-1 disk-resident buckets — every pair of equal keys lands
//             in the same bucket pair, because the key function is
//             deterministic.
//   Phase II  (slides 26-27): partition pair by partition pair — load the
//             build partition into an in-memory hash table T3, stream the
//             probe partition, and for each probe tuple
//
//                 Iterator I = T3.get(s[j])
//                 for each r in I do
//                     if r[i] = s[j] then add t = <r,s>
//
//             with the join predicate evaluated over every combined pair
//             — the hash narrows, the predicate decides, so the answer
//             never depends on the plan choice.
//
// Cost: read both inputs, write the buckets, read the buckets —
// 3(NPages(R) + NPages(S)) (the deck's formula; slide 29 states the
// memory condition that makes Phase II possible).
//
// Teaching contract: the PHASES are the assignment — a student
// re-implements this operator against what the lecture taught. The
// subtleties that would derail the assignment are PROVIDED here:
// JoinKey's canonicalization (numerics compare as doubles, so the
// evaluator's 5 = 5.0 holds — an int column and a double column with
// equal values must land in the same bucket; -0.0 is normalized to 0.0,
// the IEEE corner the predicate accepts), and hash_key, the hash
// function. The bucket files themselves are Partitions, also provided.
//
// Lifecycle: Phase I runs in open() — the Volcano contract's named
// exception where an operator materializes its input, because the
// algorithm requires it. No pins are held across next(): every bucket
// page is pinned and released inside the call.

#include "operators/Operator.h"
#include "operators/Partitions.h"

#include <cassert>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ops {

// The canonicalized join key: a double for every numeric (int and double
// columns meet when their values do, exactly as the evaluator compares
// them) or the string's bytes. Built only by join_key().
using JoinKey = std::variant<double, std::string>;

/// Purpose: canonicalize one column into its join key — the value the
/// way the evaluator compares it. Numerics become doubles (an int 5 and
/// a double 5.0 produce the SAME key), with -0.0 normalized to 0.0 so
/// the pair the predicate accepts is not split across buckets; strings
/// keep their bytes.
JoinKey join_key(const tuple::Column& column);

/// Purpose: the hash of a canonicalized join key, bucketing equal keys
/// together and (statistically) spreading distinct keys. PROVIDED: a
/// student wiring the phases uses this; nobody re-derives it.
std::size_t hash_key(const JoinKey& key);

/// PROVIDED: the in-memory table's plumbing — std::hash over hash_key
/// and the variant's own equality. Declared so the operator's T3 can be
/// a plain std::unordered_map; their bodies are one-liners.
struct JoinKeyHash {
    std::size_t operator()(const JoinKey& key) const { return hash_key(key); }
};
struct JoinKeyEq {
    bool operator()(const JoinKey& a, const JoinKey& b) const { return a == b; }
};

class HashJoin : public Operator {
public:
    // The caller's seams: where the join key lives in each side's tuple
    // (a position the executor knows when it closes the seam over the
    // side's key column).
    using KeyFn = std::function<JoinKey(const tuple::Tuple&)>;

    // The join predicate runs over the combined tuple: PROBE columns
    // first, BUILD columns appended after (the executor's orientation —
    // the probe side is the accumulated prefix, the build side the newest
    // FROM entry). A null predicate accepts every probed combination.
    using Predicate = std::function<bool(const tuple::Tuple&)>;

    // Purpose: bind the join to its two children, the key seams, the
    // predicate, and the two partition sets (one per input side, each
    // with that side's schema — the build side's table schema, the probe
    // side's prefix schema).
    // Contract: the children and the partition sets must outlive the
    // join; the two sets' bucket counts must agree (the same B-1).
    //
    // Parameters:
    // - `build` — the build side (the executor's newest FROM entry).
    //   Read to exhaustion in Phase I, every row written into the
    //   bucket named by hash_key(build_key(row)); in Phase II each
    //   build partition loads once into the in-memory table. Its
    //   columns trail every combined tuple (probe columns first).
    // - `probe` — the probe side (the accumulated prefix). Also fully
    //   partitioned in Phase I; in Phase II its current partition
    //   streams against the in-memory table, one bucket pair at a
    //   time. Its columns lead every combined tuple.
    // - `build_key` — where the join key lives in a build-side row:
    //   canonicalized through join_key() by the caller, so an int key
    //   and a double key with equal values meet in the same bucket
    //   pair. The executor closes this over the build tuple's key
    //   position.
    // - `probe_key` — the same seam for the probe side's tuple shape
    //   (the prefix's joined row — the key's position there is the
    //   executor's to know). Both sides must canonicalize the same
    //   way; that is what makes the partitioning sound.
    // - `predicate` — the join test over the combined tuple: probe
    //   columns first, build columns appended after. The hash
    //   narrows, this decides — the answer never depends on the plan
    //   choice. A null predicate accepts every probed combination
    //   (the hash key still drives which pairs ever meet).
    // - `build_partitions` — the build side's Phase I bucket files
    //   (a Partitions set over the join pool, created before this
    //   constructor runs, carrying the build side's schema). Phase I
    //   fills it; close() erases its files.
    // - `probe_partitions` — the probe side's bucket files, carrying
    //   the probe side's prefix schema. Same lifecycle; its bucket
    //   count must equal `build_partitions`'s.
    HashJoin(Operator& build, Operator& probe, KeyFn build_key, KeyFn probe_key,
             Predicate predicate, Partitions& build_partitions,
             Partitions& probe_partitions);

    /// Purpose: start one run — Phase I: opens both children, creates the
    /// bucket files, and partitions both children's full outputs on the
    /// join key (the Volcano contract's materialization exception); then
    /// loads partition pair 0 into memory for Phase II.
    /// Throws: common::Error from either child, the partition files, or
    /// the scratch directory.
    /// Contract: open() on an already-open join asserts.
    void open() override;

    /// Purpose: the next matching pair as one combined tuple — Phase II
    /// streamed: the current probe partition against the in-memory table
    /// of the current build partition, advancing through the partition
    /// pairs as they run dry.
    /// Returns: probe columns followed by build columns for the next
    /// combination passing the predicate, or nullopt once every partition
    /// pair is exhausted; further calls keep answering nullopt.
    /// Throws: common::Error from the bucket files or a predicate.
    /// Contract: next() before open() asserts.
    std::optional<tuple::Tuple> next() override;

    /// Purpose: end the run — erases the bucket files (the query-end
    /// erasure) and closes both children.
    /// Returns: void. Idempotent.
    void close() override;

private:
    // Phase I for one side: pulls the child to exhaustion, writing every
    // row into the bucket hash_key(key(row)) % bucket_count names.
    void partition_side(Operator& child, const KeyFn& key, Partitions& partitions);

    // Phase II's per-pair state: builds the in-memory table from build
    // bucket `index`, positions the probe scan on its bucket, and sets
    // partition_index_. past_bucket_count_ marks the exhaustion of every
    // pair.
    void load_partition(std::size_t index);

    Operator* build_;
    Operator* probe_;
    KeyFn build_key_;
    KeyFn probe_key_;
    Predicate predicate_;
    Partitions* build_partitions_;
    Partitions* probe_partitions_;

    // Phase II state, named after the lecture: the in-memory table T3
    // over the current build partition, the probe row being matched with
    // its position in the match vector (the cursor that survives across
    // next() calls — duplicates emit every combination once), and which
    // pair is loaded. The probe side's scan cursor lives in its
    // Partitions set.
    std::unordered_map<JoinKey, std::vector<tuple::Tuple>, JoinKeyHash, JoinKeyEq> table_;
    std::optional<tuple::Tuple> probe_row_;
    std::size_t match_position_ = 0;
    std::size_t partition_index_ = 0;
    bool past_bucket_count_ = false;
    bool running_ = false;
};

}
