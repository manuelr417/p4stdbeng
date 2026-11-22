// p4stdbeng — the storage suite: p2dbeng's TUP2 heap checks carried
// forward unchanged (the hand-built list-surgery cases and the two-list
// heap file exercise, with the structural self-check after every
// operation and the randomized insert/erase shadow model).

#include "bufpool/BufferManager.h"
#include "bufpool/PinnedPage.h"
#include "check_support.h"
#include "heapfile/HeapFile.h"
#include "policy/LRUPolicy.h"
#include "schema/TableCsv.h"
#include "schema/TableGenerator.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "tuple/Column.h"
#include "tuple/Tuple.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <unistd.h>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using checks::fails_with;
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

std::string temp_path() {
    return std::string("/tmp/p3-tuple-") + std::to_string(getpid()) + ".bin";
}

// --- storage: hand-built list-surgery cases ----------------------------------
//
// A three-page file walked through every detach/append shape the lists can
// take — head, middle, tail, and only element — with the structural check
// after every step and the scan order verified against the expected page
// sequence (the free list's pages, then the full list's).

void list_case_checks() {
    std::filesystem::remove("listcase.bin");
    const schema::TableSchema schema = schema::parse_table_schema(
        R"({"table_name": "listcase",
            "columns": [
              {"column_name": "sid", "column_type": "Integer", "size": 4},
              {"column_name": "pad", "column_type": "String",  "size": 900}]})");
    const auto make_row = [](int id) {
        tuple::Tuple row;
        row.add(id);
        row.add(std::string(900, static_cast<char>('a' + id % 26)));
        return row;
    };
    const auto scan_ids = [&](heapfile::HeapFile& file) {
        std::vector<int> ids;
        heapfile::HeapFileIterator it = file.scan();
        while (const std::optional<tuple::Tuple> row = it.next()) {
            ids.push_back(std::get<int>(row->column(0).column_value()));
        }
        return ids;
    };
    // Each step runs, then the structural check confirms the lists.
    bufman::BufferManager manager;
    manager.init(8, std::make_unique<bufman::LRUPolicy>());
    heapfile::HeapFile file(manager);
    const auto structured = [&](auto step) {
        return ok([&] {
            const bool result = step();
            file.check_structure();
            return result;
        });
    };
    const auto inserted_at = [&](int id, std::uint32_t page, std::uint32_t slot) {
        const heapfile::RowId rid = file.insert(make_row(id));
        return rid.page_id == page && rid.slot == slot;
    };
    const auto erased = [&](heapfile::RowId rid) {
        file.erase(rid);
        return true;
    };

    check(ok([&] {
              file.create("listcase.bin", schema);
              file.open("listcase.bin", schema);
          }),
          "listcase: file created and opened");

    // One page fills: the full list's only element. Erasing unlinks that
    // only element and appends it to an empty free list.
    check(structured([&] {
              bool placed = true;
              for (int id = 1; id <= 4; ++id) {
                  placed = inserted_at(id, 1, static_cast<std::uint32_t>(id - 1)) && placed;
              }
              return placed;
          }),
          "listcase: four rows fill page 1");
    check(structured([&] { return erased({1, 0}); }),
          "listcase: the full list's only element unlinks");
    check(scan_ids(file) == std::vector<int>{2, 3, 4},
          "listcase: the freed page scans before the full pages");
    check(structured([&] { return inserted_at(5, 1, 0); }),
          "listcase: the page refills and rejoins the full list");

    // Fill pages 2 and 3 completely: page 1 came back full, so ids 6..9
    // land on page 2 and 10..13 on page 3. The full list becomes
    // 1 -> 2 -> 3 and the free list is empty.
    check(structured([&] {
              bool placed = true;
              for (int id = 6; id <= 13; ++id) {
                  placed = file.insert(make_row(id)).page_id ==
                               static_cast<std::uint32_t>(id <= 9 ? 2 : 3) &&
                           placed;
              }
              return placed;
          }),
          "listcase: twelve live rows fill pages 1..3 in order");

    // Middle of the full list: erasing from page 2 unlinks it between its
    // neighbors and leaves it as the free list's only element.
    check(structured([&] { return erased({2, 0}); }),
          "listcase: a middle page unlinks from the full list");
    check(scan_ids(file) == std::vector<int>{7, 8, 9, 5, 2, 3, 4, 10, 11, 12, 13},
          "listcase: the scan walks the free list before the full list");
    check(structured([&] { return inserted_at(14, 2, 0); }),
          "listcase: the middle page refills onto the full list's tail");
    check(scan_ids(file) == std::vector<int>{5, 2, 3, 4, 10, 11, 12, 13, 14, 7, 8, 9},
          "listcase: the refilled page scans at the full list's tail");

    // Head of the full list (page 1 leads 1 -> 3 -> 2 now): unlink, refill,
    // and the page comes back at the tail.
    check(structured([&] { return erased({1, 0}); }), "listcase: the full list's head unlinks");
    check(scan_ids(file) == std::vector<int>{2, 3, 4, 10, 11, 12, 13, 14, 7, 8, 9},
          "listcase: the freed head page scans before the full pages");
    check(structured([&] { return inserted_at(15, 1, 0); }),
          "listcase: the head page refills at the full list's tail");
    check(scan_ids(file) == std::vector<int>{10, 11, 12, 13, 14, 7, 8, 9, 15, 2, 3, 4},
          "listcase: the full list's order follows the refills");

    // Tail of the full list (page 1 closes 3 -> 2 -> 1 now).
    check(structured([&] { return erased({1, 3}); }), "listcase: the full list's tail unlinks");
    check(scan_ids(file) == std::vector<int>{15, 2, 3, 10, 11, 12, 13, 14, 7, 8, 9},
          "listcase: the freed tail page scans before the full pages");
    check(structured([&] { return inserted_at(16, 1, 3); }),
          "listcase: the tail page refills onto the full list");

    // The previous format is refused by name, not misread.
    check(ok([&] { file.close(); }), "listcase: file closed for the format step");
    check(ok([&] {
              bufman::File raw = bufman::open_for_append("listcase.bin");
              std::array<char, bufman::kBlockSize> block{};
              block[0] = 'T';
              block[1] = 'U';
              block[2] = 'P';
              block[3] = '1';
              bufman::write_block(raw, 0, block.data());
              bufman::close(raw);
          }),
          "listcase: TUP1 magic written over the header");
    {
        heapfile::HeapFile old_format(manager);
        check(fails_with([&] { old_format.open("listcase.bin", schema); }, "not a tuple heap file"),
              "listcase: a TUP1 file is refused by name");
    }

    manager.close_all();
    std::filesystem::remove("listcase.bin");
}

