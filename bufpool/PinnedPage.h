#ifndef P3_PINNED_PAGE_H
#define P3_PINNED_PAGE_H

#include "bufpool/BufferManager.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace bufman {

/// One pin on one page, released when the guard leaves scope — on a
/// normal return, an early return, or an exception alike. This is the
/// "pin → operate → unpin" discipline made structural: no call site
/// unpins by hand. Movable (so guards can live in containers and be
/// returned), never copyable (one guard, one pin).
class PinnedPage {
public:
    /// Pins an existing page, reading it from disk on a miss.
    PinnedPage(BufferManager& manager, File& file, std::uint64_t page_id)
        : manager_(&manager), frame_(manager.pin(file, page_id)), page_id_(page_id) {}

    /// Allocates a brand-new, zeroed page (already dirty).
    static PinnedPage fresh(BufferManager& manager, File& file, std::uint64_t page_id) {
        return PinnedPage(manager, manager.alloc_page(file, page_id), page_id);
    }

    PinnedPage(PinnedPage&& other) noexcept
        : manager_(std::exchange(other.manager_, nullptr)),
          frame_(other.frame_), page_id_(other.page_id_), dirty_(other.dirty_) {}

    PinnedPage& operator=(PinnedPage&& other) noexcept {
        if (this != &other) {
            release();
            manager_ = std::exchange(other.manager_, nullptr);
            frame_ = other.frame_;
            page_id_ = other.page_id_;
            dirty_ = other.dirty_;
        }
        return *this;
    }

    PinnedPage(const PinnedPage&) = delete;
    PinnedPage& operator=(const PinnedPage&) = delete;

    ~PinnedPage() { release(); }

    char* data() const { return manager_->frame(frame_).buffer; }
    std::uint64_t page_id() const { return page_id_; }

    /// The page will be unpinned dirty (written back on eviction/flush).
    void mark_dirty() { dirty_ = true; }

    /// Unpins now instead of at scope exit; the guard is empty afterwards.
    void release() {
        if (manager_ != nullptr) {
            std::exchange(manager_, nullptr)->unpin(frame_, dirty_);
        }
    }

private:
    PinnedPage(BufferManager& manager, std::size_t frame, std::uint64_t page_id)
        : manager_(&manager), frame_(frame), page_id_(page_id) {}

    BufferManager* manager_; // null once released or moved from
    std::size_t frame_;
    std::uint64_t page_id_;
    bool dirty_ = false;
};

/// A pinned page together with the page view bound over its bytes — the
/// shape every page-format module works with: `page->count()` reads
/// through the view, `page.mark_dirty()` marks the pin.
template <class View>
struct PinnedView {
    PinnedPage pin;
    View view;

    View* operator->() { return &view; }
    const View* operator->() const { return &view; }
    void mark_dirty() { pin.mark_dirty(); }
    std::uint32_t id() const { return static_cast<std::uint32_t>(pin.page_id()); }
};

}

#endif // P3_PINNED_PAGE_H
