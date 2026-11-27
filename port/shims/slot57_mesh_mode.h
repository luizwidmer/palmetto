#pragma once

#include <cstdint>

namespace rt::d3d12_xs {

// Pure policy helpers for slot 57. Keeping the switch rule and the API labels here lets the GPU-free tests pin the
// opt-in/off boundary and the two distinct device entry points without constructing a PC D3D12 device.
enum class Slot57CreateApi : std::uint8_t { GraphicsDescription, MeshStream };

inline constexpr bool slot57_mesh_stream_opt_in(bool has_mesh_stage, const char* value) noexcept {
    return has_mesh_stage && value && value[0] == '1' && value[1] == '\0';
}

inline constexpr const char* slot57_create_api_label(Slot57CreateApi api) noexcept {
    return api == Slot57CreateApi::MeshStream ? "PC ID3D12Device2::CreatePipelineState"
                                              : "PC CreateGraphicsPipelineState";
}

}  // namespace rt::d3d12_xs
