#pragma once

// CPU-only policy for opt-in title-VA windows that do not consume the shared GPU-address arena.
//
// The shader address contract remains `heap_base + console_va` (docs/d3d12_vulkan.md section 3.3). A window therefore
// needs a device address at or above the arena's device address, and its positive delta plus full extent must fit the
// V#'s 48-bit base field. The title's raw VA is an alias key only; this class does not create Vulkan objects, validate
// host page readability, copy/pull bytes, or make a window live in replay. Those operations belong to a device adapter.
//
// Fixed-capacity storage makes the resource and byte bounds explicit. The 32 MiB total GPU-window budget equals the
// measured staging envelope in docs/vertex_buffer_va.md section 18 (eight 4 MiB spans); 32 fixed slots allow smaller
// windows without growing the byte budget. They are intentionally opt-in and are not a claim that a live title
// population fits. The class is thread-safe because registration can happen on title threads while
// replay resolves views on another thread.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string_view>

namespace palmetto {

enum class TitleVaWindowsSwitch : std::uint8_t { off, on, invalid };

// Strict, pure parse for PALMETTO_TITLE_VA_WINDOWS. Unset/empty is off; malformed text never opts in accidentally.
inline TitleVaWindowsSwitch title_va_windows_switch_from(const char* value) {
    if (!value || !*value) return TitleVaWindowsSwitch::off;
    const std::string_view text(value);
    if (text == "1" || text == "on" || text == "true" || text == "yes" || text == "y")
        return TitleVaWindowsSwitch::on;
    if (text == "0" || text == "off" || text == "false" || text == "no" || text == "n")
        return TitleVaWindowsSwitch::off;
    return TitleVaWindowsSwitch::invalid;
}

enum class TitleVaAliasRefusal : std::uint8_t {
    none,
    switch_off,
    invalid_switch,
    no_arena_geometry,
    zero_window_id,
    zero_alias_id,
    zero_address,
    zero_bytes,
    range_overflow,
    title_va_inside_arena_window,
    window_too_large,
    window_budget_spent,
    window_table_full,
    duplicate_window_id,
    window_below_heap_base,
    window_overlaps_arena,
    window_v48_unrepresentable,
    window_overlap,
    alias_table_full,
    duplicate_alias_id,
    alias_window_missing,
    alias_outside_window,
    alias_overlap,
    alias_unmapped,
    alias_view_out_of_bounds,
    alias_has_live_views,
    window_not_found,
};

inline const char* title_va_alias_refusal_name(TitleVaAliasRefusal refusal) {
    switch (refusal) {
        case TitleVaAliasRefusal::none: return "none";
        case TitleVaAliasRefusal::switch_off: return "switch_off";
        case TitleVaAliasRefusal::invalid_switch: return "invalid_switch";
        case TitleVaAliasRefusal::no_arena_geometry: return "no_arena_geometry";
        case TitleVaAliasRefusal::zero_window_id: return "zero_window_id";
        case TitleVaAliasRefusal::zero_alias_id: return "zero_alias_id";
        case TitleVaAliasRefusal::zero_address: return "zero_address";
        case TitleVaAliasRefusal::zero_bytes: return "zero_bytes";
        case TitleVaAliasRefusal::range_overflow: return "range_overflow";
        case TitleVaAliasRefusal::title_va_inside_arena_window: return "title_va_inside_arena_window";
        case TitleVaAliasRefusal::window_too_large: return "window_too_large";
        case TitleVaAliasRefusal::window_budget_spent: return "window_budget_spent";
        case TitleVaAliasRefusal::window_table_full: return "window_table_full";
        case TitleVaAliasRefusal::duplicate_window_id: return "duplicate_window_id";
        case TitleVaAliasRefusal::window_below_heap_base: return "window_below_heap_base";
        case TitleVaAliasRefusal::window_overlaps_arena: return "window_overlaps_arena";
        case TitleVaAliasRefusal::window_v48_unrepresentable: return "window_v48_unrepresentable";
        case TitleVaAliasRefusal::window_overlap: return "window_overlap";
        case TitleVaAliasRefusal::alias_table_full: return "alias_table_full";
        case TitleVaAliasRefusal::duplicate_alias_id: return "duplicate_alias_id";
        case TitleVaAliasRefusal::alias_window_missing: return "alias_window_missing";
        case TitleVaAliasRefusal::alias_outside_window: return "alias_outside_window";
        case TitleVaAliasRefusal::alias_overlap: return "alias_overlap";
        case TitleVaAliasRefusal::alias_unmapped: return "alias_unmapped";
        case TitleVaAliasRefusal::alias_view_out_of_bounds: return "alias_view_out_of_bounds";
        case TitleVaAliasRefusal::alias_has_live_views: return "alias_has_live_views";
        case TitleVaAliasRefusal::window_not_found: return "window_not_found";
    }
    return "unknown_refusal";
}

struct TitleVaAliasResolution {
    std::uint64_t window_id = 0;
    std::uint64_t console_va = 0;      // shader-visible address relative to heap_base
    std::uint64_t device_address = 0;  // address the Vulkan window adapter must back
    std::uint64_t available_bytes = 0;
};

class TitleVaAliasTable {
public:
    static constexpr std::uint64_t kConsoleVaSpace = std::uint64_t{1} << 48;
    static constexpr std::uint64_t kArenaWindowEnd = std::uint64_t{1} << 32;
    static constexpr std::uint64_t kMaxWindowBytes = std::uint64_t{4} << 20;
    static constexpr std::uint64_t kMaxTotalWindowBytes = std::uint64_t{32} << 20;
    static constexpr std::size_t kMaxWindows = 32;
    static constexpr std::size_t kMaxAliases = 32;

