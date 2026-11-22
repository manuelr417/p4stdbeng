// ex13 — index module checks: the key codec, the three page views, the
// block-0 identification record, bulk build, lookup and range (with
// duplicates spanning leaves), insert with overflow chaining, the verify
// pass, and close/reopen persistence.

#include "bufpool/BufferManager.h"
#include "bufpool/PinnedPage.h"
#include "check_support.h"
#include "index/IndexPages.h"
#include "index/IsamIndex.h"
#include "index/KeyCodec.h"
#include "policy/LRUPolicy.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unistd.h>
#include <utility>
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

std::string temp_path(const std::string& tag) {
    return "/tmp/ex13-index-" + std::to_string(getpid()) + "-" + tag + ".idx";
}

const char* kPersonJson =
    "{\"table_name\":\"person\",\"columns\":["
    "{\"column_name\":\"pid\",\"column_type\":\"Integer\",\"size\":4},"
    "{\"column_name\":\"name\",\"column_type\":\"String\",\"size\":10},"
    "{\"column_name\":\"age\",\"column_type\":\"Integer\",\"size\":4},"
    "{\"column_name\":\"city\",\"column_type\":\"String\",\"size\":3}]}";

std::vector<std::pair<tuple::Column, isam::Rid>> int_entries(int first, int last) {
    std::vector<std::pair<tuple::Column, isam::Rid>> entries;
    for (int value = first; value <= last; ++value) {
        entries.emplace_back(tuple::Column(value),
                             isam::Rid{static_cast<std::uint32_t>(value),
                                       static_cast<std::uint32_t>(value)});
    }
    return entries;
}

std::vector<std::pair<tuple::Column, isam::Rid>> repeated_int_entries(int value,
                                                                      std::size_t count) {
    std::vector<std::pair<tuple::Column, isam::Rid>> entries;
    for (std::size_t i = 0; i < count; ++i) {
        entries.emplace_back(tuple::Column(value), isam::Rid{static_cast<std::uint32_t>(i),
                                                             static_cast<std::uint32_t>(i)});
    }
    return entries;
}

// Reads one block straight from disk through a short-lived file handle,
// so before/after comparisons see flushed bytes, not pooled ones.
std::string disk_block(bufman::BufferManager& manager, const std::string& path,
                       std::uint32_t block) {
    bufman::File file = manager.open_file(path);
    std::string bytes;
    {
        const bufman::PinnedPage page(manager, file, block);
        bytes.assign(page.data(), bufman::kPageSize);
    }
    manager.close_file(file);
    return bytes;
}

std::optional<isam::RangeBound> bound(int value, bool inclusive = true) {
    return isam::RangeBound{tuple::Column(value), inclusive};
}

