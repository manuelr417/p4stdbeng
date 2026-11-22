// p4stdbeng — the operators suite: the Volcano pipeline operators, one
// section per operator. The join section ends with the two-form
// equivalence and the pipeline oracle (see the nested-loops-joins
// change's specs).

#include "bufpool/BufferManager.h"
#include "check_support.h"
#include "heapfile/HeapFile.h"
#include "operators/Filter.h"
#include "operators/HashJoin.h"
#include "operators/IndexNestedLoopsJoin.h"
#include "operators/Limit.h"
#include "operators/NestedLoopsJoin.h"
#include "operators/Partitions.h"
#include "operators/Project.h"
#include "operators/SeqScan.h"
#include "policy/LRUPolicy.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "tuple/Tuple.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using checks::ok;

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
    std::fflush(stdout);
}

// --- fixture: a small multi-page heap file ----------------------------------
//
// Rows of (id, 900-byte pad) — the pad forces ~4 records per 4096-byte
// page, so a 10-row file spans 3 pages and the scan order (free list's
// pages in list order, then the full list's) is observable. Built by
// plain inserts, the scan order is the insertion order.

const char* const kFixtureSchema = R"({"table_name": "opcase",
    "columns": [
      {"column_name": "id",  "column_type": "Integer", "size": 4},
      {"column_name": "pad", "column_type": "String",  "size": 900}]})";

tuple::Tuple fixture_row(int id) {
    tuple::Tuple row;
    row.add(id);
    row.add(std::string(900, static_cast<char>('a' + id % 26)));
    return row;
}

// Fills a fresh file at `path` with ids 0..count-1 and leaves it open.
heapfile::HeapFile make_file(bufman::BufferManager& manager, const std::string& path,
                             const schema::TableSchema& schema, int count) {
    std::filesystem::remove(path);
    heapfile::HeapFile file(manager);
    file.create(path, schema);
    file.open(path, schema);
    for (int id = 0; id < count; ++id) {
        file.insert(fixture_row(id));
    }
    return file;
}

std::vector<int> scan_ids(ops::Operator& op) {
    std::vector<int> ids;
    op.open();
    while (const std::optional<tuple::Tuple> row = op.next()) {
        ids.push_back(std::get<int>(row->column(0).column_value()));
    }
    op.close();
    return ids;
}

// --- SeqScan -----------------------------------------------------------------