    // The arena device address is the shader's single heap_base; arena_bytes describes its occupied BDA range.
    // A default-constructed table is off, so it cannot change the existing mapping path.
    explicit TitleVaAliasTable(TitleVaWindowsSwitch mode = TitleVaWindowsSwitch::off,
                               std::uint64_t arena_device_address = 0, std::uint64_t arena_bytes = 0)
        : mode_(mode), arena_device_address_(arena_device_address), arena_bytes_(arena_bytes) {}

    bool enabled() const { return mode_ == TitleVaWindowsSwitch::on; }

    // Register a distinct device-address window outside the arena. The adapter supplies the actual BDA after creating
    // its resource. The resulting positive `console_va` must fit the full 48-bit V# address extent; a negative bias is
    // refused instead of truncating it into a wild address.
    TitleVaAliasRefusal add_window(std::uint64_t id, std::uint64_t device_address, std::uint64_t bytes) {
        if (const auto refusal = switch_refusal(); refusal != TitleVaAliasRefusal::none) return refusal;
        if (!id) return TitleVaAliasRefusal::zero_window_id;
        if (!device_address) return TitleVaAliasRefusal::zero_address;
        if (!bytes) return TitleVaAliasRefusal::zero_bytes;
        if (bytes > kMaxWindowBytes) return TitleVaAliasRefusal::window_too_large;

        std::uint64_t arena_end = 0;
        std::uint64_t device_end = 0;
        if (!arena_device_address_ || !arena_bytes_ || !range_end(arena_device_address_, arena_bytes_, arena_end))
            return TitleVaAliasRefusal::no_arena_geometry;
        if (!range_end(device_address, bytes, device_end)) return TitleVaAliasRefusal::range_overflow;
        if (device_address < arena_device_address_) return TitleVaAliasRefusal::window_below_heap_base;
        if (device_address < arena_end) return TitleVaAliasRefusal::window_overlaps_arena;

        const std::uint64_t console_va = device_address - arena_device_address_;
        if (console_va >= kConsoleVaSpace || bytes > kConsoleVaSpace - console_va)
            return TitleVaAliasRefusal::window_v48_unrepresentable;

        std::unique_lock lock(mutex_);
        if (find_window(id)) return TitleVaAliasRefusal::duplicate_window_id;
        if (windows_used_ == kMaxWindows) return TitleVaAliasRefusal::window_table_full;
        if (bytes > kMaxTotalWindowBytes - window_bytes_) return TitleVaAliasRefusal::window_budget_spent;

        for (std::size_t i = 0; i < windows_used_; ++i) {
            const Window& other = windows_[i];
            std::uint64_t other_device_end = 0;
            if (!range_end(other.device_address, other.bytes, other_device_end))
                return TitleVaAliasRefusal::range_overflow;  // stored windows are validated; fail closed if corrupted
            if (device_address < other_device_end && other.device_address < device_end)
                return TitleVaAliasRefusal::window_overlap;
        }
        windows_[windows_used_++] = {id, device_address, console_va, bytes};
        window_bytes_ += bytes;
        return TitleVaAliasRefusal::none;
    }