void run() {
    const schema::TableSchema person = schema::parse_table_schema(kPersonJson);
    check(person.column_count() == 4, "person schema loads for the index checks");
    bufman::BufferManager manager;
    manager.init(16, std::make_unique<bufman::LRUPolicy>());
    const auto all_occupied = [](const isam::Rid&) { return true; };
    const auto verifies_clean = [&](isam::IsamIndex& idx, std::size_t walked) {
        return ok([&] {
            const idx::VerifyResult result = idx.verify(all_occupied);
            return result.findings.empty() && result.entry_count == walked;
        });
    };

    // --- 4.1. the key codec ---
    {
        const isam::KeyCodec int_codec(tuple::ColumnType::Integer, 0);
        check(int_codec.width() == 4, "int key width is 4");
        const isam::KeyCodec double_codec(tuple::ColumnType::Double, 0);
        check(double_codec.width() == 8, "double key width is 8");
        const isam::KeyCodec string_codec(tuple::ColumnType::String, 10);
        check(string_codec.width() == 10, "string key width is the declared width");

        check(ok([&] {
                  return int_codec.decode(int_codec.encode(tuple::Column(-42)).data()) ==
                         tuple::Column(-42);
              }),
              "int key encodes and decodes");
        check(ok([&] {
                  return std::get<double>(
                             double_codec.decode(double_codec.encode(tuple::Column(2.5)).data())
                                 .column_value()) == 2.5;
              }),
              "double key encodes and decodes");
        std::string ruth;
        check(ok([&] { ruth = string_codec.encode(tuple::Column(std::string("Ruth"))); }),
              "string key encodes");
        check(ruth == std::string("Ruth\0\0\0\0\0\0", 10),
              "string key is NUL-padded to the declared width");
        check(std::get<std::string>(string_codec.decode(ruth.data()).column_value()) == "Ruth",
              "string key decodes");

        check(fails_with([&] { string_codec.encode(tuple::Column(42)); }),
              "a non-string value refuses to encode as a string key");
        check(fails_with([&] { string_codec.encode(tuple::Column(std::string("0123456789A"))); }),
              "a too-wide string refuses to encode as a key");
        check(fails_with([&] { int_codec.encode(tuple::Column(2.5)); }),
              "a double refuses to encode as an int key");

        const auto int_key = [&](int value) { return int_codec.encode(tuple::Column(value)); };
        check(int_codec.compare(int_key(-5).data(), int_key(3).data()) < 0,
              "int key order is numeric across zero");
        check(int_codec.compare(int_key(3).data(), int_key(3).data()) == 0,
              "equal int keys compare equal");
        const auto str_key = [&](const std::string& value) {
            return string_codec.encode(tuple::Column(value));
        };
        check(string_codec.compare(str_key("ab").data(), str_key("abc").data()) < 0,
              "shorter prefix sorts first");
        check(string_codec.compare(str_key("b").data(), str_key("abc").data()) > 0,
              "lexicographic byte order");
        check(string_codec.compare(str_key("abc").data(), str_key("abc").data()) == 0,
              "NUL padding does not disturb equality");
    }

    // --- 4.1. page view capacities and entry machinery ---
    {
        check(isam::leaf_capacity(4) == 340, "int-key leaf capacity is 340");
        check(isam::separator_capacity(4) == 511, "int-key separator capacity is 511");
        check(isam::overflow_capacity(4) == 340, "int-key overflow capacity is 340");
        check(isam::leaf_capacity(10) == (4096 - 12) / 18,
              "10-byte string key leaf capacity follows the formula");

        static char page[bufman::kPageSize];
        const isam::KeyCodec codec(tuple::ColumnType::Integer, 0);
        const auto encode_int = [&](int value) { return codec.encode(tuple::Column(value)); };
        isam::LeafPage leaf;
        leaf.bind(page, 4);
        leaf.format(isam::kNoPage, isam::kNoPage);
        check(leaf.count() == 0 && ok([&] { leaf.attach(); }), "a formatted leaf page attaches");
        for (const int value : {5, 1, 9, 1, 7, 3}) {
            leaf.insert_entry(isam::DataEntry{encode_int(value), isam::Rid{9, 9}}, codec);
        }
        check(leaf.count() == 6, "six out-of-order inserts all land");
        bool sorted = true;
        for (std::size_t i = 1; i < leaf.count(); ++i) {
            sorted = sorted &&
                     codec.compare(leaf.get_entry(i - 1).key.data(), leaf.get_entry(i).key.data()) <= 0;
        }
        check(sorted, "entry inserts keep the page sorted");
        check(leaf.lower_bound(encode_int(4), codec) == 3, "lower_bound finds the first key >= 4");
        check(leaf.lower_bound(encode_int(0), codec) == 0, "lower_bound at the front");
        check(leaf.lower_bound(encode_int(9), codec) == 5,
              "lower_bound on an existing key lands on it");
        check(leaf.lower_bound(encode_int(10), codec) == 6, "lower_bound past the end is the count");
        while (!leaf.full()) {
            leaf.insert_entry(isam::DataEntry{encode_int(2000), isam::Rid{1, 1}}, codec);
        }
        check(leaf.count() == leaf.capacity(), "a leaf fills to its capacity");

        isam::OverflowPage overflow;
        overflow.bind(page, 4);
        overflow.format(77);
        check(ok([&] { overflow.attach(); }) && overflow.next_overflow() == 77 &&
                  overflow.count() == 0,
              "a formatted overflow page carries its chain link");

        isam::SeparatorPage separator;
        separator.bind(page, 4);
        separator.format();
        check(ok([&] { separator.attach(); }) && separator.count() == 0,
              "a formatted separator page attaches");
        separator.append(encode_int(10).data(), 11);
        separator.append(encode_int(20).data(), 21);
        separator.append(encode_int(30).data(), 31);
        check(separator.count() == 3, "separator appends land");
        separator.set_first_child(1);
        check(separator.first_child() == 1, "first child is settable");
        check(separator.entry(0).child == 11 && separator.entry(0).key == encode_int(10),
              "entry(0) reads back the first index entry");
        check(separator.entry(2).child == 31 && separator.entry(2).key == encode_int(30),
              "entry(2) reads back the last index entry");
        check(separator.descend_step(encode_int(5).data(), codec) == 1,
              "below every separator routes to the first child");
        check(separator.descend_step(encode_int(10).data(), codec) == 1,
              "equal to a separator still routes left of it");
        check(separator.descend_step(encode_int(15).data(), codec) == 11,
              "between separators routes past the smaller one");
        check(separator.descend_step(encode_int(35).data(), codec) == 31,
              "past every separator routes to the last child");
        isam::SeparatorPage fresh;
        fresh.bind(page, 4);
        fresh.format();
        check(fresh.first_child() == isam::kNoPage,
              "a formatted separator's first child is the no-page marker");
    }

    // --- 4.2. the header block: identification on open ---
    {
        const std::string path = temp_path("header");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        check(ok([&] {
                  idx.create(path);
                  return idx.is_open() && idx.entry_count() == 0;
              }),
              "create yields an open, empty index");
        check(ok([&] {
                  idx.close();
                  idx.open(path);
              }),
              "an index file reopens after close");

        const schema::TableSchema car = schema::parse_table_schema(
            "{\"table_name\":\"car\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"Integer\",\"size\":4},"
            "{\"column_name\":\"brand\",\"column_type\":\"String\",\"size\":10}]}");
        isam::IsamIndex wrong_table(manager, car, 0);
        check(fails_with([&] { wrong_table.open(path); }, "table 'person'"),
              "opening under a different table is refused by name");

        // Same key type and width, different column name: the type and
        // width checks pass, so the refusal must come from the name.
        const schema::TableSchema two_int = schema::parse_table_schema(
            "{\"table_name\":\"person\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"Integer\",\"size\":4},"
            "{\"column_name\":\"age\",\"column_type\":\"Integer\",\"size\":4}]}");
        isam::IsamIndex wrong_column(manager, two_int, 1);
        check(fails_with([&] { wrong_column.open(path); }, "column 'pid'"),
              "opening under a different column is refused by name");

        const schema::TableSchema string_pid = schema::parse_table_schema(
            "{\"table_name\":\"person\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"String\",\"size\":4},"
            "{\"column_name\":\"name\",\"column_type\":\"String\",\"size\":10}]}");
        isam::IsamIndex wrong_type(manager, string_pid, 0);
        check(fails_with([&] { wrong_type.open(path); }, "key type differs"),
              "opening under a different key type is refused");

        const std::string foreign = temp_path("foreign");
        {
            std::ofstream out(foreign, std::ios::binary | std::ios::trunc);
            // Exactly one block of text: the header block exists, its
            // magic does not.
            out << std::string(4096, 'x');
        }
        isam::IsamIndex foreign_idx(manager, person, 0);
        check(fails_with([&] { foreign_idx.open(foreign); }, "not an index file"),
              "a foreign file is refused by magic");
        std::remove(foreign.c_str());
        idx.close();
        std::remove(path.c_str());
    }

    // --- 4.3. bulk build ---
    {
        const std::string path = temp_path("build");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        std::vector<std::pair<tuple::Column, isam::Rid>> entries = int_entries(1, 1000);
        std::reverse(entries.begin(), entries.end());  // build must sort
        check(ok([&] { idx.build(path, entries); }), "build over 1000 reversed entries");
        check(idx.entry_count() == 1000, "the header reports 1000 entries");
        check(ok([&] {
                  for (int value = 1; value <= 1000; ++value) {
                      const std::vector<isam::Rid> out = idx.lookup(tuple::Column(value));
                      const auto id = static_cast<std::uint32_t>(value);
                      if (out.size() != 1 || !(out[0] == isam::Rid{id, id})) {
                          return false;
                      }
                  }
                  return true;
              }),
              "every built key is found with its exact rid");
        check(ok([&] { return idx.lookup(tuple::Column(5000)).empty(); }),
              "a missing key answers empty");
        idx.close();

        const std::string empty_path = temp_path("empty");
        std::remove(empty_path.c_str());
        isam::IsamIndex empty_idx(manager, person, 0);
        check(ok([&] { empty_idx.build(empty_path, {}); }), "building an empty index succeeds");
        check(empty_idx.entry_count() == 0, "the empty index holds no entries");
        check(ok([&] { return empty_idx.lookup(tuple::Column(1)).empty(); }),
              "lookups on an empty index answer empty");
        check(ok([&] { empty_idx.insert(tuple::Column(42), isam::Rid{1, 1}); }),
              "the first insert grows the empty index's first leaf");
        check(ok([&] { return empty_idx.lookup(tuple::Column(42)).size() == 1; }),
              "the inserted key is findable");
        empty_idx.close();
        std::remove(empty_path.c_str());

        const std::string string_path = temp_path("strings");
        std::remove(string_path.c_str());
        isam::IsamIndex name_idx(manager, person, 1);
        std::vector<std::pair<tuple::Column, isam::Rid>> names;
        for (int i = 0; i < 50; ++i) {
            char text[16];
            std::snprintf(text, sizeof(text), "name%03d", i);
            names.emplace_back(tuple::Column(std::string(text)),
                               isam::Rid{static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i)});
        }
        check(ok([&] { name_idx.build(string_path, names); }), "a string-key index builds");
        check(ok([&] {
                  const std::vector<isam::Rid> found =
                      name_idx.lookup(tuple::Column(std::string("name007")));
                  return found.size() == 1 && found[0] == isam::Rid{7, 7};
              }),
              "a string key is found");
        name_idx.close();
        std::remove(string_path.c_str());
        std::remove(path.c_str());
    }

    // --- 4.4. duplicates spanning leaves, and ranges ---
    {
        const std::string path = temp_path("dups");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        // 400 equal keys: 340 fill the first leaf, 60 spill into the
        // second — duplicates spanning a leaf boundary.
        check(ok([&] { idx.build(path, repeated_int_entries(5, 400)); }),
              "a build of 400 equal keys succeeds");
        check(ok([&] { return idx.lookup(tuple::Column(5)).size() == 400; }),
              "a lookup returns every duplicate across both leaves");
        check(ok([&] { return idx.range(bound(5), bound(5)).size() == 400; }),
              "a point range returns every duplicate");
        check(ok([&] { idx.insert(tuple::Column(5), isam::Rid{777, 0}); }),
              "another equal key inserts");
        check(ok([&] { return idx.lookup(tuple::Column(5)).size() == 401; }),
              "the inserted duplicate is found with the others");
        idx.close();
        std::remove(path.c_str());

        const std::string range_path = temp_path("range");
        std::remove(range_path.c_str());
        isam::IsamIndex ridx(manager, person, 0);
        check(ok([&] { ridx.build(range_path, int_entries(1, 1000)); }), "range index built");
        {
            const std::vector<isam::Rid> out = ridx.range(bound(10), bound(19));
            bool ascending = out.size() == 10;
            for (std::size_t i = 0; ascending && i < out.size(); ++i) {
                ascending = out[i].page_id == 10 + i;
            }
            check(ascending, "an inclusive range is ascending and complete");
        }
        {
            const std::vector<isam::Rid> out = ridx.range(bound(10, false), bound(19));
            check(out.size() == 9 && out.front().page_id == 11,
                  "an exclusive lower bound skips its key");
        }
        {
            const std::vector<isam::Rid> out = ridx.range(bound(10), bound(19, false));
            check(out.size() == 9 && out.back().page_id == 18,
                  "an exclusive upper bound skips its key");
        }
        check(ridx.range(bound(2000), bound(3000)).empty(), "a range past every key answers empty");
        check(ridx.range(bound(-1), bound(-1)).empty(), "a range below every key answers empty");
        ridx.close();
        std::remove(range_path.c_str());
    }

    // --- 4.5. insert, overflow chaining, frozen separators ---
    {
        const std::string path = temp_path("ovf");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        check(ok([&] { idx.build(path, int_entries(0, 339)); }),
              "a build filling exactly one leaf succeeds");
        check(idx.entry_count() == 340, "the full leaf holds 340 entries");
        check(ok([&] { idx.insert(tuple::Column(-1), isam::Rid{400, 0}); }),
              "an insert into the full leaf spills to overflow");
        check(ok([&] { idx.insert(tuple::Column(-2), isam::Rid{401, 0}); }),
              "a second spill joins the overflow page");
        check(ok([&] {
                  const std::vector<isam::Rid> out = idx.lookup(tuple::Column(-1));
                  return out.size() == 1 && out[0] == isam::Rid{400, 0};
              }),
              "a spilled key is findable");
        check(ok([&] { return idx.lookup(tuple::Column(339)).size() == 1; }),
              "leaf keys still answer after spills");
        check(ok([&] { return idx.range(bound(-2), bound(339)).size() == 342; }),
              "a range covering leaf and overflow returns all 342 in order");
        check(idx.entry_count() == 342, "the header counts spilled entries");
        idx.close();
        std::remove(path.c_str());

        // Separator pages never change: same index shape, byte compare.
        const std::string sep_path = temp_path("sep");
        std::remove(sep_path.c_str());
        isam::IsamIndex sidx(manager, person, 0);
        check(ok([&] { sidx.build(sep_path, int_entries(1, 400)); }),
              "a two-leaf build with one separator level succeeds");
        manager.flush_all();
        const std::string before = disk_block(manager, sep_path, 3);
        check(before.size() == bufman::kPageSize, "the separator block is readable from disk");
        check(ok([&] {
                  sidx.insert(tuple::Column(-1), isam::Rid{500, 0});
                  sidx.insert(tuple::Column(341), isam::Rid{501, 0});
              }),
              "two inserts land (one spilling, one in the open leaf)");
        manager.flush_all();
        check(before == disk_block(manager, sep_path, 3),
              "separator pages are byte-identical across inserts");
        sidx.close();
        std::remove(sep_path.c_str());
    }

    // --- 4.6. the verify pass ---
    {
        const std::string path = temp_path("verify");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        check(ok([&] { idx.build(path, int_entries(0, 339)); }), "verify index built");
        check(verifies_clean(idx, 340), "a clean index verifies with no findings");
        idx.close();

        // Corrupt entry 0's rid on disk: it must be named, key and rid.
        check(ok([&] {
                  bufman::File file = manager.open_file(path);
                  {
                      bufman::PinnedPage leaf(manager, file, 1);
                      const std::uint32_t bogus = 9999;
                      std::memcpy(leaf.data() + 12 + 4, &bogus, 4);
                      std::memcpy(leaf.data() + 12 + 8, &bogus, 4);
                      leaf.mark_dirty();
                  }
                  manager.close_file(file);
              }),
              "a leaf entry's rid corrupted on disk");
        check(ok([&] { idx.open(path); }), "the corrupted index reopens");
        check(ok([&] {
                  const idx::VerifyResult result =
                      idx.verify([](const isam::Rid& rid) { return rid.page_id < 1000; });
                  return result.findings.size() == 1 &&
                         result.findings[0].find("dangling") != std::string::npos &&
                         result.findings[0].find("(9999,9999)") != std::string::npos;
              }),
              "a dangling entry is named by rid");
        idx.close();
        std::remove(path.c_str());
    }

    // --- 4.7. persistence across close and reopen ---
    {
        const std::string path = temp_path("persist");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        check(ok([&] { idx.build(path, int_entries(1, 1000)); }), "persist index built");
        const std::vector<isam::Rid> before = idx.lookup(tuple::Column(500));
        check(before.size() == 1, "key 500 found before the close");
        const std::vector<isam::Rid> range_before = idx.range(bound(10), bound(19));
        check(range_before.size() == 10, "range answered before the close");
        check(ok([&] {
                  idx.close();
                  idx.open(path);
              }),
              "the index closes and reopens");
        check(idx.lookup(tuple::Column(500)) == before, "lookups answer identically after reopen");
        check(idx.range(bound(10), bound(19)) == range_before,
              "ranges answer identically after reopen");
        check(idx.entry_count() == 1000, "the reopened header still reports 1000 entries");
        idx.close();
        std::remove(path.c_str());
    }

    // --- 4.8. erase: exact entries removed in place ---
    {
        const std::string path = temp_path("erase");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        check(ok([&] { idx.build(path, int_entries(1, 400)); }), "erase: a two-leaf index builds");

        // Erase a middle key of the first leaf: every path agrees.
        check(ok([&] { idx.erase(tuple::Column(200), isam::Rid{200, 200}); }),
              "erase: a present (key, rid) pair erases");
        check(idx.lookup(tuple::Column(200)).empty(), "erase: the erased key answers nothing");
        check(idx.lookup(tuple::Column(199)) == std::vector<isam::Rid>{{199, 199}},
              "erase: the key before it still answers");
        check(idx.entry_count() == 399, "erase: the header count dropped by exactly one");
        check(idx.range(bound(198), bound(202)).size() == 4,
              "erase: a range covering the gap skips only the erased row");
        check(verifies_clean(idx, 399), "erase: the index verifies clean afterwards");

        // Absent pairs are refused by name, changing nothing.
        check(fails_with([&] { idx.erase(tuple::Column(200), isam::Rid{200, 200}); }),
              "erase: an absent pair is refused");
        check(fails_with([&] { idx.erase(tuple::Column(201), isam::Rid{999, 999}); }),
              "erase: a wrong rid for a present key is refused");
        check(idx.entry_count() == 399, "erase: refusals leave the count alone");

        // Separators and the leaf chain are byte-identical around erases.
        const auto leaf1_link = [&] {
            std::uint32_t link = 0;
            std::memcpy(&link, disk_block(manager, path, 1).data() + 4, 4);
            return link;
        };
        manager.flush_all();
        const std::string sep_before = disk_block(manager, path, 3);
        const std::uint32_t link_before = leaf1_link();
        check(ok([&] {
                  idx.erase(tuple::Column(1), isam::Rid{1, 1});
                  idx.erase(tuple::Column(400), isam::Rid{400, 400});
              }),
              "erase: entries leave both leaves");
        manager.flush_all();
        check(sep_before == disk_block(manager, path, 3),
              "erase: separators are byte-identical across erases");
        check(link_before == leaf1_link(), "erase: the leaf chain link is unchanged");
        check(ok([&] {
                  idx.close();
                  idx.open(path);
              }),
              "erase: the index reopens after erases");
        check(idx.lookup(tuple::Column(200)).empty() && idx.lookup(tuple::Column(2)).size() == 1,
              "erase: the erases survived the reopen");
        idx.close();
        std::remove(path.c_str());

        // An emptied overflow page stays in its chain and its room is
        // refillable: a full leaf spills twice, both spills erase, and the
        // next insert lands in the emptied page.
        const std::string ovf_path = temp_path("erase-ovf");
        std::remove(ovf_path.c_str());
        isam::IsamIndex oidx(manager, person, 0);
        check(ok([&] { oidx.build(ovf_path, int_entries(0, 339)); }),
              "erase: a one-leaf index builds for the overflow case");
        check(ok([&] {
                  oidx.insert(tuple::Column(-1), isam::Rid{700, 0});
                  oidx.insert(tuple::Column(-2), isam::Rid{701, 0});
              }),
              "erase: two entries spilled to overflow");
        check(ok([&] {
                  oidx.erase(tuple::Column(-1), isam::Rid{700, 0});
                  oidx.erase(tuple::Column(-2), isam::Rid{701, 0});
              }),
              "erase: both spilled entries erased");
        check(oidx.entry_count() == 340, "erase: the count is back to the leaf's 340");
        check(verifies_clean(oidx, 340), "erase: the emptied chain verifies clean");
        check(ok([&] { oidx.insert(tuple::Column(-3), isam::Rid{702, 0}); }),
              "erase: a later insert spills again");
        check(oidx.lookup(tuple::Column(-3)) == std::vector<isam::Rid>{{702, 0}},
              "erase: the refilled chain answers");
        oidx.close();
        std::remove(ovf_path.c_str());

        // A randomized insert/erase sequence against a shadow model, with
        // verification demanded clean after every step.
        const std::string rnd_path = temp_path("erase-rnd");
        std::remove(rnd_path.c_str());
        isam::IsamIndex ridx(manager, person, 0);
        check(ok([&] { ridx.build(rnd_path, {}); }),
              "erase: an empty index builds for the random sequence");
        std::map<int, std::vector<isam::Rid>> shadow;
        std::mt19937 rng(7);
        int next_rid = 0;
        const bool sequence_ok = ok([&] {
            for (int step = 0; step < 400; ++step) {
                if (rng() % 100 < 60 || shadow.empty()) {
                    const int key = static_cast<int>(rng() % 200) - 100;
                    const isam::Rid rid{static_cast<std::uint32_t>(next_rid),
                                        static_cast<std::uint32_t>(next_rid + 1)};
                    ++next_rid;
                    ridx.insert(tuple::Column(key), rid);
                    shadow[key].push_back(rid);
                } else {
                    auto it = shadow.begin();
                    std::advance(it, rng() % shadow.size());
                    auto& rids = it->second;
                    const isam::Rid victim = rids[rng() % rids.size()];
                    ridx.erase(tuple::Column(it->first), victim);
                    rids.erase(std::find(rids.begin(), rids.end(), victim));
                    if (rids.empty()) {
                        shadow.erase(it);
                    }
                }
                if (!verifies_clean(ridx, ridx.entry_count())) {
                    return false;
                }
            }
            std::size_t shadow_total = 0;
            for (const auto& [key, rids] : shadow) {
                shadow_total += rids.size();
                if (ridx.lookup(tuple::Column(key)).size() != rids.size()) {
                    return false;
                }
            }
            return shadow_total == ridx.entry_count();
        });
        check(sequence_ok, "erase: 400 random operations keep the index exactly the shadow");
        ridx.close();
        std::remove(rnd_path.c_str());
    }
}

}

int run_index_checks() {
    try {
        run();
    } catch (const std::exception& e) {
        check(false, std::string("unexpected error: ") + e.what());
    }
    std::cout << (failures == 0 ? "index suite: all checks passed."
                                : "index suite: " + std::to_string(failures) + " check(s) FAILED.")
              << "\n";
    return failures;
}
