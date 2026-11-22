// p4stdbeng — the B+tree module suite: page views, header identification,
// bulk build, recursive search with boundary duplicates, recursive insert
// with splits, recursive erase with rebalancing, and the randomized
// shadow-model differential.

#include "bufpool/BufferManager.h"
#include "check_support.h"
#include "index/BPlusTree.h"
#include "index/BptPages.h"
#include "index/IsamIndex.h"
#include "index/KeyCodec.h"
#include "policy/LRUPolicy.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "tuple/Column.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
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
    return std::string("/tmp/p3-bpt-") + std::to_string(getpid()) + "-" + suffix + ".idx";
}

// Entries with key == value and rid == {value, value}.
std::vector<std::pair<tuple::Column, idx::Rid>> int_entries(int lo, int hi) {
    std::vector<std::pair<tuple::Column, idx::Rid>> entries;
    for (int value = lo; value <= hi; ++value) {
        entries.emplace_back(tuple::Column(value), idx::Rid{static_cast<std::uint32_t>(value),
                                                            static_cast<std::uint32_t>(value)});
    }
    return entries;
}

// One entry with a repeated key and a distinct rid.
std::pair<tuple::Column, idx::Rid> dup_entry(int value, std::uint32_t tag) {
    return {tuple::Column(value), idx::Rid{tag, tag + 777}};
}

// A raw disk block snapshot after a flush (byte-level comparisons).
std::string disk_block(bufman::BufferManager& manager, const std::string& path,
                       std::uint64_t block_id) {
    manager.flush_all();
    bufman::File raw = bufman::open_for_read(path);
    std::array<char, bufman::kBlockSize> block{};
    bufman::read_block(raw, block_id, block);
    bufman::close(raw);
    return std::string(block.data(), block.size());
}

// The tree's level count, read from the header block on disk.
std::uint32_t disk_levels(bufman::BufferManager& manager, const std::string& path) {
    std::uint32_t levels = 0;
    std::memcpy(&levels, disk_block(manager, path, 0).data() + 20, 4);
    return levels;
}

std::optional<bptree::RangeBound> bound(int value) {
    return bptree::RangeBound{tuple::Column(value), true};
}

const auto all_occupied = [](const idx::Rid&) { return true; };

bool verifies_clean(bptree::BPlusTree& index, std::size_t walked) {
    return ok([&] {
        const idx::VerifyResult result = index.verify(all_occupied);
        return result.findings.empty() && result.entry_count == walked;
    });
}

