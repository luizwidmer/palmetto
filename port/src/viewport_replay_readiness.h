#pragma once

#include "command_list.h"

#include <array>
#include <cstdint>
#include <cstdlib>

// CPU-only, bounded storage and capability-aware preflight for D3D12 viewport/scissor arrays. The live COM shim uses
// this state only when its exact opt-in switch is set; multi-count draws require their own enabled Vulkan pipeline path.
namespace palmetto::viewport_replay_readiness {

inline constexpr char kStrictSwitch[] = "PALMETTO_D3D12X_STRICT_VIEWPORTS";
inline constexpr std::uint32_t kD3D12ViewportScissorLimit = d3d12_viewport_scissor_array_limit;

enum class Refusal : std::uint8_t {
    none,
    viewport_count_exceeds_limit,
    scissor_count_exceeds_limit,
    multiple_viewports_not_supported,
    multiple_scissors_not_supported,
    viewport_count_exceeds_vulkan_limit,
    scissor_count_exceeds_vulkan_limit,
};

enum class Disposition : std::uint8_t {
    legacy_unchanged,  // strict switch is off; the caller retains its existing behavior
    ready,             // strict mode: single-count path or a supported multi-count path
    refuse,
};

struct Assessment {
    Disposition disposition = Disposition::legacy_unchanged;
    Refusal refusal = Refusal::none;
};

// Only a logical device that actually enabled these capabilities may accept a multi-element draw. The caller reports
// the enabled device feature, the EDS with-count command availability, and VkPhysicalDeviceLimits::maxViewports.
struct ReplayCapability {
    bool multi_viewport = false;
    bool dynamic_viewport_scissor_with_count = false;
    std::uint32_t max_viewport_count = 1;
};

// This switch is deliberately exact: only the single byte "1" opts in. Empty, unset, "0" and other spellings
// leave the compatibility path alone.
constexpr bool switch_value_enabled(const char* value) noexcept {
    return value && value[0] == '1' && value[1] == '\0';
}

inline bool strict_enabled() noexcept {
    return switch_value_enabled(std::getenv(kStrictSwitch));
}

// Fixed-size, per-command-list copies of the arrays. The COM shim does not retain the caller's pointers. These helpers
// validate count before reading input and leave state unchanged on refusal, so an over-limit count cannot over-read.
struct Arrays {
    std::array<Viewport, kD3D12ViewportScissorLimit> viewports{};
    std::array<Rect, kD3D12ViewportScissorLimit> scissors{};
    std::uint32_t viewport_count = 0;
    std::uint32_t scissor_count = 0;

    constexpr void reset() noexcept {
        viewports.fill(Viewport{});
        scissors.fill(Rect{});
        viewport_count = 0;
        scissor_count = 0;
    }

    constexpr bool capture_viewports(std::uint32_t count, const Viewport* values) noexcept {
        if (count > kD3D12ViewportScissorLimit || (count && !values)) return false;
        for (std::uint32_t i = 0; i < count; ++i) viewports[i] = values[i];
        for (std::uint32_t i = count; i < kD3D12ViewportScissorLimit; ++i) viewports[i] = Viewport{};
        viewport_count = count;
        return true;
    }

    constexpr bool capture_scissors(std::uint32_t count, const Rect* values) noexcept {
        if (count > kD3D12ViewportScissorLimit || (count && !values)) return false;
        for (std::uint32_t i = 0; i < count; ++i) scissors[i] = values[i];
        for (std::uint32_t i = count; i < kD3D12ViewportScissorLimit; ++i) scissors[i] = Rect{};
        scissor_count = count;
        return true;
    }
};

// Counts only; no input pointer is inspected. Zero counts are valid existing D3D12 states (no viewport means no draw;
// no scissor rectangles clips all fragments). The compatibility overload remains fail-closed; the capability overload
// admits count > 1 only when both EDS with-count states and multiViewport are enabled and maxViewports covers them.
constexpr Assessment assess(std::uint32_t viewport_count, std::uint32_t scissor_count, bool strict,
                            ReplayCapability capability) noexcept {
    if (!strict) return {Disposition::legacy_unchanged, Refusal::none};
    if (viewport_count > kD3D12ViewportScissorLimit)
        return {Disposition::refuse, Refusal::viewport_count_exceeds_limit};
    if (scissor_count > kD3D12ViewportScissorLimit)
        return {Disposition::refuse, Refusal::scissor_count_exceeds_limit};
    if (viewport_count > 1 || scissor_count > 1) {
        if (!capability.dynamic_viewport_scissor_with_count || !capability.multi_viewport) {
            if (viewport_count > 1) return {Disposition::refuse, Refusal::multiple_viewports_not_supported};
            return {Disposition::refuse, Refusal::multiple_scissors_not_supported};
        }
        if (viewport_count > capability.max_viewport_count)
            return {Disposition::refuse, Refusal::viewport_count_exceeds_vulkan_limit};
        if (scissor_count > capability.max_viewport_count)
            return {Disposition::refuse, Refusal::scissor_count_exceeds_vulkan_limit};
    }
    return {Disposition::ready, Refusal::none};
}

constexpr Assessment assess(std::uint32_t viewport_count, std::uint32_t scissor_count, bool strict) noexcept {
    return assess(viewport_count, scissor_count, strict, ReplayCapability{});
}

constexpr const char* refusal_name(Refusal refusal) noexcept {
    switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::viewport_count_exceeds_limit: return "viewport_count_exceeds_d3d12_limit";
    case Refusal::scissor_count_exceeds_limit: return "scissor_count_exceeds_d3d12_limit";
    case Refusal::multiple_viewports_not_supported: return "multiple_viewports_require_dynamic_viewport_count";
    case Refusal::multiple_scissors_not_supported: return "multiple_scissors_require_dynamic_scissor_count";
    case Refusal::viewport_count_exceeds_vulkan_limit: return "viewport_count_exceeds_vulkan_max_viewport_count";
    case Refusal::scissor_count_exceeds_vulkan_limit: return "scissor_count_exceeds_vulkan_max_viewport_count";
    }
    return "unknown_viewport_replay_refusal";
}

} // namespace palmetto::viewport_replay_readiness

// Strict multi-count ExecuteIndirect draws with viewport/scissor arrays are not replay-ready yet. Unlike a direct Draw,
// the current ExecuteIndirect record has no snapshot of the graphics state or arrays that the indirect draw consumes.
// This pure gate is called only in strict mode; switch-off keeps the pre-existing command path untouched.
namespace palmetto::indirect_viewport_replay_readiness {

enum class Refusal : std::uint8_t { none, multi_count_state_not_captured };
enum class Disposition : std::uint8_t { legacy_unchanged, ready, refuse };

struct Assessment {
    Disposition disposition = Disposition::legacy_unchanged;
    Refusal refusal = Refusal::none;
};

constexpr Assessment assess(std::uint32_t max_command_count, std::uint32_t viewport_count,
                            std::uint32_t scissor_count, bool strict) noexcept {
    if (!strict) return {Disposition::legacy_unchanged, Refusal::none};
    if (max_command_count > 1 && (viewport_count > 1 || scissor_count > 1))
        return {Disposition::refuse, Refusal::multi_count_state_not_captured};
    return {Disposition::ready, Refusal::none};
}

constexpr const char* refusal_name(Refusal refusal) noexcept {
    switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::multi_count_state_not_captured:
        return "indirect_multi_count_viewport_state_not_captured";
    }
    return "unknown_indirect_viewport_replay_refusal";
}

} // namespace palmetto::indirect_viewport_replay_readiness
