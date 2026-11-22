// p4stdbeng — the loader suite: the TUP2 bulk loader's fill/link read-back,
// exact-division generation order, partial-tail placement, refusal rules,
// generator-failure naming, empty and zero-record loads, the differential
// against per-row insertion, and the timing contract.

#include "bufpool/BufferManager.h"
#include "check_support.h"
#include "file/BlockFile.h"
#include "heapfile/HeapFile.h"
#include "policy/LRUPolicy.h"
#include "schema/TableGenerator.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "schema/TupleSerializer.h"
#include "tools/BulkLoader.h"
#include "tuple/Tuple.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using checks::fails_with;
using checks::ok;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cout << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string temp_path(const char* suffix) {
    return std::string("/tmp/p3-load-") + std::to_string(getpid()) + "-" + suffix + ".bin";
}

// A wide-record schema: sid(4) + pad(900) = 904 bytes, 4 slots per page —
// the same fixture the storage suite uses, so page math stays familiar.
schema::TableSchema wide_schema(const std::string& table_name = "bulkcase") {
    return schema::parse_table_schema(
        R"({"table_name": ")" + table_name + R"(",
            "columns": [
              {"column_name": "sid", "column_type": "Integer", "size": 4},
              {"column_name": "pad", "column_type": "String",  "size": 900}]})");
}

tuple::Tuple wide_row(int id) {
    tuple::Tuple row;
    row.add(id);
    row.add(std::string(900, static_cast<char>('a' + id % 26)));
    return row;
}

// Generates wide rows numbered from `first`.
tools::RowGenerator rows_from(int first) {
    return [first](std::size_t index) { return wide_row(static_cast<int>(index) + first); };
}

// A generator the load must never call.
tuple::Tuple never_called(std::size_t) {
    throw common::Error("the generator was called");
}

// The ids a scan reports, in scan order.
std::vector<int> scan_ids(heapfile::HeapFile& file) {
    std::vector<int> ids;
    heapfile::HeapFileIterator it = file.scan();
    while (const std::optional<tuple::Tuple> row = it.next()) {
        ids.push_back(std::get<int>(row->column(0).column_value()));
    }
    return ids;
}

// Opens `path` through a fresh manager and hands the open file to `body`.
template <class F>
bool with_open_file(const std::string& path, const schema::TableSchema& schema, F&& body) {
    bufman::BufferManager manager;
    manager.init(8, std::make_unique<bufman::LRUPolicy>());
    heapfile::HeapFile file(manager);
    const bool result = ok([&] {
        file.open(path, schema);
        const bool passed = body(file);
        file.close();
        return passed;
    });
    manager.close_all();
    return result;
}

void create_empty(const std::string& path, const schema::TableSchema& schema) {
    bufman::BufferManager manager;
    manager.init(4, std::make_unique<bufman::LRUPolicy>());
    heapfile::HeapFile(manager).create(path, schema);
    manager.close_all();
}

