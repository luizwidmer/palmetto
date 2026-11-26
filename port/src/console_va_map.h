#pragma once

// The title's own console VAs inside the one GPU address space (design B, docs/page_address_space.md).
//
// A translated shader computes `host_address = heap_base + console_va` (docs/d3d12_vulkan.md section 3.3), where
// heap_base is the device address of the GPU address arena. The title names some of its memory by VAs the layer did
// not choose: the two-terabyte host VAs of the pages behind Xbox device slot 133, and the 64 KiB-aligned console
// addresses of the descriptor heaps slot 14 places. Those VAs can never satisfy that equation against an arena VA, so
// a draw that hands one to a shader faults the GPU (docs/replay_resolution.md section 4).
//
// The design the evidence picks is VA rebasing, not page import: one heap_base means every shader-visible VA must be
// an offset in the one arena. So the layer keeps a range in the arena that stands in for each title range, keeps the
// title VA as an alias for command arguments (vertex, index and indirect views, copy regions: `buffer_at` translates
// it through this map), and rewrites every shader-visible copy of the title VA to the range's arena VA:
//
//   * the 64-bit root descriptor and descriptor-table VAs in a draw's user data (`rebase_user_data` in
//     vulkan_replay.h);
//   * the 48-bit V# base fields inside descriptors the title writes into its own console descriptor heap
//     (`patch_descriptor_bases`, applied to the heap's arena mirror).
//
// Anything this map cannot place must keep its submission refused: `mark_incomplete` makes every draw refuse rather
// than execute an unrebased VA. The map is read on the replay thread and written by the title's recording threads, so
// it is behind a shared mutex.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace palmetto {

// What a title VA range names. A placed buffer's V# bases are rebased; a descriptor heap's table VAs are rebased too,
// but the descriptors it holds are scanned by patch_descriptor_bases as descriptor slots, not as one base field.
enum class ConsoleVaKind : std::uint8_t {
    buffer,      // a buffer on the title's pages (device slot 133): 48-bit V# base fields name it
    descriptors  // a shader-visible descriptor heap at a console address (device slot 14)
};

inline const char* console_va_kind_name(ConsoleVaKind kind) {
    return kind == ConsoleVaKind::buffer ? "buffer" : "descriptor heap";
}

struct ConsoleVaRange {
    std::uint64_t title = 0;  // first title VA of the range
    std::uint64_t size = 0;   // bytes
    std::uint64_t arena = 0;  // arena VA that stands in for `title`
    ConsoleVaKind kind = ConsoleVaKind::buffer;

    bool holds(std::uint64_t va) const { return va >= title && va - title < size; }
};

// D3D12 descriptor slots are 32 bytes (d3d12_com.h, descriptor_size).
inline constexpr std::size_t console_va_descriptor_size = 32;

class ConsoleVaMap {
public:
    // Adds one range. False when it overlaps a range of the same kind, or a size of zero: two stand-ins of one kind for
    // one VA is a choice the layer cannot make. A buffer range and a descriptor-heap range MAY overlap: the title's
    // address space packs them into one xmem region, and a VA is then read as a table or as a buffer by what the
    // command or root parameter says it is (a table VA is in a console descriptor heap, a root descriptor VA names a
    // placed buffer), which `rebase`'s preference decides.
    bool add(std::uint64_t title, std::uint64_t size, std::uint64_t arena, ConsoleVaKind kind) {
        if (!size || size > UINT64_MAX - title) return false;
        std::unique_lock lock(mutex_);
        for (const auto& range : ranges_) {
            if (kind == range.kind && title < range.title + range.size && range.title < title + size) return false;
        }
        ranges_.push_back({title, size, arena, kind});
        std::sort(ranges_.begin(), ranges_.end(),
                  [](const ConsoleVaRange& a, const ConsoleVaRange& b) { return a.title < b.title; });
        return true;
    }

    // A resource or heap the range stood for is gone; the arena range it named is released by its owner separately.
    void remove(std::uint64_t title) {
        std::unique_lock lock(mutex_);
        std::erase_if(ranges_, [title](const ConsoleVaRange& range) { return range.title == title; });
    }

    // The arena VA that stands in for `va`. When ranges of both kinds hold it (they may overlap), `preferred` decides
    // which one the caller's site means: a descriptor table names a console descriptor heap, a root descriptor names a
    // placed buffer. A range of the other kind is the fallback.
    bool rebase(std::uint64_t va, std::uint64_t& rebased, ConsoleVaKind preferred) const {
        std::shared_lock lock(mutex_);
        const ConsoleVaRange* range = find(va, preferred);
        if (!range) return false;
        rebased = range->arena + (va - range->title);
        return true;
    }

    // Buffer ranges only: which range holds a V#-shaped 64-bit value.
    bool rebase_buffer(std::uint64_t va, std::uint64_t& rebased) const {
        return rebase(va, rebased, ConsoleVaKind::buffer);
    }

    // The arena range a title span starts in: the arena VA of `va` and how many bytes are left in the range from there
    // (a page pull is clamped to that).
    bool rebase_span(std::uint64_t va, ConsoleVaKind preferred, std::uint64_t& arena, std::uint64_t& available) const {
        std::shared_lock lock(mutex_);
        const ConsoleVaRange* range = find(va, preferred);
        if (!range) return false;
        arena = range->arena + (va - range->title);
        available = range->size - (va - range->title);
        return true;
    }

