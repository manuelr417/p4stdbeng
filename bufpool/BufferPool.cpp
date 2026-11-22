#include "bufpool/BufferPool.h"

#include <cassert>

namespace bufman {

void BufferPool::init(std::size_t n) {
    frames_.assign(n, DataFrame{});
}

DataFrame& BufferPool::frame(std::size_t index) {
    assert(index < frames_.size() && "frame index out of range");
    return frames_[index];
}

const DataFrame& BufferPool::frame(std::size_t index) const {
    assert(index < frames_.size() && "frame index out of range");
    return frames_[index];
}

void BufferPool::set_dirty(std::size_t index, bool dirty) {
    frame(index).dirty = dirty;
}

void BufferPool::pin(std::size_t index) {
    ++frame(index).pin_count;
}

void BufferPool::unpin(std::size_t index) {
    DataFrame& f = frame(index);
    assert(f.pin_count > 0 && "pin count is already zero");
    --f.pin_count;
}

std::size_t BufferPool::size() const {
    return frames_.size();
}

}