void run() {
    const schema::TableSchema schema = wide_schema();

    // --- fill, link, and read back through the engine ---
    {
        const std::string path = temp_path("fill");
        std::remove(path.c_str());
        check(ok([&] {
                  const tools::BulkLoadStats stats = tools::bulk_load(path, schema, 250, rows_from(1));
                  return stats.records == 250 && stats.pages == 63 && stats.per_page_capacity == 4;
              }),
              "250 rows fill 63 pages at 4 slots (62 full, 1 partial)");
        check(with_open_file(path, schema,
                             [](heapfile::HeapFile& file) {
                                 file.check_structure();
                                 return true;
                             }),
              "the loaded file opens and passes the structural self-check");
        check(with_open_file(path, schema,
                             [](heapfile::HeapFile& file) {
                                 std::vector<int> ids = scan_ids(file);
                                 std::sort(ids.begin(), ids.end());
                                 std::vector<int> expected(250);
                                 for (int i = 0; i < 250; ++i) {
                                     expected[i] = i + 1;
                                 }
                                 return ids == expected;
                             }),
              "every loaded record scans back exactly once (order per the\n"
              "       heap's two-list rule: the partial tail page scans first)");
        check(with_open_file(path, schema,
                             [](heapfile::HeapFile& file) {
                                 const tuple::Tuple fetched = file.find(heapfile::RowId{2, 1});
                                 return std::get<int>(fetched.column(0).column_value()) == 6;
                             }),
              "a reported row id resolves to its record");
        std::remove(path.c_str());
    }

    // --- exact division: generation order across the whole file ---
    {
        const std::string path = temp_path("exact");
        std::remove(path.c_str());
        check(ok([&] { return tools::bulk_load(path, schema, 12, rows_from(1)).pages == 3; }),
              "12 rows fill exactly 3 pages");
        check(with_open_file(path, schema,
                             [](heapfile::HeapFile& file) {
                                 file.check_structure();
                                 return scan_ids(file) ==
                                        std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
                             }),
              "an exact-division load is consistent and scans in generation order");
        std::remove(path.c_str());
    }

    // --- the partial tail page sits on the free list ---
    {
        const std::string path = temp_path("partial");
        std::remove(path.c_str());
        check(ok([&] { return tools::bulk_load(path, schema, 10, rows_from(1)).pages == 3; }),
              "10 rows fill 2 pages plus a 2-record tail");
        // Raw header read: free list holds page 3, full list 1 -> 2.
        std::array<char, bufman::kBlockSize> header{};
        check(ok([&] {
                  bufman::File raw = bufman::open_for_read(path);
                  bufman::read_block(raw, 0, header);
                  bufman::close(raw);
              }),
              "the header page read back");
        const auto u32 = [&](std::size_t offset) {
            std::uint32_t value = 0;
            std::memcpy(&value, header.data() + offset, 4);
            return value;
        };
        check(u32(12) == 1 && u32(16) == 2, "the full list is 1 -> 2 in generation order");
        check(u32(4) == 3 && u32(8) == 3, "the partial tail page is the free list's only element");
        check(u32(20) == 4, "next_page_id covers every written page");
        // The engine's scan-order rule: the free list first, then the
        // full list — the tail page's rows come first here.
        check(with_open_file(path, schema,
                             [](heapfile::HeapFile& file) {
                                 file.check_structure();
                                 const std::vector<int> ids = scan_ids(file);
                                 return ids.size() == 10 && ids[0] == 9 && ids[1] == 10;
                             }),
              "the partial tail page's rows scan first, as the heap scans");
        // A later insert lands in the freed-tail page without growing.
        check(with_open_file(path, schema,
                             [](heapfile::HeapFile& file) {
                                 const heapfile::RowId rid = file.insert(wide_row(11));
                                 file.check_structure();
                                 return rid.page_id == 3;
                             }),
              "a later insert reuses the partial tail page, consistently");
        std::remove(path.c_str());
    }

    // --- refusals ---
    {
        // A non-empty file is refused, content unchanged.
        const std::string path = temp_path("refuse-full");
        std::remove(path.c_str());
        check(ok([&] { tools::bulk_load(path, schema, 4, rows_from(1)); }),
              "the refusal fixture loads once");
        check(fails_with([&] { tools::bulk_load(path, schema, 4, rows_from(100)); }),
              "a non-empty file is refused");
        check(with_open_file(path, schema,
                             [](heapfile::HeapFile& file) {
                                 const std::vector<int> ids = scan_ids(file);
                                 return ids.size() == 4 && ids[0] == 1;
                             }),
              "the refused file's records are unchanged");
        std::remove(path.c_str());

        // A foreign one-block header is refused by name.
        const std::string foreign = temp_path("refuse-foreign");
        std::remove(foreign.c_str());
        check(ok([&] {
                  bufman::File raw = bufman::open_for_append(foreign);
                  std::array<char, bufman::kBlockSize> block{};
                  std::memcpy(block.data(), "TUP1", 4);
                  bufman::write_block(raw, 0, block.data());
                  bufman::close(raw);
              }),
              "a TUP1 magic written");
        check(fails_with([&] { tools::bulk_load(foreign, schema, 4, never_called); },
                         "not a tuple heap file"),
              "a foreign header is refused by name");
        std::remove(foreign.c_str());

        // A wrong-table header is refused by name.
        const std::string wrong = temp_path("refuse-wrong");
        std::remove(wrong.c_str());
        check(ok([&] { create_empty(wrong, wide_schema("other")); }),
              "the other table's empty file created");
        check(fails_with([&] { tools::bulk_load(wrong, schema, 4, never_called); }, "other"),
              "a wrong-table header is refused by name");
        std::remove(wrong.c_str());

        // No record layout: refused.
        schema::TableSchema loose("loose");
        loose.add_column("a", tuple::ColumnType::Integer);
        loose.add_column("s", tuple::ColumnType::String);
        check(fails_with([&] { tools::bulk_load(temp_path("refuse-loose"), loose, 4, never_called); },
                         "no record layout"),
              "a schema without record layout is refused");
    }

    // --- generator failure surfaces the index ---
    {
        const std::string path = temp_path("genfail");
        std::remove(path.c_str());
        check(fails_with(
                  [&] {
                      tools::bulk_load(path, schema, 10, [](std::size_t index) {
                          if (index == 7) {
                              throw common::Error("no more rows");
                          }
                          return wide_row(static_cast<int>(index) + 1);
                      });
                  },
                  "row index 7"),
              "a generator failure names the row index");
        std::remove(path.c_str());
    }

    // --- empty and zero-record loads ---
    {
        // A zero-record load into a missing file: a valid empty heap.
        const std::string zero = temp_path("zero");
        std::remove(zero.c_str());
        check(ok([&] {
                  const tools::BulkLoadStats stats = tools::bulk_load(zero, schema, 0, never_called);
                  return stats.records == 0 && stats.pages == 0;
              }),
              "a zero-record load succeeds with no pages");
        check(with_open_file(zero, schema,
                             [](heapfile::HeapFile& file) {
                                 file.check_structure();
                                 return scan_ids(file).empty();
                             }),
              "the zero-record file opens as a valid empty heap and scans to nothing");
        std::remove(zero.c_str());

        // A zero-record load into an existing empty (created) file: reused.
        const std::string reused = temp_path("reused");
        std::remove(reused.c_str());
        check(ok([&] { create_empty(reused, schema); }), "the to-be-reused file created");
        check(ok([&] { tools::bulk_load(reused, schema, 0, never_called); }),
              "an existing empty file is reused for a zero-record load");
        std::remove(reused.c_str());
    }

    // --- the differential: tool vs per-row insertion, byte-level twins ---
    {
        const std::string tool_path = temp_path("twin-tool");
        const std::string row_path = temp_path("twin-row");
        std::remove(tool_path.c_str());
        std::remove(row_path.c_str());
        const std::size_t count = 30; // 8 pages: full pages + a partial tail
        check(ok([&] { tools::bulk_load(tool_path, schema, count, rows_from(1)); }),
              "the tool-loaded twin loaded");
        check(ok([&] {
                  create_empty(row_path, schema);
                  return with_open_file(row_path, schema, [&](heapfile::HeapFile& file) {
                      for (std::size_t i = 0; i < count; ++i) {
                          file.insert(wide_row(static_cast<int>(i) + 1));
                      }
                      file.check_structure();
                      return true;
                  });
              }),
              "the per-row twin is inserted row by row and consistent");
        // Both files, record for record: identical layout means identical
        // scans and identical row ids.
        bufman::BufferManager manager;
        manager.init(8, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile tool_file(manager);
        heapfile::HeapFile row_file(manager);
        check(ok([&] {
                  tool_file.open(tool_path, schema);
                  row_file.open(row_path, schema);
                  bool twins_match = scan_ids(tool_file) == scan_ids(row_file);
                  for (std::size_t i = 0; i < count; ++i) {
                      const heapfile::RowId rid{static_cast<std::uint32_t>(i / 4 + 1),
                                                static_cast<std::uint32_t>(i % 4)};
                      twins_match = twins_match && tool_file.find(rid) == row_file.find(rid);
                  }
                  tool_file.close();
                  row_file.close();
                  return twins_match;
              }),
              "tool-loaded and per-row-loaded files are engine-equivalent");
        manager.close_all();
        std::remove(tool_path.c_str());
        std::remove(row_path.c_str());
    }
}

}  // namespace

int run_loader_checks() {
    try {
        run();
    } catch (const std::exception& e) {
        check(false, std::string("unexpected error: ") + e.what());
    }
    std::cout << (failures == 0 ? "loader suite: all checks passed."
                                : "loader suite: " + std::to_string(failures) + " check(s) FAILED.")
              << "\n";
    return failures;
}