    // Alias one raw title VA range to a byte window. Several non-overlapping title VAs may name the same bytes in a
    // window; two overlapping raw buffer aliases are ambiguous and refused. The whole alias must stay outside the
    // 32-bit arena VA window and within the registered device window.
    TitleVaAliasRefusal add_alias(std::uint64_t id, std::uint64_t title_va, std::uint64_t bytes,
                                  std::uint64_t window_id, std::uint64_t window_offset = 0) {
        if (const auto refusal = switch_refusal(); refusal != TitleVaAliasRefusal::none) return refusal;
        if (!id) return TitleVaAliasRefusal::zero_alias_id;
        if (!title_va) return TitleVaAliasRefusal::zero_address;
        if (!bytes) return TitleVaAliasRefusal::zero_bytes;
        if (title_va < kArenaWindowEnd) return TitleVaAliasRefusal::title_va_inside_arena_window;
        std::uint64_t title_end = 0;
        if (!range_end(title_va, bytes, title_end)) return TitleVaAliasRefusal::range_overflow;

        std::unique_lock lock(mutex_);
        if (find_alias(id)) return TitleVaAliasRefusal::duplicate_alias_id;
        if (aliases_used_ == kMaxAliases) return TitleVaAliasRefusal::alias_table_full;
        const Window* window = find_window(window_id);
        if (!window) return TitleVaAliasRefusal::alias_window_missing;
        if (window_offset > window->bytes || bytes > window->bytes - window_offset)
            return TitleVaAliasRefusal::alias_outside_window;

        for (std::size_t i = 0; i < aliases_used_; ++i) {
            const Alias& other = aliases_[i];
            std::uint64_t other_end = 0;
            if (!range_end(other.title_va, other.bytes, other_end))
                return TitleVaAliasRefusal::range_overflow;  // stored aliases are validated; fail closed if corrupted
            if (title_va < other_end && other.title_va < title_end) return TitleVaAliasRefusal::alias_overlap;
        }
        aliases_[aliases_used_++] = {id, title_va, bytes, window_id, window_offset};
        return TitleVaAliasRefusal::none;
    }

    // Resolve the entire requested view; partial backing is not returned. On refusal, `out` remains byte-for-byte intact.
    TitleVaAliasRefusal resolve(std::uint64_t title_va, std::uint64_t bytes, TitleVaAliasResolution& out) const {
        if (const auto refusal = switch_refusal(); refusal != TitleVaAliasRefusal::none) return refusal;
        if (!title_va) return TitleVaAliasRefusal::zero_address;
        if (!bytes) return TitleVaAliasRefusal::zero_bytes;
        if (title_va < kArenaWindowEnd) return TitleVaAliasRefusal::title_va_inside_arena_window;
        if (!range_valid(title_va, bytes)) return TitleVaAliasRefusal::range_overflow;

        std::shared_lock lock(mutex_);
        const Alias* alias = find_alias_for_va(title_va);
        if (!alias) return TitleVaAliasRefusal::alias_unmapped;
        const std::uint64_t alias_offset = title_va - alias->title_va;
        if (bytes > alias->bytes - alias_offset) return TitleVaAliasRefusal::alias_view_out_of_bounds;
        const Window* window = find_window(alias->window_id);
        if (!window) return TitleVaAliasRefusal::alias_window_missing;

        const std::uint64_t window_offset = alias->window_offset + alias_offset;
        if (window_offset > window->bytes || bytes > window->bytes - window_offset)
            return TitleVaAliasRefusal::alias_outside_window;
        const std::uint64_t console_va = window->console_va + window_offset;
        const std::uint64_t device_address = window->device_address + window_offset;
        if (console_va >= kConsoleVaSpace || bytes > kConsoleVaSpace - console_va)
            return TitleVaAliasRefusal::window_v48_unrepresentable;

        out = {window->id, console_va, device_address, alias->bytes - alias_offset};
        return TitleVaAliasRefusal::none;
    }