void seq_scan_checks() {
    const schema::TableSchema schema = schema::parse_table_schema(kFixtureSchema);
    const std::string path = "opcase.bin";

    {
        bufman::BufferManager manager;
        manager.init(8, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile file = make_file(manager, path, schema, 10);

        // The scan yields every inserted row, in the heap's scan order —
        // the exact sequence HeapFileIterator produces (with a partial
        // tail page, the free list's page comes first).
        std::vector<int> raw_ids;
        heapfile::HeapFileIterator raw = file.scan();
        while (const std::optional<tuple::Tuple> row = raw.next()) {
            raw_ids.push_back(std::get<int>(row->column(0).column_value()));
        }
        ops::SeqScan scan(file);
        const std::vector<int> ids = scan_ids(scan);
        check(ids == raw_ids && ids.size() == 10,
              "SeqScan emits every row in scan order");

        // A half-consumed scan closes cleanly (no pin leaks past close()),
        // and a fresh open() runs the scan again from the beginning.
        scan.open();
        check(scan.next().has_value(), "SeqScan re-open produces a first row");
        scan.close();
        const std::vector<int> again = scan_ids(scan);
        check(again == ids, "SeqScan re-open restarts from the beginning");
        scan.close();

        // Exhaustion is sticky within a run.
        scan.open();
        while (scan.next().has_value()) {
        }
        check(!scan.next().has_value(), "SeqScan exhaustion is sticky");
        scan.close();

        // Every pin the scan took is home: closing the manager asserts if
        // a frame is still pinned.
        manager.close_all();
        check(true, "SeqScan leaves no pinned frames");
    }
    std::filesystem::remove(path);
}

// --- a counting child --------------------------------------------------------
//
// Emits a fixed vector of tuples and counts every next() call — the probe
// the Limit checks read to prove pulling stops at the limit.

class CountingOperator : public ops::Operator {
public:
    explicit CountingOperator(std::vector<tuple::Tuple> rows)
        : rows_(std::move(rows)) {}

    void open() override {
        assert(!running_ && "CountingOperator::open on an open operator");
        position_ = 0;
        running_ = true;
    }

    std::optional<tuple::Tuple> next() override {
        assert(running_ && "CountingOperator::next before open");
        ++pulls_;
        if (position_ >= rows_.size()) {
            return std::nullopt;
        }
        return rows_[position_++];
    }

    void close() override { running_ = false; }

    std::size_t pulls() const { return pulls_; }

private:
    std::vector<tuple::Tuple> rows_;
    std::size_t position_ = 0;
    std::size_t pulls_ = 0;
    bool running_ = false;
};

// --- Filter, Project, Limit ---------------------------------------------------

void filter_project_limit_checks() {
    const schema::TableSchema schema = schema::parse_table_schema(kFixtureSchema);
    const std::string path = "opcase.bin";

    {
        bufman::BufferManager manager;
        manager.init(8, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile file = make_file(manager, path, schema, 10);

        // The ground truth: the file's own scan order.
        std::vector<int> raw_ids;
        heapfile::HeapFileIterator raw = file.scan();
        while (const std::optional<tuple::Tuple> row = raw.next()) {
            raw_ids.push_back(std::get<int>(row->column(0).column_value()));
        }

        // Filter keeps the accepted rows in the child's order; Project
        // trims each to the requested columns, duplicates included.
        const auto even = [](const tuple::Tuple& row) {
            return std::get<int>(row.column(0).column_value()) % 2 == 0;
        };
        ops::SeqScan scan(file);
        ops::Filter evens(scan, even);
        std::vector<int> filtered_ids;
        evens.open();
        while (const std::optional<tuple::Tuple> row = evens.next()) {
            filtered_ids.push_back(std::get<int>(row->column(0).column_value()));
        }
        evens.close();
        std::vector<int> expected;
        for (const int id : raw_ids) {
            if (id % 2 == 0) {
                expected.push_back(id);
            }
        }
        check(filtered_ids == expected, "Filter keeps accepted rows in child order");

        ops::SeqScan scan2(file);
        ops::Project swapped(scan2, {1, 0}); // pad then id: order and duplicates
        swapped.open();
        std::optional<tuple::Tuple> row = swapped.next();
        check(row.has_value() && row->column_count() == 2 &&
                  std::holds_alternative<std::string>(row->column(0).column_value()) &&
                  std::get<int>(row->column(1).column_value()) == raw_ids.front(),
              "Project trims and orders columns as asked");
        swapped.close();

        // Limit stops the pipeline: a limit of 3 over 10 rows pulls the
        // child exactly 3 times, and the emitted rows are the child's
        // first 3.
        std::vector<tuple::Tuple> rows;
        for (int id = 0; id < 10; ++id) {
            tuple::Tuple t;
            t.add(id);
            rows.push_back(t);
        }
        CountingOperator source(rows);
        ops::Limit three(source, 3);
        std::vector<int> limited;
        three.open();
        while (const std::optional<tuple::Tuple> row2 = three.next()) {
            limited.push_back(std::get<int>(row2->column(0).column_value()));
        }
        three.close();
        check(limited.size() == 3 && limited[0] == 0 && limited[1] == 1 &&
                  limited[2] == 2 && source.pulls() == 3,
              "Limit stops pulling once satisfied");

        // A limit of zero yields nothing and pulls nothing.
        CountingOperator source2(rows);
        ops::Limit zero(source2, 0);
        zero.open();
        check(!zero.next().has_value() && source2.pulls() == 0,
              "Limit of zero emits and pulls nothing");
        zero.close();

        // A limit past the end passes every row through.
        CountingOperator source3(rows);
        ops::Limit all(source3, 100);
        int count = 0;
        all.open();
        while (all.next().has_value()) {
            ++count;
        }
        all.close();
        check(count == 10 && source3.pulls() == 11,
              "Limit past the end passes every row through");

        manager.close_all();
    }
    std::filesystem::remove(path);
}

// --- NestedLoopsJoin -----------------------------------------------------------

// Reads a join's output as (outer id, inner id) pairs.
std::vector<std::pair<int, int>> join_pairs(ops::Operator& join) {
    std::vector<std::pair<int, int>> pairs;
    join.open();
    while (const std::optional<tuple::Tuple> row = join.next()) {
        pairs.emplace_back(std::get<int>(row->column(0).column_value()),
                           std::get<int>(row->column(2).column_value()));
    }
    join.close();
    return pairs;
}

void nested_loops_join_checks() {
    const schema::TableSchema schema = schema::parse_table_schema(kFixtureSchema);

    // The equijoin predicate over the combined row: [id_L, pad_L, id_R, pad_R].
    const auto ids_equal = [](const tuple::Tuple& combined) {
        return std::get<int>(combined.column(0).column_value()) ==
               std::get<int>(combined.column(2).column_value());
    };

    const std::string left_path = "opcaseL.bin";
    const std::string right_path = "opcaseR.bin";
    const std::string third_path = "opcase3.bin";

    {
        bufman::BufferManager manager;
        manager.init(8, std::make_unique<bufman::LRUPolicy>());

        // Duplicates on both sides: [2,2,3] x [2,2] answers 4 pairs.
        heapfile::HeapFile left = make_file(manager, left_path, schema, 0);
        left.insert(fixture_row(2));
        left.insert(fixture_row(2));
        left.insert(fixture_row(3));
        heapfile::HeapFile right = make_file(manager, right_path, schema, 0);
        right.insert(fixture_row(2));
        right.insert(fixture_row(2));

        ops::SeqScan scan_left(left);
        ops::SeqScan scan_right(right);
        ops::NestedLoopsJoin join(scan_left, scan_right, ids_equal);
        const std::vector<std::pair<int, int>> pairs = join_pairs(join);
        bool duplicates_ok = pairs.size() == 4;
        for (const auto& [l, r] : pairs) {
            duplicates_ok = duplicates_ok && l == 2 && r == 2;
        }
        check(duplicates_ok, "NestedLoopsJoin pairs duplicates on both sides");

        // The combined row's shape: outer columns then inner columns.
        ops::SeqScan shape_left(left);
        ops::SeqScan shape_right(right);
        ops::NestedLoopsJoin shape_join(shape_left, shape_right, ids_equal);
        shape_join.open();
        const std::optional<tuple::Tuple> first = shape_join.next();
        check(first.has_value() && first->column_count() == 4 &&
                  std::holds_alternative<std::string>(first->column(1).column_value()) &&
                  std::holds_alternative<std::string>(first->column(3).column_value()),
              "NestedLoopsJoin emits outer columns then inner columns");
        shape_join.close();

        // Distinct keys answer in outer-major order.
        heapfile::HeapFile l2 = make_file(manager, left_path, schema, 0);
        for (const int id : {1, 2, 3}) {
            l2.insert(fixture_row(id));
        }
        heapfile::HeapFile r2 = make_file(manager, right_path, schema, 0);
        for (const int id : {2, 3, 4}) {
            r2.insert(fixture_row(id));
        }
        ops::SeqScan o2(l2);
        ops::SeqScan i2(r2);
        ops::NestedLoopsJoin order_join(o2, i2, ids_equal);
        const std::vector<std::pair<int, int>> ordered = join_pairs(order_join);
        check((ordered == std::vector<std::pair<int, int>>{{2, 2}, {3, 3}}),
              "NestedLoopsJoin answers outer-major, inner in scan order");

        // An empty side answers nothing, and the statement succeeds.
        heapfile::HeapFile empty = make_file(manager, third_path, schema, 0);
        ops::SeqScan o3(l2);
        ops::SeqScan i3(empty);
        ops::NestedLoopsJoin empty_join(o3, i3, ids_equal);
        check(join_pairs(empty_join).empty(), "NestedLoopsJoin with an empty side yields nothing");

        // A null predicate is a cross join: [1,2] x [1,2,3], outer-major.
        heapfile::HeapFile l4 = make_file(manager, left_path, schema, 0);
        l4.insert(fixture_row(1));
        l4.insert(fixture_row(2));
        heapfile::HeapFile r4 = make_file(manager, right_path, schema, 0);
        for (const int id : {1, 2, 3}) {
            r4.insert(fixture_row(id));
        }
        ops::SeqScan o4(l4);
        ops::SeqScan i4(r4);
        ops::NestedLoopsJoin cross(o4, i4, nullptr);
        const std::vector<std::pair<int, int>> cross_pairs = join_pairs(cross);
        check((cross_pairs ==
               std::vector<std::pair<int, int>>{{1, 1}, {1, 2}, {1, 3}, {2, 1}, {2, 2}, {2, 3}}),
              "NestedLoopsJoin with no predicate is a cross join");

        // A paused join holds no pins across next(): with the join parked
        // after its first pair, another file scans to exhaustion through
        // the same pool, and the resumed run answers exactly what an
        // uninterrupted run answers.
        heapfile::HeapFile l5 = make_file(manager, left_path, schema, 0);
        for (int id = 1; id <= 5; ++id) {
            l5.insert(fixture_row(id));
        }
        heapfile::HeapFile r5 = make_file(manager, right_path, schema, 0);
        for (const int id : {2, 3, 4}) {
            r5.insert(fixture_row(id));
        }
        heapfile::HeapFile third = make_file(manager, third_path, schema, 6);
        ops::SeqScan o5(l5);
        ops::SeqScan i5(r5);
        ops::NestedLoopsJoin paused(o5, i5, ids_equal);
        paused.open();
        check(paused.next().has_value(), "paused join produces its first pair");
        ops::SeqScan third_scan(third);
        check(scan_ids(third_scan).size() == 6,
              "a paused join frames out no pool pins");
        std::vector<std::pair<int, int>> resumed;
        while (const std::optional<tuple::Tuple> row = paused.next()) {
            resumed.emplace_back(std::get<int>(row->column(0).column_value()),
                                 std::get<int>(row->column(2).column_value()));
        }
        paused.close();
        ops::SeqScan o6(l5);
        ops::SeqScan i6(r5);
        ops::NestedLoopsJoin whole(o6, i6, ids_equal);
        const std::vector<std::pair<int, int>> expected = join_pairs(whole);
        check(resumed.size() + 1 == expected.size() &&
                  std::equal(resumed.begin(), resumed.end(), expected.begin() + 1),
              "resumed join matches the uninterrupted answer");

        manager.close_all();
        check(true, "NestedLoopsJoin leaves no pinned frames");
    }
    std::filesystem::remove(left_path);
    std::filesystem::remove(right_path);
    std::filesystem::remove(third_path);
}

// --- IndexNestedLoopsJoin -------------------------------------------------------

// The probe seam's stand-in index: row ids by key, collected from one scan
// of the inner file — the shape SessionIndex::range(key, key) hands the
// executor. The map holds no pins once the scan returns.
std::map<int, std::vector<heapfile::RowId>> rids_by_key(heapfile::HeapFile& file) {
    std::map<int, std::vector<heapfile::RowId>> by_key;
    for (heapfile::HeapFileIterator it = file.scan(); const std::optional<tuple::Tuple> row = it.next();) {
        by_key[std::get<int>(row->column(0).column_value())].push_back(it.row_id());
    }
    return by_key;
}

// Binds a stand-in probe to a key map (captured by value) and the key's
// position in the outer (combined prefix) row.
ops::IndexNestedLoopsJoin::Probe probe_on(const std::map<int, std::vector<heapfile::RowId>>& by_key,
                                          std::size_t key_position) {
    return [by_key, key_position](const tuple::Tuple& outer) {
        const auto it = by_key.find(std::get<int>(outer.column(key_position).column_value()));
        return it != by_key.end() ? it->second : std::vector<heapfile::RowId>{};
    };
}

void index_nested_loops_join_checks() {
    const schema::TableSchema schema = schema::parse_table_schema(kFixtureSchema);

    // The equijoin predicate over the combined row: [id_L, pad_L, id_R, pad_R].
    const auto ids_equal = [](const tuple::Tuple& combined) {
        return std::get<int>(combined.column(0).column_value()) ==
               std::get<int>(combined.column(2).column_value());
    };

    const std::string left_path = "opcaseIL.bin";
    const std::string right_path = "opcaseIR.bin";
    const std::string third_path = "opcaseI3.bin";

    {
        bufman::BufferManager manager;
        manager.init(8, std::make_unique<bufman::LRUPolicy>());

        // Duplicates on both sides: [2,2,3] x [2,2] answers 4 pairs,
        // outer-major — the probe returns both key-2 rows per outer 2.
        heapfile::HeapFile left = make_file(manager, left_path, schema, 0);
        left.insert(fixture_row(2));
        left.insert(fixture_row(2));
        left.insert(fixture_row(3));
        heapfile::HeapFile right = make_file(manager, right_path, schema, 0);
        right.insert(fixture_row(2));
        right.insert(fixture_row(2));
        const std::map<int, std::vector<heapfile::RowId>> right_keys = rids_by_key(right);

        ops::SeqScan scan_left(left);
        ops::IndexNestedLoopsJoin join(scan_left, right, probe_on(right_keys, 0), ids_equal);
        const std::vector<std::pair<int, int>> pairs = join_pairs(join);
        bool duplicates_ok = pairs.size() == 4;
        for (const auto& [l, r] : pairs) {
            duplicates_ok = duplicates_ok && l == 2 && r == 2;
        }
        check(duplicates_ok, "IndexNestedLoopsJoin probe answers the equijoin, duplicates included");

        // The combined row's shape: outer columns then inner columns.
        ops::SeqScan shape_left(left);
        ops::IndexNestedLoopsJoin shape_join(shape_left, right, probe_on(right_keys, 0), ids_equal);
        shape_join.open();
        const std::optional<tuple::Tuple> first = shape_join.next();
        check(first.has_value() && first->column_count() == 4 &&
                  std::holds_alternative<std::string>(first->column(1).column_value()) &&
                  std::holds_alternative<std::string>(first->column(3).column_value()),
              "IndexNestedLoopsJoin emits outer columns then inner columns");
        shape_join.close();

        // A key with no matches yields nothing for that outer tuple and
        // the run continues with the next: [1,2,3] x [2,3,4] answers
        // {2,2},{3,3} — outer 1 probed and found nothing.
        heapfile::HeapFile l2 = make_file(manager, left_path, schema, 0);
        for (const int id : {1, 2, 3}) {
            l2.insert(fixture_row(id));
        }
        heapfile::HeapFile r2 = make_file(manager, right_path, schema, 0);
        for (const int id : {2, 3, 4}) {
            r2.insert(fixture_row(id));
        }
        const std::map<int, std::vector<heapfile::RowId>> r2_keys = rids_by_key(r2);
        ops::SeqScan o2(l2);
        ops::IndexNestedLoopsJoin order_join(o2, r2, probe_on(r2_keys, 0), ids_equal);
        check((join_pairs(order_join) == std::vector<std::pair<int, int>>{{2, 2}, {3, 3}}),
              "a probe with no matches skips the outer tuple and continues");

        // An empty inner file answers nothing, and the statement succeeds.
        heapfile::HeapFile empty = make_file(manager, third_path, schema, 0);
        ops::SeqScan o3(l2);
        ops::IndexNestedLoopsJoin empty_join(o3, empty, probe_on(rids_by_key(empty), 0), ids_equal);
        check(join_pairs(empty_join).empty(), "IndexNestedLoopsJoin over an empty inner file yields nothing");

        // The predicate still decides: the probe narrows, but a combined
        // pair failing the predicate is not emitted. The probe returns
        // key-2 rows for every outer 2; a rejecting predicate emits none.
        heapfile::HeapFile l4 = make_file(manager, left_path, schema, 0);
        l4.insert(fixture_row(2));
        heapfile::HeapFile r4 = make_file(manager, right_path, schema, 0);
        r4.insert(fixture_row(2));
        r4.insert(fixture_row(2));
        const std::map<int, std::vector<heapfile::RowId>> r4_keys = rids_by_key(r4);
        ops::SeqScan o4(l4);
        ops::IndexNestedLoopsJoin rejects(o4, r4, probe_on(r4_keys, 0),
                                          [](const tuple::Tuple&) { return false; });
        check(join_pairs(rejects).empty(),
              "a probed pair failing the join predicate is not emitted");

        // Inner-side conjuncts filter probed rows: two id-2 inner rows
        // with different pads, the inner predicate keeping only the
        // 'z'-padded one — the surviving pair's inner pad proves which
        // row was fetched.
        heapfile::HeapFile r5 = make_file(manager, right_path, schema, 0);
        r5.insert(fixture_row(2));
        tuple::Tuple z_row;
        z_row.add(2);
        z_row.add(std::string(900, 'z'));
        r5.insert(z_row);
        const std::map<int, std::vector<heapfile::RowId>> r5_keys = rids_by_key(r5);
        const std::string z_pad(900, 'z');
        ops::SeqScan o5(l4);
        ops::IndexNestedLoopsJoin filtered(
            o5, r5, probe_on(r5_keys, 0), ids_equal,
            [&z_pad](const tuple::Tuple& inner) {
                return std::get<std::string>(inner.column(1).column_value()) == z_pad;
            });
        filtered.open();
        const std::optional<tuple::Tuple> kept = filtered.next();
        check(kept.has_value() && std::get<int>(kept->column(2).column_value()) == 2 &&
                  std::get<std::string>(kept->column(3).column_value()) == z_pad &&
                  !filtered.next().has_value(),
              "inner-side conjuncts filter probed rows before combining");
        filtered.close();

        // A paused join holds no pins across next(): with the join parked
        // after its first pair, another file scans to exhaustion through
        // the same pool, and the resumed run answers exactly what an
        // uninterrupted run answers.
        heapfile::HeapFile l6 = make_file(manager, left_path, schema, 0);
        for (int id = 1; id <= 5; ++id) {
            l6.insert(fixture_row(id));
        }
        heapfile::HeapFile r6 = make_file(manager, right_path, schema, 0);
        for (const int id : {2, 3, 4}) {
            r6.insert(fixture_row(id));
        }
        heapfile::HeapFile third = make_file(manager, third_path, schema, 6);
        const std::map<int, std::vector<heapfile::RowId>> r6_keys = rids_by_key(r6);
        ops::SeqScan o6(l6);
        ops::IndexNestedLoopsJoin paused(o6, r6, probe_on(r6_keys, 0), ids_equal);
        paused.open();
        check(paused.next().has_value(), "paused index join produces its first pair");
        ops::SeqScan third_scan(third);
        check(scan_ids(third_scan).size() == 6,
              "a paused index join frames out no pool pins");
        std::vector<std::pair<int, int>> resumed;
        while (const std::optional<tuple::Tuple> row = paused.next()) {
            resumed.emplace_back(std::get<int>(row->column(0).column_value()),
                                 std::get<int>(row->column(2).column_value()));
        }
        paused.close();
        ops::SeqScan o7(l6);
        ops::IndexNestedLoopsJoin whole(o7, r6, probe_on(r6_keys, 0), ids_equal);
        const std::vector<std::pair<int, int>> expected = join_pairs(whole);
        check(resumed.size() + 1 == expected.size() &&
                  std::equal(resumed.begin(), resumed.end(), expected.begin() + 1),
              "resumed index join matches the uninterrupted answer");

        // The contract asserts (double open, next before open) are carried
        // by the operator itself — this suite exercises the observable
        // behavior, and any misuse above would have tripped them.
        manager.close_all();
        check(true, "IndexNestedLoopsJoin leaves no pinned frames");
    }
    std::filesystem::remove(left_path);
    std::filesystem::remove(right_path);
    std::filesystem::remove(third_path);
}

// --- HashJoin -----------------------------------------------------------------

// The bucket files one statement's partition sets own, by label prefix —
// the tmp/-hygiene checks' eyes on the filesystem.
std::vector<std::string> bucket_files(const std::string& label_prefix) {
    std::vector<std::string> found;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator("tmp", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(label_prefix, 0) == 0) {
            found.push_back(name);
        }
    }
    return found;
}

// Pair extraction that reads both numeric shapes: column 0 may hold the
// probe side's double id (the mixed-type case), column 2 the build side's
// int id. Answers are compared as (double, double).
std::vector<std::pair<double, double>> hash_join_pairs(ops::Operator& join) {
    const auto as_number = [](const tuple::Column& column) {
        return std::holds_alternative<int>(column.column_value())
                   ? static_cast<double>(std::get<int>(column.column_value()))
                   : std::get<double>(column.column_value());
    };
    std::vector<std::pair<double, double>> pairs;
    join.open();
    while (const std::optional<tuple::Tuple> row = join.next()) {
        pairs.emplace_back(as_number(row->column(0)), as_number(row->column(2)));
    }
    join.close();
    return pairs;
}

void hash_join_checks() {
    // The two sides' schemas: the build side the fixture's int-id table,
    // the probe side the same shape — plus a double-id variant for the
    // mixed-numeric case (the evaluator promotes ints, so a hash key must
    // too).
    const schema::TableSchema schema = schema::parse_table_schema(kFixtureSchema);
    const schema::TableSchema double_schema = schema::parse_table_schema(
        R"({"table_name": "opcased",
            "columns": [
              {"column_name": "id",  "column_type": "Double", "size": 8},
              {"column_name": "pad", "column_type": "String",  "size": 900}]})");

    // The equijoin predicate over the combined row
    // [probe_id, probe_pad, build_id, build_pad].
    const auto ids_equal = [](const tuple::Tuple& combined) {
        const auto as_number = [](const tuple::Column& column) {
            return std::holds_alternative<int>(column.column_value())
                       ? static_cast<double>(std::get<int>(column.column_value()))
                       : std::get<double>(column.column_value());
        };
        return as_number(combined.column(0)) == as_number(combined.column(2));
    };

    // The key seams: each side's id is its column 0.
    const ops::HashJoin::KeyFn build_key = [](const tuple::Tuple& row) {
        return ops::join_key(row.column(0));
    };
    const ops::HashJoin::KeyFn probe_key = build_key;

    const std::string left_path = "opcaseH.bin";   // probe rows
    const std::string right_path = "opcaseHD.bin"; // build rows
    const std::string third_path = "opcaseH3.bin";

    {
        // Two pools, as the executor will wire them: the main pool for
        // the input tables, the join pool for the bucket files.
        bufman::BufferManager manager;
        manager.init(8, std::make_unique<bufman::LRUPolicy>());
        bufman::BufferManager join_pool;
        join_pool.init(8, std::make_unique<bufman::LRUPolicy>());

        // Duplicates on both sides: probe [2,2,3] x build [2,2] answers 4
        // pairs, probe-major — the table holds both build key-2 rows and
        // each probe 2 finds them both.
        heapfile::HeapFile left = make_file(manager, left_path, schema, 0);
        left.insert(fixture_row(2));
        left.insert(fixture_row(2));
        left.insert(fixture_row(3));
        heapfile::HeapFile right = make_file(manager, right_path, schema, 0);
        right.insert(fixture_row(2));
        right.insert(fixture_row(2));

        ops::Partitions build_parts(join_pool, "tmp", "hj_build", schema, 3);
        ops::Partitions probe_parts(join_pool, "tmp", "hj_probe", schema, 3);
        ops::SeqScan build_scan(right);
        ops::SeqScan probe_scan(left);
        ops::HashJoin join(build_scan, probe_scan, build_key, probe_key, ids_equal,
                           build_parts, probe_parts);
        const std::vector<std::pair<double, double>> pairs = hash_join_pairs(join);
        bool duplicates_ok = pairs.size() == 4;
        for (const auto& [p, b] : pairs) {
            duplicates_ok = duplicates_ok && p == 2 && b == 2;
        }
        check(duplicates_ok, "HashJoin probe answers the equijoin, duplicates included");

        // The bucket files were erased as the query ended.
        check(bucket_files("hj_").empty(),
              "HashJoin erases its bucket files when the query ends");

        // The combined row's shape: probe columns then build columns.
        ops::SeqScan shape_build(right);
        ops::SeqScan shape_probe(left);
        ops::Partitions shape_bp(join_pool, "tmp", "hj_build", schema, 3);
        ops::Partitions shape_pp(join_pool, "tmp", "hj_probe", schema, 3);
        ops::HashJoin shape_join(shape_build, shape_probe, build_key, probe_key,
                                 ids_equal, shape_bp, shape_pp);
        shape_join.open();
        const std::optional<tuple::Tuple> first = shape_join.next();
        check(first.has_value() && first->column_count() == 4 &&
                  std::holds_alternative<std::string>(first->column(1).column_value()) &&
                  std::holds_alternative<std::string>(first->column(3).column_value()),
              "HashJoin emits probe columns then build columns");
        check(bucket_files("hj_").size() == 6,
              "the bucket files exist under tmp/ while the run is live");
        shape_join.close();

        // A key with no matches skips the probe tuple and continues:
        // probe [1,2,3] x build [2,3,4] answers {2,2},{3,3}.
        heapfile::HeapFile l2 = make_file(manager, left_path, schema, 0);
        for (const int id : {1, 2, 3}) {
            l2.insert(fixture_row(id));
        }
        heapfile::HeapFile r2 = make_file(manager, right_path, schema, 0);
        for (const int id : {2, 3, 4}) {
            r2.insert(fixture_row(id));
        }
        ops::SeqScan b2(r2);
        ops::SeqScan p2(l2);
        ops::Partitions bp2(join_pool, "tmp", "hj_build", schema, 3);
        ops::Partitions pp2(join_pool, "tmp", "hj_probe", schema, 3);
        ops::HashJoin order_join(b2, p2, build_key, probe_key, ids_equal, bp2, pp2);
        const std::vector<std::pair<double, double>> ordered = hash_join_pairs(order_join);
        const std::vector<std::pair<double, double>> expected{{2, 2}, {3, 3}};
        check(ordered == expected, "a key with no matches skips the probe tuple and continues");

        // An empty build side answers nothing, and the run succeeds.
        heapfile::HeapFile empty = make_file(manager, right_path, schema, 0);
        ops::SeqScan eb(empty);
        ops::SeqScan p3(l2);
        ops::Partitions ebp(join_pool, "tmp", "hj_build", schema, 3);
        ops::Partitions epp(join_pool, "tmp", "hj_probe", schema, 3);
        ops::HashJoin empty_join(eb, p3, build_key, probe_key, ids_equal, ebp, epp);
        check(hash_join_pairs(empty_join).empty(),
              "HashJoin over an empty build side yields nothing");

        // An empty probe side answers nothing either.
        heapfile::HeapFile l4 = make_file(manager, left_path, schema, 0);
        ops::SeqScan b4(r2);
        ops::SeqScan p4(l4);
        ops::Partitions bp4(join_pool, "tmp", "hj_build", schema, 3);
        ops::Partitions pp4(join_pool, "tmp", "hj_probe", schema, 3);
        ops::HashJoin empty_probe(b4, p4, build_key, probe_key, ids_equal, bp4, pp4);
        check(hash_join_pairs(empty_probe).empty(),
              "HashJoin over an empty probe side yields nothing");

        // The predicate still decides: build [2,2], probe [2] — matches
        // exist, a rejecting predicate emits none.
        heapfile::HeapFile r5 = make_file(manager, right_path, schema, 0);
        r5.insert(fixture_row(2));
        r5.insert(fixture_row(2));
        ops::SeqScan b5(r5);
        ops::SeqScan p5(l4);
        ops::Partitions bp5(join_pool, "tmp", "hj_build", schema, 3);
        ops::Partitions pp5(join_pool, "tmp", "hj_probe", schema, 3);
        ops::HashJoin rejects(b5, p5, build_key, probe_key,
                              [](const tuple::Tuple&) { return false; }, bp5, pp5);
        check(hash_join_pairs(rejects).empty(),
              "a probed pair failing the join predicate is not emitted");

        // Mixed numeric keys meet: probe ids are doubles, build ids ints,
        // probe [5.0, 7.0] x build [5, 6] answers exactly {5,5} — the
        // canonicalized keys collide where the evaluator's comparison
        // would accept.
        heapfile::HeapFile l6 = make_file(manager, left_path, double_schema, 0);
        tuple::Tuple d5;
        d5.add(5.0);
        d5.add(std::string(900, 'a'));
        tuple::Tuple d7;
        d7.add(7.0);
        d7.add(std::string(900, 'b'));
        l6.insert(d5);
        l6.insert(d7);
        heapfile::HeapFile r6 = make_file(manager, right_path, schema, 0);
        r6.insert(fixture_row(5));
        r6.insert(fixture_row(6));
        ops::SeqScan b6(r6);
        ops::SeqScan p6(l6);
        ops::Partitions bp6(join_pool, "tmp", "hj_build", schema, 3);
        ops::Partitions pp6(join_pool, "tmp", "hj_probe", double_schema, 3);
        ops::HashJoin mixed(b6, p6, build_key, probe_key, ids_equal, bp6, pp6);
        const std::vector<std::pair<double, double>> mixed_pairs = hash_join_pairs(mixed);
        const std::vector<std::pair<double, double>> mixed_expected{{5.0, 5.0}};
        check(mixed_pairs == mixed_expected,
              "an int key and a double key with equal values meet");

        // A paused join holds no pins across next() in EITHER pool: with
        // the join parked after its first pair, another file scans to
        // exhaustion through the main pool AND the join pool frames out
        // scratch I/O — then the resumed run answers what an uninterrupted
        // run answers.
        heapfile::HeapFile l7 = make_file(manager, left_path, schema, 0);
        for (int id = 1; id <= 5; ++id) {
            l7.insert(fixture_row(id));
        }
        heapfile::HeapFile r7 = make_file(manager, right_path, schema, 0);
        for (const int id : {2, 3, 4}) {
            r7.insert(fixture_row(id));
        }
        heapfile::HeapFile third = make_file(manager, third_path, schema, 6);
        ops::SeqScan b7(r7);
        ops::SeqScan p7(l7);
        ops::Partitions bp7(join_pool, "tmp", "hj_build", schema, 3);
        ops::Partitions pp7(join_pool, "tmp", "hj_probe", schema, 3);
        ops::HashJoin paused(b7, p7, build_key, probe_key, ids_equal, bp7, pp7);
        paused.open();
        check(paused.next().has_value(), "paused hash join produces its first pair");
        ops::SeqScan third_scan(third);
        check(scan_ids(third_scan).size() == 6,
              "a paused hash join frames out no main-pool pins");
        {
            // Scratch I/O through the join pool while the join is parked.
            heapfile::HeapFile scratch(join_pool);
            const std::string scratch_path = "opcaseHscratch.bin";
            std::filesystem::remove(scratch_path);
            scratch.create(scratch_path, schema);
            scratch.open(scratch_path, schema);
            for (int id = 0; id < 6; ++id) {
                scratch.insert(fixture_row(id));
            }
            std::size_t count = 0;
            for (heapfile::HeapFileIterator it = scratch.scan();
                 const std::optional<tuple::Tuple> row = it.next();) {
                ++count;
            }
            scratch.close();
            std::filesystem::remove(scratch_path);
            check(count == 6, "a paused hash join frames out no join-pool pins");
        }
        std::vector<std::pair<double, double>> resumed;
        while (const std::optional<tuple::Tuple> row = paused.next()) {
            const auto as_number = [](const tuple::Column& column) {
                return std::holds_alternative<int>(column.column_value())
                           ? static_cast<double>(std::get<int>(column.column_value()))
                           : std::get<double>(column.column_value());
            };
            resumed.emplace_back(as_number(row->column(0)), as_number(row->column(2)));
        }
        paused.close();
        ops::SeqScan b8(r7);
        ops::SeqScan p8(l7);
        ops::Partitions bp8(join_pool, "tmp", "hj_build", schema, 3);
        ops::Partitions pp8(join_pool, "tmp", "hj_probe", schema, 3);
        ops::HashJoin whole(b8, p8, build_key, probe_key, ids_equal, bp8, pp8);
        const std::vector<std::pair<double, double>> uninterrupted = hash_join_pairs(whole);
        check(resumed.size() + 1 == uninterrupted.size() &&
                  std::equal(resumed.begin(), resumed.end(), uninterrupted.begin() + 1),
              "resumed hash join matches the uninterrupted answer");

        // An errored run erases its buckets on the way out: the build
        // side's filter throws on the third row — mid-partitioning — and
        // the partition sets' destructors (the RAII guard) sweep tmp/.
        heapfile::HeapFile l9 = make_file(manager, left_path, schema, 0);
        for (const int id : {1, 2, 3}) {
            l9.insert(fixture_row(id));
        }
        heapfile::HeapFile r9 = make_file(manager, right_path, schema, 0);
        for (const int id : {1, 2, 3}) {
            r9.insert(fixture_row(id));
        }
        int seen = 0;
        bool threw = false;
        try {
            ops::SeqScan b9(r9);
            ops::SeqScan p9(l9);
            ops::Filter throwing_build(
                b9, [&seen](const tuple::Tuple&) {
                    if (++seen == 3) {
                        // A reachable failure mid-partitioning.
                        throw common::Error("synthetic mid-partition failure");
                    }
                    return true;
                });
            ops::Partitions bp9(join_pool, "tmp", "hjerr_build", schema, 3);
            ops::Partitions pp9(join_pool, "tmp", "hjerr_probe", schema, 3);
            ops::HashJoin doomed(throwing_build, p9, build_key, probe_key, ids_equal,
                                 bp9, pp9);
            doomed.open();  // Phase I partitions the build side into the throw
            doomed.close();
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "an error mid-partitioning fails the run by name");
        check(bucket_files("hjerr_").empty(),
              "an errored run erases its bucket files on the way out");

        manager.close_all();
        join_pool.close_all();
        check(true, "HashJoin leaves no pinned frames in either pool");
    }
    std::filesystem::remove(left_path);
    std::filesystem::remove(right_path);
    std::filesystem::remove(third_path);
    std::filesystem::remove("tmp");
}

} // namespace

int run_operators_checks() {
    for (void (*suite)() :
         {seq_scan_checks, filter_project_limit_checks, nested_loops_join_checks,
          index_nested_loops_join_checks, hash_join_checks}) {
        try {
            suite();
        } catch (const std::exception& e) {
            check(false, (std::string("unexpected error: ") + e.what()).c_str());
        }
    }
    return failures;
}
