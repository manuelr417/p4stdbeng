#ifndef EX09_BUFFER_MANAGER_H
#define EX09_BUFFER_MANAGER_H

#include "bufpool/BufferPool.h"
#include "file/BlockFile.h"
#include "policy/ReplacementPolicy.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bufman {

// Sits between queries and disk: owns a BufferPool and moves 4096-byte
// blocks between files and frames through BlockFile. Tracks which
// (file, block) lives in which frame, evicts via an injected
// ReplacementPolicy, and writes dirty victims back before reuse.
//
// Reachable failures (I/O, every frame pinned) throw common::Error.
// Misuse — unpinning an unpinned frame, closing a file that still has a
// pinned frame — is a contract violation and asserts. Most callers pin
// through PinnedPage (bufpool/PinnedPage.h), which unpins on scope exit.
class BufferManager {
public:
    void init(std::size_t pool_size, std::unique_ptr<ReplacementPolicy> policy);
    File open_file(const std::string& path);
    File open_file_read(const std::string& path);
    // Pins (file, block_number), reading it from disk on a miss, and
    // returns its frame.
    std::size_t pin(File& file, std::uint64_t block_number);
    // Creates a brand-new page: takes a frame, zeroes its buffer, registers
    // it as (file, block_number) and marks it dirty so it reaches disk on
    // write-back. The block must not be resident or already on disk.
    std::size_t alloc_page(File& file, std::uint64_t block_number);
    // True if the page is currently resident in the pool (a pin would hit).
    bool resident(const File& file, std::uint64_t block_number) const;

    // Releases one pin of a pinned frame.
    void unpin(std::size_t frame, bool was_dirty);
    DataFrame& frame(std::size_t index);
    const DataFrame& frame(std::size_t index) const;
    // Number of frames the pool holds.
    std::size_t pool_size() const;
    void flush(std::size_t frame);
    void flush_all();
    // Flushes everything and closes every file. The teardown always
    // completes; a flush failure is re-thrown after it.
    void close_all();
    // Flushes the dirty pages of this one file, drops its entries from
    // the page table and frame map, closes its descriptor, and removes it
    // from the registry. Other files' pages and descriptors are untouched.
    // No frame of the file may still be pinned.
    void close_file(File& file);
    // True if a file with this path is currently open through this manager.
    bool is_open(const std::string& path) const;

private:
    struct PageKey {
        int fd;
        std::uint64_t block_number;
        bool operator==(const PageKey&) const = default;
    };
    struct PageKeyHash {
        std::size_t operator()(const PageKey& key) const;
    };
    struct OpenFile {
        File file;
        std::string path;
    };

    std::size_t take_frame();
    void evict_frame(std::size_t frame);
    File* find_file(int fd);
    void forget_all();

    BufferPool pool_;
    std::unique_ptr<ReplacementPolicy> policy_;
    std::unordered_map<PageKey, std::size_t, PageKeyHash> page_table_;
    std::vector<std::optional<PageKey>> frame_page_;
    std::vector<OpenFile> files_;
};

}

#endif // EX09_BUFFER_MANAGER_H
