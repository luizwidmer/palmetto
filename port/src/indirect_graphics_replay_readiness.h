#pragma once

#include "command_list.h"

#include <cstdint>

// Strict-mode replay readiness for ExecuteIndirect graphics commands. The IR currently stores the argument/count
// buffers and signature shape, but not the complete graphics state at the call site. This helper is deliberately
// conservative and CPU-only: if an indirect graphics call may execute more than one record, do not emit it until that
// state is captured and replayed. Dispatch signatures and counts 0/1 keep their existing path.
namespace palmetto::indirect_graphics_replay_readiness {

enum class Refusal : std::uint8_t { none, multi_count_graphics_state_not_captured };
enum class Disposition : std::uint8_t { legacy_unchanged, ready, refuse };

struct Assessment {
    Disposition disposition = Disposition::legacy_unchanged;
    Refusal refusal = Refusal::none;
};

constexpr Assessment assess(bool strict, IndirectKind kind, std::uint32_t max_command_count) noexcept {
    if (!strict) return {Disposition::legacy_unchanged, Refusal::none};
    if (kind != IndirectKind::draw && kind != IndirectKind::draw_indexed)
        return {Disposition::ready, Refusal::none};
    if (max_command_count <= 1) return {Disposition::ready, Refusal::none};
    return {Disposition::refuse, Refusal::multi_count_graphics_state_not_captured};
}

constexpr const char* refusal_name(Refusal refusal) noexcept {
    switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::multi_count_graphics_state_not_captured:
        return "indirect_multi_count_graphics_state_not_captured";
    }
    return "unknown_indirect_graphics_replay_refusal";
}

} // namespace palmetto::indirect_graphics_replay_readiness