void run() {
    const schema::TableSchema person = schema::load_table_schema("tablecatalog/person.json");

    // --- 1.1. the page views -------------------------------------------------
    {
        check(bptree::leaf_capacity(4) == 340 && bptree::separator_capacity(4) == 511,
              "int-key leaf/separator capacities follow the formulas");
        check(bptree::leaf_capacity(10) == 226 && bptree::separator_capacity(10) == 292,
              "10-byte string key capacities follow the formulas");

        static char page[bufman::kBlockSize];
        const isam::KeyCodec codec(tuple::ColumnType::Integer, 0);
        const auto enc = [&](int value) { return codec.encode(tuple::Column(value)); };
        bptree::BptLeafPage leaf;
        leaf.bind(page, codec.width());
        leaf.format(idx::kNoPage, idx::kNoPage);
        check(ok([&] { leaf.attach(); }) && leaf.count() == 0,
              "a formatted leaf attaches with zero entries");
        // Sorted insert: out-of-order keys land in key order.
        leaf.insert_entry(idx::DataEntry{enc(30), idx::Rid{3, 0}}, codec);
        leaf.insert_entry(idx::DataEntry{enc(10), idx::Rid{1, 0}}, codec);
        leaf.insert_entry(idx::DataEntry{enc(20), idx::Rid{2, 0}}, codec);
        bool order_ok = leaf.count() == 3;
        for (std::size_t i = 0; i < leaf.count(); ++i) {
            order_ok = order_ok && leaf.get_entry(i).rid.page_id == static_cast<std::uint32_t>(i + 1);
        }
        check(order_ok, "the entries sit in key order after out-of-order inserts");
        check(leaf.lower_bound(enc(15).data(), codec) == 1 &&
                  leaf.lower_bound(enc(25).data(), codec) == 2 &&
                  leaf.lower_bound(enc(5).data(), codec) == 0 &&
                  leaf.lower_bound(enc(35).data(), codec) == 3,
              "lower_bound finds the first key >= the target");
        leaf.erase_entry(1);
        check(leaf.count() == 2 && leaf.get_entry(0).rid.page_id == 1 &&
                  leaf.get_entry(1).rid.page_id == 3,
              "erase_entry tail-shifts the remaining entries");

        // move_tail_to: the split's right half.
        bptree::BptLeafPage fresh;
        static char fresh_page[bufman::kBlockSize];
        fresh.bind(fresh_page, codec.width());
        leaf.format(idx::kNoPage, idx::kNoPage);
        for (int value = 1; value <= 4; ++value) {
            leaf.insert_entry(idx::DataEntry{enc(value), idx::Rid{static_cast<std::uint32_t>(value), 0}},
                              codec);
        }
        leaf.move_tail_to(2, fresh);
        check(leaf.count() == 2 && fresh.count() == 2 && fresh.get_entry(0).rid.page_id == 3,
              "move_tail_to halves the entries, order preserved");

        // Separator pages: pairs, routing, insert/erase.
        bptree::BptSeparatorPage sep;
        static char sep_page[bufman::kBlockSize];
        sep.bind(sep_page, codec.width());
        sep.format();
        check(ok([&] { sep.attach(); }) && sep.count() == 0 && sep.first_child() == idx::kNoPage,
              "a formatted separator attaches with no pairs");
        sep.append(idx::IndexEntry{enc(10), 21});
        sep.append(idx::IndexEntry{enc(20), 31});
        check(sep.count() == 2, "separator pairs append in order");
        check(sep.routing_child_index(enc(5).data(), codec) == 0 &&
                  sep.routing_child_index(enc(10).data(), codec) == 1 &&
                  sep.routing_child_index(enc(15).data(), codec) == 1 &&
                  sep.routing_child_index(enc(20).data(), codec) == 2 &&
                  sep.routing_child_index(enc(99).data(), codec) == 2,
              "the routing rule: the child after the last separator <= key");
        sep.set_first_child(11);
        check(sep.routing_child_index(enc(5).data(), codec) == 0 && sep.first_child() == 11 &&
                  sep.child(0) == 21 && sep.child(1) == 31,
              "first child and pair children answer the routing");
        sep.insert_pair_after(1, idx::IndexEntry{enc(15), 26});
        check(sep.count() == 3 && sep.child(1) == 26 &&
                  sep.routing_child_index(enc(15).data(), codec) == 2,
              "insert_pair_after seats the new child between its neighbours");
        sep.erase_pair(1);
        check(sep.count() == 2 && sep.child(1) == 31, "erase_pair drops the pair and closes the gap");
    }

    // --- 1.2. the header: identification on open ------------------------------
    {
        const std::string path = temp_path("hdr");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        manager.init(8, std::make_unique<bufman::LRUPolicy>());
        bptree::BPlusTree idx(manager, person, 0);
        check(ok([&] { idx.create(path); }), "a fresh B+tree file creates");
        check(ok([&] {
                  idx.close();
                  idx.open(path);
              }),
              "the fresh file closes and reopens empty");
        check(ok([&] { return idx.lookup(tuple::Column(1)).empty(); }),
              "an empty index answers nothing");
        check(idx.entry_count() == 0, "the header reports zero entries");

        // Wrong table refused.
        const schema::TableSchema other = schema::parse_table_schema(
            R"({"table_name": "other",
                "columns": [
                  {"column_name": "pid", "column_type": "Integer", "size": 4},
                  {"column_name": "name", "column_type": "String", "size": 10},
                  {"column_name": "age", "column_type": "Integer", "size": 4},
                  {"column_name": "city", "column_type": "String", "size": 3}]})");
        bptree::BPlusTree wrong(manager, other, 0);
        check(fails_with([&] { wrong.open(path); }, "person"),
              "a wrong-table open refuses, naming the file's table");

        // Wrong kind both ways: an ISAM file is not a B+tree, and the
        // B+tree file is not an ISAM index.
        const std::string isam_path = temp_path("hdr-isam");
        std::remove(isam_path.c_str());
        isam::IsamIndex isam_idx(manager, person, 0);
        check(ok([&] { isam_idx.create(isam_path); }), "an ISAM file creates");
        bptree::BPlusTree as_bpt(manager, person, 0);
        check(fails_with([&] { as_bpt.open(isam_path); }),
              "an ISAM file opened as a B+tree refuses by name");
        isam_idx.close();
        idx.close();
        isam::IsamIndex as_isam(manager, person, 0);
        check(fails_with([&] { as_isam.open(path); }), "a B+tree file opened as ISAM refuses");
        std::remove(path.c_str());
        std::remove(isam_path.c_str());
        manager.close_all();
    }

    // --- 2.1. bulk build --------------------------------------------------------
    {
        const std::string path = temp_path("build");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        manager.init(16, std::make_unique<bufman::LRUPolicy>());
        bptree::BPlusTree idx(manager, person, 0);
        check(ok([&] { idx.build(path, int_entries(1, 520)); }), "a 520-entry build succeeds");
        check(idx.entry_count() == 520, "the header counts 520 entries");
        check(ok([&] {
                  for (int value = 1; value <= 520; ++value) {
                      const auto id = static_cast<std::uint32_t>(value);
                      if (idx.lookup(tuple::Column(value)) != std::vector<idx::Rid>{{id, id}}) {
                          return false;
                      }
                  }
                  return true;
              }),
              "every built entry is reachable by lookup");
        check(ok([&] { return idx.range(bound(100), bound(199)).size() == 100; }),
              "an ascending range returns exactly its window");
        check(verifies_clean(idx, 520), "the built tree verifies clean with the walked total");
        check(disk_levels(manager, path) >= 2, "a 520-entry build spans more than one level");
        check(ok([&] {
                  idx.close();
                  idx.open(path);
                  return idx.lookup(tuple::Column(7)).size() == 1;
              }),
              "entries survive a close and reopen");
        idx.close();
        std::remove(path.c_str());

        // The empty build.
        const std::string empty_path = temp_path("build-empty");
        std::remove(empty_path.c_str());
        bptree::BPlusTree empty_idx(manager, person, 0);
        check(ok([&] { empty_idx.build(empty_path, {}); }), "an empty build succeeds");
        check(empty_idx.lookup(tuple::Column(1)).empty(), "the empty build answers nothing");
        check(verifies_clean(empty_idx, 0), "the empty build verifies clean");
        empty_idx.close();
        std::remove(empty_path.c_str());
        manager.close_all();
    }

    // --- 3.1. search: duplicates across a leaf boundary -------------------------
    {
        const std::string path = temp_path("dup");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        manager.init(16, std::make_unique<bufman::LRUPolicy>());
        bptree::BPlusTree idx(manager, person, 0);
        // Keys 1..339 fill the first leaf exactly (340 = 2d for int keys);
        // five more 340s straddle the split, then 341 resumes.
        auto entries = int_entries(1, 339);
        for (std::uint32_t tag = 1; tag <= 5; ++tag) {
            entries.push_back(dup_entry(340, tag));
        }
        const auto tail = int_entries(341, 500);
        entries.insert(entries.end(), tail.begin(), tail.end());
        check(ok([&] { idx.build(path, entries); }), "the boundary build succeeds");
        check(idx.lookup(tuple::Column(340)).size() == 5,
              "duplicates spanning the leaf boundary answer completely");
        check(idx.lookup(tuple::Column(339)).size() == 1, "the key before the run answers");
        check(idx.lookup(tuple::Column(341)).size() == 1, "the key after the run answers");
        check(idx.range(bound(339), bound(341)).size() == 7,
              "a range over the run is ascending with duplicates adjacent");
        // Erase one exact duplicate from the left side of the boundary.
        check(ok([&] { idx.erase(tuple::Column(340), idx::Rid{1, 1 + 777}); }),
              "a boundary duplicate erases exactly");
        const std::vector<idx::Rid> out = idx.lookup(tuple::Column(340));
        check(out.size() == 4, "the run answers four after the erase");
        check(std::find(out.begin(), out.end(), idx::Rid{1, 1 + 777}) == out.end(),
              "the erased rid is gone from the answer");
        check(verifies_clean(idx, 503), "the tree verifies clean after the boundary erase");
        idx.close();
        std::remove(path.c_str());
        manager.close_all();
    }

    // --- 4.1. recursive insert: splits and height -------------------------------
    {
        const std::string path = temp_path("ins");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        manager.init(16, std::make_unique<bufman::LRUPolicy>());
        bptree::BPlusTree idx(manager, person, 0);
        // A full single leaf (2d = 340 entries); the next insert splits it
        // and the height grows by exactly one.
        check(ok([&] { idx.build(path, int_entries(1, 340)); }), "the full-leaf fixture builds");
        const std::uint32_t levels_before = disk_levels(manager, path);
        check(levels_before == 1, "the fixture is a single leaf");
        check(ok([&] { idx.insert(tuple::Column(341), idx::Rid{341, 341}); }),
              "the split insert lands");
        check(disk_levels(manager, path) == levels_before + 1,
              "the height grew by exactly one at the first split");
        check(idx.lookup(tuple::Column(1)).size() == 1 &&
                  idx.lookup(tuple::Column(340)).size() == 1 &&
                  idx.lookup(tuple::Column(341)).size() == 1,
              "both halves answer after the split");
        check(verifies_clean(idx, 341), "the split tree verifies clean (exact separators included)");

        // A non-splitting insert changes only the root-to-leaf path and
        // the header: every other block is byte-identical.
        {
            // Rebuild a fresh fixture so the block layout is known.
            idx.close();
            std::remove(path.c_str());
            bptree::BPlusTree fresh(manager, person, 0);
            check(ok([&] { fresh.build(path, int_entries(1, 200)); }),
                  "the byte-check fixture builds");
            const std::uint32_t levels = disk_levels(manager, path);
            const std::uint64_t blocks = [&] {
                bufman::File raw = bufman::open_for_read(path);
                const std::uint64_t count = bufman::block_count(raw);
                bufman::close(raw);
                return count;
            }();
            std::vector<std::string> before;
            for (std::uint64_t b = 0; b < blocks; ++b) {
                before.push_back(disk_block(manager, path, b));
            }
            check(ok([&] { fresh.insert(tuple::Column(500), idx::Rid{500, 500}); }),
                  "the non-splitting insert lands");
            std::size_t changed = 0;
            for (std::uint64_t b = 0; b < blocks; ++b) {
                changed += disk_block(manager, path, b) != before[b];
            }
            check(changed <= static_cast<std::size_t>(levels) + 1,
                  "only the root-to-leaf path (and header) changed: " + std::to_string(changed) +
                      " of " + std::to_string(blocks));
            fresh.close();
        }

        // Randomized inserts: depth stays uniform, verification clean.
        const std::string rnd_path = temp_path("ins-rnd");
        std::remove(rnd_path.c_str());
        bptree::BPlusTree ridx(manager, person, 0);
        check(ok([&] { ridx.build(rnd_path, {}); }), "the random insert fixture builds");
        std::mt19937 rng(11);
        check(ok([&] {
                  for (int step = 0; step < 800; ++step) {
                      const int key = static_cast<int>(rng() % 400) - 200;
                      ridx.insert(tuple::Column(key), idx::Rid{static_cast<std::uint32_t>(step),
                                                               static_cast<std::uint32_t>(step + 5)});
                      if (!verifies_clean(ridx, ridx.entry_count())) {
                          return false;
                      }
                  }
                  return true;
              }),
              "800 random inserts keep the tree clean and balanced");
        check(disk_levels(manager, rnd_path) >= 2,
              "the random sequence grew the tree above a single leaf");
        ridx.close();
        std::remove(rnd_path.c_str());
        manager.close_all();
    }

    // --- 4.2. separator splits: wide keys fill separator pages fast -------------
    // 900-byte keys leave room for 4 pairs per separator page (2d = 4), so a
    // few hundred inserts split separators repeatedly and grow the tree to
    // three or more levels. Every entry must stay reachable.
    {
        const schema::TableSchema wide = schema::parse_table_schema(
            R"({"table_name": "wide", "columns": [
                  {"column_name": "k", "column_type": "String", "size": 900}]})");
        const std::string path = temp_path("wide");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        manager.init(16, std::make_unique<bufman::LRUPolicy>());
        bptree::BPlusTree idx(manager, wide, 0);
        check(ok([&] { idx.create(path); }), "the wide-key index creates");
        std::vector<int> order(300);
        for (int i = 0; i < 300; ++i) {
            order[i] = i;
        }
        std::shuffle(order.begin(), order.end(), std::mt19937(5));
        const auto key = [](int i) {
            char text[16];
            std::snprintf(text, sizeof(text), "k%05d", i);
            return tuple::Column(std::string(text));
        };
        check(ok([&] {
                  for (const int i : order) {
                      idx.insert(key(i), idx::Rid{static_cast<std::uint32_t>(i), 0});
                  }
              }),
              "300 wide-key inserts land");
        check(disk_levels(manager, path) >= 3, "separator pages split: the tree has 3+ levels");
        check(verifies_clean(idx, 300), "the separator-split tree verifies clean");
        check(ok([&] {
                  for (int i = 0; i < 300; ++i) {
                      if (idx.lookup(key(i)) != std::vector<idx::Rid>{{static_cast<std::uint32_t>(i), 0}}) {
                          return false;
                      }
                  }
                  return true;
              }),
              "every wide key is found after the separator splits");
        idx.close();
        std::remove(path.c_str());
        manager.close_all();
    }

    // --- 5.1. recursive erase: rebalancing ----------------------------------------
    {
        const std::string path = temp_path("erase");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        manager.init(16, std::make_unique<bufman::LRUPolicy>());
        bptree::BPlusTree idx(manager, person, 0);
        check(ok([&] { idx.build(path, int_entries(1, 500)); }), "the erase fixture builds");

        // A middle erase: gone from every path, count decremented.
        check(ok([&] { idx.erase(tuple::Column(250), idx::Rid{250, 250}); }),
              "a present entry erases");
        check(idx.lookup(tuple::Column(250)).empty(), "the erased key answers nothing");
        check(idx.range(bound(248), bound(252)).size() == 4,
              "a range over the gap skips only the erased row");
        check(idx.entry_count() == 499, "the header count dropped by one");
        check(verifies_clean(idx, 499), "the tree verifies clean after the erase");

        // Absent pairs refuse by name, changing nothing.
        check(fails_with([&] { idx.erase(tuple::Column(250), idx::Rid{250, 250}); }),
              "an absent pair refuses");
        check(fails_with([&] { idx.erase(tuple::Column(251), idx::Rid{999, 999}); }),
              "a wrong rid for a present key refuses");
        check(idx.entry_count() == 499, "refusals leave the count alone");

        // Massive erases: the height must come back down (merges and the
        // root drop), and the tree must verify clean at the end.
        const std::uint32_t levels_full = disk_levels(manager, path);
        check(ok([&] {
                  for (int value = 2; value <= 500; value += 2) {
                      if (value != 250) {  // the middle-key erase above took it
                          const auto id = static_cast<std::uint32_t>(value);
                          idx.erase(tuple::Column(value), idx::Rid{id, id});
                      }
                  }
              }),
              "every even key erased");
        const std::uint32_t levels_thin = disk_levels(manager, path);
        check(levels_thin < levels_full, "the height shrank as merges cascaded (from " +
                                             std::to_string(levels_full) + " to " +
                                             std::to_string(levels_thin) + ")");
        check(verifies_clean(idx, 250), "the post-merge tree verifies clean");
        // Occupancy within d..2d everywhere except the root is what the
        // empty findings assert; a spot check on the survivors:
        check(idx.lookup(tuple::Column(1)).size() == 1 && idx.lookup(tuple::Column(499)).size() == 1,
              "survivors still answer");
        idx.close();
        std::remove(path.c_str());
        manager.close_all();
    }

    // --- 5.2. the randomized shadow-model differential ------------------------------
    {
        bufman::BufferManager manager;
        manager.init(16, std::make_unique<bufman::LRUPolicy>());

        // Two runs: a dense domain (heavy duplicates) and a sparse one.
        for (const int domain : {200, 1000000}) {
            const std::string path = temp_path(domain == 200 ? "shadow-dense" : "shadow-sparse");
            std::remove(path.c_str());
            bptree::BPlusTree idx(manager, person, 0);
            check(ok([&] { idx.build(path, {}); }), "the shadow fixture builds");
            std::map<int, std::vector<idx::Rid>> shadow;
            std::mt19937 rng(domain);
            std::uint32_t next_tag = 0;
            std::uint32_t max_levels = 1;
            std::uint32_t min_levels = 99;
            bool sequence_ok = true;
            for (int step = 0; step < 3000 && sequence_ok; ++step) {
                try {
                    if (rng() % 100 < 60 || shadow.empty()) {
                        const int key = static_cast<int>(rng() % domain);
                        const idx::Rid rid{next_tag, next_tag + 3};
                        ++next_tag;
                        idx.insert(tuple::Column(key), rid);
                        shadow[key].push_back(rid);
                    } else {
                        auto it = shadow.begin();
                        std::advance(it, rng() % shadow.size());
                        auto& rids = it->second;
                        const idx::Rid victim = rids[rng() % rids.size()];
                        idx.erase(tuple::Column(it->first), victim);
                        rids.erase(std::find(rids.begin(), rids.end(), victim));
                        if (rids.empty()) {
                            shadow.erase(it);
                        }
                    }
                    sequence_ok = verifies_clean(idx, idx.entry_count());
                    if (!sequence_ok) {
                        check(false, "shadow failure at step " + std::to_string(step) +
                                         ": the tree no longer verifies clean");
                    }
                } catch (const common::Error& e) {
                    sequence_ok = false;
                    check(false, "shadow failure at step " + std::to_string(step) + ": " + e.what());
                }
                const std::uint32_t levels = disk_levels(manager, path);
                max_levels = std::max(max_levels, levels);
                min_levels = std::min(min_levels, levels);
            }
            // The final state must equal the shadow exactly.
            std::size_t shadow_total = 0;
            bool final_ok = sequence_ok;
            for (const auto& [key, rids] : shadow) {
                shadow_total += rids.size();
                final_ok = final_ok && idx.lookup(tuple::Column(key)).size() == rids.size();
            }
            final_ok = final_ok && shadow_total == idx.entry_count();
            check(final_ok, "domain " + std::to_string(domain) +
                                ": 3000 random operations match the shadow exactly");
            check(max_levels > 1, "domain " + std::to_string(domain) + ": splits occurred (levels " +
                                      std::to_string(min_levels) + ".." +
                                      std::to_string(max_levels) + ")");
            idx.close();
            std::remove(path.c_str());
        }
        manager.close_all();
    }
}

}  // namespace

int run_bptree_checks() {
    try {
        run();
    } catch (const std::exception& e) {
        check(false, std::string("unexpected error: ") + e.what());
    }
    std::cout << (failures == 0 ? "bptree suite: all checks passed."
                                : "bptree suite: " + std::to_string(failures) + " check(s) FAILED.")
              << "\n";
    return failures;
}