// --- storage: the two-list heap file ----------------------------------------
//
// Exercises the free-space lists directly through the HeapFile API: the
// invariant checker runs after every single operation, so any relink
// mistake surfaces at the step that caused it, not as a mystery later.

void storage_checks() {
    // The suite may run repeatedly in one working directory.
    std::filesystem::remove("storage.bin");
    // sid(4) + pad(900) = 904-byte records -> 4 slots per page: small
    // row counts exercise multi-page behavior.
    const schema::TableSchema schema = schema::parse_table_schema(
        R"({"table_name": "storage",
            "columns": [
              {"column_name": "sid", "column_type": "Integer", "size": 4},
              {"column_name": "pad", "column_type": "String",  "size": 900}]})");
    const auto make_row = [](int id) {
        tuple::Tuple row;
        row.add(id);
        row.add(std::string(900, static_cast<char>('a' + id % 26)));
        return row;
    };
    const auto scan_ids = [](heapfile::HeapFile& file) {
        std::vector<int> scanned;
        heapfile::HeapFileIterator it = file.scan();
        while (const std::optional<tuple::Tuple> row = it.next()) {
            scanned.push_back(std::get<int>(row->column(0).column_value()));
        }
        return scanned;
    };

    bufman::BufferManager manager;
    manager.init(8, std::make_unique<bufman::LRUPolicy>());
    heapfile::HeapFile file(manager);
    check(ok([&] {
              file.create("storage.bin", schema);
              file.open("storage.bin", schema);
          }),
          "storage: file created and opened");
    check(ok([&] { file.check_structure(); }),
          "storage: an empty file passes the structural check");

    // Fill two pages completely. Tail-append keeps the full list in
    // allocation order, so the bulk load scans in insertion order.
    std::vector<heapfile::RowId> live;
    std::vector<int> keys; // parallel to live: the row's sid
    check(ok([&] {
              bool placement_ok = true;
              for (int id = 1; id <= 8; ++id) {
                  const heapfile::RowId rid = file.insert(make_row(id));
                  placement_ok = placement_ok &&
                                 rid.page_id == static_cast<std::uint32_t>((id - 1) / 4 + 1) &&
                                 rid.slot == static_cast<std::uint32_t>((id - 1) % 4);
                  live.push_back(rid);
                  keys.push_back(id);
              }
              file.check_structure();
              return placement_ok;
          }),
          "storage: eight rows fill pages 1 and 2 in order");
    check(ok([&] { return scan_ids(file) == keys; }),
          "storage: a bulk load scans in allocation order");

    // Fill/unfill transitions: erase a row of the full page 1 -> the page
    // moves to the free list; the next insert reuses exactly that slot and
    // the page fills back into the full list.
    check(ok([&] {
              file.erase(live[0]);
              file.check_structure();
          }),
          "storage: erasing from a full page frees it onto the free list");
    check(ok([&] {
              const heapfile::RowId rid = file.insert(make_row(9));
              file.check_structure();
              live[0] = rid;
              keys[0] = 9;
              return rid.page_id == 1 && rid.slot == 0;
          }),
          "storage: an insert reuses the freed slot of the oldest page");

    // Empty pages never die: erase everything and the structure keeps both
    // pages on the free list; the next insert reuses instead of allocating.
    // (The pages reach disk only at close, so the on-disk proof comes later.)
    check(ok([&] {
              for (const heapfile::RowId& row_id : live) {
                  file.erase(row_id);
              }
              file.check_structure();
          }),
          "storage: erasing everything leaves both pages on the free list");
    live.clear();
    keys.clear();
    heapfile::RowId rid;
    check(ok([&] {
              rid = file.insert(make_row(10));
              return rid.page_id == 1 && rid.slot == 0;
          }),
          "storage: the emptied file reuses a page instead of allocating");

    // A randomized insert/erase sequence, checked after every operation.
    // The shadow model (live + keys) tracks exactly which rows must exist.
    std::mt19937 rng(42);
    int next_id = 11;
    live.push_back(rid);
    keys.push_back(10);
    check(ok([&] {
              for (int step = 0; step < 200; ++step) {
                  if (rng() % 100 < 55 || live.empty()) {
                      live.push_back(file.insert(make_row(next_id)));
                      keys.push_back(next_id);
                      ++next_id;
                  } else {
                      const std::size_t victim = rng() % live.size();
                      file.erase(live[victim]);
                      live.erase(live.begin() + static_cast<long>(victim));
                      keys.erase(keys.begin() + static_cast<long>(victim));
                  }
                  file.check_structure();
              }
          }),
          "storage: 200 random operations keep the structure consistent");
    check(ok([&] {
              std::vector<int> scanned = scan_ids(file);
              std::vector<int> expected = keys;
              std::sort(scanned.begin(), scanned.end());
              std::sort(expected.begin(), expected.end());
              return scanned == expected;
          }),
          "storage: the final scan returns exactly the shadow model's rows");

    // A corrupted link must be reported, not silently obeyed: point page
    // 1's list-next at itself (the raw block layer, after a flush).
    check(ok([&] { file.close(); }), "storage: file closed for the damage step");
    // The close flushed the dirty pages: the data pages are on disk, and
    // erases never shrank the file below its first two pages.
    check(ok([&] {
              bufman::File raw = bufman::open_for_read("storage.bin");
              const bool kept = bufman::block_count(raw) >= 3;
              bufman::close(raw);
              return kept;
          }),
          "storage: the closed file keeps its data pages on disk");
    check(ok([&] {
              bufman::File raw = bufman::open_for_append("storage.bin");
              std::array<char, bufman::kBlockSize> block{};
              bufman::read_block(raw, 1, block);
              block[bufman::kBlockSize - 4] = 1; // list-next := self -> a cycle
              bufman::write_block(raw, 1, block.data());
              bufman::close(raw);
          }),
          "storage: damaged page written back");
    {
        heapfile::HeapFile damaged(manager);
        check(ok([&] { damaged.open("storage.bin", schema); }),
              "storage: the damaged file still opens");
        check(fails_with([&] { damaged.check_structure(); }, "cyclic"),
              "storage: a cyclic list is reported by the structural check");
        damaged.close();
    }

    check(ok([&] { file.open("storage.bin", schema); }), "storage: file reopened for cleanup");
    manager.close_all();
    std::filesystem::remove("storage.bin");
}