    // Drop the alias before releasing its Vulkan window; refusing a live reference prevents a dangling resolution.
    TitleVaAliasRefusal remove_alias(std::uint64_t id) {
        if (const auto refusal = switch_refusal(); refusal != TitleVaAliasRefusal::none) return refusal;
        std::unique_lock lock(mutex_);
        for (std::size_t i = 0; i < aliases_used_; ++i) {
            if (aliases_[i].id != id) continue;
            for (std::size_t j = i + 1; j < aliases_used_; ++j) aliases_[j - 1] = aliases_[j];
            aliases_[--aliases_used_] = {};
            return TitleVaAliasRefusal::none;
        }
        return TitleVaAliasRefusal::alias_unmapped;
    }

    TitleVaAliasRefusal remove_window(std::uint64_t id) {
        if (const auto refusal = switch_refusal(); refusal != TitleVaAliasRefusal::none) return refusal;
        std::unique_lock lock(mutex_);
        std::size_t at = windows_used_;
        for (std::size_t i = 0; i < windows_used_; ++i) {
            if (windows_[i].id == id) {
                at = i;
                break;
            }
        }
        if (at == windows_used_) return TitleVaAliasRefusal::window_not_found;
        for (std::size_t i = 0; i < aliases_used_; ++i)
            if (aliases_[i].window_id == id) return TitleVaAliasRefusal::alias_has_live_views;
        window_bytes_ -= windows_[at].bytes;
        for (std::size_t i = at + 1; i < windows_used_; ++i) windows_[i - 1] = windows_[i];
        windows_[--windows_used_] = {};
        return TitleVaAliasRefusal::none;
    }

    std::size_t window_count() const {
        std::shared_lock lock(mutex_);
        return windows_used_;
    }
    std::size_t alias_count() const {
        std::shared_lock lock(mutex_);
        return aliases_used_;
    }
    std::uint64_t window_bytes() const {
        std::shared_lock lock(mutex_);
        return window_bytes_;
    }

private:
    struct Window {
        std::uint64_t id = 0;
        std::uint64_t device_address = 0;
        std::uint64_t console_va = 0;
        std::uint64_t bytes = 0;
    };
    struct Alias {
        std::uint64_t id = 0;
        std::uint64_t title_va = 0;
        std::uint64_t bytes = 0;
        std::uint64_t window_id = 0;
        std::uint64_t window_offset = 0;
    };

    static bool range_end(std::uint64_t start, std::uint64_t bytes, std::uint64_t& end) {
        if (!bytes || bytes > UINT64_MAX - start) return false;
        end = start + bytes;
        return true;
    }
    static bool range_valid(std::uint64_t start, std::uint64_t bytes) {
        std::uint64_t end = 0;
        return range_end(start, bytes, end);
    }
    TitleVaAliasRefusal switch_refusal() const {
        if (mode_ == TitleVaWindowsSwitch::off) return TitleVaAliasRefusal::switch_off;
        if (mode_ != TitleVaWindowsSwitch::on) return TitleVaAliasRefusal::invalid_switch;
        return TitleVaAliasRefusal::none;
    }
    const Window* find_window(std::uint64_t id) const {
        for (std::size_t i = 0; i < windows_used_; ++i)
            if (windows_[i].id == id) return &windows_[i];
        return nullptr;
    }
    const Alias* find_alias(std::uint64_t id) const {
        for (std::size_t i = 0; i < aliases_used_; ++i)
            if (aliases_[i].id == id) return &aliases_[i];
        return nullptr;
    }
    const Alias* find_alias_for_va(std::uint64_t va) const {
        for (std::size_t i = 0; i < aliases_used_; ++i) {
            const Alias& alias = aliases_[i];
            if (va >= alias.title_va && va - alias.title_va < alias.bytes) return &alias;
        }
        return nullptr;
    }

    TitleVaWindowsSwitch mode_;
    const std::uint64_t arena_device_address_;
    const std::uint64_t arena_bytes_;
    mutable std::shared_mutex mutex_;
    std::array<Window, kMaxWindows> windows_{};
    std::array<Alias, kMaxAliases> aliases_{};
    std::size_t windows_used_ = 0;
    std::size_t aliases_used_ = 0;
    std::uint64_t window_bytes_ = 0;
};

}  // namespace palmetto