    // The title span an arena VA is a stand-in for: the reverse direction. A V# the layer rebased names the arena range,
    // while the bytes behind it are still the title's pages (docs/page_content.md).
    bool title_span(std::uint64_t arena, std::uint64_t& title, std::uint64_t& available) const {
        std::shared_lock lock(mutex_);
        for (const auto& range : ranges_) {
            if (range.kind != ConsoleVaKind::buffer || arena < range.arena) continue;
            const std::uint64_t offset = arena - range.arena;
            if (offset >= range.size) continue;
            title = range.title + offset;
            available = range.size - offset;
            return true;
        }
        return false;
    }

    bool holds(std::uint64_t va) const {
        std::shared_lock lock(mutex_);
        return find(va, ConsoleVaKind::buffer) != nullptr;
    }

    // A copy of the ranges, for a diagnostic that has to say which title range a command VA falls in or next to
    // (docs/vertex_buffer_va.md, the miss dump). Ordered by title VA like the internal list.
    std::vector<ConsoleVaRange> snapshot() const {
        std::shared_lock lock(mutex_);
        return ranges_;
    }

    std::size_t size() const {
        std::shared_lock lock(mutex_);
        return ranges_.size();
    }

    // Rewrites the V# base fields of the descriptor slots in `bytes` that name a title-placed buffer, so the
    // descriptors the title wrote into its own CPU descriptor heap name the range's arena VA once the bytes are a
    // GPU-visible mirror. A V# base is its buffer's VA: dword 0 holds bits 31:0 and dword 1 bits 47:32, with
    // dword 1's upper half carrying the format/stride (docs/d3d12_vulkan.md section 3.3, D3D-53). Returns the number
    // of fields rewritten; a slot is only touched when its whole 48-bit base is inside a registered buffer range, so
    // the odds of matching an unrelated descriptor are about one in 2^31 per slot.
    std::size_t patch_descriptor_bases(std::uint8_t* bytes, std::size_t size) const {
        if (!bytes) return 0;
        std::shared_lock lock(mutex_);
        std::size_t patched = 0;
        for (std::size_t at = 0; at + console_va_descriptor_size <= size; at += console_va_descriptor_size) {
            auto* dwords = reinterpret_cast<std::uint32_t*>(bytes + at);
            const std::uint64_t base = std::uint64_t(dwords[0]) | (std::uint64_t(dwords[1] & 0xffffu) << 32);
            if (!base) continue;
            const ConsoleVaRange* range = find(base, ConsoleVaKind::buffer);
            if (!range || range->kind != ConsoleVaKind::buffer) continue;
            const std::uint64_t rebased = range->arena + (base - range->title);
            dwords[0] = std::uint32_t(rebased);
            dwords[1] = (dwords[1] & 0xffff0000u) | std::uint32_t((rebased >> 32) & 0xffffu);
            ++patched;
        }
        return patched;
    }

    // Set when a title range the title can name as a shader-visible VA could not get an arena range. Executing any
    // draw then could fault the GPU, so the replay refuses draws instead (vulkan_replay.h, prepare_draw).
    void mark_incomplete() { incomplete_.store(true, std::memory_order_relaxed); }
    bool incomplete() const { return incomplete_.load(std::memory_order_relaxed); }

    // How many shader-visible titles VAs were rebased, for the boot's one-line report.
    std::uint64_t rebased_values() const { return rebased_values_.load(std::memory_order_relaxed); }
    std::uint64_t rebased_fields() const { return rebased_fields_.load(std::memory_order_relaxed); }
    void count_rebase() { rebased_values_.fetch_add(1, std::memory_order_relaxed); }
    void count_fields(std::size_t count) { rebased_fields_.fetch_add(count, std::memory_order_relaxed); }
    // The page content pulled into the stand-ins (docs/page_content.md), for the boot's report.
    void count_pulled(std::uint64_t bytes) {
        pulled_calls_.fetch_add(1, std::memory_order_relaxed);
        pulled_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }
    std::uint64_t pulled_calls() const { return pulled_calls_.load(std::memory_order_relaxed); }
    std::uint64_t pulled_bytes() const { return pulled_bytes_.load(std::memory_order_relaxed); }

private:
    // The range that holds `va`, preferring `preferred`'s kind when ranges of both kinds hold it.
    const ConsoleVaRange* find(std::uint64_t va, ConsoleVaKind preferred) const {
        const ConsoleVaRange* fallback = nullptr;
        for (const auto& range : ranges_) {
            if (!range.holds(va)) continue;
            if (range.kind == preferred) return &range;
            if (!fallback) fallback = &range;
        }
        return fallback;
    }

    mutable std::shared_mutex mutex_;
    std::vector<ConsoleVaRange> ranges_;
    std::atomic<bool> incomplete_{false};
    std::atomic<std::uint64_t> rebased_values_{0};
    std::atomic<std::uint64_t> rebased_fields_{0};
    std::atomic<std::uint64_t> pulled_calls_{0};
    std::atomic<std::uint64_t> pulled_bytes_{0};
};

}  // namespace palmetto
