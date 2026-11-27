#pragma once

#include "command_list.h"

#include <array>
#include <cstdint>
#include <cstdlib>

// CPU-only, bounded storage and fail-closed preflight for D3D12 viewport/scissor arrays. The live COM shim uses this
// state only when its exact opt-in switch is set; the current Vulkan pipeline path still has a count of one.
namespace palmetto::viewport_replay_readiness {

inline constexpr char kStrictSwitch[] = "PALMETTO_D3D12X_STRICT_VIEWPORTS";
inline constexpr std::uint32_t kD3D12ViewportScissorLimit = 16;

enum class Refusal : std::uint8_t {
    none,
    viewport_count_exceeds_limit,
    scissor_count_exceeds_limit,
    multiple_viewports_not_supported,
    multiple_scissors_not_supported,
};

enum class Disposition : std::uint8_t {
    legacy_unchanged,  // strict switch is off; the caller retains its existing behavior
    ready,             // strict mode: at most one viewport and one scissor rectangle
    refuse,
};

struct Assessment {
    Disposition disposition = Disposition::legacy_unchanged;
    Refusal refusal = Refusal::none;
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
// no scissor rectangles clips all fragments). More than one of either array is not replayed exactly yet.
constexpr Assessment assess(std::uint32_t viewport_count, std::uint32_t scissor_count, bool strict) noexcept {
    if (!strict) return {Disposition::legacy_unchanged, Refusal::none};
    if (viewport_count > kD3D12ViewportScissorLimit)
        return {Disposition::refuse, Refusal::viewport_count_exceeds_limit};
    if (scissor_count > kD3D12ViewportScissorLimit)
        return {Disposition::refuse, Refusal::scissor_count_exceeds_limit};
    if (viewport_count > 1) return {Disposition::refuse, Refusal::multiple_viewports_not_supported};
    if (scissor_count > 1) return {Disposition::refuse, Refusal::multiple_scissors_not_supported};
    return {Disposition::ready, Refusal::none};
}

constexpr const char* refusal_name(Refusal refusal) noexcept {
    switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::viewport_count_exceeds_limit: return "viewport_count_exceeds_d3d12_limit";
    case Refusal::scissor_count_exceeds_limit: return "scissor_count_exceeds_d3d12_limit";
    case Refusal::multiple_viewports_not_supported: return "multiple_viewports_require_dynamic_viewport_count";
    case Refusal::multiple_scissors_not_supported: return "multiple_scissors_require_dynamic_scissor_count";
    }
    return "unknown_viewport_replay_refusal";
}

} // namespace palmetto::viewport_replay_readiness