// --- schema and catalog: the vendored data-layout checks -----------------
//
// Tuple/Column construction, the record serializer (byte-for-byte), the
// JSON catalog loader, the writer with its indexes array and atomic
// install, CSV parsing and loading, and the deterministic row generator.

tuple::Tuple person_row(int pid, const std::string& name, int age, const std::string& city) {
    tuple::Tuple t;
    t.add(pid);
    t.add(name);
    t.add(age);
    t.add(city);
    return t;
}

void schema_and_catalog_checks() {
    // The fixture schemas are built here and saved to scratch paths, not
    // loaded from the working directory's tablecatalog/ — that directory
    // is also a session's live catalog dir, and a session legitimately
    // creating its own `car` (with whatever columns it wants) must not
    // change what this suite asserts. Same idiom as the catalog
    // round-trip tests below: build, save, load back.
    const std::string person_catalog = temp_path() + "-person.json";
    const std::string car_catalog = temp_path() + "-car.json";
    {
        schema::TableSchema fixture("person");
        fixture.add_column("pid", tuple::ColumnType::Integer, 4);
        fixture.add_column("name", tuple::ColumnType::String, 10);
        fixture.add_column("age", tuple::ColumnType::Integer, 4);
        fixture.add_column("city", tuple::ColumnType::String, 3);
        schema::save_table_schema(person_catalog, fixture, {});
        schema::TableSchema car_fixture("car");
        car_fixture.add_column("cid", tuple::ColumnType::Integer, 4);
        car_fixture.add_column("brand", tuple::ColumnType::String, 15);
        car_fixture.add_column("model", tuple::ColumnType::String, 10);
        car_fixture.add_column("year", tuple::ColumnType::Integer, 4);
        car_fixture.add_column("price", tuple::ColumnType::Integer, 4);
        schema::save_table_schema(car_catalog, car_fixture, {});
    }
    const schema::TableSchema person = schema::load_table_schema(person_catalog);
    const schema::TableSchema car = schema::load_table_schema(car_catalog);

    // --- 1. Column construction: type and length mirror the value ---
    {
        const tuple::Column ci(42);
        check(ci.column_type() == tuple::ColumnType::Integer, "int column type");
        check(ci.column_length() == 4, "int column length is 4");
        check(std::get<int>(ci.column_value()) == 42, "int column value");

        const tuple::Column cd(3.14);
        check(cd.column_type() == tuple::ColumnType::Double, "double column type");
        check(cd.column_length() == 8, "double column length is 8");
        check(std::get<double>(cd.column_value()) == 3.14, "double column value");

        const tuple::Column cs(std::string("Ruth"));
        check(cs.column_type() == tuple::ColumnType::String, "string column type");
        check(cs.column_length() == 4, "string column length is its byte size");
        check(std::get<std::string>(cs.column_value()) == "Ruth", "string column value");

        const tuple::Column ce(std::string(""));
        check(ce.column_type() == tuple::ColumnType::String && ce.column_length() == 0,
              "empty string column has length 0");
    }

    // --- 2. Default column ---
    {
        const tuple::Column c;
        check(c.column_type() == tuple::ColumnType::Integer, "default column type");
        check(c.column_length() == 4, "default column length is 4");
        check(std::get<int>(c.column_value()) == 0, "default column value is 0");
    }

    // --- 3. Tuple API ---
    {
        tuple::Tuple t;
        check(t.column_count() == 0, "fresh tuple is empty");
        t.add(1);
        t.add(2.5);
        t.add(std::string("ab"));
        check(t.column_count() == 3, "add overloads build columns");
        check(t.column(0).column_type() == tuple::ColumnType::Integer &&
                  t.column(1).column_type() == tuple::ColumnType::Double &&
                  t.column(2).column_type() == tuple::ColumnType::String,
              "add overloads pick the right types");
        t.add_column(tuple::Column(9.5));
        check(t.column_count() == 4 && t.column(3).column_type() == tuple::ColumnType::Double,
              "add_column accepts a prebuilt Column");
        t.clear();
        check(t.column_count() == 0, "clear empties the tuple");
    }

    // --- 4. Record serialization: values only, layout from the schema ---
    {
        const schema::TupleSerializer serializer(person);
        const tuple::Tuple t = person_row(42, "Ruth", 7, "HU");
        std::array<char, 64> buf{};
        std::memset(buf.data(), 0x5A, buf.size());
        check(ok([&] { serializer.serialize(t, buf.data()); }), "serialize succeeds");
        check(serializer.record_size() == 21, "record_size is 21 (4 + 10 + 4 + 3)");
        bool marker_survives = true;
        for (std::size_t i = 21; i < buf.size(); ++i) {
            marker_survives = marker_survives && buf[i] == 0x5A;
        }
        check(marker_survives, "bytes past the record are untouched");
        check(ok([&] { return serializer.deserialize(buf.data()) == t; }),
              "deserialize returns exactly the record's columns");
    }

    // --- 5. Round-trips through the one serializer ---
    {
        const auto round_trip = [&](const schema::TableSchema& schema, const tuple::Tuple& in) {
            return ok([&] {
                const schema::TupleSerializer serializer(schema);
                std::array<char, 128> buf{};
                serializer.serialize(in, buf.data());
                return serializer.deserialize(buf.data()) == in;
            });
        };
        check(round_trip(person, person_row(1, "Ruth", 42, "HU")), "round-trip: short strings");
        check(round_trip(person, person_row(2, "", 3, "")), "round-trip: empty strings");
        check(round_trip(person, person_row(3, "1234567890", 4, "ABC")),
              "round-trip: full-width strings");
        check(round_trip(person, person_row(-5, "x", -2, "y")), "round-trip: negative ints");
        tuple::Tuple car_tuple;
        car_tuple.add(9);
        car_tuple.add(std::string("Volvo"));
        car_tuple.add(std::string("240"));
        car_tuple.add(1976);
        car_tuple.add(3);
        check(round_trip(car, car_tuple), "round-trip: a car record");
    }

    // --- 5b. The schema-bound TupleSerializer class ---
    {
        // The binding happens once at construction; every operation works
        // against the bound schema, with no per-call schema parameter.
        const schema::TupleSerializer serializer(person);
        check(serializer.record_size() == 21,
              "the bound serializer knows the record size (4 + 10 + 4 + 3)");
        const tuple::Tuple t = person_row(7, "Ruth", 42, "HU");

        // Byte-for-byte against the hand-built values-only layout: the
        // on-disk compatibility guard — these bytes are the format.
        std::array<char, 64> buf{};
        std::memset(buf.data(), 0x5A, buf.size());
        check(ok([&] { serializer.serialize(t, buf.data()); }),
              "the bound serializer packs the tuple");
        std::array<char, 21> expected{};
        const int pid = 7, age = 42;
        std::memcpy(expected.data() + 0, &pid, 4);
        std::memcpy(expected.data() + 4, "Ruth", 4);
        std::memcpy(expected.data() + 14, &age, 4);
        std::memcpy(expected.data() + 18, "HU", 2);
        check(std::memcmp(buf.data(), expected.data(), 21) == 0,
              "record bytes match the hand-built layout exactly");
        bool marker = true;
        for (std::size_t i = 21; i < buf.size(); ++i) {
            marker = marker && buf[i] == 0x5A;
        }
        check(marker, "bytes past the record are untouched");

        // Short strings are NUL-padded to their declared width.
        const tuple::Tuple short_t = person_row(1, "Ru", 2, "H");
        check(ok([&] { serializer.serialize(short_t, buf.data()); }),
              "pack the short-string tuple");
        bool padded = buf[4] == 'R' && buf[5] == 'u';
        for (std::size_t i = 6; i < 14; ++i) {
            padded = padded && buf[i] == '\0';
        }
        padded = padded && buf[18] == 'H' && buf[19] == '\0' && buf[20] == '\0';
        check(padded, "short strings are NUL-padded to their declared width");
        check(ok([&] { return serializer.deserialize(buf.data()) == short_t; }),
              "the unpacked tuple is the packed one");

        // All-or-nothing pack: a tuple the schema refuses never touches the
        // buffer, and the bytes behind a refusal stay exactly as they were.
        std::memset(buf.data(), 0x5A, buf.size());
        tuple::Tuple arity;
        arity.add(1);
        arity.add(std::string("x"));
        check(fails_with([&] { serializer.serialize(arity, buf.data()); }),
              "wrong arity is refused by the bound serializer");
        tuple::Tuple car_tuple;
        car_tuple.add(9);
        car_tuple.add(std::string("Volvo"));
        car_tuple.add(std::string("240"));
        car_tuple.add(1976);
        car_tuple.add(3);
        check(fails_with([&] { serializer.serialize(car_tuple, buf.data()); }),
              "a tuple for another schema is refused by the binding");
        bool untouched = true;
        for (const char c : buf) {
            untouched = untouched && c == 0x5A;
        }
        check(untouched, "refused packs leave the buffer untouched");

        // Damage on unpack is a named error.
        serializer.serialize(t, buf.data());
        buf[9] = 'X'; // name-field padding (value ends at 7, NUL at 8)
        check(fails_with([&] { serializer.deserialize(buf.data()); }, "padding"),
              "garbage in the string padding is refused");

        // A schema without declared widths has no record layout.
        schema::TableSchema code_built("people");
        code_built.add_column("a", tuple::ColumnType::Integer);
        code_built.add_column("b", tuple::ColumnType::String);
        check(schema::TupleSerializer(code_built).record_size() == 0,
              "undeclared widths: record_size is 0");
    }

    // --- 6. Through the buffer pool ---
    {
        const std::string path = temp_path();
        bufman::BufferManager mgr;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File file = mgr.open_file(path);
        const schema::TupleSerializer serializer(person);
        const tuple::Tuple out = person_row(7, "persist", 2, "XY");

        check(ok([&] {
                  bufman::PinnedPage page = bufman::PinnedPage::fresh(mgr, file, 0);
                  serializer.serialize(out, page.data());
                  page.mark_dirty();
              }),
              "serialize into a fresh block 0");
        check(ok([&] {
                  const bufman::PinnedPage page = bufman::PinnedPage::fresh(mgr, file, 1);
                  return page.data()[0] == 0 && mgr.frame(0).dirty && mgr.frame(0).pin_count == 1;
              }),
              "alloc_page block 1 evicts the dirty frame and comes back zeroed, dirty, pinned");
        check(ok([&] {
                  const bufman::PinnedPage page(mgr, file, 0);
                  return serializer.deserialize(page.data()) == out;
              }),
              "tuple survives the pool/disk path unchanged");
        mgr.close_all();
        std::remove(path.c_str());
    }

    // --- Schema test 1: ColumnSchema ---
    {
        const schema::ColumnSchema c("age", tuple::ColumnType::Integer);
        check(c.column_name() == "age" && c.column_type() == tuple::ColumnType::Integer &&
                  c.column_size() == 0,
              "ColumnSchema holds name, type, and declared size");
        const schema::ColumnSchema same("age", tuple::ColumnType::Integer);
        const schema::ColumnSchema other("age", tuple::ColumnType::String, 4);
        check(c == same && !(c == other), "ColumnSchema equality");
    }

    // --- Schema test 2: building a schema ---
    schema::TableSchema people("people");
    {
        check(people.table_name() == "people", "table name stored");
        check(ok([&] { people.add_column("name", tuple::ColumnType::String); }), "add name");
        check(ok([&] { people.add_column("age", tuple::ColumnType::Integer); }), "add age");
        check(ok([&] { people.add_column("height", tuple::ColumnType::Double); }), "add height");
        check(people.column_count() == 3, "column_count grows");
        check(people.column_info(0).column_name() == "name" &&
                  people.column_info(0).column_type() == tuple::ColumnType::String,
              "column_info(0) is name");
        check(people.column_info(1).column_name() == "age" &&
                  people.column_info(1).column_type() == tuple::ColumnType::Integer,
              "column_info(1) is age");
        check(people.column_info(2).column_name() == "height" &&
                  people.column_info(2).column_type() == tuple::ColumnType::Double,
              "column_info(2) is height");
        check(fails_with([&] { people.add_column("age", tuple::ColumnType::Integer); }),
              "duplicate column name fails");
        check(fails_with([&] { people.add_column("", tuple::ColumnType::Integer); }),
              "empty column name fails");
    }

    // --- Schema test 3: column_name_map ---
    {
        const auto idx = people.column_index("age");
        check(idx.has_value() && *idx == 1, "column_index finds age at 1");
        check(!people.column_index("nope").has_value(), "unknown name returns nullopt");
    }

    // --- Schema test 4: validate ---
    {
        check(ok([&] { people.validate(people.make_tuple()); }), "a make_tuple product validates");
        tuple::Tuple one;
        one.add(1);
        check(fails_with([&] { people.validate(one); }), "wrong column count fails");
        tuple::Tuple wrong_type;
        wrong_type.add(std::string("Ruth"));
        wrong_type.add(2.5); // slot 1 must be Integer
        wrong_type.add(0.0);
        check(fails_with([&] { people.validate(wrong_type); }, "1"),
              "wrong type at a position fails, naming the position");
    }

    // --- Schema test 5: make_tuple ---
    {
        const tuple::Tuple t = people.make_tuple();
        check(t.column_count() == 3, "make_tuple column count");
        check(t.column(0).column_type() == tuple::ColumnType::String &&
                  std::get<std::string>(t.column(0).column_value()).empty(),
              "slot 0 defaults to an empty string");
        check(t.column(1).column_type() == tuple::ColumnType::Integer &&
                  std::get<int>(t.column(1).column_value()) == 0,
              "slot 1 defaults to 0");
        check(t.column(2).column_type() == tuple::ColumnType::Double &&
                  std::get<double>(t.column(2).column_value()) == 0.0,
              "slot 2 defaults to 0.0");
    }

    // --- Schema test 6: creation order, byte-for-byte ---
    {
        const schema::TupleSerializer serializer(person);
        const tuple::Tuple logical = person_row(7, "Ruth", 42, "HU");
        check(ok([&] { person.validate(logical); }), "hand-built logical tuple validates");
        std::array<char, 64> got{};
        check(ok([&] { serializer.serialize(logical, got.data()); }),
              "serialize in creation order");
        // Hand-built values-only layout: pid@0, name@4 padded to 10, age@14,
        // city@18 padded to 3.
        std::array<char, 21> expected{};
        const int pid = 7, age = 42;
        std::memcpy(expected.data() + 0, &pid, 4);
        std::memcpy(expected.data() + 4, "Ruth", 4);
        std::memcpy(expected.data() + 14, &age, 4);
        std::memcpy(expected.data() + 18, "HU", 2);
        check(std::memcmp(got.data(), expected.data(), 21) == 0,
              "creation order: bytes match the value layout exactly");
    }

    // --- Schema test 7: failure discipline on serialize ---
    {
        const schema::TupleSerializer serializer(person);
        tuple::Tuple wrong;
        wrong.add(1);
        std::array<char, 64> buf{};
        check(fails_with([&] { serializer.serialize(wrong, buf.data()); }),
              "the serializer refuses a mismatched tuple");
    }

    // --- Schema test 8: by-name workflow ---
    {
        const schema::TupleSerializer serializer(person);
        const std::size_t age_slot = *person.column_index("age");
        check(age_slot == 2, "by-name mapping lands on tuple slot 2");

        // Tuple columns are immutable values: filling by name means
        // rebuilding with the named slot replaced.
        tuple::Tuple filled;
        const tuple::Tuple fresh = person.make_tuple();
        for (std::size_t p = 0; p < fresh.column_count(); ++p) {
            filled.add_column(p == age_slot ? tuple::Column(42) : fresh.column(p));
        }
        check(ok([&] { person.validate(filled); }), "filled tuple validates");
        std::array<char, 64> buf{};
        check(ok([&] {
                  serializer.serialize(filled, buf.data());
                  const tuple::Tuple back = serializer.deserialize(buf.data());
                  return std::get<int>(back.column(age_slot).column_value()) == 42;
              }),
              "read 'age' back by name after a round-trip");
    }

    // --- Catalog test 1: loading person.json ---
    {
        check(person.table_name() == "person", "table name is 'person'");
        check(person.column_count() == 4, "four columns");
        const std::vector<std::string> names = {"pid", "name", "age", "city"};
        const std::vector<tuple::ColumnType> types = {
            tuple::ColumnType::Integer, tuple::ColumnType::String,
            tuple::ColumnType::Integer, tuple::ColumnType::String};
        const std::vector<std::size_t> sizes = {4, 10, 4, 3};
        bool names_ok = true, types_ok = true, sizes_ok = true;
        for (std::size_t i = 0; i < person.column_count(); ++i) {
            const schema::ColumnSchema& c = person.column_info(i);
            names_ok = names_ok && c.column_name() == names[i];
            types_ok = types_ok && c.column_type() == types[i];
            sizes_ok = sizes_ok && c.column_size() == sizes[i];
        }
        check(names_ok, "column names in file order");
        check(types_ok, "column types match the catalog");
        check(sizes_ok, "declared sizes match the catalog");
        check(person.column_index("age").has_value() && !person.column_index("nope").has_value(),
              "column_name_map works on a loaded schema");
        check(ok([&] { person.validate(person.make_tuple()); }),
              "a make_tuple product of the loaded schema validates");
    }

    // --- Catalog test 2: the catalog matches the hardwired Person layout ---
    {
        std::size_t total = 0;
        for (std::size_t i = 0; i < person.column_count(); ++i) {
            total += person.column_info(i).column_size();
        }
        check(total == 21,  // the legacy record width: 4 + 10 + 4 + 3
              "declared sizes sum to the legacy record width");
    }

    // --- Catalog test 3: loading car.json ---
    {
        check(car.table_name() == "car" && car.column_count() == 5, "five columns");
        const std::vector<std::size_t> sizes = {4, 15, 10, 4, 4};
        bool sizes_ok = true;
        for (std::size_t i = 0; i < car.column_count(); ++i) {
            sizes_ok = sizes_ok && car.column_info(i).column_size() == sizes[i];
        }
        check(sizes_ok, "declared sizes 4/15/10/4/4");
        check(ok([&] { car.validate(car.make_tuple()); }), "a car tuple validates");
    }

    // --- Catalog test 4: a parse yields exactly the text's columns ---
    {
        const std::string person_text =
            "{\n"
            "  \"table_name\": \"person\",\n"
            "  \"columns\": [\n"
            "    { \"column_name\": \"pid\",  \"column_type\": \"Integer\", \"size\": 4 },\n"
            "    { \"column_name\": \"name\", \"column_type\": \"String\",  \"size\": 10 },\n"
            "    { \"column_name\": \"age\",  \"column_type\": \"Integer\", \"size\": 4 },\n"
            "    { \"column_name\": \"city\", \"column_type\": \"String\",  \"size\": 3 }\n"
            "  ]\n"
            "}\n";
        check(ok([&] {
                  const schema::TableSchema parsed = schema::parse_table_schema(person_text);
                  return parsed.table_name() == "person" && parsed.column_count() == 4 &&
                         parsed.column_info(0).column_name() == "pid";
              }),
              "a parse yields exactly the text's columns");
    }

    // --- Catalog test 5: parse-level errors name the line ---
    {
        const auto parse_fails = [](const std::string& text, const std::string& part = "") {
            return fails_with([&] { schema::parse_table_schema(text); }, part);
        };
        check(parse_fails("", "line 1"), "empty text fails, naming line 1");
        check(parse_fails("{\n  \"table_name\": true\n}", "line 2"),
              "true as a value fails, naming line 2");
        check(parse_fails("{\"table_name\": \"per"), "unterminated string fails");
        check(parse_fails("{\"table_name\": \"a\\qb\"}"), "unknown escape fails");
        check(parse_fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                          "\"column_type\": \"Integer\", \"size\": 4.5}]}"),
              "floating-point size fails");
        check(parse_fails("{} "), "missing keys fail");
        check(parse_fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                          "\"column_type\": \"Integer\", \"size\": 4}]} x",
                          "after"),
              "trailing garbage fails");
    }

    // --- Catalog test 6: content errors name the key or column ---
    {
        const auto fails = [](const std::string& text, const std::string& part = "") {
            return fails_with([&] { schema::parse_table_schema(text); }, part);
        };
        check(fails("{\"columns\": []}"), "missing 'table_name' fails");
        check(fails("{\"table_name\": \"\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\", \"size\": 4}]}"),
              "empty 'table_name' fails");
        check(fails("{\"table_name\": \"t\"}"), "missing 'columns' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": 3}"), "'columns' not an array fails");
        check(fails("{\"table_name\": \"t\", \"columns\": []}"), "empty columns array fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [3]}"),
              "column entry not an object fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_type\": "
                    "\"Integer\", \"size\": 4}]}"),
              "missing 'column_name' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"size\": 4}]}"),
              "missing 'column_type' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\"}]}"),
              "missing 'size' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Float\", \"size\": 4}]}",
                    "Float"),
              "unknown type spelling fails, naming it");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\", \"size\": 0}]}"),
              "size 0 fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\", \"size\": 4}, "
                    "{\"column_name\": \"a\", \"column_type\": \"String\", "
                    "\"size\": 2}]}",
                    "already exists"),
              "duplicate column name fails with add_column's message");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"\", "
                    "\"column_type\": \"Integer\", \"size\": 4}]}"),
              "empty column name fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"nope\": 1}]}"),
              "unexpected key fails");
    }

    // --- Catalog test 7: failure discipline across bad catalogs ---
    {
        const std::vector<std::string> bad = {
            "", "not json", "[1, 2]", "{", "{\"table_name\": \"t\"}",
            "{\"table_name\": \"t\", \"columns\": [{}]}",
        };
        bool all_refused = true;
        for (const std::string& text : bad) {
            all_refused = all_refused && fails_with([&] { schema::parse_table_schema(text); });
        }
        check(all_refused, "every bad catalog is refused with a named error");
    }

    // --- Catalog test 8: missing file ---
    check(fails_with([] { schema::load_table_schema("tablecatalog/no_such_table.json"); },
                     "no_such_table.json"),
          "a missing file fails, naming the path");

    // --- CLI module test 1: CSV row parsing ---
    {
        schema::TableSchema mixed("mixed");
        mixed.add_column("a", tuple::ColumnType::Integer);
        mixed.add_column("b", tuple::ColumnType::Double);
        mixed.add_column("c", tuple::ColumnType::String);
        const auto row_of = [](const schema::TableSchema& schema, const std::string& line) {
            return schema::parse_csv_row(schema, line);
        };
        check(ok([&] { return row_of(person, "7,Ruth,42,HU") == person_row(7, "Ruth", 42, "HU"); }),
              "a valid row parses into the matching tuple");
        check(ok([&] {
                  const tuple::Tuple t = row_of(person, "1,,2,");
                  return std::get<std::string>(t.column(1).column_value()).empty() &&
                         std::get<std::string>(t.column(3).column_value()).empty();
              }),
              "empty tokens are empty strings");
        check(ok([&] { row_of(person, "3,1234567890,4,ABC"); }), "a full-width string parses");
        check(fails_with([&] { row_of(person, "7,Ruth"); }, "2 column(s)"),
              "too few tokens are refused, naming the arity");
        check(fails_with([&] { row_of(person, "7,Ruth,42,HU,x"); }), "too many tokens are refused");
        check(fails_with([&] { row_of(person, "abc,Ruth,42,HU"); },
                         "column 'pid': expected an integer"),
              "a bad integer names its column");
        check(fails_with([&] { row_of(person, "7,Ruth,4.5,HU"); }), "a fractional age is refused");
        check(ok([&] { row_of(mixed, "1,2.5,x"); }), "a Double column parses");
        check(fails_with([&] { row_of(mixed, "1,2.5.3,x"); }, "column 'b': expected a number"),
              "a bad double names its column");
        check(fails_with([&] { row_of(mixed, "1,,x"); }), "an empty numeric token is refused");
    }

    // --- CLI module test 2: width rules surface at insert, not parse ---
    {
        tuple::Tuple t;
        check(ok([&] { t = schema::parse_csv_row(person, "8,12345678901,9,AA"); }),
              "a too-wide string parses fine (syntax is legal)");
        bufman::BufferManager mgr;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        const std::string path = temp_path() + "-csv-width";
        heapfile::HeapFile file(mgr);
        check(ok([&] {
                  file.create(path, person);
                  file.open(path, person);
              }),
              "create and open for the width test");
        check(fails_with([&] { file.insert(t); }, "exceeds the declared width"),
              "insert refuses it with the schema's named error");
        file.close();
        std::remove(path.c_str());
    }

    // --- CLI module test 3: CSV file loading ---
    {
        const std::string csv_path = temp_path() + "-load.csv";
        {
            std::ofstream out(csv_path, std::ios::binary);
            out << "pid,name,age,city\r\n";      // header, CRLF ending
            out << "1,Alice,30,PR\r\n";          // valid
            out << "\r\n";                       // blank line: silent skip
            out << "2,Bob,old,NY\n";             // bad age
            out << "3,Cara,21,PARIS\n";          // too-wide city
            out << "4,Dan,19,HU\n";              // valid
        }
        std::ostringstream diagnostics;
        schema::CsvLoadResult result;
        check(ok([&] { result = schema::load_csv(person, csv_path, diagnostics); }),
              "load_csv reads the file");
        // The too-wide city row parses fine (width is insert-time), so the
        // load yields 3 valid rows and skips the header and the bad age.
        check(result.tuples.size() == 3 && result.skipped == 2, "3 valid rows, 2 skipped");
        check(result.tuples.size() == 3 &&
                  std::get<std::string>(result.tuples[0].column(1).column_value()) == "Alice" &&
                  std::get<std::string>(result.tuples[1].column(1).column_value()) == "Cara" &&
                  std::get<std::string>(result.tuples[2].column(1).column_value()) == "Dan",
              "the valid rows come back in file order");
        const std::string text = diagnostics.str();
        check(text.find("line 1: column 'pid': expected an integer") != std::string::npos &&
                  text.find("line 4: column 'age': expected an integer") != std::string::npos,
              "skipped rows are reported as line N: reason");
        check(fails_with([&] { schema::load_csv(person, temp_path() + "-no-such.csv", diagnostics); }),
              "a missing file is a named error");
        std::remove(csv_path.c_str());
    }

    // --- CLI module test 4: generation ---
    {
        const std::vector<tuple::Tuple> a = schema::generate_tuples(person, 10, 42);
        const std::vector<tuple::Tuple> b = schema::generate_tuples(person, 10, 42);
        const std::vector<tuple::Tuple> c = schema::generate_tuples(person, 10, 43);
        check(a == b, "same seed produces identical tuples");
        check(!(a == c), "a different seed produces different values");
        check(a.size() == 10, "the count is exact");
        check(ok([&] {
                  for (const tuple::Tuple& t : a) {
                      person.validate(t);
                  }
              }),
              "every generated tuple passes validate");
    }

    // --- CLI module test 5: generator conventions ---
    {
        const schema::TupleSerializer serializer(person);
        const std::vector<tuple::Tuple> tuples = schema::generate_tuples(person, 5, 7, 100);
        bool sequence_ok = true;
        bool strings_ok = true;
        for (std::size_t i = 0; i < tuples.size(); ++i) {
            sequence_ok = sequence_ok && std::get<int>(tuples[i].column(0).column_value()) ==
                                             100 + static_cast<int>(i);
            const std::string& name = std::get<std::string>(tuples[i].column(1).column_value());
            strings_ok = strings_ok && !name.empty() && name.size() <= 10 &&
                         name.find('\0') == std::string::npos;
        }
        check(sequence_ok, "the first Integer column counts first_id upward");
        check(strings_ok, "strings stay within their declared width, no NUL");
        std::array<char, 64> buf{};
        check(ok([&] {
                  serializer.serialize(tuples[0], buf.data());
                  return serializer.deserialize(buf.data()) == tuples[0];
              }),
              "generated rows round-trip through the serializer");
    }

    // --- ex13: catalog indexes — loader extension and the writer ---
    {
        check(ok([] { return schema::load_catalog("tablecatalog/person.json").indexes.empty(); }),
              "a catalog without 'indexes' loads with an empty index list");
        check(ok([] {
                  const schema::Catalog parsed = schema::parse_catalog(
                      "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                      "\"column_type\":\"Integer\",\"size\":4}],"
                      "\"indexes\":[{\"name\":\"t_a\",\"column\":\"a\"}]}");
                  return parsed.indexes.size() == 1 && parsed.indexes[0].name == "t_a" &&
                         parsed.indexes[0].column == "a";
              }),
              "an indexes array parses into name/column pairs");
        check(ok([] {
                  return schema::parse_catalog(
                             "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                             "\"column_type\":\"Integer\",\"size\":4}],\"indexes\":[]}")
                      .indexes.empty();
              }),
              "an empty indexes array parses to no indexes");

        const char* bad_catalogs[] = {
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"i\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"column\":\"a\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"\",\"column\":\"a\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"i\",\"column\":\"a\"},"
            "{\"name\":\"i\",\"column\":\"a\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"i\",\"column\":\"a\",\"extra\":1}]}",
        };
        for (const char* bad : bad_catalogs) {
            check(checks::error_of([&] { schema::parse_catalog(bad); }).rfind("line ", 0) == 0,
                  "malformed index entry refused with a named error");
        }

        // The writer: round-trip through a temp catalog file.
        const std::string catalog_path = temp_path() + "-catalog.json";
        std::remove(catalog_path.c_str());
        check(ok([&] { schema::save_table_schema(catalog_path, person, {{"person_pid", "pid"}}); }),
              "save_table_schema writes a catalog with an index");
        schema::Catalog written{schema::TableSchema(""), {}};
        check(ok([&] {
                  written = schema::load_catalog(catalog_path);
                  return written.schema.table_name() == "person" &&
                         written.schema.column_count() == person.column_count();
              }),
              "a saved catalog loads again");
        bool columns_match = written.schema.column_count() == person.column_count();
        for (std::size_t i = 0; columns_match && i < person.column_count(); ++i) {
            const schema::ColumnSchema& w = written.schema.column_info(i);
            const schema::ColumnSchema& p = person.column_info(i);
            columns_match = w.column_name() == p.column_name() &&
                            w.column_type() == p.column_type() &&
                            w.column_size() == p.column_size();
        }
        check(columns_match, "saved columns round-trip exactly");
        check(written.indexes.size() == 1 && written.indexes[0].name == "person_pid" &&
                  written.indexes[0].column == "pid",
              "saved indexes round-trip exactly");
        check(ok([&] { schema::save_table_schema(catalog_path, person, {}); }),
              "saving with no indexes overwrites cleanly");
        check(ok([&] { return schema::load_catalog(catalog_path).indexes.empty(); }),
              "the overwritten catalog has no indexes");

        // The primary key: saved when set, absent when not, refused when
        // it names a column the schema does not have.
        check(!schema::load_catalog(catalog_path).primary_key.has_value(),
              "a catalog saved without a primary key loads with none");
        check(ok([&] { schema::save_table_schema(catalog_path, person, {}, "pid"); }),
              "save_table_schema writes a catalog with a primary key");
        check(schema::load_catalog(catalog_path).primary_key == std::optional<std::string>("pid"),
              "a saved primary key loads back exactly");
        check(fails_with([&] { schema::save_table_schema(catalog_path, person, {}, "nosuch"); }),
              "a primary key naming an unknown column refuses to save");
        check(fails_with([&] {
                  schema::parse_catalog(
                      "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                      "\"column_type\":\"Integer\",\"size\":4}],\"primary_key\":\"b\"}");
              }),
              "a catalog whose primary_key names an unknown column is refused");
        // The old shape (no primary_key member) parses unchanged.
        check(ok([&] {
                  return schema::parse_catalog(
                             "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                             "\"column_type\":\"Integer\",\"size\":4}]}")
                             .primary_key.has_value() == false;
              }),
              "a pre-primary-key catalog parses with no primary key");

        // The foreign keys: saved when declared, refused when they name
        // unknown columns, absent from catalogs without them.
        check(ok([&] {
                  schema::save_table_schema(catalog_path, person, {{"person_pid", "pid"}}, "pid",
                                            {{"age", "person", "pid"}});
                  return true;
              }),
              "save_table_schema writes a catalog with a foreign key");
        {
            const schema::Catalog loaded = schema::load_catalog(catalog_path);
            check(loaded.foreign_keys.size() == 1 &&
                      loaded.foreign_keys[0].column == "age" &&
                      loaded.foreign_keys[0].parent_table == "person" &&
                      loaded.foreign_keys[0].parent_column == "pid",
                  "a saved foreign key loads back exactly");
        }
        check(fails_with([&] {
                  schema::save_table_schema(catalog_path, person, {{"person_pid", "pid"}}, "pid",
                                            {{"nosuch", "person", "pid"}});
              }),
              "a foreign key naming an unknown column refuses to save");
        check(fails_with([&] {
                  schema::save_table_schema(catalog_path, person, {{"person_pid", "pid"}}, "pid",
                                            {{"pid", "person", "pid"}, {"pid", "other", "k"}});
              }),
              "two foreign keys on one column refuse to save");
        check(fails_with([&] {
                  schema::parse_catalog(
                      "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                      "\"column_type\":\"Integer\",\"size\":4}],\"foreign_keys\":"
                      "[{\"column\":\"b\",\"parent_table\":\"p\",\"parent_column\":\"k\"}]}");
              }),
              "a catalog whose foreign key names an unknown column is refused");
        check(schema::parse_catalog("{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                                    "\"column_type\":\"Integer\",\"size\":4}]}")
                  .foreign_keys.empty(),
              "a pre-foreign-key catalog parses with no foreign keys");

        // A refused save leaves the installed catalog byte-for-byte.
        const auto file_text = [&] {
            std::ifstream in(catalog_path, std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        };
        const std::string before = file_text();
        check(fails_with([&] { schema::save_table_schema(catalog_path, person, {{"bad", "nosuch"}}); }),
              "an index naming an unknown column refuses to save");
        check(before == file_text(), "a failed save leaves the catalog unchanged");
        check(std::remove(catalog_path.c_str()) == 0, "catalog temp file cleaned up");
    }
}

} // namespace

int run_storage_checks() {
    // An unexpected failure escaping a suite counts as one failed check.
    for (void (*suite)() : {schema_and_catalog_checks, list_case_checks, storage_checks}) {
        try {
            suite();
        } catch (const std::exception& e) {
            check(false, (std::string("unexpected error: ") + e.what()).c_str());
        }
    }
    return failures;
}
