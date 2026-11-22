#include "operators/Partitions.h"

namespace ops {

Partitions::Partitions(bufman::BufferManager& pool, std::filesystem::path directory,
                       std::string label, const schema::TableSchema& schema,
                       std::size_t bucket_count)
    : pool_(&pool), directory_(std::move(directory)), label_(std::move(label)),
      schema_(&schema), bucket_count_(bucket_count) {}

Partitions::~Partitions() { erase_all(); }

void Partitions::create() {
    assert(!created_ && "Partitions::create on an already created set");
    // The scratch directory is created when the first join needs it; the
    // session wiped any stale contents at startup.
    std::error_code fs_error;
    std::filesystem::create_directories(directory_, fs_error);
    if (fs_error) {
        throw common::Error("cannot create the scratch directory '" +
                            directory_.string() + "': " + fs_error.message());
    }
    files_.reserve(bucket_count_);
    for (std::size_t i = 0; i < bucket_count_; ++i) {
        files_.push_back(std::make_unique<heapfile::HeapFile>(*pool_));
        // HouseFile::create refuses a path that already holds a non-empty
        // file, so a name collision with an unerased earlier set surfaces
        // here, named. create() formats and leaves the file closed; the
        // bucket is used through an open file like any other.
        files_.back()->create(path(i), *schema_);
        files_.back()->open(path(i), *schema_);
    }
    created_ = true;
}

void Partitions::write(std::size_t index, const tuple::Tuple& row) {
    assert(created_ && "Partitions::write before create");
    assert(index < bucket_count_ && "bucket index out of range");
    files_[index]->insert(row);
}

void Partitions::scan_start(std::size_t index) {
    assert(created_ && "Partitions::scan_start before create");
    assert(index < bucket_count_ && "bucket index out of range");
    scan_ = files_[index]->scan();
}

std::optional<tuple::Tuple> Partitions::scan_next() {
    assert(created_ && "Partitions::scan_next before create");
    if (!scan_) {
        return std::nullopt;
    }
    return scan_->next();
}

void Partitions::erase_all() {
    scan_.reset();
    // Close first (flush + drop the manager's entry), then remove the
    // file. A never-created or already-erased set is a no-op. A removal
    // failure is not named: erase_all runs in destructors, where throwing
    // is forbidden, and a leftover file is wiped by the session's startup
    // sweep.
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (files_[i]->is_open()) {
            files_[i]->close();
        }
        std::error_code fs_error;
        std::filesystem::remove(path(i), fs_error);
    }
    files_.clear();
    created_ = false;
}

std::string Partitions::path(std::size_t index) const {
    return (directory_ / (label_ + "_" + std::to_string(index) + ".tbl")).string();
}

}
