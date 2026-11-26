#pragma once

// The title-memory pull coalescer's policy (docs/title_memory_model.md): the set of title-VA byte ranges a submission
// has already copied into the GPU's arena stand-in. A later draw in the same submission that names a range already
// covered by an earlier pull does not need the copy again - the arena range already holds those bytes and no GPU work
// has read them yet - so the copy can be skipped. This is the measured, reversible half of the peers' "copies narrowed
// to what changed" model; it decides nothing about *which* pages changed (that needs the write signal
// docs/card_copies.md readied), only whether this submission already paid for the same bytes.
//
// The class is pure policy: no Windows, no Vulkan, no allocation (a fixed array), so the GPU-free test can exercise the
// overlap cases and the bound without a device. Ranges are merged, so two overlapping pulls are covered by their union,
// and a pull that starts or ends inside a window is *not* covered (its bytes were not all copied): the caller copies it
// and the window grows. `kMaxWindows` bounds the state: past it the oldest insertion is dropped, which only means its
// bytes are copied again - a missed skip, never a stale read.

#include <cstddef>
#include <cstdint>

namespace palmetto {

class TitlePullWindows {
public:
    static constexpr std::size_t kMaxWindows = 64;      // bounded per-submission state
    static constexpr std::uint64_t kMaxAddress = ~std::uint64_t{0};

    void reset() { count_ = 0; }
    std::size_t size() const { return count_; }
    bool full() const { return count_ >= kMaxWindows; }

    // True when every byte of [va, va + bytes) is inside one window: the earlier pull copied all of it.
    bool covers(std::uint64_t va, std::uint64_t bytes) const {
        if (!bytes || va > kMaxAddress - (bytes - 1)) return false;
        const std::uint64_t end = va + bytes;
        for (std::size_t i = 0; i < count_; ++i) {
            if (windows_[i].begin <= va && end <= windows_[i].end) return true;
        }
        return false;
    }

    // Records that [va, va + bytes) has been copied: merges it into every overlapping or adjacent window. False when
    // the range is empty/overflowing or the insertion would exceed the budget (the range is then not recorded, so a
    // later identical pull copies again).
    bool add(std::uint64_t va, std::uint64_t bytes) {
        if (!bytes || va > kMaxAddress - (bytes - 1)) return false;
        std::uint64_t begin = va;
        std::uint64_t end = va + bytes;
        // Merge every window the new one touches (adjacency merges too: a contiguous range is one window).
        std::size_t kept = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (windows_[i].end < begin || windows_[i].begin > end) {
                windows_[kept++] = windows_[i];
                continue;
            }
            if (windows_[i].begin < begin) begin = windows_[i].begin;
            if (windows_[i].end > end) end = windows_[i].end;
        }
        count_ = kept;
        if (count_ >= kMaxWindows) {
            // The oldest (lowest-address) window goes: dropping a window can only cause a copy that would have been
            // skipped. The windows are kept sorted by begin, as the merge above preserved.
            for (std::size_t i = 1; i < count_; ++i) windows_[i - 1] = windows_[i];
            --count_;
        }
        std::size_t at = 0;
        while (at < count_ && windows_[at].begin < begin) ++at;
        for (std::size_t i = count_; i > at; --i) windows_[i] = windows_[i - 1];
        windows_[at] = Window{begin, end};
        ++count_;
        return true;
    }

private:
    struct Window {
        std::uint64_t begin;
        std::uint64_t end;  // exclusive
    };
    Window windows_[kMaxWindows]{};
    std::size_t count_ = 0;
};

}  // namespace palmetto
