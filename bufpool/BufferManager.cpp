#include "bufpool/BufferManager.h"

#include "common/Error.h"

#include <cassert>
#include <cstring>
#include <functional>

namespace bufman {

static_assert(kPageSize == kBlockSize, "frames and file blocks must match");

std::size_t BufferManager::PageKeyHash::operator()(const PageKey& key) const {
    const std::size_t h1 = std::hash<int>{}(key.fd);
    const std::size_t h2 = std::hash<std::uint64_t>{}(key.block_number);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
}

void BufferManager::init(std::size_t pool_size, std::unique_ptr<ReplacementPolicy> policy) {
    // Resetting a manager that still holds files is a close_all first.
    close_all();
    frame_page_.assign(pool_size, std::nullopt);
    pool_.init(pool_size);
    policy_ = std::move(policy);
    if (policy_) {
        policy_->init(pool_size);
    }
}

File BufferManager::open_file(const std::string& path) {
    const File opened = open_for_append(path);
    files_.push_back({opened, path});
    return opened;
}

File BufferManager::open_file_read(const std::string& path) {
    const File opened = open_for_read(path);
    files_.push_back({opened, path});
    return opened;
}

File* BufferManager::find_file(int fd) {
    for (OpenFile& entry : files_) {
        if (entry.file.fd == fd) {
            return &entry.file;
        }
    }
    return nullptr;
}

// A frame to reuse, already emptied: never-used frames first (they never
// cost a write-back), then the policy's pick among the page-holding,
// unpinned frames.
std::size_t BufferManager::take_frame() {
    assert(policy_ && pool_.size() > 0 && "buffer manager is not initialized");
    for (std::size_t i = 0; i < frame_page_.size(); ++i) {
        if (!frame_page_[i]) {
            return i;
        }
    }
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < frame_page_.size(); ++i) {
        if (frame_page_[i] && pool_.frame(i).pin_count == 0) {
            candidates.push_back(i);
        }
    }
    const std::optional<std::size_t> victim = policy_->pick_victim(candidates);
    if (!victim) {
        throw common::Error("all frames pinned");
    }
    evict_frame(*victim);
    return *victim;
}

// Writes the frame's page back if it is dirty, then forgets it so the
// frame is free for a new page.
void BufferManager::evict_frame(std::size_t frame) {
    const PageKey old = *frame_page_[frame];
    DataFrame& data = pool_.frame(frame);
    if (data.dirty) {
        File* old_file = find_file(old.fd);
        assert(old_file != nullptr && "victim page's file is not open");
        write_block(*old_file, old.block_number, data.buffer);
        data.dirty = false;
    }
    page_table_.erase(old);
    policy_->on_remove(frame);
    frame_page_[frame] = std::nullopt;
}

std::size_t BufferManager::pin(File& file, std::uint64_t block_number) {
    const PageKey key{file.fd, block_number};
    if (const auto hit = page_table_.find(key); hit != page_table_.end()) {
        pool_.pin(hit->second);
        policy_->on_access(hit->second);
        return hit->second;
    }

    const std::size_t frame = take_frame();
    DataFrame& data = pool_.frame(frame);
    read_block(file, block_number, data.buffer);
    frame_page_[frame] = key;
    page_table_[key] = frame;
    pool_.pin(frame);
    policy_->on_load(frame);
    return frame;
}

std::size_t BufferManager::alloc_page(File& file, std::uint64_t block_number) {
    assert(file.fd >= 0 && "alloc on a closed file");
    const PageKey key{file.fd, block_number};
    assert(!page_table_.contains(key) && "block is already resident");
    if (block_number < block_count(file)) {
        throw common::Error("block already exists on disk");
    }

    const std::size_t frame = take_frame();
    DataFrame& data = pool_.frame(frame);
    std::memset(data.buffer, 0, kPageSize);
    frame_page_[frame] = key;
    page_table_[key] = frame;
    data.dirty = true;
    pool_.pin(frame);
    policy_->on_load(frame);
    return frame;
}

bool BufferManager::resident(const File& file, std::uint64_t block_number) const {
    return page_table_.contains(PageKey{file.fd, block_number});
}

void BufferManager::unpin(std::size_t frame, bool was_dirty) {
    assert(pool_.frame(frame).pin_count > 0 && "frame is not pinned");
    if (was_dirty) {
        pool_.set_dirty(frame, true);
    }
    pool_.unpin(frame);
}

DataFrame& BufferManager::frame(std::size_t index) {
    return pool_.frame(index);
}

const DataFrame& BufferManager::frame(std::size_t index) const {
    return pool_.frame(index);
}

std::size_t BufferManager::pool_size() const {
    return pool_.size();
}

void BufferManager::flush(std::size_t frame) {
    const std::optional<PageKey>& entry = frame_page_[frame];
    DataFrame& data = pool_.frame(frame);
    if (!entry || !data.dirty) {
        return;
    }
    File* file = find_file(entry->fd);
    assert(file != nullptr && "frame's file is not open");
    write_block(*file, entry->block_number, data.buffer);
    data.dirty = false;
}

void BufferManager::flush_all() {
    for (std::size_t i = 0; i < pool_.size(); ++i) {
        flush(i);
    }
}

void BufferManager::forget_all() {
    for (OpenFile& entry : files_) {
        close(entry.file);
    }
    files_.clear();
    page_table_.clear();
    frame_page_.assign(pool_.size(), std::nullopt);
    if (policy_) {
        policy_->init(pool_.size());
    }
}

void BufferManager::close_all() {
    // The fds are going away either way: tear down, then report a flush
    // failure.
    try {
        flush_all();
    } catch (...) {
        forget_all();
        throw;
    }
    forget_all();
}

void BufferManager::close_file(File& file) {
    assert(find_file(file.fd) != nullptr && "file is not open through this manager");
    // Flush the file's dirty pages, then release every frame it holds.
    for (std::size_t i = 0; i < pool_.size(); ++i) {
        if (frame_page_[i] && frame_page_[i]->fd == file.fd) {
            assert(pool_.frame(i).pin_count == 0 && "a frame of the file is still pinned");
            flush(i);
        }
    }
    for (std::size_t i = 0; i < pool_.size(); ++i) {
        if (frame_page_[i] && frame_page_[i]->fd == file.fd) {
            frame_page_[i] = std::nullopt;
            policy_->on_remove(i);
        }
    }
    std::erase_if(page_table_, [&](const auto& entry) { return entry.first.fd == file.fd; });
    std::erase_if(files_, [&](const OpenFile& entry) { return entry.file.fd == file.fd; });
    // Closes the descriptor and sets file.fd = -1, invalidating the
    // caller's copy the same way close_all's teardown does.
    close(file);
}

bool BufferManager::is_open(const std::string& path) const {
    for (const OpenFile& entry : files_) {
        if (entry.path == path) {
            return true;
        }
    }
    return false;
}

}
