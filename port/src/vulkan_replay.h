#pragma once

#include "resource_tiling.h"

// Replays a closed CommandList (command_list.h) into a Vulkan command buffer.
//
// Layouts. With a LayoutTracker, every image's current Vulkan layout is known per subresource, and barriers and
// clears start from it rather than from what the application claims. That is how D3D12's implicit state promotion
// and decay are honoured: a texture in `common` sits in GENERAL, which every promotable access (shader read, copy)
// can use directly, so a later barrier whose `before` names the promoted state still starts from GENERAL; decay
// back to `common` changes no layout. Buffers have no layouts, and simultaneous-access textures stay in GENERAL
// for their whole life, so their decay is free too. Without a tracker, `before` is trusted as the D3D12 runtime
// trusts it.
//
// Depth/stencil planes. With separate_depth_stencil_layouts (Vulkan 1.2 separateDepthStencilLayouts, enabled on
// the device), each plane has its own layout, so a transition or clear of one plane leaves the other alone. Without
// it, both planes always move together.
//
// Requirements on the device and images:
// - a queue with graphics support (the stage masks use ALL_GRAPHICS);
// - images cleared without rectangles have TRANSFER_DST usage; images cleared with rectangles have attachment usage
//   and image_view must return views for them (dynamicRendering, Vulkan 1.3); reinterpreted whole-view clears
//   use the same attachment path because vkCmdClearColorImage has no view-format argument;
// - enhanced barriers need pipeline_barrier2 (synchronization2, Vulkan 1.3);
// - unordered-access clears need fill_buffer, update_buffer and copy_buffer for buffers; texture views cleared with
//   rectangles need color_attachment usage on the image (ResourceBinding::color_attachment);
// - query commands need the query functions, fill_buffer and query_pool; resolve destinations have TRANSFER_DST
//   usage and binary occlusion resolves need resolve_binary_occlusion.
// - predicated buffer copies need the byte-copy helper and buffers with STORAGE_BUFFER usage; helper descriptors
//   must remain alive through submission completion (VulkanQueue owns them).
// A list that needs a missing function fails with ReplayResult::unsupported before anything is written.

#include "command_list.h"
#include "depth24.h"
#include "gfx10_descriptors.h"
#include "uav_clear.h"

#include <vulkan/vulkan.h>
#include "vulkan_predicate.h"
#include "vulkan_predicated_copy.h"
#include "vulkan_indirect_dispatch_count.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace palmetto {

// What the resolver knows about a resource. image == VK_NULL_HANDLE means a buffer in the shared GPU address
// space: its transitions become global memory barriers.
struct ResourceBinding {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t mip_levels = 1;
    std::uint32_t array_layers = 1;
    bool presentable = false;          // a swapchain image: common/present maps to PRESENT_SRC_KHR
    bool simultaneous_access = false;  // D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS: always GENERAL
    bool not_zeroed = false;           // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED: first use skips the zero fill
    // A descriptor in the bindless set 0 has one layout, but D3D12 lets a texture be sampled both in a
    // shader-resource state and while promoted from common (GENERAL). With this set, the shader-resource states
    // use GENERAL too, so the descriptor's layout is right in both.
    bool sampled_in_general = false;
    VkBuffer buffer = VK_NULL_HANDLE;  // a buffer resource, for copies
    VkDeviceSize buffer_offset = 0;    // where it starts in `buffer` (the GPU address arena holds many)
    VkDeviceSize size = 0;             // its size in bytes
    const std::uint8_t* host = nullptr;  // mapped CPU-written bytes (UPLOAD/write-back), for small CPU-side reads
    std::uint64_t va = 0;              // a buffer's console VA (GetGPUVirtualAddress), for vertex/index views
    std::uint32_t depth = 1;
    bool volume = false;               // one array layer, including at mips with a single slice
    bool line = false;                 // a 1D image (TEXTURE1D): its attachment views are 1D arrays
    std::uint32_t samples = 1;         // more than one: a multisampled 2D texture
    bool typeless = false;  // A TYPELESS color resource: views name a format of its family (D3D-275).
    bool castable_view_formats = false;  // A creation-time castable list made the image mutable (D3D-85/D3D-90),
                                         // so views may reinterpret it as typeless resources do.
    // A UAV texture stored as a mutable unsigned image because its format has no storage support (R16_UINT for
    // A1R5G5B5): the format its render-target views use. UNDEFINED otherwise.
    VkFormat uint_backed = VK_FORMAT_UNDEFINED;
    // D24_UNORM_S8_UINT stored as D32_SFLOAT_S8_UINT (the GPU has no D24): footprint copies of the depth plane convert
    // (depth24.h) and clears quantise to 24 bits.
    bool depth24_as_float = false;
    bool depth24 = false;  // a D24 family format, native or not: depth clears are quantised to 24 bits first
    // A D3D12 R32G32B32 texture (12 bytes a texel) held in an R32G32B32A32 image (16): footprints keep D3D12's 12
    // bytes, and copies between it and a buffer are repacked by the depth24 pass (Depth24Job::rgb96).
    bool rgb96 = false;
    bool color_attachment = false;     // has COLOR_ATTACHMENT usage: unordered-access rectangle clears need it
    bool sampled = false;              // has SAMPLED usage (no DENY_SHADER_RESOURCE): the present compose pass reads it
    bool host_readable = false;        // D3D12_HEAP_TYPE_READBACK (or CUSTOM write-back): needs host-read flush
    bool sparse_buffer = false;
};

struct VulkanAccess {
    VkImageLayout layout;
    VkAccessFlags access;
    VkPipelineStageFlags stages;
};

inline constexpr VkPipelineStageFlags shader_stages =
    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
inline constexpr VkImageAspectFlags depth_stencil_aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

// Maps a legacy D3D12_RESOURCE_STATES combination to the Vulkan layout, accesses and stages that cover it.
// Read-only combinations share one layout when every bit agrees on it (depth_read with shader reads stays in
// DEPTH_STENCIL_READ_ONLY_OPTIMAL, which can be sampled); otherwise they fall back to GENERAL.
// Buffer-only and extension states map to GENERAL with conservative memory access.
// conditional_rendering: VK_EXT_conditional_rendering is enabled. PREDICATION shares its bit with
// INDIRECT_ARGUMENT, and conditional rendering reads the predicate in a stage of its own, so the bit covers both.
inline VulkanAccess vulkan_access(std::uint32_t states, bool presentable, bool conditional_rendering = false) {
    namespace s = d3d12_state;
    constexpr VkAccessFlags any_memory = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if (states == s::common) {
        if (presentable) return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        return {VK_IMAGE_LAYOUT_GENERAL, any_memory, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    }

    struct Bit {
        std::uint32_t state;
        VkImageLayout layout;
        VkAccessFlags access;
        VkPipelineStageFlags stages;
    };
    constexpr VkPipelineStageFlags fragment_tests =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    static constexpr Bit bits[] = {
        // A root CBV can be loaded through a shader buffer device address.
        {s::vertex_and_constant_buffer, VK_IMAGE_LAYOUT_GENERAL,
         VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | shader_stages},
        {s::index_buffer, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_INDEX_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT},
        {s::render_target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
        {s::unordered_access, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
         shader_stages},
        {s::depth_write, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, fragment_tests},
        {s::depth_read, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
         fragment_tests},
        {s::non_pixel_shader_resource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
         shader_stages},
        {s::pixel_shader_resource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT},
        {s::stream_out, VK_IMAGE_LAYOUT_GENERAL, any_memory, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT},
        {s::indirect_argument, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT},
        {s::copy_dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT},
        {s::copy_source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT},
        {s::resolve_dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT},
        {s::resolve_source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT},
        {s::raytracing_acceleration_structure, VK_IMAGE_LAYOUT_GENERAL, any_memory,
         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT},
        {s::shading_rate_source, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_READ_BIT,
         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT},
    };

    const auto merge = [](VkImageLayout a, VkImageLayout b) {
        if (a == VK_IMAGE_LAYOUT_UNDEFINED || a == b) return b;
        const auto pair = [&](VkImageLayout x, VkImageLayout y) { return (a == x && b == y) || (a == y && b == x); };
        if (pair(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        return VK_IMAGE_LAYOUT_GENERAL;
    };

    VulkanAccess out{VK_IMAGE_LAYOUT_UNDEFINED, 0, 0};
    std::uint32_t known = 0;
    for (const auto& bit : bits) {
        if (!(states & bit.state)) continue;
        known |= bit.state;
        out.layout = merge(out.layout, bit.layout);
        out.access |= bit.access;
        out.stages |= bit.stages;
    }
    if (states & ~known) {
        out.layout = VK_IMAGE_LAYOUT_GENERAL;
        out.access |= any_memory;
        out.stages |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    if (conditional_rendering && (states & s::predication)) {
        out.access |= VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT;
        out.stages |= VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT;
    }
    return out;
}

// Enhanced barriers: D3D12_BARRIER_SYNC -> VkPipelineStageFlags2. split (the end half of a split barrier) waits
// for everything, since the begin half's scope is not kept.
inline VkPipelineStageFlags2 vulkan_sync(std::uint32_t sync) {
    namespace y = d3d12_sync;
    if (sync == y::none) return VK_PIPELINE_STAGE_2_NONE;
    constexpr VkPipelineStageFlags2 pre_raster = VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
    struct Bit {
        std::uint32_t sync;
        VkPipelineStageFlags2 stages;
    };
    static constexpr Bit bits[] = {
        {y::all, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
        {y::draw, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT},
        {y::index_input, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT},
        {y::vertex_shading, pre_raster | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT},
        {y::pixel_shading, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT},
        {y::depth_stencil,
         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT},
        {y::render_target, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
        {y::compute_shading, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
        {y::copy, VK_PIPELINE_STAGE_2_COPY_BIT},
        {y::resolve, VK_PIPELINE_STAGE_2_RESOLVE_BIT},
        {y::execute_indirect, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT},
        {y::all_shading, pre_raster | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
        {y::non_pixel_shading, pre_raster | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
        {y::clear_unordered_access_view, VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
    };
    VkPipelineStageFlags2 out = 0;
    std::uint32_t known = 0;
    for (const auto& bit : bits) {
        if (!(sync & bit.sync)) continue;
        known |= bit.sync;
        out |= bit.stages;
    }
    // Ray tracing, video and split have no narrower equivalent without extensions.
    if (sync & ~known) out |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return out;
}

// D3D12_BARRIER_ACCESS -> VkAccessFlags2. common means "any access the layout allows".
inline VkAccessFlags2 vulkan_barrier_access(std::uint32_t access) {
    namespace a = d3d12_access;
    constexpr VkAccessFlags2 any_memory = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    if (access & a::no_access) return VK_ACCESS_2_NONE;
    if (access == a::common) return any_memory;
    struct Bit {
        std::uint32_t access;
        VkAccessFlags2 flags;
    };
    static constexpr Bit bits[] = {
        {a::vertex_buffer, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT},
        {a::constant_buffer, VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT},
        {a::index_buffer, VK_ACCESS_2_INDEX_READ_BIT},
        {a::render_target, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
        {a::unordered_access, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT},
        {a::depth_stencil_write,
         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
        {a::depth_stencil_read, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT},
        {a::shader_resource, VK_ACCESS_2_SHADER_READ_BIT},
        {a::stream_output, VK_ACCESS_2_MEMORY_WRITE_BIT},
        {a::indirect_argument, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT},
        {a::copy_dest, VK_ACCESS_2_TRANSFER_WRITE_BIT},
        {a::copy_source, VK_ACCESS_2_TRANSFER_READ_BIT},
        {a::resolve_dest, VK_ACCESS_2_TRANSFER_WRITE_BIT},
        {a::resolve_source, VK_ACCESS_2_TRANSFER_READ_BIT},
        {a::raytracing_acceleration_structure_read, VK_ACCESS_2_MEMORY_READ_BIT},
        {a::raytracing_acceleration_structure_write, VK_ACCESS_2_MEMORY_WRITE_BIT},
        {a::shading_rate_source, VK_ACCESS_2_MEMORY_READ_BIT},
    };
    VkAccessFlags2 out = 0;
    std::uint32_t known = 0;
    for (const auto& bit : bits) {
        if (!(access & bit.access)) continue;
        known |= bit.access;
        out |= bit.flags;
    }
    if (access & ~known) out |= any_memory;  // video and unknown bits
    return out;
}

// For accesses that vulkan_barrier_access produces: the stages Vulkan lets perform them (the "supported
// access types" table of the synchronization2 chapter, with the meta stages that contain them), and the narrowest
// stage to add when a barrier's stages have none of those. MEMORY_READ/WRITE are valid in every stage.
struct AccessStages {
    VkAccessFlags2 access;
    VkPipelineStageFlags2 supported;
    VkPipelineStageFlags2 narrowest;
};

inline constexpr VkPipelineStageFlags2 shader_stages2 =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
    VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
    VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

inline constexpr AccessStages access_stages[] = {
    {VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
     VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
     VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT},
    {VK_ACCESS_2_INDEX_READ_BIT,
     VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
     VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT},
    {VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
     VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
         VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
     VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT},
    // Shader accesses: D3D12 does not say which shader stage, so every one that can run (all shading).
    {VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, shader_stages2,
     VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
    {VK_ACCESS_2_SHADER_READ_BIT, shader_stages2,
     VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
    {VK_ACCESS_2_SHADER_WRITE_BIT, shader_stages2,
     VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT},
    {VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
    {VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
    {VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
         VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT},
    {VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
         VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT},
    // Transfers: ALL_GRAPHICS does not contain the transfer stages, and CLEAR only writes.
    {VK_ACCESS_2_TRANSFER_READ_BIT,
     VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT},
    {VK_ACCESS_2_TRANSFER_WRITE_BIT,
     VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
         VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT},
    {VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT,
     VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
     VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT},
};

// One side of an enhanced barrier: D3D12 sync and access to a Vulkan stage mask and access mask that Vulkan accepts
// together. D3D12 lets an access be named with a sync scope whose Vulkan stages cannot perform it (SYNC_ALL_SHADING
// with ACCESS_VERTEX_BUFFER is valid D3D12, but VERTEX_ATTRIBUTE_READ needs VERTEX_ATTRIBUTE_INPUT). The stages are
// widened by the narrowest stage that can, never the access dropped: the dependency still covers every access the
// application named, only over a slightly larger scope. With no stages (SYNC_NONE) there is no access either.
struct BarrierScope {
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 access;
};

// conditional_rendering: VK_EXT_conditional_rendering is enabled. SYNC_PREDICATION and ACCESS_PREDICATION share
// their bits with EXECUTE_INDIRECT and INDIRECT_ARGUMENT, and conditional rendering reads the predicate in a stage
// of its own, so those bits cover it too.
inline BarrierScope vulkan_barrier_scope(std::uint32_t sync, std::uint32_t access, bool conditional_rendering = false) {
    BarrierScope out{vulkan_sync(sync), 0};
    if (!out.stages) return out;
    if (conditional_rendering && (sync & d3d12_sync::predication))
        out.stages |= VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
    out.access = vulkan_barrier_access(access);
    if (conditional_rendering && (access & d3d12_access::predication) && !(access & d3d12_access::no_access))
        out.access |= VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT;
    for (const AccessStages& entry : access_stages)
        if ((out.access & entry.access) && !(out.stages & entry.supported)) out.stages |= entry.narrowest;
    return out;
}

// D3D12_BARRIER_LAYOUT -> VkImageLayout. Queue-specific layouts map like their generic forms.
inline VkImageLayout vulkan_layout(std::uint32_t layout, bool presentable) {
    namespace l = d3d12_layout;
    switch (layout) {
    case l::undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
    case l::common: return presentable ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL;
    case l::render_target: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case l::depth_stencil_write: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case l::depth_stencil_read: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    case l::shader_resource:
    case l::direct_queue_shader_resource:
    case l::compute_queue_shader_resource: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case l::copy_source:
    case l::resolve_source:
    case l::direct_queue_copy_source:
    case l::compute_queue_copy_source: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case l::copy_dest:
    case l::resolve_dest:
    case l::direct_queue_copy_dest:
    case l::compute_queue_copy_dest: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    default: return VK_IMAGE_LAYOUT_GENERAL;  // generic read, UAV, shading rate, video, *_common
    }
}

// The layout an image actually uses: simultaneous-access images live in GENERAL, and so do images in a
// shader-resource state when set 0 names them (sampled_in_general).
inline VkImageLayout image_layout(const ResourceBinding& resource, VkImageLayout layout) {
    if (resource.simultaneous_access && layout != VK_IMAGE_LAYOUT_UNDEFINED &&
        layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        return VK_IMAGE_LAYOUT_GENERAL;
    if (resource.sampled_in_general && layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return VK_IMAGE_LAYOUT_GENERAL;
    return layout;
}

// Bytes per texel, or per 4x4 block for block-compressed formats, of one aspect as copies lay it out in a buffer
// (D3D12 footprints and Vulkan buffer-image copies agree on these). bytes == 0: not a format copies support yet.
struct BlockFormat {
    std::uint32_t bytes = 0;
    std::uint32_t block = 1;  // texels per block edge
};

inline BlockFormat block_format(VkFormat format, VkImageAspectFlags aspect) {
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) return {1, 1};
    switch (format) {
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_UINT: return {16, 1};
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: return {8, 1};
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:  // depth aspect
    case VK_FORMAT_D24_UNORM_S8_UINT: return {4, 1};
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_A4R4G4B4_UNORM_PACK16:
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
    case VK_FORMAT_D16_UNORM: return {2, 1};
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT: return {1, 1};
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK: return {8, 4};
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: return {16, 4};
    default: return {};
    }
}

// The same for a resource as D3D12 lays it out, which for an rgb96 texture is not its image's layout.
inline constexpr std::uint32_t rgb96_bytes = 12;
inline BlockFormat copy_block_format(const ResourceBinding& resource, VkImageAspectFlags aspect) {
    BlockFormat format = block_format(resource.format, aspect);
    if (resource.rgb96 && format.bytes) format.bytes = rgb96_bytes;
    return format;
}

inline bool two_planes(const ResourceBinding& resource) {
    return (resource.aspects & depth_stencil_aspects) == depth_stencil_aspects;
}

// D3D12 copies between a depth format and a colour format of the same typeless family, bit for bit: D32_FLOAT with
// R32_FLOAT/UINT/SINT (R32_TYPELESS) and D16_UNORM with R16_FLOAT/UNORM/SNORM/UINT/SINT (R16_TYPELESS). Single-sampled, single-plane depth only: D24 and D32 with stencil have two planes, and a D24
// stored as D32 would need the depth24 conversion. Vulkan's vkCmdCopyImage refuses mismatched aspects, so the replay
// stages these through a buffer.
inline bool depth_colour_pair(const ResourceBinding& a, const ResourceBinding& b) {
    const auto family = [](VkFormat format) -> int {
        switch (format) {
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT: return 32;
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT: return 16;
        default: return 0;
        }
    };
    const auto depth_only = [](const ResourceBinding& r) {
        return r.aspects == VK_IMAGE_ASPECT_DEPTH_BIT && !r.depth24_as_float;
    };
    const ResourceBinding *depth = &a, *colour = &b;
    if (!depth_only(*depth)) std::swap(depth, colour);
    return depth_only(*depth) && colour->aspects == VK_IMAGE_ASPECT_COLOR_BIT && a.image && b.image &&
           a.samples == 1 && b.samples == 1 && !a.volume && !b.volume && family(a.format) &&
           family(a.format) == family(b.format);
}

// Decodes a D3D12 subresource index. Combined depth-stencil images have two planes (0 depth, 1 stencil);
// every other image has one.
inline bool image_range(const ResourceBinding& resource, std::uint32_t subresource, VkImageSubresourceRange& out) {
    if (!resource.mip_levels || !resource.array_layers) return false;
    if (subresource == all_subresources) {
        out = {resource.aspects, 0, resource.mip_levels, 0, resource.array_layers};
        return true;
    }
    const std::uint64_t per_plane = std::uint64_t(resource.mip_levels) * resource.array_layers;
    const std::uint64_t plane = subresource / per_plane;
    if (plane >= (two_planes(resource) ? 2u : 1u)) return false;
    const auto within = static_cast<std::uint32_t>(subresource % per_plane);
    const VkImageAspectFlags aspect =
        two_planes(resource) ? (plane ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT) : resource.aspects;
    out = {aspect, within % resource.mip_levels, 1, within / resource.mip_levels, 1};
    return true;
}

// Decodes D3D12_BARRIER_SUBRESOURCE_RANGE.
inline bool barrier_range(const ResourceBinding& resource, const BarrierSubresourceRange& range,
                          VkImageSubresourceRange& out) {
    if (!range.mip_count) return image_range(resource, range.index_or_first_mip, out);
    const std::uint32_t planes = two_planes(resource) ? 2 : 1;
    if (range.index_or_first_mip >= resource.mip_levels ||
        range.mip_count > resource.mip_levels - range.index_or_first_mip)
        return false;
    if (range.first_layer >= resource.array_layers || !range.layer_count ||
        range.layer_count > resource.array_layers - range.first_layer)
        return false;
    if (range.first_plane >= planes || !range.plane_count || range.plane_count > planes - range.first_plane)
        return false;
    VkImageAspectFlags aspects = resource.aspects;
    if (planes == 2) {
        aspects = 0;
        for (std::uint32_t p = range.first_plane; p < range.first_plane + range.plane_count; ++p)
            aspects |= p ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    out = {aspects, range.index_or_first_mip, range.mip_count, range.first_layer, range.layer_count};
    return true;
}

// A part of an image range with one current layout.
struct LayoutPiece {
    VkImageSubresourceRange range;
    VkImageLayout layout;
};

// The layout pieces of one range: nearly always one (the tracked layouts agree over the range) or two (depth and
// stencil apart), so those live inline and a barrier or clear makes no heap allocation; more spill to the heap.
class LayoutPieces {
public:
    LayoutPiece* begin() { return data(); }
    LayoutPiece* end() { return data() + size_; }
    const LayoutPiece* begin() const { return data(); }
    const LayoutPiece* end() const { return data() + size_; }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    LayoutPiece& operator[](std::size_t i) { return data()[i]; }
    const LayoutPiece& operator[](std::size_t i) const { return data()[i]; }

    void clear() {
        size_ = 0;
        heap_.clear();
    }
    void push_back(const LayoutPiece& piece) {
        if (heap_.empty() && size_ < inline_.size()) {
            inline_[size_++] = piece;
            return;
        }
        if (heap_.empty()) heap_.assign(inline_.begin(), inline_.begin() + size_);
        heap_.push_back(piece);
        ++size_;
    }

private:
    LayoutPiece* data() { return heap_.empty() ? inline_.data() : heap_.data(); }
    const LayoutPiece* data() const { return heap_.empty() ? inline_.data() : heap_.data(); }

    std::array<LayoutPiece, 2> inline_{};
    std::vector<LayoutPiece> heap_;
    std::size_t size_ = 0;
};

// The current Vulkan layout of every tracked image subresource. Shared by all queues of a device; lists are
// emitted in submission order. Images are tracked from creation (their initial state) until destruction.
class LayoutTracker {
public:
    explicit LayoutTracker(bool separate_depth_stencil_layouts = false)
        : separate_(separate_depth_stencil_layouts) {}

    bool separate_depth_stencil_layouts() const { return separate_; }

    void track(ResourceId id, const ResourceBinding& resource, VkImageLayout layout) {
        Entry entry{resource.mip_levels, resource.array_layers, two_planes(resource) ? 2u : 1u,
                    separate_ && two_planes(resource), {}};
        entry.layouts.assign(std::size_t(entry.mips) * entry.layers * (entry.two_planes ? 2 : 1), layout);
        entries_[id] = std::move(entry);
    }

    void forget(ResourceId id) { entries_.erase(id); }
    bool tracked(ResourceId id) const { return entries_.count(id) != 0; }

    // Current layout of one D3D12 subresource; UNDEFINED when the resource or index is unknown.
    VkImageLayout layout(ResourceId id, std::uint32_t subresource) const {
        const auto it = entries_.find(id);
        if (it == entries_.end()) return VK_IMAGE_LAYOUT_UNDEFINED;
        const Entry& e = it->second;
        const std::uint32_t per_plane = e.mips * e.layers;
        const std::uint32_t plane = subresource / per_plane;
        if (plane >= e.planes) return VK_IMAGE_LAYOUT_UNDEFINED;
        return e.layouts[(e.two_planes ? plane : 0) * per_plane + subresource % per_plane];  // merged planes share
    }

    // The current layouts over a range: one piece when they all agree, else one per subresource.
    // Out is LayoutPieces or std::vector<LayoutPiece>.
    template<typename Out>
    bool pieces(ResourceId id, const VkImageSubresourceRange& range, Out& out) const {
        const auto it = entries_.find(id);
        if (it == entries_.end()) return false;
        const Entry& e = it->second;
        out.clear();
        bool uniform = true;
        VkImageLayout first = VK_IMAGE_LAYOUT_UNDEFINED;
        bool any = false;
        visit(e, range, [&](std::size_t index, VkImageAspectFlags, std::uint32_t, std::uint32_t) {
            if (!any) first = e.layouts[index];
            uniform = uniform && e.layouts[index] == first;
            any = true;
        });
        if (uniform) {
            out.push_back({range, first});
            return true;
        }
        visit(e, range, [&](std::size_t index, VkImageAspectFlags aspect, std::uint32_t mip, std::uint32_t layer) {
            out.push_back({{aspect, mip, 1, layer, 1}, e.layouts[index]});
        });
        return true;
    }

    void set(ResourceId id, const VkImageSubresourceRange& range, VkImageLayout layout) {
        const auto it = entries_.find(id);
        if (it == entries_.end()) return;
        Entry& e = it->second;
        visit(e, range, [&](std::size_t index, VkImageAspectFlags, std::uint32_t, std::uint32_t) {
            e.layouts[index] = layout;
        });
    }

private:
    struct Entry {
        std::uint32_t mips;
        std::uint32_t layers;
        std::uint32_t planes;  // of the resource
        bool two_planes;       // depth and stencil tracked apart
        std::vector<VkImageLayout> layouts;
    };

    template<typename F>
    static void visit(const Entry& e, const VkImageSubresourceRange& range, F&& f) {
        const std::uint32_t mip_end = range.levelCount == VK_REMAINING_MIP_LEVELS ? e.mips
                                                                                  : range.baseMipLevel + range.levelCount;
        const std::uint32_t layer_end = range.layerCount == VK_REMAINING_ARRAY_LAYERS
                                            ? e.layers
                                            : range.baseArrayLayer + range.layerCount;
        for (std::uint32_t plane = 0; plane < (e.two_planes ? 2u : 1u); ++plane) {
            VkImageAspectFlags aspect = range.aspectMask;
            if (e.two_planes) {
                aspect = plane ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
                if (!(range.aspectMask & aspect)) continue;
            }
            for (std::uint32_t layer = range.baseArrayLayer; layer < std::min(layer_end, e.layers); ++layer)
                for (std::uint32_t mip = range.baseMipLevel; mip < std::min(mip_end, e.mips); ++mip)
                    f((std::size_t(plane) * e.layers + layer) * e.mips + mip, aspect, mip, layer);
        }
    }

    bool separate_;
    std::unordered_map<ResourceId, Entry> entries_;
};

// Each physical slot records one non-overlapping interval. Logical queries can share slots.
struct OcclusionSegmentPool {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDestroyQueryPool destroy = nullptr;
    VkQueryPool pool = VK_NULL_HANDLE;
    // Pipeline-statistics segments only: a second pool with the counters a mesh draw allows (D3D-291), and for each
    // segment index whether it runs there. Empty when no segment of the submission holds a mesh draw.
    VkQueryPool mesh_pool = VK_NULL_HANDLE;
    std::vector<std::uint8_t> mesh;

    OcclusionSegmentPool() = default;
    OcclusionSegmentPool(const OcclusionSegmentPool&) = delete;
    OcclusionSegmentPool& operator=(const OcclusionSegmentPool&) = delete;
    ~OcclusionSegmentPool() {
        if (pool) destroy(device, pool, nullptr);
        if (mesh_pool) destroy(device, mesh_pool, nullptr);
    }
};

struct OcclusionResult {
    std::shared_ptr<const OcclusionSegmentPool> segments;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

// A D3D12 query heap on Vulkan. The host tracks which queries hold a result. A resolve of a query that
// was never ended writes zeros instead of waiting on it.
struct QueryPool {
    // Null for a stream-output statistics heap: the layer has no stream output (PSOs with StreamOutput entries are
    // refused), so every SO_STATISTICS_STREAMn query counts 0 written and 0 needed, and a resolve fills zeros.
    VkQueryPool pool = VK_NULL_HANDLE;
    std::uint32_t heap_type = 0;  // d3d12_query_heap
    std::uint32_t count = 0;
    bool precise = false;         // occlusionQueryPrecise: D3D12_QUERY_TYPE_OCCLUSION counts exact samples
    std::vector<bool> ended;
    std::unordered_map<std::uint32_t, OcclusionResult> occlusion;
};

// A compute pipeline as replay needs it. code_base is the console VA of the shader's FSSM blob (push-constant
// dwords 34-35), 0 when the shader does not read it.
struct ComputePipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::uint64_t code_base = 0;
    VkDescriptorSet bindless_set = VK_NULL_HANDLE;  // set 0 of the layout, bound at every dispatch; null: none
};

// A graphics pipeline as replay needs it: the formats its attachments must have and which vertex-input slots it
// reads. code_base: as for compute (one per pipeline; two stages that both read theirs are refused, question 14).
struct GraphicsPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderStageFlags push_stages = 0;   // the layout's push-constant range; vkCmdPushConstants must name the same
    std::uint64_t code_base = 0;
    VkDescriptorSet bindless_set = VK_NULL_HANDLE;  // set 0 of the layout, bound at every draw; null: none
    std::vector<VkFormat> color_formats;  // by render-target slot
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags depth_aspects = 0;
    std::uint32_t vertex_slots = 0;       // bit per input slot the input layout uses
    std::uint32_t zero_step_slots = 0;    // per-instance slots with InstanceDataStepRate 0: stride 0 at draw time
    std::uint32_t strip_cut = 0;          // D3D12_INDEX_BUFFER_STRIP_CUT_VALUE (0: off, 1: 0xFFFF, 2: 0xFFFFFFFF)
    bool mesh = false;                    // a mesh shader instead of a vertex shader: drawn by DispatchMesh only
    // Which user-data dwords hold a descriptor table's 64-bit VA, from the root signature's parameter types
    // (RootArgumentLayout; the graphics layout gives every table two dwords). The title's descriptor-heap and
    // placed-buffer VAs overlap in its address space, so a value in both ranges is rebased by what the parameter says
    // it is (docs/page_address_space.md).
    std::uint32_t table_windows = 0;
    // The same pipeline with some attachment formats VK_FORMAT_UNDEFINED, for a draw that leaves those slots
    // unbound (D3D12 allows it; dynamic rendering needs the formats to agree). Bit i: colour slot i, bit 8: depth
    // and stencil. Null when it cannot be built.
    static constexpr std::uint32_t unbound_depth = 1u << 8;
    std::function<VkPipeline(std::uint32_t unbound)> variant;
    // Diagnostic (docs/frame_capture.md): the same pipeline with a substituted pixel shader (the frame dump's
    // constant-output PS control). The shim builds it from the same recipe, so the vertex stage, input layout and
    // fixed-function state are the title's; cull_none overrides only the substituted variant's cull mode, so a culling
    // vs degenerate-geometry question can be answered. Null when the shim did not offer it or the recipe has no
    // fragment stage; consumed only by the env-gated diagnostic.
    std::function<VkPipeline(VkShaderModule ps, bool cull_none)> with_ps;
    // Diagnostic (docs/vertex_path.md): the vertex path of an armed draw - the shader-corpus identity the PSO resolved and
    // the input layout actually bound to the Vulkan pipeline. Filled by the shim; read only by the env-gated dump.
    std::function<void(struct VertexPathInfo& out)> describe_vertex_path;
    std::uint32_t samples = 1;  // rasterizationSamples; every bound attachment must have this count
    // Diagnostic (docs/page_content.md): what the translated pixel stage does with images, counted from its SPIR-V at
    // creation, so a black frame can be told apart from a texture-content gap. has_pixel_stage is false when the
    // pipeline has no PS (depth/stencil only).
    bool has_pixel_stage = false;
    std::uint32_t ps_images = 0;  // image sample/gather/read instructions in the pixel shader
    std::uint32_t vs_loads = 0;   // OpLoad instructions in the vertex shader
};

// The 144-byte push-constant block every translated shader declares: s0..s31, heap base, code base.
struct ShaderUserData {
    std::uint32_t s[32];
    std::uint64_t heap_base;
    std::uint64_t code_base;
};
static_assert(sizeof(ShaderUserData) == 144);

// Diagnostic (docs/vertex_path.md): the vertex path of an armed draw, as the shim knows it - the shader-corpus module
// identity the PSO resolved and the input layout actually bound to the Vulkan pipeline. Fixed-size so it can cross the
// replay/shim boundary without allocation; read only by the env-gated dump.
struct VertexPathInfo {
    char vs_name[96]{};    // the translated module's corpus name (index entry name)
    char vs_path[288]{};   // <shader dir>/<name>.spv, so the SPIR-V can be disassembled or evaluated
    char ps_name[96]{};
    char ps_path[288]{};
    struct Attribute {
        std::uint32_t location = 0;  // the Vulkan input location (the VS input's register index)
        std::uint32_t format = 0;    // VkFormat of the attribute
        std::uint32_t offset = 0;    // byte offset inside the vertex
        std::uint32_t binding = 0;   // input slot
    } attribute[16];
    std::uint32_t attribute_count = 0;
    struct Binding {
        std::uint32_t binding = 0;
        std::uint32_t stride = 0;    // 0: the stride is dynamic, given by the draw's vertex view
        std::uint32_t step_rate = 0;  // VK_VERTEX_INPUT_RATE_VERTEX / INSTANCE
        std::uint32_t divisor = 0;
    } binding[16];
    std::uint32_t binding_count = 0;
    // The pipeline's root signature parameter table (docs/vertex_path.md): which push-constant dwords each root
    // parameter owns, so a dword's provenance can be printed. `type` is rt::rootsig::ParamType as an integer
    // (0 constants, 1 root descriptor, 2 descriptor table); `dwords` is the number of user-data dwords it occupies.
    struct RootParameter {
        std::uint32_t type = 0;
        std::uint32_t user_offset = 0;
        std::uint32_t dwords = 0;
        std::uint32_t constant_count = 0;
    } parameter[16];
    std::uint32_t parameter_count = 0;
    // Set by the slot-57 record path when it supplies the draw's root arguments itself (rootargs increment): the dump
    // then tags a root-argument dword as coming from the record's block instead of the title's recorded API calls.
    std::uint32_t record_root_arguments = 0;
};

// The bytes one vertex attribute of `format` takes, or 0 for a format the layer does not bind as a vertex input. Pure
// (port/test/frame_dump_test.cpp pins it).
inline std::uint32_t vertex_format_bytes(std::uint32_t format) {
    switch (format) {
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT: return 4;
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: return 8;
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_R32G32B32_UINT:
    case VK_FORMAT_R32G32B32_SINT: return 12;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT: return 16;
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: return 8;
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT: return 4;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT: return 2;
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT: return 1;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 4;
    default: return 0;
    }
}

// A bitmask of input-layout problems: a location past Vulkan's 32, an unknown format, or an attribute running past a
// stride the pipeline itself declared. A dynamic (0) stride is checked at draw time and is not a problem here. Pure.
inline constexpr std::uint32_t kVertexLocationOutOfRange = 1u << 0;
inline constexpr std::uint32_t kVertexFormatUnknown = 1u << 1;
inline constexpr std::uint32_t kVertexOverrunsStride = 1u << 2;
inline std::uint32_t vertex_layout_problems(const VertexPathInfo& info) {
    std::uint32_t problems = 0;
    for (std::uint32_t i = 0; i < info.attribute_count && i < std::size(info.attribute); ++i) {
        const VertexPathInfo::Attribute& a = info.attribute[i];
        if (a.location >= 32) problems |= kVertexLocationOutOfRange;
        const std::uint32_t bytes = vertex_format_bytes(a.format);
        if (!bytes) {
            problems |= kVertexFormatUnknown;
            continue;
        }
        for (std::uint32_t b = 0; b < info.binding_count && b < std::size(info.binding); ++b) {
            const VertexPathInfo::Binding& binding = info.binding[b];
            if (binding.binding != a.binding || binding.stride == 0) continue;
            if (a.offset + bytes > binding.stride) problems |= kVertexOverrunsStride;
        }
    }
    return problems;
}

// The FSPX blit vertex shader measured in docs/vertex_path.md (module fspx/lib_vs_00823): it does not read the bound
// input layout at all. It fetches its own vertex register and builds the clip position as x = s8 * vx, y = s10 * vx,
// z = s9 * vx, w = s28 * vw from the push-constant block (dwords s0..s31), so the four scalars in s8/s9/s10/s28 are the
// whole transform. All four zero collapses every vertex to the origin with w = 0 and nothing rasterizes. Pure.
struct BlitTransform {
    float x = 0;  // s8
    float y = 0;  // s10
    float z = 0;  // s9
    float w = 0;  // s28
};
inline BlitTransform blit_transform(const std::uint32_t* s) {
    const auto as_float = [](std::uint32_t bits) {
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    return {as_float(s[8]), as_float(s[10]), as_float(s[9]), as_float(s[28])};
}
inline bool blit_transform_degenerate(const BlitTransform& transform) {
    // Clipping keeps a vertex only when w > 0 (a NaN comparison is false too), so a non-positive w is degenerate no
    // matter what x/y/z say; a zero and a denormal x/y/z with w = 0 is exactly the measured state.
    return !(transform.w > 0) || (transform.x == 0 && transform.y == 0 && transform.z == 0);
}

// Where one of a draw's 32 push-constant dwords comes from (docs/vertex_path.md): the pipeline's root signature says
// which dwords a descriptor table occupies, which are a root constant or root descriptor (supplied by the draw's root
// arguments), and which no parameter covers. `loose` is a nonzero dword no parameter explains (the title wrote it into
// the recorded user data directly, or a decode wrote it without a layout entry). Pure; pinned by frame_dump_test.
inline constexpr std::uint32_t kPushConstantUnset = 0;
inline constexpr std::uint32_t kPushConstantTable = 1;
inline constexpr std::uint32_t kPushConstantRootArgument = 2;
inline constexpr std::uint32_t kPushConstantLoose = 3;
struct PushConstantProvenance {
    std::uint32_t kind = kPushConstantUnset;
    std::uint32_t parameter = UINT32_MAX;  // index into VertexPathInfo::parameter, or UINT32_MAX
};
inline PushConstantProvenance push_constant_provenance(const std::uint32_t* user_data, std::uint32_t table_windows,
                                                       const VertexPathInfo& info, std::uint32_t index) {
    PushConstantProvenance out;
    if (!user_data || index >= 32) return out;
    for (std::uint32_t p = 0; p < info.parameter_count && p < std::size(info.parameter); ++p) {
        const VertexPathInfo::RootParameter& parameter = info.parameter[p];
        if (!parameter.dwords) continue;
        if (index < parameter.user_offset || index >= parameter.user_offset + parameter.dwords) continue;
        out.parameter = p;
        out.kind = parameter.type == 2 ? kPushConstantTable : kPushConstantRootArgument;
        return out;
    }
    if ((table_windows >> index) & 1u) {
        out.kind = kPushConstantTable;  // the pipeline's mask names a table window the parameter table did not
        return out;
    }
    if (user_data[index]) out.kind = kPushConstantLoose;
    return out;
}
// The dump's one-word name for a provenance kind.
inline const char* push_constant_provenance_name(std::uint32_t kind) {
    switch (kind) {
    case kPushConstantTable: return "table";
    case kPushConstantRootArgument: return "root";
    case kPushConstantLoose: return "loose";
    default: return "unset";
    }
}

// A host read of a small CPU-written input either succeeds, names no VA, or names GPU-written memory. The last is a
// capability limit, not bad contents: DEFAULT and READBACK bytes may still be stale while replay prepares a command.
enum class HostReadStatus { ok, absent, not_cpu_written };

enum class ReplayResult {
    success,
    list_open,
    unresolved_resource,
    invalid_subresource,
    invalid_clear,
    invalid_copy,
    invalid_predication,
    invalid_query,
    invalid_indirect_arguments,
    invalid_draw,  // DispatchMesh with a pipeline that has no mesh shader, or a vertex draw with one that has
    unsupported
};

// A submission was refused because a binding did not resolve to a resource the replay layer knows. The whole list is
// dropped (d3d12_com.cpp, "nothing was submitted"), so which check refused and which id it could not resolve is what
// a boot has to report: the id is the D3D12 object pointer (pipeline) or the console VA (buffer) the command named.
// The first few per site print, then a count; PALMETTO_REPLAY_MISS=0 silences them. Diagnostic only: no call changes.
inline ReplayResult unresolved_at(const char* site, std::uint64_t id = 0, std::uint64_t extra = 0) {
    struct Note {
        const char* site;
        unsigned printed;
    };
    static Note notes[32];
    static unsigned sites = 0;
    static std::mutex mutex;
    static const bool enabled = [] {
        const char* value = std::getenv("PALMETTO_REPLAY_MISS");
        return !value || value[0] == '\0' || (value[0] != '0' && value[0] != 'n');
    }();
    if (enabled) {
        std::lock_guard lock(mutex);
        Note* note = nullptr;
        for (unsigned i = 0; i < sites; ++i) {
            if (notes[i].site != site) continue;
            note = &notes[i];
            break;
        }
        if (!note && sites < std::size(notes)) note = &notes[sites++];
        if (note) {
            const unsigned kPerSite = 3;
            if (note->printed < kPerSite) {
                note->site = site;
                std::fprintf(stderr, "[rt] replay: unresolved resource at %s: id 0x%llx, extra 0x%llx\n", site,
                             static_cast<unsigned long long>(id), static_cast<unsigned long long>(extra));
                const bool last = note->printed + 1 == kPerSite;
                if (last) std::fprintf(stderr, "[rt] replay: %s: further misses are counted silently\n", site);
            }
            ++note->printed;
        }
    }
    return ReplayResult::unresolved_resource;
}

// Diagnostic (docs/frame_capture.md): one console descriptor table as the shim decoded it, for the frame dump's
// sampled-texture readback. Bounded: at most max_sampled_keys slots are copied out.
inline constexpr std::uint32_t max_sampled_keys = 16;
struct SampledKey {
    std::uint32_t words[8]{};  // the slot's 8-dword key, as the title wrote it into the table
    std::uint32_t slot = 0;    // the descriptor slot in the table (docs/frame_capture.md): table base + slot * 0x20
    bool image = false;        // a T# (gfx10::is_image): a texture, not a V# or S#
    // Resolved by the shim when set 0 holds a layer image for this T#: that image, the D3D12 resource behind it, and the
    // title VA the resource was placed at. title_va 0 means no title-VA alias: an ordinary committed resource, which no
    // title page write can reach (docs/frame_capture.md, the texture-source mapping chain).
    bool resolved = false;
    VkImage image_handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    ResourceId resource = 0;
    std::uint64_t title_va = 0;
};
struct SampledTable {
    std::uint32_t slots = 0;  // slots the table holds (bounded by the caller's max_descriptors)
    std::uint32_t keys = 0;   // T# entries copied into key[] (every T# slot, whatever its position)
    SampledKey key[max_sampled_keys];
    // The table's V#/S# slots (bounded): the constants probe reads the first decodable V#, and a slot that is neither a
    // T# nor a valid V#/S# (float data in a descriptor-sized slot) is reported as such rather than as a T#.
    std::uint32_t buffer_keys = 0;
    SampledKey buffer_key[8];
};

struct ReplayFunctions {
    // Set 0 for the pipelines that declare one (their own `bindless_set` is only the marker): the device's current
    // generation of it, which a flush may have replaced since the pipeline was made. Null: the pipeline's.
    VkDescriptorSet bindless_set = VK_NULL_HANDLE;
    PFN_vkCmdPipelineBarrier pipeline_barrier = nullptr;
    PFN_vkCmdClearColorImage clear_color_image = nullptr;
    PFN_vkCmdClearDepthStencilImage clear_depth_stencil_image = nullptr;
    // Diagnostic (docs/vertex_path.md): read bytes at an address a translated shader builds for a physical-storage
    // load (heap_base + a push-constant value), through the layer's host-visible arena. Set by the shim; reads only,
    // and never copies into the arena. False when the address is outside it.
    std::function<bool(std::uint64_t device_address, std::uint32_t bytes, std::uint8_t* out, std::uint32_t& read)>
        read_device_memory;
    // Diagnostic (docs/vertex_path.md): write the console driver-block vertex-stream V# into the arena mirror at an
    // address a translated shader fetches it from (heap_base + a push-constant value). Set by the shim; opt-in only.
    std::function<bool(std::uint64_t device_address, std::uint32_t bytes, const std::uint8_t* data)>
        write_device_memory;
    // Diagnostic (docs/vertex_path.md): a small scratch range in the GPU address arena, so the geometry fix can give the
    // translated shader a distinct driver-block address (the s0/s1 pair) instead of the descriptor table's own. Set by
    // the shim; opt-in only.
    std::function<bool(std::uint32_t bytes, std::uint64_t& arena_address)> allocate_driver_block;
    // Diagnostic (docs/frame_capture.md, the colour path): bind layer images for the T#s a shader fetches itself out of its
    // published matrix block (PALMETTO_TEXTURE_BIND_FETCHED). Set by the shim; opt-in only.
    std::function<void(std::uint64_t table_va)> bind_fetched_textures;
    // Optional; a list that needs a missing one is rejected with ReplayResult::unsupported.
    PFN_vkCmdPipelineBarrier2 pipeline_barrier2 = nullptr;  // enhanced barriers
    VkPipelineStageFlags2 queue_stages = 0;
    PFN_vkCmdBeginRendering begin_rendering = nullptr;      // rectangle clears
    PFN_vkCmdEndRendering end_rendering = nullptr;
    PFN_vkCmdClearAttachments clear_attachments = nullptr;
    PFN_vkCmdCopyBuffer copy_buffer = nullptr;                  // copies
    PFN_vkCmdCopyBufferToImage copy_buffer_to_image = nullptr;
    PFN_vkCmdCopyImageToBuffer copy_image_to_buffer = nullptr;
    PFN_vkCmdCopyImage copy_image = nullptr;
    PFN_vkCmdResolveImage resolve_image = nullptr;              // resolves
    PFN_vkCmdFillBuffer fill_buffer = nullptr;                  // unordered-access buffer clears, with copy_buffer
    PFN_vkCmdUpdateBuffer update_buffer = nullptr;              // WriteBufferImmediate, unordered-access clears
    // VK_EXT_conditional_rendering: SetPredication. The predicate buffer must have been created with
    // VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT.
    PFN_vkCmdBeginConditionalRenderingEXT begin_conditional_rendering = nullptr;
    PFN_vkCmdEndConditionalRenderingEXT end_conditional_rendering = nullptr;
    PFN_vkCmdResetQueryPool reset_query_pool = nullptr;  // queries, with fill_buffer for unavailable results
    PFN_vkCmdBeginQuery begin_query = nullptr;
    PFN_vkCmdEndQuery end_query = nullptr;
    PFN_vkCmdWriteTimestamp write_timestamp = nullptr;
    PFN_vkCmdCopyQueryPoolResults copy_query_pool_results = nullptr;
    // Set only where a copy of timestamp results is not ordered after the timestamps written before it (MoltenVK on
    // Apple GPUs, docs/d3d12_vulkan.md section 4.6): records what orders them, before each timestamp resolve.
    std::function<void(VkCommandBuffer)> order_timestamp_copy;
    std::function<void(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer, VkDeviceSize)>
        sum_occlusion;
    // Creates a query pool of `count` queries of that type for the single-list replay()'s occlusion and
    // pipeline-statistics segments (D3D-399); the batched path creates its own. Unset, or with any sum helper unset,
    // replay() refuses a list whose queries overlap, as before.
    std::function<std::shared_ptr<const OcclusionSegmentPool>(VkQueryType, std::uint32_t count)> create_segment_pool;
    PFN_vkCmdBindPipeline bind_pipeline = nullptr;              // dispatches
    PFN_vkCmdPushConstants push_constants = nullptr;
    PFN_vkCmdBindDescriptorSets bind_descriptor_sets = nullptr;  // set 0 of pipelines that declare bindings
    PFN_vkCmdDispatch dispatch = nullptr;
    std::function<const ComputePipeline*(std::uintptr_t)> compute_pipeline;
    PFN_vkCmdBindVertexBuffers2 bind_vertex_buffers2 = nullptr;  // draws
    PFN_vkCmdBindIndexBuffer bind_index_buffer = nullptr;
    PFN_vkCmdSetViewport set_viewport = nullptr;
    PFN_vkCmdSetScissor set_scissor = nullptr;
    PFN_vkCmdSetPrimitiveTopology set_primitive_topology = nullptr;
    PFN_vkCmdSetPrimitiveRestartEnable set_primitive_restart_enable = nullptr;
    PFN_vkCmdSetStencilReference set_stencil_reference = nullptr;
    PFN_vkCmdSetBlendConstants set_blend_constants = nullptr;
    PFN_vkCmdDraw draw = nullptr;
    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks = nullptr;  // VK_EXT_mesh_shader; null: mesh draws are unsupported
    PFN_vkCmdDrawIndexed draw_indexed = nullptr;
    std::function<const GraphicsPipeline*(std::uintptr_t)> graphics_pipeline;
    // The buffer and offset a console VA range starts in (from ResourceBinding::va), false when none does. `size` is
    // cut to what the buffer holds from there: vertex and index views use what fits, WriteBufferImmediate needs all
    // of it.
    std::function<bool(std::uint64_t va, std::uint64_t& size, VkBuffer&, VkDeviceSize&)> buffer_at;
    // Host bytes for a whole console VA range; `not_cpu_written` when it names GPU-writable memory. For small
    // CPU-written inputs, such as the pointer list of an ARRAY_OF_POINTERS TLAS build.
    std::function<HostReadStatus(std::uint64_t va, std::uint64_t bytes, const std::uint8_t*&)> host_at;
    // The title's own console VAs (design B, docs/page_address_space.md): a VA inside a range the layer aliased
    // returns the arena VA that stands in for it. Every shader-visible copy of a title VA is rebased with this before
    // it reaches a shader, because the shader computes heap_base + console_va and only an arena offset satisfies that.
    // `descriptor_range` says which of two overlapping title ranges the site means: a descriptor table names a console
    // descriptor heap, a root descriptor names a placed buffer. Unset: no title VAs are aliased, so nothing to rebase.
    std::function<bool(std::uint64_t va, std::uint64_t& rebased, bool descriptor_range)> rebase_console_va;
    // False while a title range the title can name as a shader-visible VA has no arena stand-in: a draw that might
    // carry such a VA must stay refused, never executed (it would fault the GPU). Unset: nothing to check.
    std::function<bool()> console_va_ready;
    // Refreshes the arena mirrors of the title's shader-visible console descriptor heaps from the title's CPU
    // descriptors, rebasing the V# bases the title wrote (console_va_map.h). Called once per submission, before its
    // command buffers are recorded.
    std::function<void()> sync_console_heaps;
    // The title's page content (docs/page_content.md): copy the bytes behind a title VA into the arena range that
    // stands in for it, so a draw reads the title's data and not the stand-in's; and pull the buffer data a descriptor
    // table's V#s name (a table VA, and how many descriptor slots to read). Called while a draw is prepared, before
    // its command buffer is recorded. Both are no-ops for a VA no title range holds.
    std::function<void(std::uint64_t va, std::uint64_t bytes)> pull_title_pages;
    std::function<void(std::uint64_t table_va, std::uint32_t max_descriptors)> pull_title_table;
    // A copy's source range, by the arena VA of the stand-in it reads (docs/page_content.md): a copy that fills a
    // texture from the title's placed pages must read the title's bytes, not the stand-in's.
    std::function<void(std::uint64_t arena_va, std::uint64_t bytes)> pull_title_arena;
    // Diagnostic (docs/page_content.md): read the first draw's render target back after its submission, so a frame
    // that stays black can be told apart from a draw that writes nothing. Optional and bounded to one capture.
    std::function<void(VkImage image, VkFormat format, VkExtent2D extent, ResourceId resource,
                       std::uint64_t draw_index)>
        capture_attachment;
    // Diagnostic (docs/frame_capture.md): the armed draw's descriptor table, decoded by the shim from the console heap
    // mirror, and a readback of the first sampled image. Both optional; consumed only by the frame dump's sample mode.
    std::function<bool(std::uint64_t table_va, std::uint32_t max_descriptors, SampledTable& out)> sampled_table;
    std::function<void(VkImage image, VkFormat format, VkExtent2D extent, ResourceId resource)> capture_sample;
    // Diagnostic (docs/frame_capture.md): read up to `bytes` of the title's own memory at `va` (the constants a draw's
    // V# names), in place. False when the range is not committed/readable; `read` returns the bytes actually copied.
    std::function<bool(std::uint64_t va, std::uint32_t bytes, std::uint8_t* out, std::uint32_t& read)> read_title_memory;
    // Zeroed memory that unbound vertex-buffer slots and an unbound index buffer read (D3D12 reads zeros there).
    VkBuffer zero_buffer = VK_NULL_HANDLE;
    VkDeviceSize zero_offset = 0;
    VkDeviceSize zero_size = 0;
    std::uint64_t heap_base = 0;  // device address of the GPU address arena (push-constant dwords 32-33)
    // Resolves available binary-occlusion queries to 64-bit 0/1 values, outside conditional rendering.
    std::function<void(VkCommandBuffer, VkQueryPool, std::uint32_t first, std::uint32_t count,
                       VkBuffer dst, VkDeviceSize offset)> resolve_binary_occlusion;
    // Adds `count` pipeline-statistics results into one D3D12_QUERY_DATA_PIPELINE_STATISTICS, outside conditional
    // rendering: the segments of a statistics query split around resolves (D3D-162) and mesh draws (D3D-291).
    // Segment i is in the second pool when mesh[i] is set (mesh may be null: none is).
    std::function<void(VkCommandBuffer, VkQueryPool, VkQueryPool mesh_pool, const std::uint8_t* mesh,
                       std::uint32_t first, std::uint32_t count, VkBuffer dst, VkDeviceSize offset)>
        sum_statistics;
    // Sums `count` binary-occlusion segments and normalizes the total to a 64-bit 0 or 1, outside conditional
    // rendering: a binary query split over physical segments because it overlaps other occlusion queries (D3D-252).
    std::function<void(VkCommandBuffer, VkQueryPool, std::uint32_t first, std::uint32_t count,
                       VkBuffer dst, VkDeviceSize offset)> sum_binary_occlusion;
    // VK_EXT_debug_utils, set only when the instance enabled it; debug markers are skipped without them.
    PFN_vkCmdInsertDebugUtilsLabelEXT insert_label = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT begin_label = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT end_label = nullptr;
    // ExecuteIndirect (Vulkan 1.0/1.2 core).
    PFN_vkCmdDrawIndirect draw_indirect = nullptr;
    PFN_vkCmdDrawIndexedIndirect draw_indexed_indirect = nullptr;
    PFN_vkCmdDrawIndirectCount draw_indirect_count = nullptr;
    PFN_vkCmdDrawIndexedIndirectCount draw_indexed_indirect_count = nullptr;
    PFN_vkCmdDispatchIndirect dispatch_indirect = nullptr;
    // A DISPATCH-only ExecuteIndirect signature with a GPU count buffer needs a compute prepass to clamp
    // the number of commands. The resulting scratch buffer is owned by the queue's timeline-retained batch.
    std::function<bool(VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, std::uint32_t, std::uint32_t,
                       CountedDispatch&)> prepare_counted_dispatch;
    std::function<void(VkCommandBuffer, const CountedDispatch&)> emit_counted_dispatch;
    // Optional VK_KHR_acceleration_structure: BuildRaytracingAccelerationStructure.
    PFN_vkCmdBuildAccelerationStructuresKHR build_acceleration_structures = nullptr;
    // A KHR TLAS built from D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS needs a pointer array whose entries are Vulkan
    // device addresses, not console VAs. This stages one that stays alive until the submission completes and returns
    // its device address. A GPU-written list is refused as unsupported; a bad entry is invalid_copy.
    std::function<ReplayResult(const BuildAccelerationStructure&, VkDeviceAddress&)> prepare_khr_instances;
    // XDXR BVH4 builds (xdxr_build.h), used instead of the KHR ones when set: `xdxr_check` refuses inputs the builder
    // cannot read while the list is prepared, `xdxr_record` records the build.
    std::function<bool(const BuildAccelerationStructure&)> xdxr_check;
    std::function<void(VkCommandBuffer, const BuildAccelerationStructure&)> xdxr_record;
    // CopyRaytracingAccelerationStructure (CLONE, COMPACT) of XDXR structures: `bytes` is the room the destination
    // resource has; and one postbuild value (COMPACTED_SIZE, CURRENT_SIZE) of the structure at `src` to `dest`.
    std::function<void(VkCommandBuffer, std::uint64_t src_va, std::uint64_t dst_va, std::uint64_t bytes)> xdxr_copy;
    std::function<void(VkCommandBuffer, std::uint64_t src_va, std::uint64_t dest_va)> xdxr_postbuild;
    std::function<VkAccelerationStructureKHR(std::uint64_t)> resolve_acceleration_structure;
    // CopyRaytracingAccelerationStructure (CLONE, COMPACT) and COMPACTED_SIZE postbuild on KHR acceleration structures.
    PFN_vkCmdCopyAccelerationStructureKHR copy_acceleration_structure = nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR write_acceleration_structures_properties = nullptr;
    std::function<bool(std::uint32_t count, VkQueryPool& pool, std::uint32_t& first)> prepare_khr_postbuild;
    // Preparation only allocates a slot; capture records the GPU snapshot at the SetPredication command.
    std::function<bool(VkBuffer, VkDeviceSize, PredicateCapture&)> prepare_predicate;
    std::function<void(VkCommandBuffer, const PredicateCapture&)> capture_predicate;
    std::function<bool(VkBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, VkDeviceSize,
                       std::vector<PredicatedCopyDispatch>&)> prepare_predicated_copy;
    std::function<void(VkCommandBuffer, const std::vector<PredicatedCopyDispatch>&)> emit_predicated_copy;
    // Returns a 2D-array view of the range (caller-owned, alive until execution completes); rectangle clears.
    std::function<VkImageView(const ResourceBinding&, const VkImageSubresourceRange&)> image_view;
    // Optional. The framebuffer, and through the last argument the render pass, that resolve the depth of the
    // source's one-layer view into the destination's (both from image_view, each image in the layout after its
    // view); alive until execution completes. Null: not available. For the resolves prepare names (D3D-417).
    std::function<VkFramebuffer(const ResourceBinding&, VkImageView, VkImageLayout, const ResourceBinding&, VkImageView,
                                VkImageLayout, VkResolveModeFlagBits, VkExtent2D, VkRenderPass&)>
        depth_resolve_framebuffer;
    PFN_vkCmdBeginRenderPass begin_render_pass = nullptr;
    PFN_vkCmdEndRenderPass end_render_pass = nullptr;
    // Optional format check for color-attachment reinterpretation on mutable UAV clears; unset assumes every format
    // except E5B9G9R9_UFLOAT_PACK32 supports COLOR_ATTACHMENT.
    std::function<bool(VkFormat)> color_attachment_format;
    // Footprint copies of a depth24_as_float texture (depth24.h): `depth24_prepare` makes the scratch and descriptors
    // for a job while the list is checked, `depth24_run` records its dispatch. Without them such a copy is unsupported.
    std::function<bool(Depth24Job&, std::vector<Depth24Job>&)> depth24_prepare;  // whole copy in, dispatches out
    std::function<void(VkCommandBuffer, const Depth24Job&)> depth24_run;
    // `bytes` of transfer scratch that stays alive until the submission completes (vulkan_scratch.h): depth <-> colour
    // copies go through it, image to buffer to image. Without it such a copy is unsupported.
    std::function<bool(VkDeviceSize bytes, VkBuffer&, VkDeviceSize&)> copy_scratch;
    // Returns the query heap with that ID, or nullptr; it stays valid while the lists that use it run.
    std::function<QueryPool*(QueryHeapId)> query_pool;
    bool separate_depth_stencil_layouts = false;  // must match the LayoutTracker, if one is used
};

// Replaces the title VAs among the 32 user-data dwords every translated shader receives (s0..s31, docs section 3.5)
// with the arena VAs of the ranges they alias (design B, docs/page_address_space.md): the shader adds heap_base to a
// root-descriptor or descriptor-table VA, so a two-terabyte title VA there is a wild address and the draw faults the
// whole queue submission. `table_windows` is the pipeline's mask of descriptor-table dwords (two per table); a window
// in it is rebased as a descriptor-heap VA, every other window as a placed-buffer VA, because the title's two ranges
// overlap. Matches are collected before anything is written, because rewriting one dword pair changes the bytes the
// next pair would be read from. A match whose neighbour was already rewritten is dropped: two real addresses cannot
// overlap.
inline unsigned rebase_user_data(const ReplayFunctions& vk, ShaderUserData& data, std::uint32_t table_windows = 0) {
    if (!vk.rebase_console_va) return 0;
    std::uint64_t rebased[32] = {};
    bool match[32] = {};
    unsigned matches = 0;
    for (unsigned i = 0; i + 1 < 32 && matches < 16; ++i) {
        const std::uint64_t value = std::uint64_t(data.s[i]) | (std::uint64_t(data.s[i + 1]) << 32);
        if (!value || !vk.rebase_console_va(value, rebased[i], (table_windows >> i) & 1u)) continue;
        match[i] = true;
        ++matches;
    }
    if (!matches) return 0;
    unsigned applied = 0;
    for (unsigned i = 0; i + 1 < 32; ++i) {
        if (!match[i] || (i && match[i - 1])) continue;  // overlapping windows cannot both be addresses
        data.s[i] = std::uint32_t(rebased[i]);
        data.s[i + 1] = std::uint32_t(rebased[i] >> 32);
        ++applied;
    }
    return applied;
}

// ---- Frame dump: the helpers prepare_draw uses (docs/frame_capture.md) --------------------------------------------------
// The rest of the diagnostic lives in namespace frame_dump further down, after PreparedList; these two are needed while
// a draw is prepared, so they are declared here.
namespace frame_dump {

// The sampled-texture readback (docs/frame_capture.md): PALMETTO_FRAME_DUMP_SAMPLE=1 prints the armed draw's
// descriptor-table keys and copies the first sampled image into host memory after the submission, so "the textures are
// black" can be told from "the textures are fine and the pixel shader computes black". Diagnostic only.
inline bool sample_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_FRAME_DUMP_SAMPLE");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// The descriptor-table VAs a draw's recorded root arguments name: one 64-bit value at each set bit of `table_windows`
// (the graphics layout gives every table two dwords). At most `max_tables`, in dword order. Pure.
inline std::vector<std::uint64_t> descriptor_tables(const Draw& draw, std::uint32_t table_windows,
                                                    std::size_t max_tables = 8) {
    std::vector<std::uint64_t> out;
    for (unsigned i = 0; i + 1 < 32 && out.size() < max_tables; i += 2) {
        if (!((table_windows >> i) & 1u)) continue;
        const std::uint64_t value =
            std::uint64_t(draw.user_data[i]) | (std::uint64_t(draw.user_data[i + 1]) << 32);
        if (value) out.push_back(value);
    }
    return out;
}

// The draws the one-shot readback arms at (docs/frame_capture.md): PALMETTO_READBACK_AT_DRAW accepts a comma-separated
// list (bounded to four), so a submission's draws - the even intermediate pass and the odd six-image pass - can both be
// measured in one boot. Pure; read once.
inline const std::vector<unsigned long long>& readback_draws() {
    static const std::vector<unsigned long long> draws = [] {
        std::vector<unsigned long long> out;
        const char* value = std::getenv("PALMETTO_READBACK_AT_DRAW");
        while (value && *value && out.size() < 4) {
            char* end = nullptr;
            const unsigned long long draw = std::strtoull(value, &end, 0);
            if (end != value) out.push_back(draw);
            if (!end || *end != ',') break;
            value = end + 1;
        }
        return out;
    }();
    return draws;
}
// Diagnostic (docs/frame_capture.md): PALMETTO_READBACK_FULL=1 copies the armed draw target in full (the whole
// 3840x2160 subresource, one copy per draw) and reports statistics plus an 8x8 tile occupancy map, so content outside the
// 128x128 origin window is not mistaken for a black target. Off by default (a full 4K RGBA copy is 33 MB per draw).
inline bool full_target_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_READBACK_FULL");
        return value && *value && value[0] != 48 && value[0] != 110;
    }();
    return on;
}

inline bool readback_armed_at(std::uint64_t draw) {
    for (const unsigned long long listed : readback_draws())
        if (listed == draw) return true;
    return false;
}
inline unsigned long long readback_primary() { return readback_draws().empty() ? 0 : readback_draws().front(); }

// The descriptor-table V#s a draw's s8 table holds at the slot the module reads as matrix A (s8+0) and at the next one
// (s8+0x40), decoded from the arena. Used by the readback arming line so both draws of the frame can be compared; reads
// only.
inline void print_matrix_slot(const ReplayFunctions& vk, std::uint64_t heap_base, std::uint64_t s8, const char* label) {
    if (!vk.read_device_memory) return;
    std::uint32_t words[8] = {};
    std::uint32_t got = 0;
    if (!vk.read_device_memory(heap_base + (s8 & ~std::uint64_t(3)), sizeof(words),
                               reinterpret_cast<std::uint8_t*>(words), got) ||
        got < 4 * sizeof(std::uint32_t)) {
        std::fprintf(stderr, "[rt] replay: %s: table s8=0x%llx slot 0 is not readable\n", label,
                     static_cast<unsigned long long>(s8));
        return;
    }
    const gfx10::BufferWords buffer_words{words[0], words[1], words[2], words[3]};
    gfx10::Buffer buffer{};
    if (gfx10::decode(buffer_words, buffer)) {
        std::fprintf(stderr,
                     "[rt] replay: %s: matrix A slot (s8+0):  %08x %08x %08x %08x -> V# address 0x%llx stride %u "
                     "records %u format %u\n",
                     label, words[0], words[1], words[2], words[3],
                     static_cast<unsigned long long>(buffer.address), buffer.stride, buffer.num_records,
                     buffer.format);
        // What the matrix block holds: the pass's constants (a constant-output pixel shader reads only these), so a black
        // pass can be told from a non-zero one.
        if (vk.read_device_memory && buffer.address) {
            const std::uint64_t declared = buffer.stride ? std::uint64_t(buffer.stride) * buffer.num_records
                                                         : std::uint64_t(buffer.num_records);
            const std::uint32_t want = static_cast<std::uint32_t>(std::min<std::uint64_t>(declared, 5768));
            std::uint32_t got = 0;
            std::uint8_t data[5768] = {};
            if (want && vk.read_device_memory(vk.heap_base + buffer.address, want, data, got) && got) {
                std::uint64_t sum = 0, nonzero = 0, peak = 0;
                for (std::uint32_t i = 0; i < got; ++i) {
                    sum += data[i];
                    if (data[i]) ++nonzero;
                    peak = std::max<std::uint64_t>(peak, data[i]);
                }
                std::fprintf(stderr,
                             "[rt] replay: %s: matrix A content: read %u of %llu B: mean %llu, non-black %llu/%u, peak "
                             "%llu; dwords",
                             label, got, static_cast<unsigned long long>(declared), sum / got, nonzero, got, peak);
                const std::uint32_t dwords = std::min<std::uint32_t>(got / 4, 24);
                for (std::uint32_t w = 0; w < dwords; ++w) {
                    std::uint32_t value = 0;
                    std::memcpy(&value, data + w * 4, sizeof(value));
                    std::fprintf(stderr, " %08x", value);
                }
                std::fprintf(stderr, "\n");
                // The published block is where a shader can fetch further descriptors itself: scan it (32-byte T#-shaped
                // windows, 8-byte stride) for decodable T#s and name each one, so the compose pass's image inputs that are
                // not table slots are located.
                unsigned found = 0;
                for (std::uint32_t at = 0; at + 32 <= got && found < 8; at += 8) {
                    std::uint32_t words_at[8] = {};
                    for (unsigned w = 0; w < 8; ++w) std::memcpy(&words_at[w], data + at + w * 4, 4);
                    const gfx10::ImageWords image_words{words_at[0], words_at[1], words_at[2], words_at[3],
                                                        words_at[4], words_at[5], words_at[6], words_at[7]};
                    gfx10::Image image{};
                    if (!gfx10::is_image(image_words) || !gfx10::decode(image_words, image)) continue;
                    std::fprintf(stderr,
                                 "[rt] replay: %s: matrix block T# at +0x%x: image title VA 0x%llx format %u %ux%u\n",
                                 label, at, static_cast<unsigned long long>(image.address), image.format,
                                 image.width, image.height);
                    ++found;
                }
            } else {
                std::fprintf(stderr, "[rt] replay: %s: matrix A content is not readable (%llu B)\n", label,
                             static_cast<unsigned long long>(declared));
            }
        }
    } else if (!words[0] && !words[1] && !words[2] && !words[3]) {
        std::fprintf(stderr, "[rt] replay: %s: matrix A slot (s8+0) is empty (all four dwords zero)\n", label);
    } else {
        std::fprintf(stderr, "[rt] replay: %s: matrix A slot (s8+0):  %08x %08x %08x %08x (no V# form)\n", label,
                     words[0], words[1], words[2], words[3]);
    }
}

// The vertex path (docs/vertex_path.md): PALMETTO_VERTEX_PATH=1 dumps the armed draw's shader-corpus identity, the input
// layout bound to the Vulkan pipeline, the raw vertex/index bytes and the constants, so the translated vertex shader's
// inputs can be evaluated off the boot. Diagnostic only and reads only.
inline bool vertex_path_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_VERTEX_PATH");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// The geometry fix's opt-in switch (docs/vertex_path.md): PALMETTO_VERTEX_STREAM_DESCRIPTOR=1 has the replay synthesise
// the console driver-block V# for a draw's bound vertex stream and write it into the descriptor-table slot the
// translated shader fetches it from. Off by default: the default path records nothing extra and writes no memory.
inline bool vertex_stream_descriptor_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_VERTEX_STREAM_DESCRIPTOR");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// The coverage experiment's switch (docs/vertex_path.md): PALMETTO_VERTEX_MATRIX_DESCRIPTOR=1 writes a matrix V# over the
// title's constants buffer at the s8 table's slot 0 (the module's matrix A source). Off by default.
// The colour-path experiment switch (docs/frame_capture.md): PALMETTO_TEXTURE_BIND_FETCHED=1 has the shim give every T#
// the compose pass fetches itself out of its published matrix block a layer image from the title-placed resource it names.
// Off by default.
inline bool bind_fetched_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_TEXTURE_BIND_FETCHED");
        return value && *value && value[0] != 48 && value[0] != 110;
    }();
    return on;
}

inline bool matrix_descriptor_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_VERTEX_MATRIX_DESCRIPTOR");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// Diagnostic (docs/vertex_path.md): the console driver-block vertex-stream V# for a bound vertex view, encoded as the
// module reads it (docs/root_arguments.md section 7): base = ((w1 & 0xFFFF) << 32) | (w0 & ~3), stride = w1[29:16],
// count = w2, bounds = w3[29:28], format = w3[18:12] (must be non-zero: the module gates its inlined colour conversion on
// format != 0), select = w3[11:0], and w1 bit 31 must be 0. Format 22 (GFX10_FORMAT_32_FLOAT) is the pass-through for the
// stream's 32-bit components. Pure; frame_dump_test pins the round trip through gfx10::decode.
inline gfx10::BufferWords vertex_stream_descriptor(std::uint64_t arena_address, std::uint32_t stride,
                                                   std::uint32_t records) {
    gfx10::Buffer buffer{};
    buffer.address = arena_address;
    buffer.stride = stride;
    buffer.num_records = records;
    buffer.format = 22;  // 32_FLOAT: non-zero so the module's colour fetch converts, a no-op for 32-bit data
    buffer.swizzle = gfx10::identity_swizzle;
    buffer.oob_select = gfx10::OobSelect::structured_with_offset;  // w3[29:28] = 0
    return gfx10::encode(buffer);
}

// The MATRIX V# form the module reads from the s8 table's slot 0 (docs/root_arguments.md section 7): a raw buffer over a
// 4x4 matrix (stride 0, the byte count in w2, w3 = 0x30000FAC: bounds raw, format 0, identity select). Used only by the
// coverage experiment; pure.
inline gfx10::BufferWords matrix_descriptor(std::uint64_t arena_address, std::uint32_t bytes) {
    gfx10::Buffer buffer{};
    buffer.address = arena_address;
    buffer.stride = 0;
    buffer.num_records = std::max(bytes, 64u);  // at least one 4x4
    buffer.format = 0;
    buffer.swizzle = gfx10::identity_swizzle;
    buffer.oob_select = gfx10::OobSelect::raw;  // w3[29:28] = 3, word 3 = 0x30000FAC
    return gfx10::encode(buffer);
}

// Reads and prints the constants the draw's descriptor tables name: the first decodable V# of each table, bounded to one
// 480-byte window, as byte statistics and every dword (up to 120), so a transform can be evaluated from the boot log.
// Reads only (docs/vertex_path.md); `label` prefixes the lines.
inline void print_vertex_constants(const ReplayFunctions& vk, const Draw& draw, const GraphicsPipeline& pipeline,
                                   const char* label) {
    if (!vk.sampled_table) return;
    for (const std::uint64_t table_va : descriptor_tables(draw, pipeline.table_windows)) {
        SampledTable table;
        if (!vk.sampled_table(table_va, 64, table)) continue;
        std::fprintf(stderr, "[rt] replay: %s: table VA 0x%llx: %u slot(s), %u key(s):", label,
                     static_cast<unsigned long long>(table_va), table.slots, table.keys);
        for (std::uint32_t k = 0; k < table.keys; ++k) {
            std::fprintf(stderr, " T#[%u]", table.key[k].slot);
            for (unsigned w = 0; w < 8; ++w) std::fprintf(stderr, "%s%08x", w ? " " : "", table.key[k].words[w]);
            std::fprintf(stderr, "]");
        }
        for (std::uint32_t k = 0; k < table.buffer_keys; ++k) {
            std::fprintf(stderr, " data/V#[%u]", table.buffer_key[k].slot);
            for (unsigned w = 0; w < 4; ++w) std::fprintf(stderr, "%s%08x", w ? " " : "", table.buffer_key[k].words[w]);
            std::fprintf(stderr, "]");
        }
        std::fprintf(stderr, "\n");
        if (!vk.read_title_memory) continue;
        for (std::uint32_t k = 0; k < table.keys; ++k) {
            if (table.key[k].image) continue;
            const gfx10::BufferWords words{table.key[k].words[0], table.key[k].words[1],
                                          table.key[k].words[2], table.key[k].words[3]};
            gfx10::Buffer buffer{};
            if (!gfx10::decode(words, buffer) || !buffer.address) continue;
            const std::uint64_t declared =
                buffer.stride ? std::uint64_t(buffer.stride) * buffer.num_records : std::uint64_t(buffer.num_records);
            if (!declared) continue;
            constexpr std::uint32_t kMaxConstants = 480;
            const std::uint32_t want =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(declared, kMaxConstants));
            std::uint32_t got = 0;
            std::uint8_t data[kMaxConstants] = {};
            if (!vk.read_title_memory(buffer.address, want, data, got) || !got) {
                std::fprintf(stderr, "[rt] replay: %s: V# base 0x%llx + 0x%llx is not readable\n", label,
                             static_cast<unsigned long long>(buffer.address),
                             static_cast<unsigned long long>(declared));
                break;
            }
            std::uint64_t sum = 0, nonzero = 0, peak = 0;
            for (std::uint32_t i = 0; i < got; ++i) {
                sum += data[i];
                if (data[i]) ++nonzero;
                peak = std::max<std::uint64_t>(peak, data[i]);
            }
            std::fprintf(stderr,
                         "[rt] replay: %s: constants at 0x%llx + 0x%llx, read %u: mean %llu, non-black %llu/%u, peak "
                         "%llu; dwords",
                         label, static_cast<unsigned long long>(buffer.address),
                         static_cast<unsigned long long>(declared), got, sum / got, nonzero, got, peak);
            const std::uint32_t dwords = std::min<std::uint32_t>(got / 4, 120);
            for (std::uint32_t w = 0; w < dwords; ++w) {
                std::uint32_t value = 0;
                std::memcpy(&value, data + w * 4, sizeof(value));
                std::fprintf(stderr, "%s%08x", (w % 15) == 0 ? "\n  " : " ", value);
            }
            std::fprintf(stderr, "\n");
            break;
        }
    }
}

// Diagnostic (docs/vertex_path.md): one descriptor fetch slot the translated shader reads itself - the dwords at
// heap_base + `relative`, decoded with the existing gfx10 classifiers (T#, V#, S#, or empty). A V# prints its buffer
// address/stride/records, a T# its address/format/extent, so the slot can be compared with the draw's bound views.
// Reads only, through the shim's device-memory reader; bounded to one line.
inline void print_fetch_slot(const ReplayFunctions& vk, std::uint64_t heap_base, const char* label,
                             std::uint64_t relative) {
    if (!vk.read_device_memory) return;
    const std::uint64_t address = heap_base + relative;
    std::uint32_t words[8] = {};
    std::uint32_t got = 0;
    if (!vk.read_device_memory(address, sizeof(words), reinterpret_cast<std::uint8_t*>(words), got) ||
        got < 4 * sizeof(std::uint32_t)) {
        std::fprintf(stderr,
                     "[rt] replay: vertex path: fetch slot %s: heap_base + 0x%llx (device 0x%llx) is not readable\n",
                     label, static_cast<unsigned long long>(relative),
                     static_cast<unsigned long long>(address));
        return;
    }
    const bool empty = !words[0] && !words[1] && !words[2] && !words[3];
    const gfx10::ImageWords image_words{words[0], words[1], words[2], words[3],
                                        words[4], words[5], words[6], words[7]};
    const gfx10::BufferWords buffer_words{words[0], words[1], words[2], words[3]};
    std::fprintf(stderr, "[rt] replay: vertex path: fetch slot %s: heap_base + 0x%llx (device 0x%llx):",
                 label, static_cast<unsigned long long>(relative),
                 static_cast<unsigned long long>(address));
    for (unsigned w = 0; w < 4; ++w) std::fprintf(stderr, " %08x", words[w]);
    if (empty) {
        std::fprintf(stderr, " -> empty (all four dwords zero)\n");
        return;
    }
    if (gfx10::is_image(image_words)) {
        gfx10::Image image{};
        if (gfx10::decode(image_words, image))
            std::fprintf(stderr, " -> T# image address 0x%llx format %u %ux%u\n",
                         static_cast<unsigned long long>(image.address), image.format, image.width, image.height);
        else
            std::fprintf(stderr, " -> T# (undecodable)\n");
        return;
    }
    gfx10::Buffer buffer{};
    if (gfx10::decode(buffer_words, buffer)) {
        std::fprintf(stderr, " -> V# buffer address 0x%llx stride %u records %u format %u\n",
                     static_cast<unsigned long long>(buffer.address), buffer.stride, buffer.num_records,
                     buffer.format);
        return;
    }
    const gfx10::Sampler sampler = gfx10::decode(gfx10::SamplerWords{words[0], words[1], words[2], words[3]});
    std::fprintf(stderr, " -> S# sampler (max anisotropy %u)\n", sampler.max_anisotropy);
}

// Diagnostic (docs/vertex_path.md): every 8-dword slot of a console descriptor table the shader can reach, decoded. The
// table base is the arena-relative value the shader forms heap_base + base from; eight slots is the bounded bound.
inline void print_fetch_slots_table(const ReplayFunctions& vk, std::uint64_t heap_base, const char* label,
                                    std::uint64_t base) {
    for (unsigned slot = 0; slot < 8; ++slot) {
        char slot_label[48] = {};
        std::snprintf(slot_label, sizeof(slot_label), "%s+0x%02x", label, slot * 0x20u);
        print_fetch_slot(vk, heap_base, slot_label, base + slot * 0x20u);
    }
}

}  // namespace frame_dump

namespace detail {

struct QueryResolveRun {
    VkQueryPool pool = VK_NULL_HANDLE;    // null: zero fill for queries that were never ended
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    VkDeviceSize offset = 0;
    bool sum = false;
    // Statistics segments: the mesh-compatible pool and the kind of each segment (OcclusionSegmentPool::mesh).
    VkQueryPool mesh_pool = VK_NULL_HANDLE;
    const std::uint8_t* mesh = nullptr;
};

struct PreparedQuery {
    static constexpr std::uint32_t no_segment = UINT32_MAX;
    std::size_t command = 0;
    QueryPool* heap = nullptr;
    bool statistics_active = false;
    std::uint32_t begin_segment = no_segment;
    std::uint32_t end_segment = no_segment;
    std::uint32_t first_result = 0;
    std::uint32_t result_count = 0;
    // A pipeline-statistics query split around the helpers it contains (plan_statistics_segments): the physical
    // statistics segment this command ends or begins, and for its EndQuery the segments that hold its result.
    std::uint32_t statistics_end = no_segment;
    std::uint32_t statistics_begin = no_segment;
    std::uint32_t statistics_first = 0;
    std::uint32_t statistics_count = 0;
    std::vector<QueryResolveRun> resolves;
};

struct PreparedKhrPostbuild {
    VkBuffer dst_buffer = VK_NULL_HANDLE;
    VkDeviceSize dst_offset = 0;
    VkQueryPool query_pool = VK_NULL_HANDLE;
    std::uint32_t first_query = 0;
    std::vector<VkAccelerationStructureKHR> sources;
};

struct Prepared {
    // Helper commands split the active statistics query just like query resolves, and so does a draw whose kind (mesh
    // or vertex) differs from the draws before it in the current segment (plan_statistics_segments, D3D-291).
    std::uint32_t statistics_end = PreparedQuery::no_segment;
    std::uint32_t statistics_begin = PreparedQuery::no_segment;
    PredicateCapture predicate;
    std::vector<PredicatedCopyDispatch> predicated_copy;
    CountedDispatch counted_dispatch;
    ResourceBinding binding;
    VkImageSubresourceRange range{};  // the barrier range, or the cleared view (cleared aspects only)
    VkImageView view = VK_NULL_HANDLE;
    VkFormat clear_format = VK_FORMAT_UNDEFINED; // The clear view format (RTV or reinterpreted UAV), without changing the resource binding.
    std::vector<VkClearRect> rects;   // rectangle clears, clipped to the mip
    bool skip = false;                // every rectangle was empty
    ResourceBinding source;                   // copies: the source resource
    VkImageSubresourceRange source_range{};   // copies: the source subresources (range: the destination's)
    std::vector<VkBufferCopy> buffer_copies;
    std::vector<VkBufferImageCopy> buffer_image_copies;  // one direction, given by which side is an image
    VkBuffer tile_scratch = VK_NULL_HANDLE;            // CopyTiles with a byte-unaligned linear buffer offset
    std::vector<VkBufferCopy> tile_rows;
    std::vector<VkImageCopy> image_copies;
    bool same_copy_subresource = false;
    VkImageResolve resolve{};  // resolves
    // Depth or stencil resolves: the mode, and the destination view (`view` is the source's).
    VkResolveModeFlagBits resolve_mode = VK_RESOLVE_MODE_NONE;
    VkImageView resolve_view = VK_NULL_HANDLE;
    // Set: the resolve runs in this render pass and framebuffer instead of a dynamic-rendering scope.
    VkRenderPass resolve_pass = VK_NULL_HANDLE;
    VkFramebuffer resolve_framebuffer = VK_NULL_HANDLE;
    bool has_depth24 = false;  // a depth plane copy of a depth24_as_float texture: convert through depth24
                               // (or a copy of an rgb96 texture: repacked by the same pass)
    std::uint32_t rgb96_row_words = 0;    // an rgb96 footprint: words between its rows and between its slices
    std::uint32_t rgb96_slice_words = 0;
    Depth24Job depth24;                    // the whole copy: where its scratch floats are
    std::vector<Depth24Job> depth24_parts;  // the dispatches that convert it
    // A depth <-> colour copy: the source into `staged` (image to buffer), then `staged` into the destination.
    VkBuffer staged = VK_NULL_HANDLE;
    std::vector<VkBufferImageCopy> staged_reads, staged_writes;
    std::vector<VkImageSubresourceRange> discards;  // DiscardResource: the subresources to discard
    VkBuffer write_buffer = VK_NULL_HANDLE;          // WriteBufferImmediate: where the address lands
    VkDeviceSize write_offset = 0;
    uav_clear::Element element;                      // unordered-access clears: one element of the view's format
    VkClearColorValue clear_color{};                 // ... the value that writes it into the image
    std::vector<std::array<VkDeviceSize, 2>> fills;  // ... buffer byte ranges, offset and size
    VkBuffer fill_scratch = VK_NULL_HANDLE;         // short buffer clears: one repeated 8-byte pattern
    VkDeviceSize fill_scratch_offset = 0;
    QueryPool* queries = nullptr;  // query commands: the heap
    ComputePipeline pipeline;  // dispatches
    std::uint32_t draw = 0;    // draws: its PreparedDraw in PreparedList::draws
    VkAccelerationStructureKHR accel_dst = VK_NULL_HANDLE;
    VkDeviceAddress khr_instances = 0;  // staged ARRAY_OF_POINTERS instance list for a KHR TLAS; 0: use the VA directly
    std::uint64_t xdxr_bytes = 0;     // CopyAccelerationStructure: the most bytes the copy may write
    VkAccelerationStructureKHR accel_src = VK_NULL_HANDLE;
    std::uint32_t khr_postbuild = ~0u;  // index into PreparedList::khr_postbuilds; ~0u: skipped or XDXR
};

inline constexpr std::size_t max_render_targets = 8;  // D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT

// What a draw needs beyond Prepared, kept apart in PreparedList::draws: every command's entry is refilled on every
// ExecuteCommandLists, and most commands are not draws.
struct PreparedDraw {
    struct Attachment {
        ResourceBinding binding;
        ResourceId id = 0;
        VkImageSubresourceRange range{};
        VkImageView view = VK_NULL_HANDLE;
    };
    // The context's pipeline: alive while the list is prepared and emitted (VulkanQueue::execute holds the resource
    // lock across both), so it is not copied.
    const GraphicsPipeline* graphics = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;  // graphics->pipeline, or its variant for the slots the draw leaves unbound
    std::vector<Attachment> colors;  // by slot; id 0 = unbound
    Attachment depth;
    std::vector<VkBuffer> vertex_buffers;
    std::vector<VkDeviceSize> vertex_offsets, vertex_sizes, vertex_strides;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    VkExtent2D area{};
    std::uint32_t layers = 1;  // the fewest layers any bound attachment view has
    // Diagnostic (docs/vertex_path.md): the arena-relative address of the driver-block V# the geometry fix filled for
    // this draw's vertex stream; 0 when the fix is off. Emit puts it in the s0/s1 user-data pair.
    std::uint64_t driver_block = 0;
    // Diagnostic (docs/frame_capture.md): this draw is one of the readback-armed draws, so the queue copies its sampled
    // textures mid-submission (right after this draw list) as well as post-submission.
    bool armed_readback = false;

    // Back to a new draw's state, keeping the vectors' storage.
    void reset() {
        graphics = nullptr;
        pipeline = VK_NULL_HANDLE;
        colors.clear();
        depth = {};
        vertex_buffers.clear();
        vertex_offsets.clear();
        vertex_sizes.clear();
        vertex_strides.clear();
        index_buffer = VK_NULL_HANDLE;
        index_offset = 0;
        index_type = VK_INDEX_TYPE_UINT16;
        area = {};
        layers = 1;
        driver_block = 0;
        armed_readback = false;
    }
};

// D3D converts a float clear value to a normalised format by rounding to nearest even, so an exact tie goes to the
// even neighbour (0.5 on 8 bits is 128). Vulkan leaves the rounding of ties to the GPU, and NVIDIA writes 127, one
// step below WARP and AMD. The value is rounded here and handed over as the exact fraction, which no GPU rounds
// differently. NaN is 0 and values outside 0..1 clamp, as in D3D.
//
// Ties go to even whatever the thread's floating-point rounding mode: std::nearbyint follows it, so a game that
// changes the mode would round differently. floor() does not depend on it, and x + 0.5 is exact here (x is at most
// a float times 65535, well inside a double's 53 bits).
inline double round_half_even(double x) {
    const double r = std::floor(x + 0.5);
    return r - x == 0.5 && std::fmod(r, 2.0) != 0.0 ? r - 1.0 : r;
}

inline float quantize_unorm(float value, double maximum) {
    if (!(value > 0.0f)) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    return static_cast<float>(round_half_even(double(value) * maximum) / maximum);
}

// The same for a signed normalised format: -1..1 on a scale of 127 (8 bits) or 32767 (16 bits), where D3D writes -1.0
// as -127 and ties go to the even neighbour.
inline float quantize_snorm(float value, double maximum) {
    if (!(value > -1.0f)) return value != value ? 0.0f : -1.0f;
    if (value >= 1.0f) return 1.0f;
    return static_cast<float>(round_half_even(double(value) * maximum) / maximum);
}

// ClearRenderTargetView takes FLOATs even for integer RTVs. Windows D3D12 (WARP and AMD) clamps to the
// component range and truncates fractions. Vulkan instead interprets the matching union member as an integer
// already; copying float bits changes the value without a validation error. The normalised formats are rounded like
// D3D (quantize_unorm, quantize_snorm).
inline VkClearColorValue render_target_clear_color(VkFormat format, const ClearColor& input) {
    // Integer formats convert the float numerically: NaN maps to zero, values below the range clamp to the low
    // end, positive overflow (and +infinity) to the high end, and fractions truncate toward zero. Unsigned uses
    // uint32, signed int32. A2B10G10R10 clamps RGB to 10 bits and alpha (channel 3) to 2 bits.
    constexpr double u32_max = 4294967295.0, s32_min = -2147483648.0, s32_max = 2147483647.0;
    double low = 0, high = 0, alpha_high = 0;
    bool is_integer = true, is_signed = false;
    std::array<double, 4> unorm{};  // the largest value of each normalised channel; 0 leaves the channel to the GPU
    double snorm = 0;               // the largest value of a signed normalised format
    switch (format) {
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8B8A8_UINT: high = 255; break;
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16B16A16_UINT: high = 65535; break;
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT: high = u32_max; break;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: high = 1023; alpha_high = 3; break;
    case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8A8_SINT: low = -128; high = 127; is_signed = true; break;
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16A16_SINT: low = -32768; high = 32767; is_signed = true; break;
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32A32_SINT: low = s32_min; high = s32_max; is_signed = true; break;
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8B8A8_SNORM: is_integer = false; snorm = 127; break;
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16B16A16_SNORM: is_integer = false; snorm = 32767; break;
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM: is_integer = false; unorm = {255, 255, 255, 255}; break;
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:  // the colour channels go through the sRGB curve on the GPU
        is_integer = false;
        unorm = {0, 0, 0, 255};
        break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: is_integer = false; unorm = {1023, 1023, 1023, 3}; break;
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16B16A16_UNORM: is_integer = false; unorm = {65535, 65535, 65535, 65535}; break;
    default: is_integer = false; break;
    }
    if (format != VK_FORMAT_A2B10G10R10_UINT_PACK32) alpha_high = high;
    VkClearColorValue value{};
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (!is_integer) {
            if (snorm != 0) value.float32[i] = quantize_snorm(input[i], snorm);
            else value.float32[i] = unorm[i] != 0.0 ? quantize_unorm(input[i], unorm[i]) : input[i];
            continue;
        }
        const double maximum = i == 3 ? alpha_high : high;
        const double v = double(input[i]);
        // In range the cast truncates toward zero; out-of-range values never reach a float-to-int cast.
        const double clamped = v != v ? 0.0 : v <= low ? low : v >= maximum ? maximum : v;
        if (is_signed) value.int32[i] = static_cast<std::int32_t>(clamped);
        else value.uint32[i] = static_cast<std::uint32_t>(clamped);
    }
    return value;
}

// The DXGI typeless family a color view format belongs to, by the Vulkan formats the layer maps its members to: the
// same nonzero number for formats one typeless resource can be viewed as, 0 for a format of no such family.
inline int typeless_view_family(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: return 1;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: return 3;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return 4;
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return 5;
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: return 6;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 7;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: return 8;
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return 9;
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT: return 10;
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: return 11;
    default: return 0;
    }
}

// The RGBA8 and BGRA8 UNORM and SRGB formats: a castable-format list may cross between the two channel orders.
inline bool rgba8_any_order(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

// The format a color view clears or renders in. A view of the resource's own format keeps it (a typeless resource
// has none, so it needs an explicit one). A typeless resource, or one created with a castable-format list (its image
// is mutable and lists the view formats), may be viewed in another format of its family: the image is mutable, so
// the view reinterprets its bits (D3D-275; before it only the RGBA8 family was). A castable RGBA8 or BGRA8 image may
// also be viewed in the other channel order, which the device let through only when the list names it. A volume
// keeps its own format.
inline VkFormat color_view_format(const ResourceBinding& resource, const TargetView& target) {
    if (target.format == ColorViewFormat::resource)
        return resource.typeless ? VK_FORMAT_UNDEFINED : resource.format;
    const auto format = static_cast<VkFormat>(target.format);
    if (resource.uint_backed && format == resource.uint_backed) return format;  // its logical format, listed at creation
    const int family = typeless_view_family(format);
    if (!family) return VK_FORMAT_UNDEFINED;
    if (!resource.typeless && !resource.castable_view_formats)
        return format == resource.format ? format : VK_FORMAT_UNDEFINED;
    const bool other_order = resource.castable_view_formats && rgba8_any_order(format) &&
                             rgba8_any_order(resource.format);
    if ((family != typeless_view_family(resource.format) && !other_order) || resource.volume || resource.depth != 1)
        return VK_FORMAT_UNDEFINED;
    return format;
}

inline bool view_range(const ResourceBinding& resource, const TargetView& view, VkImageAspectFlags aspects,
                       VkImageSubresourceRange& out) {
    if (view.mip >= resource.mip_levels) return false;
    if (resource.volume) {
        const auto depth = std::max(1u, resource.depth >> view.mip);
        if (aspects != VK_IMAGE_ASPECT_COLOR_BIT || view.first_layer >= depth ||
            !view.layer_count || view.layer_count > depth - view.first_layer) return false;
        // Layouts and transfers address one array layer for the whole 3D mip.
        out = {aspects, view.mip, 1, 0, 1};
        return true;
    }
    if (view.first_layer >= resource.array_layers || view.layer_count > resource.array_layers - view.first_layer)
        return false;
    out = {aspects, view.mip, 1, view.first_layer, view.layer_count};
    return true;
}

// Without separate depth/stencil layouts both planes of a combined image move together.
inline VkImageSubresourceRange planes_for_layout(const ResourceBinding& resource, VkImageSubresourceRange range,
                                                 bool separate) {
    if (two_planes(resource) && !separate) range.aspectMask = depth_stencil_aspects;
    return range;
}

inline VkExtent2D mip_extent(const ResourceBinding& resource, std::uint32_t mip) {
    return {std::max(1u, resource.width >> mip), std::max(1u, resource.height >> mip)};
}

// Into `out`, replacing what it held: a kept PreparedList entry reuses the vector's storage.
inline void clip(std::span<const Rect> rects, VkExtent2D extent, std::uint32_t layers, std::vector<VkClearRect>& out) {
    out.clear();
    for (const Rect& r : rects) {
        const std::int64_t left = std::max<std::int64_t>(r.left, 0), top = std::max<std::int64_t>(r.top, 0);
        const std::int64_t right = std::min<std::int64_t>(r.right, extent.width);
        const std::int64_t bottom = std::min<std::int64_t>(r.bottom, extent.height);
        if (right <= left || bottom <= top) continue;
        out.push_back({{{std::int32_t(left), std::int32_t(top)}, {std::uint32_t(right - left), std::uint32_t(bottom - top)}},
                       0, layers});
    }
}

inline constexpr VkPipelineStageFlags2 queue_stages_copy =
    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT |
    VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
inline constexpr VkPipelineStageFlags2 queue_stages_compute =
    queue_stages_copy | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
    VK_PIPELINE_STAGE_2_CLEAR_BIT;

inline void restrict_to_queue(const ReplayFunctions& vk, VkPipelineStageFlags2& stages, VkAccessFlags2& access) {
    if (!vk.queue_stages) return;
    VkAccessFlags2 allowed = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT |
                             VK_ACCESS_2_HOST_WRITE_BIT;
    if (vk.queue_stages & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
        allowed |= VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_UNIFORM_READ_BIT |
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    stages &= vk.queue_stages;
    if (!stages) stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    access &= allowed;
}

inline void restrict_to_queue(const ReplayFunctions& vk, VkPipelineStageFlags& stages, VkAccessFlags& access) {
    if (!vk.queue_stages) return;
    VkPipelineStageFlags2 s2 = stages;
    VkAccessFlags2 a2 = access;
    restrict_to_queue(vk, s2, a2);
    stages = static_cast<VkPipelineStageFlags>(s2 & 0xFFFFFFFFull);
    access = static_cast<VkAccessFlags>(a2 & 0xFFFFFFFFull);
}

// A D3D12 barrier on a volume mip covers every W slice, including on devices with maintenance9.
inline VkImageSubresourceRange layout_range(const ResourceBinding& resource, VkImageSubresourceRange range) {
    if (resource.volume) range.layerCount = VK_REMAINING_ARRAY_LAYERS;
    return range;
}

inline void image_barrier(const ReplayFunctions& vk, VkCommandBuffer command_buffer, const ResourceBinding& resource,
                          const VkImageSubresourceRange& range, VkImageLayout old_layout, VkImageLayout new_layout,
                          VkAccessFlags src_access, VkPipelineStageFlags src_stages, VkAccessFlags dst_access,
                          VkPipelineStageFlags dst_stages) {
    restrict_to_queue(vk, src_stages, src_access);
    restrict_to_queue(vk, dst_stages, dst_access);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = resource.image;
    barrier.subresourceRange = layout_range(resource, range);
    vk.pipeline_barrier(command_buffer, src_stages, dst_stages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

inline void memory_barrier(const ReplayFunctions& vk, VkCommandBuffer command_buffer, VkAccessFlags src_access,
                           VkPipelineStageFlags src_stages, VkAccessFlags dst_access, VkPipelineStageFlags dst_stages) {
    restrict_to_queue(vk, src_stages, src_access);
    restrict_to_queue(vk, dst_stages, dst_access);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    vk.pipeline_barrier(command_buffer, src_stages, dst_stages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

// The current layouts of a range: from the tracker, or `assumed` for an untracked image.
inline LayoutPieces current(const LayoutTracker* tracker, ResourceId id, const VkImageSubresourceRange& range,
                            VkImageLayout assumed) {
    LayoutPieces out;
    if (!tracker || !tracker->pieces(id, range, out)) {
        out.clear();
        out.push_back({range, assumed});
    }
    return out;
}

// Zero-fills a block-compressed image range via vkCmdCopyBufferToImage from either the device's zero page
// (`vk.zero_buffer`) or a zeroed transfer-scratch chunk (`vk.copy_scratch` + `vk.fill_buffer`). Vulkan forbids
// vkCmdClearColorImage on BC formats, yet D3D12 committed resources start zeroed unless CREATE_NOT_ZEROED is set
// (D3D-220).
inline bool zero_compressed_image(const ReplayFunctions& vk, VkCommandBuffer command_buffer,
                                  const ResourceBinding& resource, const VkImageSubresourceRange& range,
                                  VkImageLayout dst_layout) {
    const BlockFormat fmt = block_format(resource.format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (fmt.block < 1 || !fmt.bytes || !vk.copy_buffer_to_image) return false;
    const std::uint32_t mip_begin = range.baseMipLevel;
    const std::uint32_t mip_end =
        range.levelCount == VK_REMAINING_MIP_LEVELS ? resource.mip_levels
                                                    : std::min(resource.mip_levels, mip_begin + range.levelCount);
    const std::uint32_t layer_begin = range.baseArrayLayer;
    const std::uint32_t layer_end =
        range.layerCount == VK_REMAINING_ARRAY_LAYERS ? resource.array_layers
                                                      : std::min(resource.array_layers, layer_begin + range.layerCount);
    if (mip_begin >= mip_end || layer_begin >= layer_end) return true;

    constexpr VkDeviceSize kMaxChunk = 64u * 1024u;
    const std::uint32_t layer_count = layer_end - layer_begin;
    VkDeviceSize needed = 0;
    for (std::uint32_t mip = mip_begin; mip < mip_end; ++mip) {
        const VkExtent2D ext = mip_extent(resource, mip);
        const std::uint64_t bx = (std::uint64_t(ext.width) + fmt.block - 1) / fmt.block;
        const std::uint64_t by = (std::uint64_t(ext.height) + fmt.block - 1) / fmt.block;
        const std::uint64_t mip_d = std::max(1u, resource.depth >> mip);
        const std::uint64_t slice_bytes = bx * by * fmt.bytes;
        if (slice_bytes >= kMaxChunk || mip_d > kMaxChunk / slice_bytes ||
            layer_count > kMaxChunk / (slice_bytes * mip_d)) {
            needed = kMaxChunk;
            break;
        }
        needed = std::max<VkDeviceSize>(needed, slice_bytes * mip_d * layer_count);
    }
    if (!needed) return true;

    VkBuffer zero_buf = VK_NULL_HANDLE;
    VkDeviceSize zero_off = 0;
    VkDeviceSize zero_cap = 0;
    if (vk.zero_buffer && vk.zero_size >= fmt.bytes) {
        const VkDeviceSize align = fmt.bytes;
        const VkDeviceSize pad = (align - (vk.zero_offset % align)) % align;
        if (vk.zero_size >= pad + fmt.bytes) {
            zero_buf = vk.zero_buffer;
            zero_off = vk.zero_offset + pad;
            zero_cap = vk.zero_size - pad;
        }
    }
    if (!zero_buf && vk.copy_scratch && vk.fill_buffer) {
        // Round up to a multiple of fmt.bytes (8 or 16, always a multiple of 4 for vkCmdFillBuffer).
        const VkDeviceSize fill_bytes = ((needed + fmt.bytes - 1) / fmt.bytes) * fmt.bytes;
        VkBuffer scratch = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        if (vk.copy_scratch(fill_bytes, scratch, offset)) {
            const VkDeviceSize align = fmt.bytes;
            const VkDeviceSize pad = (align - (offset % align)) % align;
            if (fill_bytes >= pad + fmt.bytes) {
                vk.fill_buffer(command_buffer, scratch, offset + pad, fill_bytes - pad, 0);
                memory_barrier(vk, command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                zero_buf = scratch;
                zero_off = offset + pad;
                zero_cap = fill_bytes - pad;
            }
        }
    }
    if (!zero_buf || zero_cap < fmt.bytes) return false;

    std::vector<VkBufferImageCopy> regions;
    for (std::uint32_t mip = mip_begin; mip < mip_end; ++mip) {
        const VkExtent2D ext = mip_extent(resource, mip);
        const std::uint32_t mip_d = std::max(1u, resource.depth >> mip);
        const std::uint64_t bx_total = (std::uint64_t(ext.width) + fmt.block - 1) / fmt.block;
        const std::uint64_t by_total = (std::uint64_t(ext.height) + fmt.block - 1) / fmt.block;
        const std::uint64_t slice_bytes = bx_total * by_total * fmt.bytes;
        if (slice_bytes && slice_bytes <= zero_cap && mip_d <= zero_cap / slice_bytes &&
            layer_count <= zero_cap / (slice_bytes * mip_d)) {
            VkBufferImageCopy r{};
            r.bufferOffset = zero_off;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer_begin, layer_count};
            r.imageExtent = {ext.width, ext.height, mip_d};
            regions.push_back(r);
            continue;
        }
        const std::uint64_t row_bytes = bx_total * fmt.bytes;
        for (std::uint32_t layer = layer_begin; layer < layer_end; ++layer) {
            if (slice_bytes <= zero_cap) {
                const std::uint32_t slices_per_copy =
                    std::max<std::uint32_t>(1u, std::uint32_t(std::min<std::uint64_t>(mip_d, zero_cap / slice_bytes)));
                for (std::uint32_t z = 0; z < mip_d; z += slices_per_copy) {
                    VkBufferImageCopy r{};
                    r.bufferOffset = zero_off;
                    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
                    r.imageOffset = {0, 0, std::int32_t(z)};
                    r.imageExtent = {ext.width, ext.height, std::min(slices_per_copy, mip_d - z)};
                    regions.push_back(r);
                }
            } else if (row_bytes <= zero_cap) {
                const std::uint32_t rows_per_copy =
                    std::max<std::uint32_t>(1u, std::uint32_t(std::min<std::uint64_t>(by_total, zero_cap / row_bytes)));
                for (std::uint32_t z = 0; z < mip_d; ++z) {
                    for (std::uint64_t y_blk = 0; y_blk < by_total; y_blk += rows_per_copy) {
                        const std::uint32_t oy = std::uint32_t(y_blk * fmt.block);
                        const std::uint32_t eh = std::min<std::uint32_t>(rows_per_copy * fmt.block, ext.height - oy);
                        VkBufferImageCopy r{};
                        r.bufferOffset = zero_off;
                        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
                        r.imageOffset = {0, std::int32_t(oy), std::int32_t(z)};
                        r.imageExtent = {ext.width, eh, 1};
                        regions.push_back(r);
                    }
                }
            } else {
                const std::uint32_t cols_per_copy =
                    std::max<std::uint32_t>(1u, std::uint32_t(std::min<std::uint64_t>(bx_total, zero_cap / fmt.bytes)));
                for (std::uint32_t z = 0; z < mip_d; ++z) {
                    for (std::uint64_t y_blk = 0; y_blk < by_total; ++y_blk) {
                        const std::uint32_t oy = std::uint32_t(y_blk * fmt.block);
                        const std::uint32_t eh = std::min<std::uint32_t>(fmt.block, ext.height - oy);
                        for (std::uint64_t x_blk = 0; x_blk < bx_total; x_blk += cols_per_copy) {
                            const std::uint32_t ox = std::uint32_t(x_blk * fmt.block);
                            const std::uint32_t ew =
                                std::min<std::uint32_t>(cols_per_copy * fmt.block, ext.width - ox);
                            VkBufferImageCopy r{};
                            r.bufferOffset = zero_off;
                            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
                            r.imageOffset = {std::int32_t(ox), std::int32_t(oy), std::int32_t(z)};
                            r.imageExtent = {ew, eh, 1};
                            regions.push_back(r);
                        }
                    }
                }
            }
        }
    }
    if (!regions.empty())
        vk.copy_buffer_to_image(command_buffer, zero_buf, resource.image, dst_layout,
                                std::uint32_t(regions.size()), regions.data());
    return true;
}

// Images registered with the tracker in UNDEFINED have never been used. D3D12 committed resources start zeroed,
// so each such piece is zero-filled (clear for uncompressed images, zero-buffer copy for block-compressed formats
// when a zero page or transfer scratch is available) and moved to `layout`, the layout the operation about to run
// expects; with not_zeroed, it is only moved. The tracker records it.
inline void initialize(const ReplayFunctions& vk, VkCommandBuffer command_buffer, const ResourceBinding& resource,
                       ResourceId id, LayoutPieces& pieces, VkImageLayout layout, LayoutTracker* tracker) {
    constexpr VkAccessFlags any_memory = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    for (auto& piece : pieces) {
        if (piece.layout != VK_IMAGE_LAYOUT_UNDEFINED) continue;
        const BlockFormat fmt = block_format(resource.format, VK_IMAGE_ASPECT_COLOR_BIT);
        const bool color = piece.range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT;
        const bool can_clear = !vk.queue_stages || ((vk.queue_stages & VK_PIPELINE_STAGE_2_CLEAR_BIT) && color);
        const bool compressed = fmt.block > 1 || (!can_clear && color);
        if (resource.not_zeroed || (!can_clear && !color)) {
            image_barrier(vk, command_buffer, resource, piece.range, VK_IMAGE_LAYOUT_UNDEFINED, layout, 0,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, any_memory, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        } else if (compressed) {
            const bool can_zero =
                vk.copy_buffer_to_image &&
                ((vk.zero_buffer && vk.zero_size >= fmt.bytes) || (vk.copy_scratch && vk.fill_buffer));
            if (can_zero) {
                const VkImageLayout dst = image_layout(resource, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                image_barrier(vk, command_buffer, resource, piece.range, VK_IMAGE_LAYOUT_UNDEFINED, dst, 0,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT);
                const bool zeroed = zero_compressed_image(vk, command_buffer, resource, piece.range, dst);
                image_barrier(vk, command_buffer, resource, piece.range, dst, layout,
                              zeroed ? VK_ACCESS_TRANSFER_WRITE_BIT : 0,
                              zeroed ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, any_memory,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            } else {
                image_barrier(vk, command_buffer, resource, piece.range, VK_IMAGE_LAYOUT_UNDEFINED, layout, 0,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, any_memory, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            }
        } else {
            const VkImageLayout dst = image_layout(resource, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            image_barrier(vk, command_buffer, resource, piece.range, VK_IMAGE_LAYOUT_UNDEFINED, dst, 0,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
            if (piece.range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
                const VkClearColorValue zero{};
                vk.clear_color_image(command_buffer, resource.image, dst, &zero, 1, &piece.range);
            } else {
                const VkClearDepthStencilValue zero{};
                vk.clear_depth_stencil_image(command_buffer, resource.image, dst, &zero, 1, &piece.range);
            }
            image_barrier(vk, command_buffer, resource, piece.range, dst, layout, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, any_memory, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        piece.layout = layout;
        if (tracker) tracker->set(id, piece.range, layout);
    }
}

inline bool within(std::uint64_t offset, std::uint64_t size, std::uint64_t total) {
    return offset <= total && size <= total - offset;
}

inline VkDeviceSize indirect_record_size(IndirectKind kind) {
    switch (kind) {
    case IndirectKind::draw: return sizeof(VkDrawIndirectCommand);
    case IndirectKind::draw_indexed: return sizeof(VkDrawIndexedIndirectCommand);
    case IndirectKind::dispatch: return sizeof(VkDispatchIndirectCommand);
    }
    return 0;
}

inline bool indirect_range(const ResourceBinding& binding, VkDeviceSize offset, VkDeviceSize bytes) {
    return (binding.buffer_offset | offset) % sizeof(std::uint32_t) == 0 &&
           within(offset, bytes, binding.size) &&
           within(binding.buffer_offset, offset + bytes, UINT64_MAX);
}

// The bytes of a buffer UAV clear, and whether the transfer commands can write them. Elements of up to 4 bytes
// repeat in every aligned word, so vkCmdFillBuffer writes the aligned middle and copies from it the unaligned ends
// (vkCmdCopyBuffer has no alignment rule); ranges without that word use transfer scratch. Wider elements are written
// with vkCmdUpdateBuffer and doubled with copies, which needs a word-aligned range.
inline bool buffer_fill_needs_scratch(const uav_clear::Element& element, VkDeviceSize offset, VkDeviceSize size) {
    const VkDeviceSize prefix = (4 - offset % 4) % 4;
    return element.size <= 4 && size && size < prefix + 4;
}

inline ReplayResult prepare_buffer_uav_clear(const ClearUnorderedAccess& clear, Prepared& p) {
    const UavView& view = clear.view;
    if (!p.binding.buffer || p.binding.image) return ReplayResult::invalid_clear;
    if (view.offset >= p.binding.size) return ReplayResult::invalid_subresource;
    // A V# may reach past the end of the buffer (the hardware bounds-checks it); the clear stops at the end.
    const std::uint64_t size = std::min(view.size, p.binding.size - view.offset);
    if (view.format) {
        if (!uav_clear::encode(VkFormat(view.format), clear.is_float, clear.values, p.element))
            return ReplayResult::unsupported;
        if (view.element_size != p.element.size) return ReplayResult::invalid_clear;
    } else {
        p.element = uav_clear::raw_element(clear.values);
    }
    const std::uint32_t e = p.element.size;
    const auto add = [&](std::uint64_t offset, std::uint64_t bytes) {
        if (!bytes) return ReplayResult::success;
        if (offset % std::min(e, 4u)) return ReplayResult::invalid_clear;
        if (offset > UINT64_MAX - p.binding.buffer_offset ||
            bytes > UINT64_MAX - (p.binding.buffer_offset + offset)) return ReplayResult::invalid_clear;
        offset += p.binding.buffer_offset;  // the view is relative to the resource, which starts at buffer_offset
        if (e > 4 && bytes % 4) {
            return ReplayResult::unsupported;
        }
        p.fills.push_back({offset, bytes});
        return ReplayResult::success;
    };
    if (clear.rects.empty()) return add(view.offset, size);
    const std::uint64_t elements = size / view.element_size;
    for (const Rect& r : clear.rects) {
        if (r.top > 0 || r.bottom <= 0) continue;  // a buffer has one row
        const std::uint64_t left = std::uint64_t(std::max<std::int64_t>(r.left, 0));
        const std::uint64_t right = std::min<std::uint64_t>(std::uint64_t(std::max<std::int64_t>(r.right, 0)), elements);
        if (right <= left) continue;
        if (const auto result = add(view.offset + left * view.element_size, (right - left) * view.element_size);
            result != ReplayResult::success)
            return result;
    }
    if (p.fills.empty()) p.skip = true;
    return ReplayResult::success;
}

// Writes one prepared range; the caller has made the buffer writable by transfers.
inline void emit_buffer_fill(const ReplayFunctions& vk, VkCommandBuffer command_buffer, VkBuffer buffer,
                             const uav_clear::Element& element, VkDeviceSize offset, VkDeviceSize size,
                             VkBuffer scratch = VK_NULL_HANDLE, VkDeviceSize scratch_offset = 0) {
    const auto transfer_barrier = [&] {
        memory_barrier(vk, command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    };
    if (element.size <= 4) {
        std::uint32_t word = 0;
        for (unsigned i = 0; i < 4; ++i) word |= std::uint32_t(element.bytes[i % element.size]) << (8 * i);
        if (buffer_fill_needs_scratch(element, offset, size)) {
            // At most six bytes lack a complete aligned word. Fill eight scratch bytes, then copy just the
            // requested range: never borrow a word from the application's neighbouring bytes.
            vk.fill_buffer(command_buffer, scratch, scratch_offset, 8, word);
            transfer_barrier();
            const VkBufferCopy region{scratch_offset, offset, size};
            vk.copy_buffer(command_buffer, scratch, buffer, 1, &region);
            return;
        }
        const VkDeviceSize a0 = (offset + 3) / 4 * 4, a1 = (offset + size) / 4 * 4;
        if (a1 > a0) vk.fill_buffer(command_buffer, buffer, a0, a1 - a0, word);
        VkBufferCopy ends[2]{};
        std::uint32_t count = 0;
        if (a0 > offset) ends[count++] = {offset + 4, offset, a0 - offset};
        if (a1 < offset + size) ends[count++] = {a1 - 4, a1, offset + size - a1};
        if (count) {
            transfer_barrier();
            vk.copy_buffer(command_buffer, buffer, buffer, count, ends);
        }
        return;
    }
    constexpr VkDeviceSize update_limit = 65536;  // vkCmdUpdateBuffer's, a multiple of every element size
    const VkDeviceSize first = std::min(size, update_limit);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(first));
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = element.bytes[i % element.size];
    vk.update_buffer(command_buffer, buffer, offset, first, data.data());
    for (VkDeviceSize filled = first; filled < size;) {
        const VkDeviceSize step = std::min(filled, size - filled);
        transfer_barrier();
        const VkBufferCopy copy{offset, offset + filled, step};
        vk.copy_buffer(command_buffer, buffer, buffer, 1, &copy);
        filled += step;
    }
}

// The one plane of one subresource a texture copy location names.
inline bool copy_subresource(const ResourceBinding& resource, std::uint32_t subresource, VkImageSubresourceRange& out) {
    return resource.image && subresource != all_subresources && image_range(resource, subresource, out);
}

// CopyTextureRegion: texture to texture, footprint to texture (upload) or texture to footprint (readback).
// Block-compressed footprints are padded to whole blocks; the Vulkan extent stops at the mip edge.
inline ReplayResult prepare_texture_copy(const CopyTexture& copy, Prepared& p) {
    if (!copy.dst.footprint && !copy.src.footprint) {
        VkImageSubresourceRange dst{}, src{};
        if (!copy_subresource(p.binding, copy.dst.subresource, dst) ||
            !copy_subresource(p.source, copy.src.subresource, src))
            return ReplayResult::invalid_subresource;
        if ((dst.aspectMask != src.aspectMask && !depth_colour_pair(p.binding, p.source)) ||
            p.binding.volume != p.source.volume || p.binding.samples != p.source.samples ||
            p.binding.rgb96 != p.source.rgb96)
            return ReplayResult::invalid_copy;
        p.same_copy_subresource = p.source.image == p.binding.image && src.aspectMask == dst.aspectMask &&
                                  src.baseMipLevel == dst.baseMipLevel && src.baseArrayLayer == dst.baseArrayLayer;
        const VkExtent2D from = mip_extent(p.source, src.baseMipLevel), to = mip_extent(p.binding, dst.baseMipLevel);
        const auto from_depth = std::max(1u, p.source.depth >> src.baseMipLevel);
        const auto to_depth = std::max(1u, p.binding.depth >> dst.baseMipLevel);
        const CopyBox box = copy.has_box ? copy.box : CopyBox{0, 0, 0, from.width, from.height, from_depth};
        // A block-compressed box is in whole blocks and may run to the block-padded edge of the mip (the host runtime
        // accepts it); Vulkan wants the extent to stop at the mip edge, as with a footprint.
        const std::uint32_t block = block_format(p.source.format, src.aspectMask).block;
        const auto padded = [block](std::uint32_t size) { return (size + block - 1) / block * block; };
        if (box.right > padded(from.width) || box.bottom > padded(from.height) || box.back > from_depth)
            return ReplayResult::invalid_copy;
        if (const std::uint32_t dst_block = block_format(p.binding.format, dst.aspectMask).block; dst_block != block) {
            // A BC format and its uncompressed equivalent ("Format Conversion using Direct3D 10.1"): one texel of the
            // uncompressed side is one block of the other. The box is in source texels, the offset in destination
            // texels, and Vulkan takes the extent in source texels, cut at the source's mip edge.
            const auto blocks = [](std::uint32_t size, std::uint32_t b) { return (size + b - 1) / b; };
            const bool same_bytes =
                block_format(p.binding.format, dst.aspectMask).bytes == block_format(p.source.format, src.aspectMask).bytes;
            if (!same_bytes || (box.left | box.top) % block || (copy.x | copy.y) % dst_block || box.left >= from.width ||
                box.top >= from.height || !within(copy.z, box.back - box.front, to_depth) ||
                copy.x / dst_block + blocks(box.right - box.left, block) > blocks(to.width, dst_block) ||
                copy.y / dst_block + blocks(box.bottom - box.top, block) > blocks(to.height, dst_block))
                return ReplayResult::invalid_copy;
            const std::uint32_t w = std::min(padded(box.right - box.left), from.width - box.left);
            const std::uint32_t h = std::min(padded(box.bottom - box.top), from.height - box.top);
            const VkImageCopy region{{src.aspectMask, src.baseMipLevel, src.baseArrayLayer, 1},
                                     {std::int32_t(box.left), std::int32_t(box.top), std::int32_t(box.front)},
                                     {dst.aspectMask, dst.baseMipLevel, dst.baseArrayLayer, 1},
                                     {std::int32_t(copy.x), std::int32_t(copy.y), std::int32_t(copy.z)},
                                     {w, h, box.back - box.front}};
            const std::uint32_t dst_w = w * dst_block, dst_h = h * dst_block;
            if (dst_block > 1 && (copy.x + dst_w > to.width || copy.y + dst_h > to.height)) {
                // Each uncompressed texel supplies a whole BC block. Keep the same scratch row and slice strides
                // in bytes on both sides, with a destination extent clipped to the actual mip.
                const VkBufferImageCopy read{0, w, h, region.srcSubresource, region.srcOffset, region.extent};
                const VkBufferImageCopy write{0, dst_w, dst_h, region.dstSubresource, region.dstOffset,
                    {std::min(dst_w, to.width - copy.x), std::min(dst_h, to.height - copy.y), region.extent.depth}};
                p.staged_reads.push_back(read);
                p.staged_writes.push_back(write);
            } else {
                p.image_copies.push_back(region);
            }
            p.range = dst;
            p.source_range = src;
            return ReplayResult::success;
        }
        std::uint32_t w = box.right - box.left, h = box.bottom - box.top;
        const std::uint32_t d = box.back - box.front;
        if (p.same_copy_subresource) {
            const auto intersects = [](std::uint32_t a, std::uint32_t b, std::uint32_t size) {
                return (a < b ? b - a : a - b) < size;
            };
            if (intersects(box.left, copy.x, padded(w)) && intersects(box.top, copy.y, padded(h)) &&
                intersects(box.front, copy.z, d))
                return ReplayResult::invalid_copy;
        }
        if (block > 1) {
            // D3D12 copies whole blocks: the box is rounded up to blocks and must fit the destination's padded mip.
            // Vulkan takes an extent that is a block multiple or reaches the edge of each image separately, so the
            // extent is cut where it reaches an edge, and a copy that cannot satisfy both images is staged (below).
            if ((box.left | box.top | copy.x | copy.y) % block || box.left >= from.width || box.top >= from.height ||
                copy.x >= padded(to.width) || copy.y >= padded(to.height))
                return ReplayResult::invalid_copy;
            w = padded(w);
            h = padded(h);
            if (w > padded(to.width) - copy.x || h > padded(to.height) - copy.y) return ReplayResult::invalid_copy;
            // Each image's own extent: the whole blocks, cut where they reach that image's edge.
            const std::uint32_t src_w = std::min(w, from.width - box.left), src_h = std::min(h, from.height - box.top);
            const std::uint32_t dst_w = std::min(w, to.width - copy.x), dst_h = std::min(h, to.height - copy.y);
            const std::uint32_t blocks_w = w, blocks_h = h;
            w = std::min(src_w, dst_w);
            h = std::min(src_h, dst_h);
            const auto fits = [block](std::uint32_t offset, std::uint32_t extent, std::uint32_t size) {
                return extent % block == 0 || offset + extent == size;
            };
            if (!fits(box.left, w, from.width) || !fits(copy.x, w, to.width) || !fits(box.top, h, from.height) ||
                !fits(copy.y, h, to.height)) {
                // No one extent does for both images: a mip edge inside a block on one side only (a 4x4 box into a
                // 3x3 mip, a 6x6 mip into a 12x12 one). The source's blocks go into transfer scratch with its own
                // cut extent and from there into the destination with the destination's (`staged_reads` and
                // `staged_writes`, offsets from the start of the scratch; prepare allocates it). The box fits the
                // source's padded mip, so the scratch holds every block the destination reads.
                if (!within(copy.z, d, to_depth)) return ReplayResult::invalid_copy;
                VkBufferImageCopy read{};
                read.bufferRowLength = blocks_w;
                read.bufferImageHeight = blocks_h;
                read.imageSubresource = {src.aspectMask, src.baseMipLevel, src.baseArrayLayer, 1};
                read.imageOffset = {std::int32_t(box.left), std::int32_t(box.top), std::int32_t(box.front)};
                read.imageExtent = {src_w, src_h, d};
                VkBufferImageCopy write = read;
                write.imageSubresource = {dst.aspectMask, dst.baseMipLevel, dst.baseArrayLayer, 1};
                write.imageOffset = {std::int32_t(copy.x), std::int32_t(copy.y), std::int32_t(copy.z)};
                write.imageExtent = {dst_w, dst_h, d};
                p.staged_reads.push_back(read);
                p.staged_writes.push_back(write);
                p.range = dst;
                p.source_range = src;
                return ReplayResult::success;
            }
        }
        if (!within(copy.x, w, to.width) || !within(copy.y, h, to.height) || !within(copy.z, d, to_depth))
            return ReplayResult::invalid_copy;
        p.image_copies.push_back({{src.aspectMask, src.baseMipLevel, src.baseArrayLayer, 1},
                                  {std::int32_t(box.left), std::int32_t(box.top), std::int32_t(box.front)},
                                  {dst.aspectMask, dst.baseMipLevel, dst.baseArrayLayer, 1},
                                  {std::int32_t(copy.x), std::int32_t(copy.y), std::int32_t(copy.z)},
                                  {w, h, d}});
        p.range = dst;
        p.source_range = src;
        return ReplayResult::success;
    }

    const bool upload = copy.src.footprint;
    const ResourceBinding& image = upload ? p.binding : p.source;
    const ResourceBinding& buffer = upload ? p.source : p.binding;
    const CopyLocation& texture = upload ? copy.dst : copy.src;
    const CopyLocation& placed = upload ? copy.src : copy.dst;
    if (!buffer.buffer) return ReplayResult::invalid_copy;
    VkImageSubresourceRange range{};
    if (!copy_subresource(image, texture.subresource, range)) return ReplayResult::invalid_subresource;
    if (image.samples != 1) return ReplayResult::invalid_copy;  // multisampled textures have no footprint
    const BlockFormat format = copy_block_format(image, range.aspectMask);
    // A 12-byte texel does not divide D3D12's 256-byte pitches; an rgb96 copy hands Vulkan the scratch buffer of
    // the repack, never this pitch, and the pass reads the footprint in words.
    if (!format.bytes || placed.row_pitch % (image.rgb96 ? 4 : format.bytes)) return ReplayResult::unsupported;
    const VkExtent2D mip = mip_extent(image, range.baseMipLevel);
    const auto mip_depth = std::max(1u, image.depth >> range.baseMipLevel);
    // The box is in the source's coordinates: the footprint for an upload, the texture for a readback.
    const CopyBox box = copy.has_box ? copy.box
                        : upload     ? CopyBox{0, 0, 0, placed.width, placed.height, placed.depth}
                                     : CopyBox{0, 0, 0, mip.width, mip.height, mip_depth};
    const std::uint32_t texture_x = upload ? copy.x : box.left, texture_y = upload ? copy.y : box.top;
    const std::uint32_t placed_x = upload ? box.left : copy.x, placed_y = upload ? box.top : copy.y;
    std::uint32_t w = box.right - box.left, h = box.bottom - box.top;
    const auto texture_z = upload ? copy.z : box.front, placed_z = upload ? box.front : copy.z;
    const auto depth = box.back - box.front;
    if (!depth || !within(placed_z, depth, placed.depth) || !within(texture_z, depth, mip_depth) ||
        !within(placed_x, w, placed.width) || !within(placed_y, h, placed.height)) return ReplayResult::invalid_copy;
    if (texture_x >= mip.width || texture_y >= mip.height) return ReplayResult::invalid_copy;
    if (format.block > 1) {
        w = std::min(w, mip.width - texture_x);
        h = std::min(h, mip.height - texture_y);
    }
    if (!within(texture_x, w, mip.width) || !within(texture_y, h, mip.height)) return ReplayResult::invalid_copy;
    if ((placed_x | placed_y | texture_x | texture_y) % format.block) return ReplayResult::invalid_copy;
    const auto slice_rows = (std::uint64_t(placed.height) + format.block - 1) / format.block;
    const std::uint64_t slice_pitch = slice_rows * placed.row_pitch;
    if (!slice_pitch || placed.offset > buffer.size || placed_z > (buffer.size - placed.offset) / slice_pitch)
        return ReplayResult::invalid_copy;
    const auto slice_start = placed.offset + placed_z * slice_pitch;
    const auto in_slice = std::uint64_t(placed_y / format.block) * placed.row_pitch +
                          std::uint64_t(placed_x / format.block) * format.bytes;
    if (!within(slice_start, in_slice, buffer.size)) return ReplayResult::invalid_copy;
    const auto offset = slice_start + in_slice;
    // Vulkan wants a buffer offset that is a multiple of the texel size (VUID-vkCmdCopyBufferToImage-dstImage-07975),
    // which D3D12's 512-byte placement does not give a 12-byte texel: one more reason an rgb96 copy is repacked.
    const std::uint32_t alignment = (range.aspectMask & depth_stencil_aspects) || image.rgb96 ? 4 : format.bytes;
    if (offset % alignment && !copy.tile_footprint) return ReplayResult::unsupported;
    const std::uint64_t rows = (std::uint64_t(h) + format.block - 1) / format.block;
    const std::uint64_t row_bytes = (std::uint64_t(w) + format.block - 1) / format.block * format.bytes;
    if (!rows || !row_bytes || !within(std::uint64_t(placed_x / format.block) * format.bytes, row_bytes, placed.row_pitch) ||
        depth - 1 > (buffer.size - offset) / slice_pitch ||
        !within(offset + (depth - 1) * slice_pitch, (rows - 1) * placed.row_pitch + row_bytes, buffer.size))
        return ReplayResult::invalid_copy;
    if (buffer.size > UINT64_MAX - buffer.buffer_offset) return ReplayResult::invalid_copy;
    const VkDeviceSize backing_offset = buffer.buffer_offset + offset;
    if (backing_offset % alignment && !copy.tile_footprint) return ReplayResult::unsupported;
    VkBufferImageCopy region{};
    region.bufferOffset = backing_offset;
    region.bufferRowLength = placed.row_pitch / format.bytes * format.block;
    region.bufferImageHeight = depth > 1 ? std::uint32_t(slice_rows * format.block) : 0;
    region.imageSubresource = {range.aspectMask, range.baseMipLevel, range.baseArrayLayer, 1};
    region.imageOffset = {std::int32_t(texture_x), std::int32_t(texture_y), std::int32_t(texture_z)};
    region.imageExtent = {w, h, depth};
    if (image.rgb96) {  // the repack's scratch replaces this buffer in the copy; these are for the pass
        if (placed.row_pitch / 4 > UINT32_MAX || slice_pitch / 4 > UINT32_MAX) return ReplayResult::unsupported;
        p.rgb96_row_words = std::uint32_t(placed.row_pitch / 4);
        p.rgb96_slice_words = std::uint32_t(slice_pitch / 4);
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
    }
    p.buffer_image_copies.push_back(region);
    (upload ? p.range : p.source_range) = range;
    return ReplayResult::success;
}

// RGBA8 CopyTiles accepts arbitrary byte offsets into its linear buffer, while VkBufferImageCopy requires texel
// alignment. Stage only the occupied texels through aligned transfer scratch; leave partial-tile padding alone.
inline bool stage_unaligned_tile(const ReplayFunctions& vk, Prepared& p, bool upload) {
    auto& region = p.buffer_image_copies.front();
    const VkDeviceSize row_bytes = VkDeviceSize(region.imageExtent.width) * 4;
    const VkDeviceSize pitch = VkDeviceSize(region.bufferRowLength) * 4;
    const auto rows = region.imageExtent.height;
    VkDeviceSize scratch_offset = 0;
    if (!vk.copy_scratch || !vk.copy_scratch(row_bytes * rows, p.tile_scratch, scratch_offset)) return false;
    const auto app_offset = region.bufferOffset;
    const auto add_copy = [&](VkDeviceSize linear, VkDeviceSize scratch, VkDeviceSize bytes) {
        p.tile_rows.push_back(upload ? VkBufferCopy{linear, scratch, bytes} : VkBufferCopy{scratch, linear, bytes});
    };
    if (row_bytes == pitch) {
        add_copy(app_offset, scratch_offset, row_bytes * rows);
    } else {
        for (std::uint32_t row = 0; row < rows; ++row)
            add_copy(app_offset + row * pitch, scratch_offset + row * row_bytes, row_bytes);
    }
    region.bufferOffset = scratch_offset;
    region.bufferRowLength = 0;
    return true;
}

// CopyResource: the whole buffer, or every subresource of two textures of the same shape.
inline ReplayResult prepare_resource_copy(Prepared& p) {
    if (p.binding.buffer && p.source.buffer) {
        if (p.binding.size != p.source.size || !p.source.size) return ReplayResult::invalid_copy;
        if (p.source.size > UINT64_MAX - p.source.buffer_offset ||
            p.binding.size > UINT64_MAX - p.binding.buffer_offset)
            return ReplayResult::invalid_copy;
        const VkDeviceSize src = p.source.buffer_offset;
        const VkDeviceSize dst = p.binding.buffer_offset;
        if (p.source.buffer == p.binding.buffer && src < dst + p.source.size && dst < src + p.source.size)
            return ReplayResult::invalid_copy;
        p.buffer_copies.push_back({src, dst, p.source.size});
        return ReplayResult::success;
    }
    const ResourceBinding &dst = p.binding, &src = p.source;
    // A BC texture and one of its uncompressed equivalent match block for texel ("Format Conversion using
    // Direct3D 10.1"); the Vulkan extent is in source texels.
    const std::uint32_t sb = block_format(src.format, VK_IMAGE_ASPECT_COLOR_BIT).block;
    const std::uint32_t db = block_format(dst.format, VK_IMAGE_ASPECT_COLOR_BIT).block;
    const auto blocks = [](std::uint32_t size, std::uint32_t b) { return (size + b - 1) / b; };
    const bool same_shape = sb == db ? dst.width == src.width && dst.height == src.height
                                     : blocks(dst.width, db) == blocks(src.width, sb) &&
                                           blocks(dst.height, db) == blocks(src.height, sb);
    if (!dst.image || !src.image || !same_shape ||
        dst.depth != src.depth || dst.volume != src.volume || dst.mip_levels != src.mip_levels ||
        dst.array_layers != src.array_layers || (dst.aspects != src.aspects && !depth_colour_pair(dst, src)) ||
        dst.samples != src.samples || dst.rgb96 != src.rgb96 ||
        block_format(dst.format, VK_IMAGE_ASPECT_COLOR_BIT).bytes != block_format(src.format, VK_IMAGE_ASPECT_COLOR_BIT).bytes)
        return ReplayResult::invalid_copy;
    // A depth <-> colour pair has one aspect on each side; the copies name each side's own (staged by the caller).
    const bool cross = dst.aspects != src.aspects;
    for (const VkImageAspectFlags aspect : {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT}) {
        if (!(dst.aspects & aspect)) continue;
        const VkImageAspectFlags src_aspect = cross ? src.aspects : aspect;
        for (std::uint32_t mip = 0; mip < dst.mip_levels; ++mip) {
            const VkExtent2D extent = mip_extent(src, mip), to = mip_extent(dst, mip);
            // Lower mips can differ in blocks (BC 12x8 mip 1 is 2x1 blocks, R32G32 3x2 mip 1 is 1x1): more source
            // blocks than the destination holds is refused, as by WARP. shortcut: a BC destination mip with a partial
            // edge block cannot take a whole uncompressed texel per block in one vkCmdCopyImage; refused until such
            // a copy is staged through a buffer.
            if (sb != db && (blocks(extent.width, sb) * db > to.width || blocks(extent.height, sb) * db > to.height))
                return ReplayResult::invalid_copy;
            p.image_copies.push_back({{src_aspect, mip, 0, dst.array_layers}, {0, 0, 0}, {aspect, mip, 0, dst.array_layers},
                                      {0, 0, 0}, {extent.width, extent.height, std::max(1u, src.depth >> mip)}});
        }
    }
    p.range = {dst.aspects, 0, dst.mip_levels, 0, dst.array_layers};
    p.source_range = {src.aspects, 0, src.mip_levels, 0, src.array_layers};
    return ReplayResult::success;
}

// Resolve a multisampled colour image into a single-sampled image of the same format, or (a region) one plane of a
// depth/stencil image into the same plane of another: MIN or MAX, or AVERAGE of depth.
inline ReplayResult prepare_resolve(const ResolveSubresource& resolve, Prepared& p) {
    VkImageSubresourceRange dst{}, src{};
    if (!copy_subresource(p.binding, resolve.dst_subresource, dst) ||
        !copy_subresource(p.source, resolve.src_subresource, src))
        return ReplayResult::invalid_subresource;
    if (p.source.samples <= 1 || p.binding.samples != 1 || p.binding.volume) return ReplayResult::invalid_copy;
    const VkExtent2D from = mip_extent(p.source, src.baseMipLevel), to = mip_extent(p.binding, dst.baseMipLevel);
    VkOffset3D src_offset{}, dst_offset{};
    VkExtent3D extent{from.width, from.height, 1};
    if (const auto& region = resolve.region) {
        dst_offset = {std::int32_t(region->dst_x), std::int32_t(region->dst_y), 0};
        if (const auto& r = region->src_rect) {
            if (std::uint32_t(r->right) > from.width || std::uint32_t(r->bottom) > from.height)
                return ReplayResult::invalid_copy;
            src_offset = {r->left, r->top, 0};
            extent = {std::uint32_t(r->right - r->left), std::uint32_t(r->bottom - r->top), 1};
        }
        if (!within(region->dst_x, extent.width, to.width) || !within(region->dst_y, extent.height, to.height))
            return ReplayResult::invalid_copy;
    } else if (from.width != to.width || from.height != to.height) {
        return ReplayResult::invalid_copy;
    }
    if (src.aspectMask != dst.aspectMask || p.source.format != p.binding.format) return ReplayResult::unsupported;
    const auto& region = resolve.region;
    if (src.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
        if (region && region->mode != d3d12_resolve_mode::average) return ReplayResult::unsupported;
    } else {
        // A rendering scope resolves each pixel in place, so the destination offset must be the source's.
        if (!region || dst_offset.x != src_offset.x || dst_offset.y != src_offset.y ||
            (region->mode == d3d12_resolve_mode::average && src.aspectMask != VK_IMAGE_ASPECT_DEPTH_BIT))
            return ReplayResult::unsupported;
        p.resolve_mode = region->mode == d3d12_resolve_mode::minimum   ? VK_RESOLVE_MODE_MIN_BIT
                       : region->mode == d3d12_resolve_mode::maximum   ? VK_RESOLVE_MODE_MAX_BIT
                                                                       : VK_RESOLVE_MODE_AVERAGE_BIT;
    }
    p.resolve = {{src.aspectMask, src.baseMipLevel, src.baseArrayLayer, 1}, src_offset,
                 {dst.aspectMask, dst.baseMipLevel, dst.baseArrayLayer, 1}, dst_offset, extent};
    p.range = dst;
    p.source_range = src;
    return ReplayResult::success;
}

// A depth or stencil resolve of a rectangle runs in a rendering scope whose render area is the rectangle, and Vulkan
// resolves only inside it. MoltenVK resolves the whole destination subresource instead: the resolve is Metal's
// store action, which has no render area (measured on MoltenVK 1.4.2 and 1.4.3, docs/d3d12_vulkan.md). No Vulkan
// property tells, so on every GPU the texels around the rectangle go into `staged` before the resolve and come back
// after it, in up to four bands (`staged_reads`, one region per band, used in both directions).
inline ReplayResult keep_around_resolve(const ReplayFunctions& vk, Prepared& p) {
    const VkExtent2D to = mip_extent(p.binding, p.range.baseMipLevel);
    const auto x0 = std::uint32_t(p.resolve.dstOffset.x), y0 = std::uint32_t(p.resolve.dstOffset.y);
    const std::uint32_t x1 = x0 + p.resolve.extent.width, y1 = y0 + p.resolve.extent.height;
    const VkRect2D bands[4] = {
        {{0, 0}, {to.width, y0}},                                         // above
        {{0, std::int32_t(y1)}, {to.width, to.height - y1}},              // below
        {{0, std::int32_t(y0)}, {x0, y1 - y0}},                           // left
        {{std::int32_t(x1), std::int32_t(y0)}, {to.width - x1, y1 - y0}}, // right
    };
    // Depth aspects want 4-byte buffer offsets; 16 covers every texel size here.
    const std::uint32_t texel = block_format(p.binding.format, p.range.aspectMask).bytes;
    VkDeviceSize total = 0;
    for (const VkRect2D& band : bands) {
        if (!band.extent.width || !band.extent.height) continue;
        VkBufferImageCopy region{};
        region.bufferOffset = total;
        region.imageSubresource = p.resolve.dstSubresource;
        region.imageOffset = {band.offset.x, band.offset.y, 0};
        region.imageExtent = {band.extent.width, band.extent.height, 1};
        p.staged_reads.push_back(region);
        total += (VkDeviceSize(band.extent.width) * band.extent.height * texel + 15) / 16 * 16;
    }
    if (p.staged_reads.empty()) return ReplayResult::success;  // the rectangle is the whole subresource
    VkDeviceSize base = 0;
    if (!texel || !vk.copy_image_to_buffer || !vk.copy_buffer_to_image || !vk.copy_scratch ||
        !vk.copy_scratch(total, p.staged, base))
        return ReplayResult::unsupported;
    for (VkBufferImageCopy& region : p.staged_reads) region.bufferOffset += base;
    return ReplayResult::success;
}

// The layout a copy uses for a range: D3D12 requires copy_dest/copy_source, or common (GENERAL), where an implicit
// promotion lets the copy run in place. Anything else is moved to `wanted` and tracked.
inline VkImageLayout copy_layout(const ReplayFunctions& vk, VkCommandBuffer command_buffer,
                                 const ResourceBinding& resource, ResourceId id, const VkImageSubresourceRange& range,
                                 VkImageLayout wanted, VkAccessFlags access, LayoutTracker* tracker) {
    const VkImageLayout target = image_layout(resource, wanted);
    auto pieces = current(tracker, id, range, target);
    initialize(vk, command_buffer, resource, id, pieces, target, tracker);
    // D3D12 orders back-to-back copies and resolves on a resource that stays in COPY_DEST or COPY_SOURCE without a
    // barrier from the application; Vulkan does not. A piece already in the transfer layout gets no layout
    // transition, so an earlier transfer write to it is ordered here instead (write-after-write and read-after-write,
    // which synchronization validation reports otherwise).
    const auto after_transfer_writes = [&] {
        memory_barrier(vk, command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, access,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
    };
    if (pieces.size() == 1 && (pieces[0].layout == target || pieces[0].layout == VK_IMAGE_LAYOUT_GENERAL)) {
        after_transfer_writes();
        return pieces[0].layout;
    }
    bool ordered = false;
    for (const auto& piece : pieces) {
        if (piece.layout == target) {
            if (!ordered) after_transfer_writes();
            ordered = true;
            continue;
        }
        image_barrier(vk, command_buffer, resource, piece.range, piece.layout, target,
                      VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, access,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    if (tracker) tracker->set(id, range, target);
    return target;
}

// Moves every piece to `target` (src: the D3D12 state the operation requires), runs `body`, and moves the pieces
// back to where they were. Pieces already in `target` need no transition.
template<typename Body>
void round_trip(const ReplayFunctions& vk, VkCommandBuffer command_buffer, const ResourceBinding& resource,
                const LayoutPieces& pieces, VkImageLayout target, const VulkanAccess& state,
                VkAccessFlags target_access, VkPipelineStageFlags target_stages, Body&& body) {
    for (const auto& piece : pieces) {
        if (piece.layout == target) {
            memory_barrier(vk, command_buffer, state.access, state.stages, target_access, target_stages);
            continue;
        }
        image_barrier(vk, command_buffer, resource, piece.range, piece.layout, target, state.access, state.stages,
                      target_access, target_stages);
    }
    body();
    for (const auto& piece : pieces) {
        if (piece.layout == target) {
            memory_barrier(vk, command_buffer, target_access, target_stages, state.access, state.stages);
            continue;
        }
        image_barrier(vk, command_buffer, resource, piece.range, target, piece.layout, target_access, target_stages,
                      state.access, state.stages);
    }
}

} // namespace detail

namespace detail {

// Pulls the title's page content for a draw's root arguments and descriptor tables (docs/page_content.md). A descriptor
// table's VA is scanned in the console heap's mirror for the V#s it holds (each V# names a buffer range whose bytes are
// pulled from the title's pages); any other 64-bit user-data window may be a root descriptor VA, for which a page
// window is pulled. Both calls are filtered by the title-VA map, so an ordinary arena VA or a numeric constant costs a
// lookup and nothing else.
inline void draw_va_pulls(const ReplayFunctions& vk, const Draw& draw, const GraphicsPipeline& pipeline) {
    if (!vk.pull_title_pages && !vk.pull_title_table) return;
    for (unsigned i = 0; i + 1 < 32; ++i) {
        const std::uint64_t value = std::uint64_t(draw.user_data[i]) | (std::uint64_t(draw.user_data[i + 1]) << 32);
        if (!value) continue;
        if ((pipeline.table_windows >> i) & 1u) {
            if (vk.pull_title_table) vk.pull_title_table(value, 64);
        } else if (vk.pull_title_pages) {
            vk.pull_title_pages(value, 0x10000);
        }
    }
}

// Resolves a draw's pipeline, attachments and buffers. Attachment views take the pipeline's formats, so a
// typeless texture is viewed as the format the pipeline renders.
template<typename Resolve>
ReplayResult prepare_draw(const Draw& draw, Resolve&& resolve, const ReplayFunctions& vk, PreparedDraw& p) {
    if (!vk.graphics_pipeline || !vk.buffer_at || !vk.image_view || !vk.begin_rendering || !vk.end_rendering ||
        !vk.bind_vertex_buffers2 || !vk.bind_index_buffer || !vk.set_viewport || !vk.set_scissor ||
        !vk.set_primitive_topology || !vk.set_primitive_restart_enable || !vk.set_stencil_reference ||
        !vk.set_blend_constants || !vk.draw ||
        !vk.draw_indexed || !vk.bind_pipeline || !vk.push_constants)
        return ReplayResult::unsupported;
    // A title VA the layer could not give an arena stand-in would reach a shader unrebased and fault the GPU. Such a
    // draw stays refused, never executed (design B, docs/page_address_space.md). Unset when no title VAs exist.
    {
        // Diagnostic (bounded): the 64-bit user-data values of the first draws, so the VAs the title's root arguments
        // name are measured rather than assumed. Values only; no call changes.
        static std::atomic<unsigned> dumped{0};
        if (dumped.fetch_add(1, std::memory_order_relaxed) < 4) {
            std::fprintf(stderr, "[rt] replay: draw user data 64-bit windows:");
            for (unsigned i = 0; i + 1 < 32; ++i) {
                const std::uint64_t value = std::uint64_t(draw.user_data[i]) |
                                            (std::uint64_t(draw.user_data[i + 1]) << 32);
                if (value >> 32) std::fprintf(stderr, " s%u=0x%llx", i, static_cast<unsigned long long>(value));
            }
            std::fprintf(stderr, "\n");
        }
    }
    if (vk.console_va_ready && !vk.console_va_ready()) return unresolved_at("draw.console_va");
    const GraphicsPipeline* pipeline = vk.graphics_pipeline(draw.pipeline);
    if (!pipeline || !pipeline->pipeline) return unresolved_at("draw.pipeline", draw.pipeline);
    if (pipeline->mesh != draw.mesh) return ReplayResult::invalid_draw;
    if (draw.mesh && !vk.draw_mesh_tasks) return ReplayResult::unsupported;
    if (pipeline->bindless_set && !vk.bind_descriptor_sets) return ReplayResult::unsupported;
    if (pipeline->color_formats.size() > max_render_targets) return ReplayResult::unsupported;
    p.graphics = pipeline;
    p.pipeline = pipeline->pipeline;
    p.area = {UINT32_MAX, UINT32_MAX};
    std::uint32_t layers = UINT32_MAX;
    const auto shrink = [&](const ResourceBinding& b, const TargetView& view, const VkImageSubresourceRange& range) {
        const VkExtent2D e = mip_extent(b, view.mip);
        p.area = {std::min(p.area.width, e.width), std::min(p.area.height, e.height)};
        layers = std::min(layers, range.layerCount);
    };

    p.colors.assign(pipeline->color_formats.size(), {});
    const std::uint32_t render_target_count = draw.views ? draw.views->render_target_count : 0;
    for (std::size_t slot = 0; slot < render_target_count && slot < p.colors.size(); ++slot) {
        const TargetView& target = draw.views->render_targets[slot];
        if (!target.resource) continue;
        const ResourceBinding* binding = resolve(target.resource);
        if (!binding) return unresolved_at("draw.render_target", target.resource, slot);
        auto& a = p.colors[slot];
        a.binding = *binding;
        a.id = target.resource;
        if (!a.binding.image || a.binding.aspects != VK_IMAGE_ASPECT_COLOR_BIT) return ReplayResult::invalid_clear;
        // D3D12 requires the PSO's SampleDesc on every bound target (a mismatch is undefined); Vulkan's dynamic
        // rendering requires the counts to agree, so the list fails here instead of in the driver.
        if (a.binding.samples != pipeline->samples) return ReplayResult::invalid_subresource;
        if (!view_range(a.binding, target, VK_IMAGE_ASPECT_COLOR_BIT, a.range)) return ReplayResult::invalid_subresource;
        ResourceBinding viewed = a.binding;
        if (pipeline->color_formats[slot] != VK_FORMAT_UNDEFINED) viewed.format = pipeline->color_formats[slot];
        a.view = vk.image_view(viewed, a.range);
        if (!a.view) return ReplayResult::unsupported;
        shrink(a.binding, target, a.range);
    }
    if (draw.depth.resource && pipeline->depth_format != VK_FORMAT_UNDEFINED) {
        const ResourceBinding* binding = resolve(draw.depth.resource);
        if (!binding) return unresolved_at("draw.depth", draw.depth.resource);
        auto& a = p.depth;
        a.binding = *binding;
        a.id = draw.depth.resource;
        if (!a.binding.image || !(a.binding.aspects & depth_stencil_aspects)) return ReplayResult::invalid_clear;
        if (a.binding.samples != pipeline->samples) return ReplayResult::invalid_subresource;
        if (!view_range(a.binding, draw.depth, a.binding.aspects, a.range)) return ReplayResult::invalid_subresource;
        ResourceBinding viewed = a.binding;
        viewed.format = pipeline->depth_format;
        a.view = vk.image_view(viewed, a.range);
        if (!a.view) return ReplayResult::unsupported;
        shrink(a.binding, draw.depth, a.range);
    }
    if (p.area.width == UINT32_MAX) {  // no attachment: the viewport bounds the rendering
        p.area = {std::uint32_t(std::max(0.0f, draw.viewport.x + draw.viewport.width)),
                  std::uint32_t(std::max(0.0f, draw.viewport.y + draw.viewport.height))};
    }
    p.layers = layers == UINT32_MAX ? 1 : std::max(1u, layers);

    // Bounded diagnostic (docs/page_content.md): what the first draws render into, so a frame that stays black can be
    // told apart from a draw that writes the presented resource. Ids, formats, extents and counts only. The one-shot
    // readback can be armed at a later draw (PALMETTO_READBACK_AT_DRAW) so a long boot can measure the frame after the
    // title has been running, not only its first frame.
    {
        static std::atomic<unsigned> logged{0};
        static std::atomic<unsigned long long> draws_seen{0};
        const unsigned long long seen = draws_seen.fetch_add(1, std::memory_order_relaxed);
        const unsigned n = logged.fetch_add(1, std::memory_order_relaxed);
        if (n < 8) {
            std::fprintf(stderr, "[rt] replay: draw attachments:");
            for (std::size_t slot = 0; slot < p.colors.size(); ++slot) {
                const auto& a = p.colors[slot];
                if (!a.id) continue;
                std::fprintf(stderr, " rt%zu id 0x%llx image %d format %d", slot,
                             static_cast<unsigned long long>(a.id), a.binding.image ? 1 : 0,
                             static_cast<int>(a.binding.format));
            }
            if (p.depth.id)
                std::fprintf(stderr, " depth id 0x%llx format %d",
                             static_cast<unsigned long long>(p.depth.id), static_cast<int>(p.depth.binding.format));
            if (!p.colors.size() || (!p.colors[0].id && !p.depth.id)) std::fprintf(stderr, " none");
            std::fprintf(stderr,
                         "; area %ux%u layers %u; viewport %.1fx%.1f at %.1f,%.1f depth %.3f..%.3f; "
                         "scissor %d,%d..%d,%d; topology %u; %s counts %u/%u first %d; ps %s %u image(s), vs %u load(s)\n",
                         p.area.width, p.area.height, p.layers, static_cast<double>(draw.viewport.width),
                         static_cast<double>(draw.viewport.height), static_cast<double>(draw.viewport.x),
                         static_cast<double>(draw.viewport.y), static_cast<double>(draw.viewport.min_depth),
                         static_cast<double>(draw.viewport.max_depth), draw.scissor.left, draw.scissor.top,
                         draw.scissor.right, draw.scissor.bottom, draw.topology,
                         draw.indexed ? "indexed" : "vertices", draw.count, draw.instances, draw.first,
                         pipeline->has_pixel_stage ? "present" : "absent", pipeline->ps_images, pipeline->vs_loads);
        }
        // Once (or at every draw PALMETTO_READBACK_AT_DRAW names - a comma-separated list arms the frame's even and odd
        // draws together): this draw's colour target is read back after its submission (docs/page_content.md), so the
        // frame's own bytes can be compared with what the present blits.
        if (frame_dump::readback_armed_at(seen) && vk.capture_attachment && !p.colors.empty() && p.colors[0].id &&
            p.colors[0].binding.image) {
            const VkFormat captured = pipeline->color_formats[0] != VK_FORMAT_UNDEFINED
                                          ? pipeline->color_formats[0]
                                          : p.colors[0].binding.format;
            vk.capture_attachment(p.colors[0].binding.image, captured, p.area, p.colors[0].id, seen);
            p.armed_readback = true;
            std::fprintf(stderr,
                         "[rt] replay: readback armed at draw %llu: pipeline 0x%llx target resource 0x%llx image 0x%llx "
                         "format %d area %ux%u viewport %.1fx%.1f at %.1f,%.1f\n",
                         seen, static_cast<unsigned long long>(draw.pipeline),
                         static_cast<unsigned long long>(p.colors[0].id),
                         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p.colors[0].binding.image)),
                         static_cast<int>(captured), p.area.width, p.area.height,
                         static_cast<double>(draw.viewport.width), static_cast<double>(draw.viewport.height),
                         static_cast<double>(draw.viewport.x), static_cast<double>(draw.viewport.y));
            if (draw.user_data[8])
                frame_dump::print_matrix_slot(vk, vk.heap_base, draw.user_data[8], "readback target");
        }
        // Diagnostic (docs/frame_capture.md): every armed draw's sampled textures. Prints the descriptor tables' keys
        // the shim decodes from the console heap mirror and arms a readback of each sampled image, so a black target can
        // be told apart from black sampled content. Bounded; off unless PALMETTO_FRAME_DUMP_SAMPLE is set.
        if (frame_dump::readback_armed_at(seen) && frame_dump::sample_enabled() && vk.sampled_table && vk.capture_sample) {
            std::fprintf(stderr, "[rt] replay: frame dump sample: draw %llu\n", seen);
            for (const std::uint64_t table_va : frame_dump::descriptor_tables(draw, pipeline->table_windows)) {
                SampledTable table;
                if (!vk.sampled_table(table_va, 64, table)) continue;
                std::fprintf(stderr, "[rt] replay: frame dump sample: table VA 0x%llx: %u slot(s), %u T# key(s):",
                             static_cast<unsigned long long>(table_va), table.slots, table.keys);
                for (std::uint32_t k = 0; k < table.keys; ++k) {
                    std::fprintf(stderr, " T#[%u]", table.key[k].slot);
                    for (unsigned w = 0; w < 8; ++w)
                        std::fprintf(stderr, "%s%08x", w ? " " : "", table.key[k].words[w]);
                    std::fprintf(stderr, "]");
                }
                for (std::uint32_t k = 0; k < table.buffer_keys; ++k) {
                    std::fprintf(stderr, " data/V#[%u]", table.buffer_key[k].slot);
                    for (unsigned w = 0; w < 4; ++w)
                        std::fprintf(stderr, "%s%08x", w ? " " : "", table.buffer_key[k].words[w]);
                    std::fprintf(stderr, "]");
                }
                std::fprintf(stderr, "\n");
                // Diagnostic (docs/frame_capture.md): the constants a draw's V# names, read straight from the title's
                // memory: a plausible transform or zeros/garbage (a vertex shader with wrong constants transforms to
                // nowhere). The first decodable V# of the table, bounded to one 480-byte window.
                if (vk.read_title_memory) {
                    for (std::uint32_t k = 0; k < table.buffer_keys; ++k) {
                        const gfx10::BufferWords words{table.buffer_key[k].words[0], table.buffer_key[k].words[1],
                                                      table.buffer_key[k].words[2], table.buffer_key[k].words[3]};
                        gfx10::Buffer buffer{};
                        if (!gfx10::decode(words, buffer) || !buffer.address) continue;
                        const std::uint64_t declared = buffer.stride
                                                           ? std::uint64_t(buffer.stride) * buffer.num_records
                                                           : std::uint64_t(buffer.num_records);
                        if (!declared) continue;
                        constexpr std::uint32_t kMaxConstants = 480;
                        const std::uint32_t want =
                            static_cast<std::uint32_t>(std::min<std::uint64_t>(declared, kMaxConstants));
                        std::uint32_t got = 0;
                        std::uint8_t data[kMaxConstants] = {};
                        if (!vk.read_title_memory(buffer.address, want, data, got) || !got) {
                            std::fprintf(stderr,
                                         "[rt] replay: frame dump sample: V# base 0x%llx + 0x%llx is not readable\n",
                                         static_cast<unsigned long long>(buffer.address),
                                         static_cast<unsigned long long>(declared));
                            break;
                        }
                        std::uint64_t sum = 0, nonzero = 0, peak = 0;
                        for (std::uint32_t i = 0; i < got; ++i) {
                            sum += data[i];
                            if (data[i]) ++nonzero;
                            peak = std::max<std::uint64_t>(peak, data[i]);
                        }
                        std::fprintf(stderr,
                                     "[rt] replay: frame dump sample: V# base 0x%llx + 0x%llx, read %u: mean %llu, "
                                     "non-black %llu/%u, peak %llu; dwords",
                                     static_cast<unsigned long long>(buffer.address),
                                     static_cast<unsigned long long>(declared), got, sum / got, nonzero, got, peak);
                        const std::uint32_t dwords = std::min<std::uint32_t>(got / 4, 8);
                        for (std::uint32_t w = 0; w < dwords; ++w) {
                            std::uint32_t value = 0;
                            std::memcpy(&value, data + w * 4, sizeof(value));
                            std::fprintf(stderr, " %08x", value);
                        }
                        std::fprintf(stderr, "\n");
                        break;
                    }
                }
                // The mapping chain (docs/frame_capture.md): T# key -> layer image -> D3D12 resource -> the title VA the
                // resource was placed at (0: an ordinary committed resource no title page write can reach). Then a
                // readback of each resolved sampled image, so "the sampled content is empty" is measured per texture. Each
                // key names its descriptor slot, so an unresolved T# can be traced back to the console heap position a
                // producer (a view wrapper, a slot-70 publication or the title itself) wrote or should have written.
                for (std::uint32_t k = 0; k < table.keys; ++k) {
                    const SampledKey& key = table.key[k];
                    if (!key.resolved) continue;
                    std::fprintf(stderr,
                                 "[rt] replay: frame dump sample: key %u slot %u (table + 0x%x) T# image 0x%llx format %d "
                                 "%ux%u, resource 0x%llx, title VA %s0x%llx\n",
                                 k, key.slot, key.slot * 0x20u,
                                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(key.image_handle)),
                                 static_cast<int>(key.format), key.extent.width, key.extent.height,
                                 static_cast<unsigned long long>(key.resource),
                                 key.title_va ? "" : "none (committed, not placed) ",
                                 static_cast<unsigned long long>(key.title_va));
                    vk.capture_sample(key.image_handle, key.format, key.extent, key.resource);
                }
                // Every T# that does not resolve is named in full: its descriptor slot, its 8 dwords, and the image its
                // own T# fields name (address/format/extent) - the origin the missing layer image should come from.
                for (std::uint32_t k = 0; k < table.keys; ++k) {
                    const SampledKey& key = table.key[k];
                    if (!key.image || key.resolved) continue;
                    const gfx10::ImageWords image_words{key.words[0], key.words[1], key.words[2], key.words[3],
                                                        key.words[4], key.words[5], key.words[6], key.words[7]};
                    gfx10::Image image{};
                    const bool decodable = gfx10::decode(image_words, image);
                    std::fprintf(stderr,
                                 "[rt] replay: frame dump sample: key %u slot %u (table + 0x%x) T# UNRESOLVED: %08x %08x "
                                 "%08x %08x %08x %08x %08x %08x",
                                 k, key.slot, key.slot * 0x20u, key.words[0], key.words[1], key.words[2], key.words[3],
                                 key.words[4], key.words[5], key.words[6], key.words[7]);
                    if (decodable)
                        std::fprintf(stderr, " -> names image at title VA 0x%llx, format %u, %ux%u\n",
                                     static_cast<unsigned long long>(image.address), image.format, image.width,
                                     image.height);
                    else
                        std::fprintf(stderr, " -> not a decodable T# image\n");
                }
            }
        }
        // Diagnostic (docs/frame_capture.md, the colour path): what the armed draw's pixel shader consumes. The image
        // count says whether the shader samples images at all (a constant-output PS reads none); the non-zero push-constant
        // dwords are the only per-draw scalars it can receive (s0..s31). Bounded, reads only.
        if (frame_dump::readback_armed_at(seen) && frame_dump::sample_enabled()) {
            unsigned nonzero_constants = 0;
            std::fprintf(stderr, "[rt] replay: colour path: ps %s, %u image sample(s) -> %s; non-zero push constants:",
                         pipeline->has_pixel_stage ? "present" : "absent", pipeline->ps_images,
                         pipeline->ps_images ? "the PS reads sampled images (no constant-output path)"
                                             : "constant-output path (no image reads)");
            for (unsigned w = 0; w < 32; ++w) {
                if (!draw.user_data[w]) continue;
                ++nonzero_constants;
                if (nonzero_constants <= 8) std::fprintf(stderr, " s%u=%08x", w, draw.user_data[w]);
            }
            std::fprintf(stderr, "%s (%u of 32)\n", nonzero_constants > 8 ? " ..." : "", nonzero_constants);
        }
        // Diagnostic (docs/vertex_path.md, the geometry fix's opt-in): the translated vertex shader reads the vertex-stream
        // descriptor from the console driver block whose heap-relative address is in its s0/s1 user-data pair, and two more
        // V#s from the descriptor table at s8. The draw carries the descriptor table's address in s0/s1 (the PC packing),
        // so on this path the shader would read the table itself as its driver block. Give it a distinct driver block: one
        // scratch range per pipeline, the vertex stream's V# written there, and that address emitted in s0/s1. The s8 table
        // and its descriptors are not touched. Off by default; no memory is written unless the switch is set.
        if (frame_dump::vertex_stream_descriptor_enabled() && vk.write_device_memory && vk.allocate_driver_block &&
            !pipeline->mesh && (pipeline->vertex_slots & 1u) && draw.views && !draw.views->vertex_buffers.empty() &&
            draw.views->vertex_buffers[0].va && draw.user_data[8]) {
            const VertexBufferView& view = draw.views->vertex_buffers[0];
            const std::uint32_t stride = view.stride;
            if (stride && view.size >= stride) {
                std::uint64_t arena_address = 0;
                {
                    VkBuffer resolved = VK_NULL_HANDLE;
                    VkDeviceSize offset = 0;
                    std::uint64_t size = view.size;
                    if (vk.buffer_at && vk.buffer_at(view.va, size, resolved, offset)) arena_address = offset;
                }
                static std::mutex blocks_mutex;
                static std::unordered_map<std::uintptr_t, std::uint64_t> blocks;
                std::uint64_t block = 0;
                {
                    std::lock_guard lock(blocks_mutex);
                    const auto found = blocks.find(draw.pipeline);
                    if (found != blocks.end()) block = found->second;
                }
                if (!block && arena_address) {
                    std::uint64_t fresh = 0;
                    if (vk.allocate_driver_block(64, fresh) && fresh) {
                        std::lock_guard lock(blocks_mutex);
                        blocks.emplace(draw.pipeline, fresh);
                        block = fresh;
                    }
                }
                if (block && arena_address) {
                    const std::uint32_t records = static_cast<std::uint32_t>(view.size / stride);
                    const gfx10::BufferWords words =
                        frame_dump::vertex_stream_descriptor(arena_address, stride, records);
                    if (vk.write_device_memory(vk.heap_base + block, sizeof(words),
                                               reinterpret_cast<const std::uint8_t*>(words.data()))) {
                        p.driver_block = block;
                        // The table's s8+0 and s8+0x40 slots are MATRIX-A and MATRIX-B buffers, not vertex streams
                        // (docs/root_arguments.md section 7): the vertex V# deliberately does not go there. The optional
                        // coverage experiment writes a matrix V# over the title's 480-byte constants buffer at s8+0.
                        std::uint64_t matrix_address = 0, matrix_bytes = 0;
                        if (frame_dump::matrix_descriptor_enabled() && vk.sampled_table) {
                            // The descriptor-table probe reads the console heap mirror by the table's TITLE VA; the
                            // emitted s8 carries the arena-relative form the shader adds heap_base to, so probe with the
                            // raw s0/s1 pair (both name the same table) and write at the arena-relative one.
                            const std::uint64_t table_title =
                                std::uint64_t(draw.user_data[0]) | (std::uint64_t(draw.user_data[1]) << 32);
                            const std::uint64_t table_arena = std::uint64_t(draw.user_data[8]) & ~std::uint64_t(3);
                            SampledTable table_probe;
                            if (table_title && table_arena && vk.sampled_table(table_title, 64, table_probe)) {
                                for (std::uint32_t k = 0; k < table_probe.keys; ++k) {
                                    if (table_probe.key[k].image) continue;
                                    gfx10::Buffer buffer{};
                                    const gfx10::BufferWords probe_words{
                                        table_probe.key[k].words[0], table_probe.key[k].words[1],
                                        table_probe.key[k].words[2], table_probe.key[k].words[3]};
                                    if (!gfx10::decode(probe_words, buffer) || !buffer.address) continue;
                                    matrix_address = buffer.address;
                                    matrix_bytes = buffer.stride ? std::uint64_t(buffer.stride) * buffer.num_records
                                                                 : std::uint64_t(buffer.num_records);
                                    break;
                                }
                            }
                            if (matrix_address && matrix_bytes >= 64 &&
                                vk.write_device_memory(
                                    vk.heap_base + table_arena, sizeof(gfx10::BufferWords),
                                    reinterpret_cast<const std::uint8_t*>(
                                        frame_dump::matrix_descriptor(matrix_address, static_cast<std::uint32_t>(
                                                                                     std::min<std::uint64_t>(
                                                                                         matrix_bytes, UINT32_MAX)))
                                            .data()))) {
                                static std::atomic<unsigned> matrix_logged{0};
                                if (matrix_logged.fetch_add(1, std::memory_order_relaxed) < 8)
                                    std::fprintf(stderr,
                                                 "[rt] replay: vertex matrix descriptor: pipeline 0x%llx table s8=0x%llx "
                                                 "slot 0: wrote matrix V# over the constants buffer address 0x%llx + "
                                                 "%llu B (stride 0)\n",
                                                 static_cast<unsigned long long>(draw.pipeline),
                                                 static_cast<unsigned long long>(table_arena),
                                                 static_cast<unsigned long long>(matrix_address),
                                                 static_cast<unsigned long long>(matrix_bytes));
                            }
                        }
                        static std::atomic<unsigned> logged{0};
                        if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                            std::fprintf(stderr,
                                         "[rt] replay: vertex stream descriptor: pipeline 0x%llx driver block 0x%llx "
                                         "(emitted at s0/s1): wrote V# address 0x%llx stride %u records %u format 22 "
                                         "(dwords %08x %08x %08x %08x)\n",
                                         static_cast<unsigned long long>(draw.pipeline),
                                         static_cast<unsigned long long>(block),
                                         static_cast<unsigned long long>(arena_address), stride, records, words[0],
                                         words[1], words[2], words[3]);
                    }
                }
            }
        }
        // Diagnostic (docs/frame_capture.md, the colour path): the compose pass pixel shader fetches T# descriptors
        // itself out of its published matrix block; bind a layer image for each T# that names a title-placed resource.
        // Opt-in and read-only otherwise (the shim only adds set-0 registrations).
        if (frame_dump::bind_fetched_enabled() && vk.bind_fetched_textures && draw.user_data[8] &&
            frame_dump::readback_armed_at(seen)) {
            const std::uint64_t table_title = std::uint64_t(draw.user_data[0]) | (std::uint64_t(draw.user_data[1]) << 32);
            if (table_title) vk.bind_fetched_textures(table_title);
        }
// Diagnostic (docs/vertex_path.md): the vertex path of the armed draw. Prints the shader-corpus identity the PSO
        // resolved, the input layout bound to the Vulkan pipeline, the raw vertex and index bytes the draw reads (through
        // the title-memory hook) and the constants, so the translated vertex shader's inputs can be evaluated off the
        // boot. Off unless PALMETTO_VERTEX_PATH is set; bounded and reads only.
        if (frame_dump::readback_armed_at(seen) && frame_dump::vertex_path_enabled()) {
            VertexPathInfo info;
            if (pipeline->describe_vertex_path) pipeline->describe_vertex_path(info);
            std::fprintf(stderr,
                         "[rt] replay: vertex path: pipeline 0x%llx vs \"%s\" (%s) ps \"%s\" (%s); layout problems 0x%x\n",
                         static_cast<unsigned long long>(draw.pipeline), info.vs_name, info.vs_path, info.ps_name,
                         info.ps_path, vertex_layout_problems(info));
            for (std::uint32_t b = 0; b < info.binding_count && b < std::size(info.binding); ++b)
                std::fprintf(stderr, "[rt] replay: vertex path: binding %u stride %u step rate %s divisor %u\n",
                             info.binding[b].binding, info.binding[b].stride,
                             info.binding[b].step_rate == VK_VERTEX_INPUT_RATE_INSTANCE ? "instance" : "vertex",
                             info.binding[b].divisor);
            for (std::uint32_t a = 0; a < info.attribute_count && a < std::size(info.attribute); ++a) {
                const VertexPathInfo::Attribute& attribute = info.attribute[a];
                std::fprintf(stderr,
                             "[rt] replay: vertex path: attribute location %u format %d (%u byte(s)) offset %u binding "
                             "%u\n",
                             attribute.location, static_cast<int>(attribute.format),
                             vertex_format_bytes(attribute.format), attribute.offset, attribute.binding);
            }
            // The vertex views the draw binds, their resolved buffers/offsets, and the first 16 vertices' raw bytes.
            if (draw.views) {
                for (std::uint32_t slot = 0; slot < 32; ++slot) {
                    const VertexBufferView& view = draw.views->vertex_buffers[slot];
                    if (!view.va) continue;
                    // Resolved through the same hook the binding loop uses (called again, read-only), so this line is
                    // what the draw actually binds even though the binding loop runs later.
                    VkBuffer resolved = VK_NULL_HANDLE;
                    VkDeviceSize resolved_offset = 0;
                    if (vk.buffer_at) {
                        std::uint64_t resolved_size = view.size;
                        if (!vk.buffer_at(view.va, resolved_size, resolved, resolved_offset)) resolved = VK_NULL_HANDLE;
                    }
                    const bool zero_step = (pipeline->zero_step_slots & (1u << slot)) != 0;
                    std::fprintf(stderr,
                                 "[rt] replay: vertex path: vb%u va 0x%llx size %u stride %u; resolved buffer 0x%llx "
                                 "offset %llu stride-at-draw %u\n",
                                 slot, static_cast<unsigned long long>(view.va), view.size, view.stride,
                                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(resolved)),
                                 static_cast<unsigned long long>(resolved_offset), zero_step ? 0 : view.stride);
                    if (!vk.read_title_memory || !view.stride) continue;
                    const std::uint32_t stride = view.stride;
                    // Whole declared range (bounded to 4 KiB), so the classification below covers every vertex record.
                    const std::uint32_t want = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(view.size ? view.size : std::uint64_t(stride) * 16, 4096));
                    std::uint32_t got = 0;
                    std::uint8_t bytes[4096] = {};
                    if (!vk.read_title_memory(view.va, want, bytes, got) || !got) {
                        std::fprintf(stderr, "[rt] replay: vertex path: vb%u is not readable through the layer\n",
                                     slot);
                        continue;
                    }
                    std::uint64_t nonzero = 0;
                    for (std::uint32_t i = 0; i < got; ++i)
                        if (bytes[i]) ++nonzero;
                    std::fprintf(stderr,
                                 "[rt] replay: vertex path: vb%u read %u byte(s), %llu non-zero; first vertices:\n", slot,
                                 got, static_cast<unsigned long long>(nonzero));
                    const std::uint32_t vertices = std::min<std::uint32_t>(16, got / stride);
                    const std::uint32_t words_per_vertex = std::min<std::uint32_t>(stride / 4, 8);
                    for (std::uint32_t v = 0; v < vertices; ++v) {
                        std::fprintf(stderr, "[rt] replay: vertex path:   v%u", v);
                        for (std::uint32_t w = 0; w < words_per_vertex; ++w) {
                            std::uint32_t value = 0;
                            std::memcpy(&value, bytes + v * stride + w * 4, sizeof(value));
                            std::fprintf(stderr, " %08x", value);
                        }
                        std::fprintf(stderr, "\n");
                    }
                    // Diagnostic (docs/frame_capture.md): the whole vertex range's per-component classification, so a
                    // constant-output pixel shader's colour source can be told from its position/UV attributes without
                    // reading out the title's vertex data: counts and value classes only.
                    {
                        const std::uint32_t records = got / stride;
                        for (std::uint32_t w = 0; w < stride / 4; ++w) {
                            std::uint32_t distinct[8] = {};
                            unsigned distinct_count = 0;
                            std::uint64_t component_nonzero = 0;
                            for (std::uint32_t v = 0; v < records; ++v) {
                                std::uint32_t value = 0;
                                std::memcpy(&value, bytes + v * stride + w * 4, sizeof(value));
                                if (value) ++component_nonzero;
                                bool known = false;
                                for (unsigned d = 0; d < distinct_count; ++d)
                                    if (distinct[d] == value) known = true;
                                if (!known && distinct_count < 8) distinct[distinct_count++] = value;
                            }
                            const char* value_class = "varied";
                            if (!component_nonzero) value_class = "all zero";
                            else if (records && component_nonzero == records) {
                                if (distinct_count == 1 && distinct[0] == 0xffffffffu) value_class = "all ffffffff";
                                else if (distinct_count == 1 && distinct[0] == 0x3f800000u) value_class = "all 3f800000";
                                else if (distinct_count == 1) value_class = "one value";
                                else value_class = "every record non-zero";
                            }
                            std::fprintf(stderr,
                                         "[rt] replay: vertex path: component %u of stride %u: %u records, %llu "
                                         "non-zero, %s\n",
                                         w, stride, records,
                                         static_cast<unsigned long long>(component_nonzero), value_class);
                        }
                    }
                }
            }
            // The index view: its format, size and first 16 indices.
            if (draw.indexed && draw.index.va) {
                const bool wide = draw.index.format != 57;  // 57 is R16_UINT; the rest are R32_UINT
                std::fprintf(stderr,
                             "[rt] replay: vertex path: index va 0x%llx size %u format %u (%s); first indices:",
                             static_cast<unsigned long long>(draw.index.va), draw.index.size, draw.index.format,
                             wide ? "u32" : "u16");
                if (vk.read_title_memory) {
                    std::uint32_t got = 0;
                    std::uint8_t bytes[128] = {};
                    const std::uint32_t want = static_cast<std::uint32_t>(std::min<std::uint32_t>(draw.index.size, 64));
                    if (vk.read_title_memory(draw.index.va, want, bytes, got) && got) {
                        const std::uint32_t indices =
                            std::min<std::uint32_t>(16, got / (wide ? 4u : 2u));
                        for (std::uint32_t i = 0; i < indices; ++i) {
                            std::uint32_t value = 0;
                            if (wide) {
                                std::memcpy(&value, bytes + i * 4, sizeof(value));
                            } else {
                                std::uint16_t half_word = 0;  // `small` is an rpcndr.h macro on Windows
                                std::memcpy(&half_word, bytes + i * 2, sizeof(half_word));
                                value = half_word;
                            }
                            std::fprintf(stderr, " %u", value);
                        }
                    } else {
                        std::fprintf(stderr, " not readable through the layer");
                    }
                }
                std::fprintf(stderr, "\n");
            }
            frame_dump::print_vertex_constants(vk, draw, *pipeline, "vertex path");
            // The push constants the shader receives: the draw's 32 user-data dwords plus the heap/code bases the shader
            // adds to every address it loads through (docs/d3d12_vulkan.md section 3.5). Every 64-bit pair that is
            // readable through the layer is printed with its first dwords, so the addresses a shader fetches from can be
            // told from plain scalars.
            std::fprintf(stderr, "[rt] replay: vertex path: heap_base 0x%llx code_base 0x%llx; user data",
                         static_cast<unsigned long long>(vk.heap_base),
                         static_cast<unsigned long long>(pipeline->code_base));
            for (unsigned w = 0; w < 32; ++w)
                std::fprintf(stderr, "%s%08x", (w % 8) == 0 ? "\n  " : " ", draw.user_data[w]);
            std::fprintf(stderr, "\n");
            const BlitTransform transform = blit_transform(draw.user_data.data());
            std::fprintf(stderr,
                         "[rt] replay: vertex path: clip-position scalars x %.6g y %.6g z %.6g w %.6g -> %s\n",
                         static_cast<double>(transform.x), static_cast<double>(transform.y),
                         static_cast<double>(transform.z), static_cast<double>(transform.w),
                         blit_transform_degenerate(transform)
                             ? "degenerate: every vertex at (0,0,0,0), nothing rasterizes"
                             : "non-degenerate");
            // The measured module does not use these push-constant dwords as the transform: it loads its own constants
            // from the buffers its descriptors name and overwrites the same function slots before the position store, so
            // the fetch slots and the readback are the acceptance for the descriptor fix (docs/vertex_path.md).
            std::fprintf(stderr,
                         "[rt] replay: vertex path: note: the module overwrites these four dwords with constants it "
                         "fetches itself; the fetch slots and the readback are the coverage acceptance\n");
            // Where each of the 32 push-constant dwords comes from: the pipeline's root signature parameter table, the
            // replay's table-window mask, and the dword's own value. The acceptance inputs are the four dwords the FSPX
            // blit vertex shader uses as its clip-position transform (docs/vertex_path.md section 4).
            std::fprintf(stderr, "[rt] replay: vertex path: root parameters: %u%s", info.parameter_count,
                         info.record_root_arguments ? " (the record path supplies the root arguments)" : "");
            for (std::uint32_t p = 0; p < info.parameter_count && p < std::size(info.parameter); ++p) {
                const VertexPathInfo::RootParameter& parameter = info.parameter[p];
                std::fprintf(stderr, "\n  p%u type %u at s%u dwords %u constants %u", p, parameter.type,
                             parameter.user_offset, parameter.dwords, parameter.constant_count);
            }
            std::fprintf(stderr, "\n[rt] replay: vertex path: push-constant provenance");
            for (std::uint32_t i = 0; i < 32; ++i) {
                const PushConstantProvenance provenance =
                    push_constant_provenance(draw.user_data.data(), pipeline->table_windows, info, i);
                std::fprintf(stderr, "%s  s%u %s", (i % 4) == 0 ? "\n " : "", i,
                             push_constant_provenance_name(provenance.kind));
                if (provenance.parameter != UINT32_MAX) std::fprintf(stderr, "(p%u)", provenance.parameter);
            }
            {
                const std::uint32_t acceptance[] = {8, 9, 10, 28};
                std::fprintf(stderr, "\n[rt] replay: vertex path: acceptance inputs");
                for (const std::uint32_t i : acceptance) {
                    const PushConstantProvenance provenance =
                        push_constant_provenance(draw.user_data.data(), pipeline->table_windows, info, i);
                    std::fprintf(stderr, " s%u %s", i, push_constant_provenance_name(provenance.kind));
                }
            }
            std::fprintf(stderr, "\n");
            if (vk.read_title_memory) {
                unsigned shown = 0;
                for (unsigned i = 0; i + 1 < 32 && shown < 8; ++i) {
                    const std::uint64_t value =
                        std::uint64_t(draw.user_data[i]) | (std::uint64_t(draw.user_data[i + 1]) << 32);
                    if (!value) continue;
                    std::uint32_t got = 0;
                    std::uint8_t bytes[16] = {};
                    if (!vk.read_title_memory(value, sizeof(bytes), bytes, got) || !got) continue;
                    std::uint32_t dwords[4] = {};
                    std::memcpy(dwords, bytes, std::min<std::uint32_t>(got, sizeof(dwords)));
                    std::fprintf(stderr,
                                 "[rt] replay: vertex path: user data %u+%u = 0x%llx readable +%u; dwords %08x %08x "
                                 "%08x %08x\n",
                                 i, i + 1, static_cast<unsigned long long>(value), got, dwords[0], dwords[1],
                                 dwords[2], dwords[3]);
                    ++shown;
                }
            }
            // Diagnostic (docs/vertex_path.md): the descriptor fetch slots the translated shader reads itself. The
            // measured module builds each address as heap_base + (a value from its push constants): the console table
            // from the s8/s9 pair and the same table through the s0/s1 pair, plus the +0x40 slot. The dwords printed here
            // are the *emitted* ones (after the replay's title-VA rebase), because that is what the shader receives; the
            // raw recorded pair above can be a title VA.
            if (vk.read_device_memory) {
                const std::uint64_t heap = vk.heap_base;
                ShaderUserData emitted{};
                std::copy(draw.user_data.begin(), draw.user_data.end(), emitted.s);
                emitted.heap_base = vk.heap_base;
                emitted.code_base = pipeline->code_base;
                const unsigned rebased = rebase_user_data(vk, emitted, pipeline->table_windows);
                if (p.driver_block) {  // the geometry fix's driver block is emitted in s0/s1 (see emit)
                    emitted.s[0] = std::uint32_t(p.driver_block);
                    emitted.s[1] = std::uint32_t(p.driver_block >> 32);
                }
                std::fprintf(stderr, "[rt] replay: vertex path: emitted user data (%u title VA(s) rebased)", rebased);
                for (unsigned w = 0; w < 32; ++w)
                    std::fprintf(stderr, "%s%08x", (w % 8) == 0 ? "\n  " : " ", emitted.s[w]);
                std::fprintf(stderr, "\n");
                const std::uint64_t pc_pair = (std::uint64_t(emitted.s[1]) << 32) |
                                              (std::uint64_t(emitted.s[0]) & ~std::uint64_t(3));
                const std::uint64_t console_pair = (std::uint64_t(emitted.s[9]) << 32) |
                                                   (std::uint64_t(emitted.s[8]) & ~std::uint64_t(3));
                std::fprintf(stderr,
                             "[rt] replay: vertex path: fetch bases: s0/s1 pair 0x%llx, s8/s9 pair 0x%llx\n",
                             static_cast<unsigned long long>(pc_pair),
                             static_cast<unsigned long long>(console_pair));
                const bool split = pc_pair != console_pair;
                frame_dump::print_fetch_slots_table(vk, heap, split ? "driver(s0/s1)" : "table(s0)", pc_pair);
                if (split) frame_dump::print_fetch_slots_table(vk, heap, "table(s8)", console_pair);
                if (draw.views && vk.buffer_at) {
                    for (std::uint32_t slot = 0; slot < 32; ++slot) {
                        const VertexBufferView& view = draw.views->vertex_buffers[slot];
                        if (!view.va) continue;
                        VkBuffer buffer = VK_NULL_HANDLE;
                        VkDeviceSize offset = 0;
                        std::uint64_t size = view.size;
                        if (!vk.buffer_at(view.va, size, buffer, offset)) continue;
                        std::fprintf(stderr,
                                     "[rt] replay: vertex path: driver-block-shaped V# for vb%u: arena-relative "
                                     "address 0x%llx stride %u records %u (view %u B, %u record(s) at that stride)\n",
                                     slot, static_cast<unsigned long long>(offset), view.stride,
                                     view.stride ? static_cast<std::uint32_t>(view.size / view.stride) : 0u, view.size,
                                     view.stride ? static_cast<std::uint32_t>(view.size / view.stride) : 0u);
                    }
                }
            }
        }
    }

    // Slots the pipeline has a format for but the draw leaves unbound take a variant whose format there is
    // VK_FORMAT_UNDEFINED: D3D12 discards those writes, Vulkan wants the formats to match the attachments.
    std::uint32_t unbound = 0;
    for (std::size_t slot = 0; slot < p.colors.size(); ++slot)
        if (pipeline->color_formats[slot] != VK_FORMAT_UNDEFINED && !p.colors[slot].id) unbound |= 1u << slot;
    if (pipeline->depth_format != VK_FORMAT_UNDEFINED && !p.depth.id) unbound |= GraphicsPipeline::unbound_depth;
    if (unbound) {
        p.pipeline = pipeline->variant ? pipeline->variant(unbound) : VK_NULL_HANDLE;
        if (!p.pipeline) return ReplayResult::unsupported;
    }

    // An unbound vertex-buffer slot reads zeros, as in D3D12: the zero page with stride 0. Per-instance slots with
    // step rate 0 also take stride 0, so every instance reads element 0.
    for (std::uint32_t slot = 0; slot < 32; ++slot) {
        if (!(pipeline->vertex_slots & (1u << slot))) continue;
        p.vertex_buffers.resize(slot + 1, VK_NULL_HANDLE);
        p.vertex_offsets.resize(slot + 1, 0);
        p.vertex_sizes.resize(slot + 1, VK_WHOLE_SIZE);
        p.vertex_strides.resize(slot + 1, 0);
        const bool bound = draw.views && slot < draw.views->vertex_buffers.size() &&
                           draw.views->vertex_buffers[slot].va;
        if (!bound) {
            if (!vk.zero_buffer) return unresolved_at("draw.vertex_zero_page", slot);
            p.vertex_buffers[slot] = vk.zero_buffer;
            p.vertex_offsets[slot] = vk.zero_offset;
            p.vertex_sizes[slot] = vk.zero_size;
            p.vertex_strides[slot] = 0;
            continue;
        }
        const VertexBufferView& view = draw.views->vertex_buffers[slot];
        std::uint64_t size = view.size;
        if (!vk.buffer_at(view.va, size, p.vertex_buffers[slot], p.vertex_offsets[slot]))
            return unresolved_at("draw.vertex_buffer", view.va, view.size);
        p.vertex_sizes[slot] = size;
        p.vertex_strides[slot] = (pipeline->zero_step_slots & (1u << slot)) ? 0 : view.stride;
        // Content (docs/page_content.md): the bytes behind this view, pulled from the title's pages into the arena
        // range that stands in for them; a view into ordinary arena memory is not a title VA and the pull is a no-op.
        if (vk.pull_title_pages) vk.pull_title_pages(view.va, view.size);
    }
    if (draw.indexed) {
        if (draw.index.format == 57) p.index_type = VK_INDEX_TYPE_UINT16;
        else if (draw.index.format == 42) p.index_type = VK_INDEX_TYPE_UINT32;
        else if (draw.index.va) return ReplayResult::unsupported;
        if (!draw.index.va) {  // no index buffer: every index reads 0
            if (!vk.zero_buffer) return unresolved_at("draw.index_zero_page", 0);
            p.index_buffer = vk.zero_buffer;
            p.index_offset = vk.zero_offset;
        } else {
            std::uint64_t size = draw.index.size;
            if (!vk.buffer_at(draw.index.va, size, p.index_buffer, p.index_offset))
                return unresolved_at("draw.index_buffer", draw.index.va, draw.index.size);
            if (vk.pull_title_pages) vk.pull_title_pages(draw.index.va, draw.index.size);
            draw_va_pulls(vk, draw, *pipeline);
        }
    } else {
        draw_va_pulls(vk, draw, *pipeline);
    }
    return ReplayResult::success;
}

inline bool strip_topology(VkPrimitiveTopology topology) {
    return topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
           topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY ||
           topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
}

// D3D_PRIMITIVE_TOPOLOGY -> VkPrimitiveTopology (patch lists are not modelled).
inline VkPrimitiveTopology vulkan_topology(std::uint32_t topology) {
    switch (topology) {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 10: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 11: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 12: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// Moves an attachment's tracked pieces to `layout` before rendering (first use zero-fills, as everywhere).
inline void to_attachment(const ReplayFunctions& vk, VkCommandBuffer command_buffer, const PreparedDraw::Attachment& a,
                          VkImageLayout layout, VkAccessFlags access, VkPipelineStageFlags stages,
                          LayoutTracker* tracker, bool separate) {
    const VkImageSubresourceRange range = planes_for_layout(a.binding, a.range, separate);
    auto pieces = current(tracker, a.id, range, layout);
    initialize(vk, command_buffer, a.binding, a.id, pieces, layout, tracker);
    for (const auto& piece : pieces) {
        if (piece.layout == layout) {
            // Attachment writes of an earlier rendering scope are not ordered with this one without a barrier.
            memory_barrier(vk, command_buffer, access, stages, access, stages);
            continue;
        }
        image_barrier(vk, command_buffer, a.binding, piece.range, piece.layout, layout,
                      VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, access, stages);
    }
    if (tracker) tracker->set(a.id, range, layout);
}

} // namespace detail

// Destination and source of a copy or resolve command.
inline std::pair<ResourceId, ResourceId> copy_ids(const Command& command) {
    if (const auto* c = std::get_if<CopyBuffer>(&command)) return {c->dst, c->src};
    if (const auto* c = std::get_if<CopyTexture>(&command)) return {c->dst.resource, c->src.resource};
    if (const auto* c = std::get_if<CopyResource>(&command)) return {c->dst, c->src};
    if (const auto* c = std::get_if<ResolveSubresource>(&command)) return {c->dst, c->src};
    return {0, 0};
}

// The heap, type and query range of a query command; count 0 for any other command.
struct QueryRange {
    QueryHeapId heap = 0;
    std::uint32_t type = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

inline QueryRange query_range(const Command& command) {
    if (const auto* q = std::get_if<BeginQuery>(&command)) return {q->heap, q->type, q->index, 1};
    if (const auto* q = std::get_if<EndQuery>(&command)) return {q->heap, q->type, q->index, 1};
    if (const auto* q = std::get_if<ResolveQueries>(&command)) return {q->heap, q->type, q->first, q->count};
    return {};
}

// A list checked against the current resources, ready to emit.
class PreparedList {
public:
    const CommandList* list = nullptr;
    std::vector<detail::Prepared> commands;
    // The draws' own data, by Prepared::draw. Kept from one execution to the next like the PreparedList itself: the
    // first draw_count entries are this list's, and the rest keep their vectors' storage for later draws.
    std::vector<detail::PreparedDraw> draws;
    std::size_t draw_count = 0;
    std::vector<detail::PreparedQuery> queries;
    VkQueryPool segment_pool = VK_NULL_HANDLE;
    std::uint32_t segment_base = 0;
    std::uint32_t segment_count = 0;
    bool precise_segments = false;
    // Physical pipeline-statistics segments (plan_statistics_segments), in a pool of their own.
    VkQueryPool statistics_pool = VK_NULL_HANDLE;
    std::uint32_t statistics_base = 0;
    std::uint32_t statistics_segment_count = 0;
    // The list's statistics queries overlap (D3D-252): without a statistics pool they cannot run whole, because
    // two Vulkan queries of one type may not be active at once; QueryReplay::plan refuses such a list.
    bool statistics_overlaps = false;
    // For each of those segments, whether it holds mesh draws and so runs in statistics_mesh_pool (D3D-291).
    std::vector<std::uint8_t> statistics_mesh;
    VkQueryPool statistics_mesh_pool = VK_NULL_HANDLE;
    // Copies, resolves and UAV clears that run as transfers, recorded while a predicate was set. D3D12 skips them
    // when the predicate says so, but Vulkan's conditional rendering does not cover transfer commands, so they run
    // unconditionally (docs/d3d12_vulkan.md section 4.7). Counted so the caller can report it.
    std::size_t unpredicated_copies = 0;
    // Set when any command writes to a host_readable (READBACK) buffer so VulkanQueue only flushes to HOST_BIT when needed.
    bool wrote_readback = false;
    VkPipelineStageFlags2 readback_src_stages = 0;
    std::vector<detail::PreparedKhrPostbuild> khr_postbuilds;
};

inline ReplayResult plan_occlusion_segments(PreparedList& out) {
    // Active logical queries cover a contiguous range of physical slots. A Begin or End closes the old
    // slot and opens a new one if any query remains active. Each shared interval is recorded only once.
    std::map<std::pair<QueryHeapId, std::uint32_t>, std::uint32_t> active;
    std::uint32_t current = detail::PreparedQuery::no_segment;
    for (auto& query : out.queries) {
        const Command& command = out.list->commands()[query.command];
        if (std::holds_alternative<ResolveQueries>(command)) continue;
        const auto range = query_range(command);
        // Binary occlusion is virtualized alongside the ordinary kind (D3D-252): both are VK_QUERY_TYPE_OCCLUSION,
        // so one physical query is open at a time and a binary resolve normalizes the summed counts (0 or 1).
        if (range.type != d3d12_query::occlusion && range.type != d3d12_query::binary_occlusion) continue;
        const auto key = std::pair{range.heap, range.first};
        query.end_segment = current;
        if (std::holds_alternative<BeginQuery>(command)) {
            // A flattened bundle can reuse a slot already active in the parent list.
            if (!active.emplace(key, out.segment_count).second) return ReplayResult::invalid_query;
            out.precise_segments |= query.heap->precise;
        } else {
            const auto start = active.find(key);
            if (start == active.end()) return ReplayResult::invalid_query;
            query.first_result = start->second;
            query.result_count = out.segment_count - start->second;
            active.erase(start);
        }
        current = active.empty() ? detail::PreparedQuery::no_segment : out.segment_count++;
        query.begin_segment = current;
    }
    return active.empty() ? ReplayResult::success : ReplayResult::invalid_query;
}

// Helper dispatches must not contribute to the application's CSInvocations (D3D-162). Query resolves and
// ordinary helper commands share one sequence of physical statistics segments. Resolve dispatch requirements
// depend on heap state at planning time, so every non-timestamp resolve splits conservatively.
// A mesh draw cannot run while a query with the input-assembly, vertex, geometry or tessellation counters is active
// (VUID-vkCmdDrawMeshTasksEXT-pipelineStatistics-07076), though D3D12 allows it and reads those fields as 0. A query
// that holds a mesh draw is split as well (D3D-291): a segment's first draw decides whether it runs in the full pool
// or in the mesh one (statistics_mesh), and a later draw of the other kind ends it and begins one of its own kind.
// Dispatches and segments without draws use the full pool; ExecuteIndirect draws count as vertex draws. Each draw
// has its own rendering scope, so a split before one stays outside rendering, as a helper's does.
inline void plan_statistics_segments(PreparedList& out) {
    detail::PreparedQuery* begin = nullptr;
    // A split point (a helper or a resolve), or a draw: kind 0 vertex, 1 mesh.
    struct Event { std::uint32_t* end; std::uint32_t* begin; int kind; };
    std::vector<Event> inner;
    bool splits = false, mesh = false;
    std::size_t next_query = 0;
    const auto commands = out.list->commands();
    for (std::size_t i = 0; i < out.commands.size(); ++i) {
        auto& p = out.commands[i];
        if (begin && (p.predicate.scratch || !p.predicated_copy.empty() || p.has_depth24 ||
                      p.counted_dispatch.indirect)) {
            inner.push_back({&p.statistics_end, &p.statistics_begin, -1});
            splits = true;
        }
        if (begin) {
            int kind = -1;
            if (const auto* draw = std::get_if<Draw>(&commands[i])) kind = draw->mesh ? 1 : 0;
            else if (const auto* indirect = std::get_if<ExecuteIndirect>(&commands[i]))
                kind = indirect->kind == IndirectKind::dispatch ? -1 : 0;
            if (kind >= 0) inner.push_back({&p.statistics_end, &p.statistics_begin, kind});
            mesh |= kind == 1;
        }
        if (next_query == out.queries.size() || out.queries[next_query].command != i) continue;
        auto& query = out.queries[next_query++];
        const Command& command = commands[i];
        const auto range = query_range(command);
        if (std::holds_alternative<ResolveQueries>(command)) {
            // A stream-output resolve only fills zeros (no dispatch), so it needs no split.
            if (begin && range.type != d3d12_query::timestamp && !is_so_statistics(range.type)) {
                inner.push_back({&query.statistics_end, &query.statistics_begin, -1});
                splits = true;
            }
        } else if (range.type == d3d12_query::pipeline_statistics) {
            if (std::holds_alternative<BeginQuery>(command)) {
                begin = &query;
                inner.clear();
                splits = mesh = false;
            } else if (begin) {
                if (splits || mesh) {
                    const std::uint32_t first = out.statistics_segment_count;
                    const auto open = [&] {
                        out.statistics_mesh.push_back(0);
                        return out.statistics_segment_count++;
                    };
                    std::uint32_t current = begin->statistics_begin = open();
                    int kind = -1;  // of the current segment's draws, -1 before its first
                    for (const auto& event : inner) {
                        if (event.kind < 0) {
                            *event.end = current;
                            current = *event.begin = open();
                            kind = -1;
                            continue;
                        }
                        if (kind >= 0 && event.kind != kind) {
                            *event.end = current;
                            current = *event.begin = open();
                        }
                        kind = event.kind;
                        out.statistics_mesh[current] = std::uint8_t(kind);
                    }
                    query.statistics_end = current;
                    query.statistics_first = first;
                    query.statistics_count = out.statistics_segment_count - first;
                }
                begin = nullptr;
                inner.clear();
            }
        }
    }
}

// Overlapping pipeline-statistics queries (D3D-252): the same physical-segment layout the occlusion planner
// builds, in the statistics pool. Each Begin and End of a statistics query — and every non-timestamp resolve
// while one runs, because its helper dispatch would be counted — closes the one open physical query and opens
// a new one, so each logical query keeps a contiguous slot range and its result is the sum of that range
// (statistics_sum.comp). A Begin or End of another query kind leaves the segments alone.
inline ReplayResult plan_overlapping_statistics_segments(PreparedList& out) {
    std::map<std::pair<QueryHeapId, std::uint32_t>, std::uint32_t> active;
    std::uint32_t current = detail::PreparedQuery::no_segment;
    std::size_t next_query = 0;
    // Each physical segment's draw kind (D3D-291): -1 before its first draw, 0 vertex, 1 mesh.
    int kind = -1;
    const auto open = [&] {
        out.statistics_mesh.push_back(0);
        kind = -1;
        return out.statistics_segment_count++;
    };
    const auto commands = out.list->commands();
    for (std::size_t i = 0; i < out.commands.size(); ++i) {
        auto& p = out.commands[i];
        detail::PreparedQuery* query = nullptr;
        if (next_query != out.queries.size() && out.queries[next_query].command == i)
            query = &out.queries[next_query++];
        if (!query) {
            // A helper dispatch while a query runs would be counted into it (D3D-162): close the shared physical
            // segment before it and open a new one after, the way resolves do.
            if (!active.empty() && (p.predicate.scratch || !p.predicated_copy.empty() || p.has_depth24 ||
                                    p.counted_dispatch.indirect)) {
                p.statistics_end = current;
                current = open();
                p.statistics_begin = current;
                continue;
            }
            // A draw of the other kind than the open segment's starts a segment in its own pool (D3D-291).
            int draw = -1;
            if (const auto* d = std::get_if<Draw>(&commands[i])) draw = d->mesh ? 1 : 0;
            else if (const auto* indirect = std::get_if<ExecuteIndirect>(&commands[i]))
                draw = indirect->kind == IndirectKind::dispatch ? -1 : 0;
            if (!active.empty() && draw >= 0) {
                if (kind >= 0 && draw != kind) {
                    p.statistics_end = current;
                    current = open();
                    p.statistics_begin = current;
                }
                kind = draw;
                out.statistics_mesh[current] = std::uint8_t(draw);
            }
            continue;
        }
        const Command& command = out.list->commands()[i];
        const auto range = query_range(command);
        if (!std::holds_alternative<ResolveQueries>(command)) {
            // A Begin or End of another query kind leaves the shared segment alone.
            if (range.type != d3d12_query::pipeline_statistics) continue;
            query->statistics_end = current;
            const auto key = std::pair{range.heap, range.first};
            if (std::holds_alternative<BeginQuery>(command)) {
                // A flattened bundle can reuse a slot already active in the parent list.
                if (!active.emplace(key, out.statistics_segment_count).second) return ReplayResult::invalid_query;
            } else {
                const auto start = active.find(key);
                if (start == active.end()) return ReplayResult::invalid_query;
                query->statistics_first = start->second;
                query->statistics_count = out.statistics_segment_count - start->second;
                active.erase(start);
            }
        } else {
            if (current == detail::PreparedQuery::no_segment || range.type == d3d12_query::timestamp) continue;
            query->statistics_end = current;
            current = open();
            query->statistics_begin = current;
            continue;
        }
        current = active.empty() ? detail::PreparedQuery::no_segment : open();
        query->statistics_begin = current;
    }
    return active.empty() ? ReplayResult::success : ReplayResult::invalid_query;
}

// detail::initialize zero-clears an image piece the tracker holds in UNDEFINED (D3D12 committed resources start
// zeroed) the first time a command touches it, with vkCmdClearColorImage or vkCmdClearDepthStencilImage. True when
// a command of the list would need one of those and it is unset; replay() then refuses the list as unsupported, as
// prepare() does for every other optional function, rather than emit calling a null pointer.
inline bool zero_clear_unavailable(const PreparedList& prepared, const ReplayFunctions& vk,
                                   const LayoutTracker& tracker) {
    if (vk.clear_color_image && vk.clear_depth_stencil_image) return false;
    std::vector<LayoutPiece> pieces;
    const auto missing = [&](ResourceId id, const ResourceBinding& resource, const VkImageSubresourceRange& range) {
        if (!id || !resource.image || resource.not_zeroed ||
            block_format(resource.format, VK_IMAGE_ASPECT_COLOR_BIT).block > 1)   // only moved, never cleared
            return false;
        pieces.clear();
        if (!tracker.pieces(id, range, pieces)) return false;
        for (const auto& piece : pieces)
            if (piece.layout == VK_IMAGE_LAYOUT_UNDEFINED &&
                ((piece.range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) ? !vk.clear_color_image
                                                                      : !vk.clear_depth_stencil_image))
                return true;
        return false;
    };
    const auto commands = prepared.list->commands();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto& command = commands[i];
        const auto& p = prepared.commands[i];
        if (p.skip) continue;
        if (const auto [dst, src] = copy_ids(command); dst || src) {
            if (missing(dst, p.binding, p.range) || missing(src, p.source, p.source_range)) return true;
            continue;
        }
        const ResourceId id = std::visit(
            [](const auto& c) -> ResourceId {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, ClearRenderTarget> || std::is_same_v<T, ClearDepthStencil>)
                    return c.target.resource;
                else if constexpr (std::is_same_v<T, ClearUnorderedAccess>)
                    return c.view.resource;
                else if constexpr (requires { c.resource; })
                    return c.resource;
                else
                    return 0;
            },
            command);
        if (missing(id, p.binding, p.range)) return true;
    }
    return false;
}

// Resolves and checks every command. Nothing is written to a command buffer.
// Resolve maps a ResourceId to a const ResourceBinding* (nullptr: unknown).
// statistics_helpers promises a statistics segment pool at QueryReplay::plan(). Keep the default for replay()
// and callers without a summation helper: reject before allocating any helper scratch.
template<typename Resolve>
ReplayResult prepare(const CommandList& list, Resolve&& resolve, const ReplayFunctions& vk, PreparedList& out,
                     bool statistics_helpers = false) {
    if (!list.closed()) return ReplayResult::list_open;
    if (list.queries_open()) return ReplayResult::invalid_query;  // a Vulkan query must end in its command buffer
    const auto commands = list.commands();
    out.list = &list;
    out.commands.assign(commands.size(), {});
    out.draw_count = 0;
    out.queries.clear();
    out.segment_pool = VK_NULL_HANDLE;
    out.segment_base = out.segment_count = 0;
    out.precise_segments = false;
    out.statistics_pool = out.statistics_mesh_pool = VK_NULL_HANDLE;
    out.statistics_base = out.statistics_segment_count = 0;
    out.statistics_mesh.clear();
    out.khr_postbuilds.clear();
    const bool rendering = vk.begin_rendering && vk.end_rendering && vk.clear_attachments && vk.image_view;
    const bool copies = vk.copy_buffer && vk.copy_buffer_to_image && vk.copy_image_to_buffer && vk.copy_image;
    const bool conditional = vk.begin_conditional_rendering && vk.end_conditional_rendering;
    // While a predicate is set, a whole-view clear is replayed as one rectangle over the view: vkCmdClearColorImage
    // and vkCmdClearDepthStencilImage ignore conditional rendering, vkCmdClearAttachments does not.
    bool predicated = false;
    constexpr Rect whole_view{0, 0, INT32_MAX, INT32_MAX};
    const auto clear_rects = [&](std::span<const Rect> rects) {
        return predicated && rects.empty() ? std::span<const Rect>(&whole_view, 1) : rects;
    };
    out.unpredicated_copies = 0;
    const bool queries = vk.reset_query_pool && vk.begin_query && vk.end_query && vk.write_timestamp &&
                         vk.copy_query_pool_results && vk.fill_buffer && vk.query_pool;
    // Vulkan allows one active query per type. Overlapping ordinary and binary occlusion share one physical
    // segment pool, and overlapping statistics split into the statistics pool (D3D-252); a physical query of a
    // type is still open at most once at any point, because each event closes the one open slot and opens a new
    // one. The occlusion kinds share their D3D12 heap, statistics have their own, so their states are separate.
    std::size_t ordinary_active = 0, binary_active = 0, statistics_active_count = 0;
    bool overlaps = false, statistics_overlaps = false, statistics_active = false;
    out.wrote_readback = false;
    out.readback_src_stages = 0;

    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto& command = commands[i];
        auto& p = out.commands[i];
        const auto bind = [&](ResourceId id) {
            const ResourceBinding* binding = resolve(id);
            if (binding) p.binding = *binding;
            return binding != nullptr;
        };
        const auto rect_view = [&](std::span<const Rect> rects, const TargetView& target, VkImageAspectFlags view_aspects) {
            if (rects.empty()) return ReplayResult::success;
            if (!rendering) return ReplayResult::unsupported;
            detail::clip(rects, detail::mip_extent(p.binding, target.mip), target.layer_count, p.rects);
            if (p.rects.empty()) {
                p.skip = true;
                return ReplayResult::success;
            }
            VkImageSubresourceRange range = p.range;
            range.aspectMask = view_aspects;
            if (p.binding.volume) {
                // A 2D-array attachment view of a 3D image selects W slices here.
                range.baseArrayLayer = target.first_layer;
                range.layerCount = target.layer_count;
            }
            ResourceBinding view_binding = p.binding;
            if (p.clear_format != VK_FORMAT_UNDEFINED) view_binding.format = p.clear_format;
            p.view = vk.image_view(view_binding, range);
            return p.view ? ReplayResult::success : ReplayResult::unsupported;
        };

        if (const auto* transition = std::get_if<TransitionBarrier>(&command)) {
            if (!bind(transition->resource)) return ReplayResult::unresolved_resource;
            if (p.binding.image == VK_NULL_HANDLE) {
                if (transition->subresource != 0 && transition->subresource != all_subresources)
                    return ReplayResult::invalid_subresource;
            } else if (!image_range(p.binding, transition->subresource, p.range)) {
                return ReplayResult::invalid_subresource;
            }
        } else if (std::holds_alternative<GlobalBarrier>(command) || std::holds_alternative<BufferBarrier>(command)) {
            if (!vk.pipeline_barrier2) return ReplayResult::unsupported;
            if (const auto* buffer_barrier = std::get_if<BufferBarrier>(&command))
                if (!bind(buffer_barrier->resource)) return ReplayResult::unresolved_resource;
        } else if (const auto* texture = std::get_if<TextureBarrier>(&command)) {
            if (!vk.pipeline_barrier2) return ReplayResult::unsupported;
            if (!bind(texture->resource)) return ReplayResult::unresolved_resource;
            if (!p.binding.image || !barrier_range(p.binding, texture->range, p.range))
                return ReplayResult::invalid_subresource;
        } else if (const auto* color_clear = std::get_if<ClearRenderTarget>(&command)) {
            if (!bind(color_clear->target.resource)) return ReplayResult::unresolved_resource;
            if (!p.binding.image || p.binding.aspects != VK_IMAGE_ASPECT_COLOR_BIT) return ReplayResult::invalid_clear;
            if (!detail::view_range(p.binding, color_clear->target, VK_IMAGE_ASPECT_COLOR_BIT, p.range))
                return ReplayResult::invalid_subresource;
            p.clear_format = detail::color_view_format(p.binding, color_clear->target);
            // Older ownerless replay callers omit the resource format for float-only clears. Preserve
            // that path; an explicit format or a typeless resource must still resolve a concrete RTV.
            if (p.clear_format == VK_FORMAT_UNDEFINED &&
                (p.binding.typeless || color_clear->target.format != ColorViewFormat::resource))
                return ReplayResult::invalid_clear;
            const bool partial_volume = p.binding.volume &&
                color_clear->target.layer_count != std::max(1u, p.binding.depth >> color_clear->target.mip);
            if ((p.clear_format != p.binding.format || partial_volume) && color_clear->rects.empty()) {
                // vkCmdClearColorImage can neither reinterpret the format nor select W slices of a mip.
                const auto extent = detail::mip_extent(p.binding, color_clear->target.mip);
                const Rect full{0, 0, std::int32_t(extent.width), std::int32_t(extent.height)};
                if (const auto r = rect_view(std::span<const Rect>(&full, 1), color_clear->target,
                                            VK_IMAGE_ASPECT_COLOR_BIT); r != ReplayResult::success) return r;
            } else if (const auto r = rect_view(clear_rects(color_clear->rects), color_clear->target, VK_IMAGE_ASPECT_COLOR_BIT);
                       r != ReplayResult::success) return r;
            if (!p.skip && p.rects.empty() && !vk.clear_color_image) return ReplayResult::unsupported;
        } else if (const auto* depth_clear = std::get_if<ClearDepthStencil>(&command)) {
            if (!bind(depth_clear->target.resource)) return ReplayResult::unresolved_resource;
            VkImageAspectFlags aspects = 0;
            if (depth_clear->flags & clear_depth) aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (depth_clear->flags & clear_stencil) aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
            const VkImageAspectFlags available = p.binding.aspects & depth_stencil_aspects;
            if (!p.binding.image || !available || (p.binding.aspects & ~depth_stencil_aspects))
                return ReplayResult::invalid_clear;
            // A flag for a plane the view's format doesn't have is ignored, as the PC D3D12 runtime (WARP) and native
            // drivers do: STENCIL on a D32_FLOAT view clears nothing, DEPTH | STENCIL clears the depth plane. Until
            // 2026-11-23 it returned invalid_clear here and the whole list was dropped (D3D-437).
            aspects &= available;
            if (!detail::view_range(p.binding, depth_clear->target, aspects, p.range))
                return ReplayResult::invalid_subresource;
            if (!aspects) p.skip = true;
            if (const auto r = rect_view(clear_rects(depth_clear->rects), depth_clear->target, p.binding.aspects);
                r != ReplayResult::success)
                return r;
            if (!p.skip && p.rects.empty() && !vk.clear_depth_stencil_image) return ReplayResult::unsupported;
        } else if (const auto* uav = std::get_if<ClearUnorderedAccess>(&command)) {
            const UavView& view = uav->view;
            if (!bind(view.resource)) return ReplayResult::unresolved_resource;
            if (view.buffer) {
                if (!vk.fill_buffer || !vk.update_buffer || !vk.copy_buffer) return ReplayResult::unsupported;
                if (predicated) ++out.unpredicated_copies;  // fills and copies are transfer commands
                if (const auto r = detail::prepare_buffer_uav_clear(*uav, p); r != ReplayResult::success) return r;
                const bool short_fill = std::any_of(p.fills.begin(), p.fills.end(), [&](const auto& fill) {
                    return detail::buffer_fill_needs_scratch(p.element, fill[0], fill[1]);
                });
                if (short_fill && (!vk.copy_scratch ||
                    !vk.copy_scratch(8, p.fill_scratch, p.fill_scratch_offset) || !p.fill_scratch ||
                    p.fill_scratch_offset % 4 || p.fill_scratch_offset > UINT64_MAX - 8))
                    return ReplayResult::unsupported;
                if (!p.skip && !p.fills.empty() && p.binding.host_readable) {
                    out.wrote_readback = true;
                    out.readback_src_stages |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                }
                continue;
            }
            if (!p.binding.image || p.binding.aspects != VK_IMAGE_ASPECT_COLOR_BIT) return ReplayResult::invalid_clear;
            const TargetView target{view.resource, view.mip, view.first_layer, view.layer_count};
            if (!detail::view_range(p.binding, target, VK_IMAGE_ASPECT_COLOR_BIT, p.range))
                return ReplayResult::invalid_subresource;
            // The view's format turns the values into bytes. When the view format differs from the image's backing
            // format (a typeless or castable texture) and can be cleared as a color attachment, clear through a view of
            // view_format instead of decoding into the backing format (which turns non-float bit patterns like
            // R16G16_UNORM 1.0 = 0xffff or R32_UINT 0x7f800001 into SFLOAT NaNs that vkCmdClearColorImage quiets).
            // Emulated R9G9B9E5_SHAREDEXP images (backed by R32_UINT) lack COLOR_ATTACHMENT on E5B9G9R9 and keep the
            // bit-exact R32_UINT backing path.
            const VkFormat view_format = view.format ? VkFormat(view.format) : p.binding.format;
            if (!uav_clear::encode(view_format, uav->is_float, uav->values, p.element))
                return ReplayResult::unsupported;
            const auto* image_layout = uav_clear::layout(p.binding.format);
            if (!image_layout || image_layout->bytes != p.element.size)
                return ReplayResult::unsupported;
            const bool attachment = p.binding.color_attachment && !p.binding.volume;
            const bool view_attachable = vk.color_attachment_format
                                             ? vk.color_attachment_format(view_format)
                                             : view_format != VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
            const bool reinterpret = view_format != p.binding.format && attachment && view_attachable;
            const VkFormat clear_format = reinterpret ? view_format : p.binding.format;
            if (!uav_clear::decode(clear_format, p.element, p.clear_color))
                return ReplayResult::unsupported;
            if (reinterpret) p.clear_format = view_format;
            if (!uav->rects.empty() && !attachment)
                return ReplayResult::unsupported;
            // Under a predicate a whole view is cleared as one rectangle where the image can be an attachment, as
            // render targets are; otherwise vkCmdClearColorImage runs unconditionally and is counted.
            if (predicated && uav->rects.empty() && !attachment) ++out.unpredicated_copies;
            const auto rects = (reinterpret && uav->rects.empty())
                                   ? std::span<const Rect>(&whole_view, 1)
                                   : attachment ? clear_rects(uav->rects) : std::span<const Rect>(uav->rects);
            if (const auto r = rect_view(rects, target, VK_IMAGE_ASPECT_COLOR_BIT); r != ReplayResult::success)
                return r;
            if (!p.skip && p.rects.empty() && !vk.clear_color_image) return ReplayResult::unsupported;
        } else if (std::holds_alternative<CopyBuffer>(command) || std::holds_alternative<CopyTexture>(command) ||
                   std::holds_alternative<CopyResource>(command)) {
            if (!copies) return ReplayResult::unsupported;
            if (predicated && !std::holds_alternative<CopyBuffer>(command)) ++out.unpredicated_copies;
            const auto [dst_id, src_id] = copy_ids(command);
            const ResourceBinding* source = resolve(src_id);
            if (!bind(dst_id) || !source) return ReplayResult::unresolved_resource;
            p.source = *source;
            ReplayResult result = ReplayResult::success;
            if (const auto* buffer_copy = std::get_if<CopyBuffer>(&command)) {
                const auto size = [&](const ResourceBinding& binding, ResourceId id) {
                    if (buffer_copy->tiled_resource != id) return binding.size;
                    if (!binding.sparse_buffer) return VkDeviceSize(0);
                    return (binding.size + tile_size_in_bytes - 1) / tile_size_in_bytes * tile_size_in_bytes;
                };
                if (!p.binding.buffer || !p.source.buffer ||
                    !detail::within(buffer_copy->dst_offset, buffer_copy->size, size(p.binding, dst_id)) ||
                    !detail::within(buffer_copy->src_offset, buffer_copy->size, size(p.source, src_id)))
                    return ReplayResult::invalid_copy;
                if (buffer_copy->src_offset > UINT64_MAX - p.source.buffer_offset ||
                    buffer_copy->dst_offset > UINT64_MAX - p.binding.buffer_offset) return ReplayResult::invalid_copy;
                const VkDeviceSize src = p.source.buffer_offset + buffer_copy->src_offset;
                const VkDeviceSize dst = p.binding.buffer_offset + buffer_copy->dst_offset;
                if (buffer_copy->size > UINT64_MAX - src || buffer_copy->size > UINT64_MAX - dst)
                    return ReplayResult::invalid_copy;
                if (p.source.buffer == p.binding.buffer && src < dst + buffer_copy->size && dst < src + buffer_copy->size)
                    return ReplayResult::invalid_copy;
                // The copy reads the title's pages for these bytes (docs/page_content.md): pull the source range into
                // the arena stand-in first, bounded so one huge CopyResource cannot dominate a submission.
                if (vk.pull_title_arena)
                    vk.pull_title_arena(src, std::min<std::uint64_t>(buffer_copy->size, 1u << 23));
                if (predicated) {
                    if ((statistics_active && !statistics_helpers) || !vk.prepare_predicated_copy || !vk.emit_predicated_copy ||
                        !vk.prepare_predicated_copy(p.source.buffer, p.binding.buffer, src, dst, buffer_copy->size, p.predicated_copy))
                        return ReplayResult::unsupported;
                } else p.buffer_copies.push_back({src, dst, buffer_copy->size});
            } else if (const auto* texture_copy = std::get_if<CopyTexture>(&command)) {
                result = detail::prepare_texture_copy(*texture_copy, p);
                // A buffer -> image upload reads the title's pages for its footprint rows (docs/page_content.md):
                // pull each region's source bytes into the arena stand-in before anything is recorded. This is where
                // the title's texture data enters the GPU, so a sampling pass sees real texels.
                if (result == ReplayResult::success && vk.pull_title_arena && texture_copy->src.footprint) {
                    for (const VkBufferImageCopy& region : p.buffer_image_copies) {
                        const BlockFormat f = block_format(p.binding.format, region.imageSubresource.aspectMask);
                        if (!f.bytes) continue;
                        const std::uint64_t row = std::uint64_t(region.bufferRowLength ? region.bufferRowLength
                                                                                      : region.imageExtent.width) *
                                                  f.bytes;
                        const std::uint64_t rows = region.bufferImageHeight ? region.bufferImageHeight
                                                                            : region.imageExtent.height;
                        const std::uint64_t bytes = row * rows * region.imageExtent.depth;
                        if (bytes)
                            vk.pull_title_arena(region.bufferOffset, std::min<std::uint64_t>(bytes, 1u << 23));
                    }
                }
                if (result == ReplayResult::success && !p.staged_reads.empty()) {
                    // A block-compressed copy staged through transfer scratch (prepare_texture_copy): whole blocks of
                    // the read's row length and height, one region a copy.
                    const VkBufferImageCopy& read = p.staged_reads[0];
                    const BlockFormat f = block_format(p.source.format, read.imageSubresource.aspectMask);
                    const VkDeviceSize bytes = VkDeviceSize(read.bufferRowLength / f.block) *
                                               (read.bufferImageHeight / f.block) * read.imageExtent.depth * f.bytes;
                    VkDeviceSize base = 0;
                    if (!f.bytes || !vk.copy_image_to_buffer || !vk.copy_buffer_to_image || !vk.copy_scratch ||
                        !vk.copy_scratch(bytes, p.staged, base))
                        return ReplayResult::invalid_copy;  // refused as before this path existed
                    p.staged_reads[0].bufferOffset += base;
                    p.staged_writes[0].bufferOffset += base;
                }
                // A depth plane footprint of a D24 texture stored as D32: words of 24-bit UNORM, floats in the image.
                const bool upload = texture_copy->src.footprint;
                const ResourceBinding& image = upload ? p.binding : p.source;
                if (result == ReplayResult::success && texture_copy->tile_footprint && !p.buffer_image_copies.empty() &&
                    p.buffer_image_copies.front().bufferOffset % 4 && !detail::stage_unaligned_tile(vk, p, upload))
                    return ReplayResult::unsupported;
                const bool depth24_footprint = result == ReplayResult::success && image.depth24_as_float &&
                    !p.buffer_image_copies.empty() &&
                    p.buffer_image_copies[0].imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT;
                const bool rgb96_footprint = result == ReplayResult::success && image.rgb96 &&
                    !p.buffer_image_copies.empty();
                // Without a segment pool, refuse before allocating conversion scratch/descriptors.
                if (statistics_active && !statistics_helpers && (depth24_footprint || rgb96_footprint)) return ReplayResult::unsupported;
                if (depth24_footprint) {
                    const VkBufferImageCopy& region = p.buffer_image_copies[0];
                    const ResourceBinding& buffer = upload ? p.source : p.binding;
                    Depth24Job& job = p.depth24;
                    job.to_float = upload;
                    job.app = buffer.buffer;
                    job.app_offset = region.bufferOffset;
                    job.app_size = buffer.buffer_offset + buffer.size;
                    job.width = region.imageExtent.width;
                    job.height = region.imageExtent.height;
                    job.slices = region.imageExtent.depth;
                    job.app_row_words = region.bufferRowLength ? region.bufferRowLength : job.width;
                    job.app_slice_words = job.app_row_words * (region.bufferImageHeight ? region.bufferImageHeight : job.height);
                    if (!vk.depth24_prepare || !vk.depth24_run || !vk.depth24_prepare(job, p.depth24_parts)) return ReplayResult::unsupported;
                    p.has_depth24 = true;
                } else if (rgb96_footprint) {
                    // A footprint of a 96-bit texture: 12-byte texels, 16 in the image. The same pass and scratch.
                    const VkBufferImageCopy& region = p.buffer_image_copies[0];
                    const ResourceBinding& buffer = upload ? p.source : p.binding;
                    Depth24Job& job = p.depth24;
                    job.rgb96 = true;
                    job.to_float = upload;
                    job.app = buffer.buffer;
                    job.app_offset = region.bufferOffset;
                    job.app_size = buffer.buffer_offset + buffer.size;
                    job.width = region.imageExtent.width;
                    job.height = region.imageExtent.height;
                    job.slices = region.imageExtent.depth;
                    job.app_row_words = p.rgb96_row_words;
                    job.app_slice_words = p.rgb96_slice_words;
                    if (!vk.depth24_prepare || !vk.depth24_run || !vk.depth24_prepare(job, p.depth24_parts)) return ReplayResult::unsupported;
                    p.has_depth24 = true;
                }
            } else {
                result = detail::prepare_resource_copy(p);
                // A whole-resource copy from a buffer reads the title's pages too (docs/page_content.md).
                if (result == ReplayResult::success && vk.pull_title_arena && p.source.buffer && !p.source.image)
                    vk.pull_title_arena(p.source.buffer_offset, std::min<std::uint64_t>(p.source.size, 1u << 23));
            }
            if (result == ReplayResult::success && !p.image_copies.empty() &&
                p.image_copies[0].srcSubresource.aspectMask != p.image_copies[0].dstSubresource.aspectMask) {
                // Depth <-> colour: vkCmdCopyImage cannot mix aspects, so each region goes image -> scratch -> image,
                // tightly packed. Depth aspects want 4-byte buffer offsets; 16 covers every texel size here.
                const std::uint32_t texel =
                    block_format(p.source.format, p.image_copies[0].srcSubresource.aspectMask).bytes;
                VkDeviceSize total = 0;
                std::vector<VkDeviceSize> offsets;
                for (const VkImageCopy& c : p.image_copies) {
                    offsets.push_back(total);
                    total += (VkDeviceSize(c.extent.width) * c.extent.height * c.extent.depth *
                                  c.srcSubresource.layerCount * texel + 15) / 16 * 16;
                }
                VkDeviceSize base = 0;
                if (!texel || !vk.copy_scratch || !vk.copy_scratch(total, p.staged, base))
                    return ReplayResult::unsupported;
                for (std::size_t r = 0; r < p.image_copies.size(); ++r) {
                    const VkImageCopy& c = p.image_copies[r];
                    VkBufferImageCopy region{};
                    region.bufferOffset = base + offsets[r];
                    region.imageSubresource = c.srcSubresource;
                    region.imageOffset = c.srcOffset;
                    region.imageExtent = c.extent;
                    p.staged_reads.push_back(region);
                    region.imageSubresource = c.dstSubresource;
                    region.imageOffset = c.dstOffset;
                    p.staged_writes.push_back(region);
                }
                p.image_copies.clear();
            }
            if (result != ReplayResult::success) return result;
            if (p.binding.host_readable) {
                out.wrote_readback = true;
                const bool compute_write =
                    (predicated && std::holds_alternative<CopyBuffer>(command)) || (p.has_depth24 && !p.binding.image);
                out.readback_src_stages |= compute_write
                    ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_COPY_BIT;
            }
        } else if (const auto* resolve_command = std::get_if<ResolveSubresource>(&command)) {
            if (!vk.resolve_image) return ReplayResult::unsupported;
            if (predicated) ++out.unpredicated_copies;  // a transfer command, like the copies
            const ResourceBinding* source = resolve(resolve_command->src);
            if (!bind(resolve_command->dst) || !source) return ReplayResult::unresolved_resource;
            p.source = *source;
            if (const auto result = detail::prepare_resolve(*resolve_command, p); result != ReplayResult::success)
                return result;
            if (p.resolve_mode != VK_RESOLVE_MODE_NONE) {
                if (!vk.begin_rendering || !vk.end_rendering || !vk.image_view) return ReplayResult::unsupported;
                p.view = vk.image_view(p.source, p.source_range);
                p.resolve_view = vk.image_view(p.binding, p.range);
                if (!p.view || !p.resolve_view) return ReplayResult::unsupported;
                if (const auto result = detail::keep_around_resolve(vk, p); result != ReplayResult::success)
                    return result;
                // D3D-417, a driver workaround: AMD's Windows driver (32.0.21042.62, Radeon RDNA2 iGPU) resolves a
                // dynamic-rendering depth attachment from layer 0 of the image when the multisampled view starts at
                // another layer (the resolve view's first layer it honours). The same two views in a framebuffer
                // of a render pass resolve the right layer there, and on NVIDIA both ways do
                // (docs/d3d12_vulkan.md). So a depth-only source above layer 0 goes through a render pass, on
                // every GPU. A depth-stencil source does not: its planes can be in different layouts here.
                if (p.source_range.baseArrayLayer && p.source.aspects == VK_IMAGE_ASPECT_DEPTH_BIT &&
                    vk.depth_resolve_framebuffer && vk.begin_render_pass && vk.end_render_pass) {
                    const VkExtent2D from = detail::mip_extent(p.source, p.source_range.baseMipLevel);
                    const VkExtent2D to = detail::mip_extent(p.binding, p.range.baseMipLevel);
                    p.resolve_framebuffer = vk.depth_resolve_framebuffer(
                        p.source, p.view, image_layout(p.source, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL),
                        p.binding, p.resolve_view,
                        image_layout(p.binding, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL), p.resolve_mode,
                        {std::min(from.width, to.width), std::min(from.height, to.height)}, p.resolve_pass);
                    if (!p.resolve_framebuffer) return ReplayResult::unsupported;
                }
            }
        } else if (const auto* discard = std::get_if<DiscardResource>(&command)) {
            // Buffers and partial (rectangle) discards keep their contents, which "undefined" allows.
            if (!bind(discard->resource)) return ReplayResult::unresolved_resource;
            const std::uint64_t end = std::uint64_t(discard->first_subresource) + discard->count;
            if (!p.binding.image) {
                if (!discard->whole && end > 1) return ReplayResult::invalid_subresource;
            } else if (discard->whole) {
                VkImageSubresourceRange range{};
                if (!image_range(p.binding, all_subresources, range)) return ReplayResult::invalid_subresource;
                if (!discard->partial) p.discards.push_back(range);
            } else {
                for (std::uint64_t sub = discard->first_subresource; sub < end; ++sub) {
                    const auto s = static_cast<std::uint32_t>(sub);
                    VkImageSubresourceRange range{};
                    if (!image_range(p.binding, s, range)) return ReplayResult::invalid_subresource;
                    if (!discard->partial) p.discards.push_back(range);
                }
            }
        } else if (const auto* write = std::get_if<WriteImmediate>(&command)) {
            if (!vk.update_buffer || !vk.buffer_at) return ReplayResult::unsupported;
            std::uint64_t bytes = 4;
            if (!vk.buffer_at(write->address, bytes, p.write_buffer, p.write_offset) || bytes < 4)
                return ReplayResult::unresolved_resource;
            if (p.write_offset % 4) return ReplayResult::invalid_copy;
            // WriteBufferImmediate resolves by VA and may target a readback buffer in COPY_DEST.
            out.wrote_readback = true;
            out.readback_src_stages |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        } else if (const auto* atomic = std::get_if<AtomicCopyBuffer>(&command)) {
            if (!vk.copy_buffer) return ReplayResult::unsupported;
            if (predicated) ++out.unpredicated_copies;  // a transfer command, like the copies
            const ResourceBinding* source = resolve(atomic->src);
            if (!bind(atomic->dst) || !source) return ReplayResult::unresolved_resource;
            p.source = *source;
            if (!p.binding.buffer || !p.source.buffer || !detail::within(atomic->dst_offset, atomic->bytes, p.binding.size) ||
                !detail::within(atomic->src_offset, atomic->bytes, p.source.size))
                return ReplayResult::invalid_copy;
            for (const auto& d : atomic->dependencies) {
                const ResourceBinding* dependent = resolve(d.resource);
                if (!dependent) return ReplayResult::unresolved_resource;
                if (!dependent->image) {
                    const std::uint64_t end = d.end == UINT64_MAX ? dependent->size : d.end;
                    if (d.subresource != 0 || d.begin >= end || end > dependent->size)
                        return ReplayResult::invalid_copy;
                }
            }
            p.buffer_copies.push_back({p.source.buffer_offset + atomic->src_offset,
                                       p.binding.buffer_offset + atomic->dst_offset, atomic->bytes});
            if (p.binding.host_readable) {
                out.wrote_readback = true;
                out.readback_src_stages |= VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            }
        } else if (const auto* predication = std::get_if<Predication>(&command)) {
            predicated = predication->buffer != 0;
            if (!predicated) continue;
            if (!conditional) return ReplayResult::unsupported;
            // Without physical segments the capture dispatch would pollute CSInvocations.
            if (statistics_active && !statistics_helpers) return ReplayResult::unsupported;
            if (!bind(predication->buffer)) return ReplayResult::unresolved_resource;
            if (!p.binding.buffer || !detail::within(predication->offset, 8, p.binding.size) ||
                predication->offset > UINT64_MAX - p.binding.buffer_offset)
                return ReplayResult::invalid_predication;
            if (!vk.prepare_predicate || !vk.capture_predicate ||
                !vk.prepare_predicate(p.binding.buffer, p.binding.buffer_offset + predication->offset, p.predicate))
                return ReplayResult::unsupported;
        } else if (const QueryRange range = query_range(command); range.count) {
            // BeginQuery, EndQuery and ResolveQueryData are not among D3D12's predicated calls, so a predicate does
            // not suppress them (emit_query suspends conditional rendering around ResolveQueryData).
            if (!queries) return ReplayResult::unsupported;
            QueryPool* heap = vk.query_pool(range.heap);
            if (!heap) return ReplayResult::unresolved_resource;
            if (!query_fits_heap(heap->heap_type, range.type)) return ReplayResult::invalid_query;
            if (range.first >= heap->count || range.count > heap->count - range.first)
                return ReplayResult::invalid_query;
            if (!query_result_size(range.type)) return ReplayResult::unsupported;
            if (range.type != d3d12_query::timestamp && !std::holds_alternative<ResolveQueries>(command)) {
                const bool begin = std::holds_alternative<BeginQuery>(command);
                if (range.type == d3d12_query::occlusion) {
                    overlaps |= begin && (ordinary_active || binary_active);
                    if (begin) ++ordinary_active; else --ordinary_active;
                } else if (range.type == d3d12_query::binary_occlusion) {
                    overlaps |= begin && (ordinary_active || binary_active);
                    if (begin) ++binary_active; else --binary_active;
                } else if (is_so_statistics(range.type)) {
                    // No Vulkan query runs for it (QueryPool::pool is null), so it overlaps nothing.
                } else {
                    statistics_overlaps |= begin && statistics_active_count;
                    if (begin) ++statistics_active_count; else --statistics_active_count;
                    statistics_active = statistics_active_count != 0;
                }
            }
            if (const auto* resolve_queries = std::get_if<ResolveQueries>(&command)) {
                // The destination is a buffer, as D3D12 requires, large enough for every result.
                if (!bind(resolve_queries->dst)) return ReplayResult::unresolved_resource;
                const std::uint64_t bytes = std::uint64_t(range.count) * query_result_size(range.type);
                if (!p.binding.buffer || p.binding.buffer_offset % 8 ||
                    !detail::within(resolve_queries->offset, bytes, p.binding.size) ||
                    !detail::within(p.binding.buffer_offset, resolve_queries->offset + bytes,
                                    std::numeric_limits<VkDeviceSize>::max()))
                    return ReplayResult::invalid_query;
                // Inside a pipeline-statistics query, QueryReplay::plan checks that the helper dispatch is kept out
                // of the application's counters.
                if (range.type == d3d12_query::binary_occlusion && !vk.resolve_binary_occlusion)
                    return ReplayResult::unsupported;
                if (p.binding.host_readable) {
                    out.wrote_readback = true;
                    out.readback_src_stages |= VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                }
            }
            out.queries.push_back({i, heap, statistics_active});
        } else if (const auto* ind = std::get_if<ExecuteIndirect>(&command)) {
            if (!bind(ind->argument_buffer) || !p.binding.buffer) return ReplayResult::unresolved_resource;
            const VkDeviceSize record_size = detail::indirect_record_size(ind->kind);
            if (!record_size || ind->stride < record_size || ind->stride % sizeof(std::uint32_t))
                return ReplayResult::invalid_indirect_arguments;
            // The span is max_count * stride, the bytes the signature can walk, as D3D12 measures it: a record that
            // fits at the end of the buffer while its stride runs past it is refused (`240` with stride `20` in a
            // `256`-byte buffer). A zero max_count still has to name an offset inside the buffer.
            if (ind->argument_offset > p.binding.size ||
                ind->max_count > (p.binding.size - ind->argument_offset) / ind->stride ||
                !detail::indirect_range(p.binding, ind->argument_offset,
                                        VkDeviceSize(ind->max_count) * ind->stride))
                return ReplayResult::invalid_indirect_arguments;
            if (ind->count_buffer) {
                const ResourceBinding* cnt = resolve(ind->count_buffer);
                if (!cnt || !cnt->buffer) return ReplayResult::unresolved_resource;
                if (!detail::indirect_range(*cnt, ind->count_offset, sizeof(std::uint32_t)))
                    return ReplayResult::invalid_indirect_arguments;
                p.source = *cnt;
                if (ind->kind == IndirectKind::dispatch) {
                    // Vulkan lacks vkCmdDispatchIndirectCount. Copy the GPU-visible commands to private scratch
                    // and replace commands past min(count, max_count) with zero-group dispatches.
                    if (!ind->pipeline || !vk.dispatch_indirect || !vk.prepare_counted_dispatch ||
                        !vk.emit_counted_dispatch || (statistics_active && !statistics_helpers) ||
                        !vk.prepare_counted_dispatch(p.binding.buffer, p.binding.buffer_offset + ind->argument_offset,
                                                     p.source.buffer, p.source.buffer_offset + ind->count_offset,
                                                     ind->stride, ind->max_count, p.counted_dispatch))
                        return ReplayResult::unsupported;
                } else if ((ind->kind == IndirectKind::draw && !vk.draw_indirect_count) ||
                           (ind->kind == IndirectKind::draw_indexed && !vk.draw_indexed_indirect_count)) {
                    return ReplayResult::unsupported;
                }
            } else {
                if ((ind->kind == IndirectKind::draw && !vk.draw_indirect) ||
                    (ind->kind == IndirectKind::draw_indexed && !vk.draw_indexed_indirect) ||
                    (ind->kind == IndirectKind::dispatch && !vk.dispatch_indirect))
                    return ReplayResult::unsupported;
            }
            if (ind->kind == IndirectKind::dispatch && ind->pipeline) {  // the pipeline and root arguments it was recorded
                if (!vk.bind_pipeline || !vk.push_constants || !vk.compute_pipeline) return ReplayResult::unsupported;
                const ComputePipeline* pipeline = vk.compute_pipeline(ind->pipeline);
                if (!pipeline || !pipeline->pipeline) return ReplayResult::unresolved_resource;
                if (pipeline->bindless_set && !vk.bind_descriptor_sets) return ReplayResult::unsupported;
                p.pipeline = *pipeline;
            }
        } else if (const auto* build = std::get_if<BuildAccelerationStructure>(&command)) {
            p.khr_instances = 0;
            if (vk.xdxr_check) {
                if (!vk.xdxr_check(*build)) return ReplayResult::unsupported;
            } else {
                if (!vk.build_acceleration_structures || !vk.resolve_acceleration_structure)
                    return ReplayResult::unsupported;
                p.accel_dst = vk.resolve_acceleration_structure(build->dst_va);
                if (!p.accel_dst) return ReplayResult::unresolved_resource;
                if (build->update && build->src_va) {
                    p.accel_src = vk.resolve_acceleration_structure(build->src_va);
                    if (!p.accel_src) return ReplayResult::unresolved_resource;
                }
                if (build->is_tlas && build->instance_pointers && build->instance_count) {
                    if (!vk.prepare_khr_instances || !vk.host_at || !vk.buffer_at) return ReplayResult::unsupported;
                    return vk.prepare_khr_instances(*build, p.khr_instances);
                }
            }
        } else if (const auto* copy = std::get_if<CopyAccelerationStructure>(&command)) {
            if (vk.xdxr_copy) {
                // XDXR structures: the destination's room bounds the copy, as the size is only known to the GPU.
                if (!vk.buffer_at || copy->mode > 1) return ReplayResult::unsupported;
                std::uint64_t room = ~std::uint64_t(0);
                VkBuffer buffer = VK_NULL_HANDLE;
                VkDeviceSize offset = 0;
                if (!vk.buffer_at(copy->dst_va, room, buffer, offset)) return ReplayResult::unresolved_resource;
                std::uint64_t source = ~std::uint64_t(0);
                if (!vk.buffer_at(copy->src_va, source, buffer, offset)) return ReplayResult::unresolved_resource;
                p.xdxr_bytes = std::min(room, source);
            } else {
                if (vk.queue_stages == detail::queue_stages_copy || !vk.copy_acceleration_structure ||
                    !vk.resolve_acceleration_structure || copy->mode > 1)
                    return ReplayResult::unsupported;
                p.accel_dst = vk.resolve_acceleration_structure(copy->dst_va);
                p.accel_src = vk.resolve_acceleration_structure(copy->src_va);
                if (!p.accel_dst || !p.accel_src) return ReplayResult::unresolved_resource;
                // Vulkan forbids overlapping source and destination handles in CLONE/COMPACT copies.
                if (p.accel_dst == p.accel_src) return ReplayResult::invalid_copy;
            }
        } else if (const auto* info = std::get_if<AccelerationStructurePostbuild>(&command)) {
            if (vk.xdxr_postbuild) {
                if (!vk.buffer_at || (info->type != 0 && info->type != 3)) return ReplayResult::unsupported;
                std::uint64_t room = 8 * info->sources.size();
                VkBuffer buffer = VK_NULL_HANDLE;
                VkDeviceSize offset = 0;
                if (!vk.buffer_at(info->dest_va, room, buffer, offset)) return ReplayResult::unresolved_resource;
                if (room < 8 * info->sources.size()) return ReplayResult::invalid_copy;
            } else if (info->type == 0 && vk.write_acceleration_structures_properties &&
                       vk.resolve_acceleration_structure && vk.reset_query_pool && vk.copy_query_pool_results &&
                       vk.pipeline_barrier && vk.buffer_at && vk.prepare_khr_postbuild) {
                if (vk.queue_stages == detail::queue_stages_copy || info->sources.empty() ||
                    info->sources.size() > std::numeric_limits<std::uint32_t>::max() / 8u)
                    return ReplayResult::unsupported;
                const std::uint64_t need = 8ull * info->sources.size();
                std::uint64_t room = need;
                detail::PreparedKhrPostbuild pb;
                if (!vk.buffer_at(info->dest_va, room, pb.dst_buffer, pb.dst_offset))
                    return ReplayResult::unresolved_resource;
                // vkCmdCopyQueryPoolResults with VK_QUERY_RESULT_64_BIT requires an 8-byte aligned dstOffset.
                if (!pb.dst_buffer || room < need || (pb.dst_offset % 8) != 0)
                    return ReplayResult::invalid_copy;
                pb.sources.reserve(info->sources.size());
                for (const std::uint64_t src_va : info->sources) {
                    const VkAccelerationStructureKHR as = vk.resolve_acceleration_structure(src_va);
                    if (!as) return ReplayResult::unresolved_resource;
                    pb.sources.push_back(as);
                }
                if (!vk.prepare_khr_postbuild(static_cast<std::uint32_t>(pb.sources.size()), pb.query_pool,
                                              pb.first_query) ||
                    !pb.query_pool)
                    return ReplayResult::unsupported;
                p.khr_postbuild = static_cast<std::uint32_t>(out.khr_postbuilds.size());
                out.khr_postbuilds.push_back(std::move(pb));
                // dest_va is resolved by VA and may target a host-readable buffer in COPY_DEST.
                out.wrote_readback = true;
                out.readback_src_stages |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            } else {
                // Other postbuild info types (or callers without KHR postbuild wired) keep skipping rather than failing.
                p.skip = true;
            }
        } else if (const auto* dispatch = std::get_if<Dispatch>(&command)) {
            if (!vk.bind_pipeline || !vk.push_constants || !vk.dispatch || !vk.compute_pipeline)
                return ReplayResult::unsupported;
            if (vk.console_va_ready && !vk.console_va_ready()) return unresolved_at("dispatch.console_va");
            const ComputePipeline* pipeline = vk.compute_pipeline(dispatch->pipeline);
            if (!pipeline || !pipeline->pipeline) return ReplayResult::unresolved_resource;
            if (pipeline->bindless_set && !vk.bind_descriptor_sets) return ReplayResult::unsupported;
            p.pipeline = *pipeline;
        } else if (const auto* draw = std::get_if<Draw>(&command)) {
            if (out.draw_count == out.draws.size()) out.draws.emplace_back();
            auto& prepared_draw = out.draws[out.draw_count];
            prepared_draw.reset();
            p.draw = std::uint32_t(out.draw_count++);
            if (const auto r = detail::prepare_draw(*draw, resolve, vk, prepared_draw); r != ReplayResult::success)
                return r;
        }
    }
    if (ordinary_active || binary_active) return ReplayResult::invalid_query;
    out.statistics_overlaps = statistics_overlaps;
    if (statistics_overlaps) {
        const auto result = plan_overlapping_statistics_segments(out);
        if (result != ReplayResult::success) return result;
    } else {
        plan_statistics_segments(out);
    }
    if (overlaps) {
        if (!vk.sum_occlusion || out.queries.size() >= detail::PreparedQuery::no_segment)
            return ReplayResult::unsupported;
        return plan_occlusion_segments(out);
    }
    return ReplayResult::success;
}

// ---- Frame dump (docs/frame_capture.md) ---------------------------------------------------------------------
// Bounded diagnostic, PALMETTO_FRAME_DUMP only: prints the commands of the submissions around one draw in replay
// order, with the resource ids each one reads and writes, so "which command writes the presented resource, and
// when relative to the draws" is measured rather than guessed. The switch is read once; with it off nothing here
// runs, and the commands the replay records are the same bytes as without the diagnostic.
namespace frame_dump {

// The command's kind, one line per command. Pure: no state, no Vulkan call (port/test/frame_dump_test.cpp pins it).
inline const char* kind_name(const Command& command) {
    return std::visit(
        [](const auto& c) -> const char* {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, TransitionBarrier> || std::is_same_v<T, UavBarrier> ||
                          std::is_same_v<T, AliasingBarrier> || std::is_same_v<T, GlobalBarrier> ||
                          std::is_same_v<T, BufferBarrier> || std::is_same_v<T, TextureBarrier>) {
                return "barrier";
            } else if constexpr (std::is_same_v<T, ClearRenderTarget> || std::is_same_v<T, ClearDepthStencil> ||
                                 std::is_same_v<T, ClearUnorderedAccess>) {
                return "clear";
            } else if constexpr (std::is_same_v<T, Draw>) {
                return "draw";
            } else if constexpr (std::is_same_v<T, Dispatch>) {
                return "dispatch";
            } else if constexpr (std::is_same_v<T, CopyBuffer> || std::is_same_v<T, CopyTexture> ||
                                 std::is_same_v<T, CopyResource>) {
                return "copy";
            } else if constexpr (std::is_same_v<T, ResolveSubresource>) {
                return "resolve";
            } else if constexpr (std::is_same_v<T, Predication>) {
                return "predication";
            } else if constexpr (std::is_same_v<T, DiscardResource>) {
                return "discard";
            } else if constexpr (std::is_same_v<T, WriteImmediate>) {
                return "write-immediate";
            } else if constexpr (std::is_same_v<T, AtomicCopyBuffer>) {
                return "atomic-copy";
            } else if constexpr (std::is_same_v<T, BeginQuery> || std::is_same_v<T, EndQuery> ||
                                 std::is_same_v<T, ResolveQueries>) {
                return "query";
            } else if constexpr (std::is_same_v<T, DebugMarker>) {
                return "marker";
            } else if constexpr (std::is_same_v<T, ExecuteIndirect>) {
                return "execute-indirect";
            } else if constexpr (std::is_same_v<T, BuildAccelerationStructure> ||
                                 std::is_same_v<T, CopyAccelerationStructure> ||
                                 std::is_same_v<T, AccelerationStructurePostbuild>) {
                return "acceleration-structure";
            } else {
                return "other";
            }
        },
        command);
}

// Whether the command's own writes reach `id`: a clear, a copy or resolve destination, a UAV clear, a discard, or
// a draw whose colour or depth attachment is that resource. 0 never matches. Pure, as kind_name is.
inline bool command_writes(const Command& command, ResourceId id) {
    if (!id) return false;
    return std::visit(
        [id](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, ClearRenderTarget> || std::is_same_v<T, ClearDepthStencil>) {
                return c.target.resource == id;
            } else if constexpr (std::is_same_v<T, ClearUnorderedAccess>) {
                return c.view.resource == id;
            } else if constexpr (std::is_same_v<T, Draw>) {
                if (c.depth.resource == id) return true;
                if (!c.views) return false;
                const std::uint32_t count =
                    std::min<std::uint32_t>(c.views->render_target_count,
                                            static_cast<std::uint32_t>(c.views->render_targets.size()));
                for (std::uint32_t slot = 0; slot < count; ++slot)
                    if (c.views->render_targets[slot].resource == id) return true;
                return false;
            } else if constexpr (std::is_same_v<T, CopyBuffer>) {
                return c.dst == id;
            } else if constexpr (std::is_same_v<T, CopyTexture>) {
                return c.dst.resource == id;
            } else if constexpr (std::is_same_v<T, CopyResource>) {
                return c.dst == id;
            } else if constexpr (std::is_same_v<T, ResolveSubresource>) {
                return c.dst == id;
            } else if constexpr (std::is_same_v<T, AtomicCopyBuffer>) {
                return c.dst == id;
            } else if constexpr (std::is_same_v<T, DiscardResource>) {
                return c.resource == id;
            } else {
                return false;
            }
        },
        command);
}

// The indices of the commands that write `id`, in order: the ordered writer list of a resource in one submission.
inline std::vector<std::size_t> writers(std::span<const Command> commands, ResourceId id) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < commands.size(); ++i)
        if (command_writes(commands[i], id)) out.push_back(i);
    return out;
}

// The first command after `after` that writes `id`, or commands.size() when none does: whether a frame's resource
// is written again after its last draw (candidates (a) and (b) for a black frame).
inline std::size_t first_writer_after(std::span<const Command> commands, ResourceId id, std::size_t after) {
    for (std::size_t i = after + 1; i < commands.size(); ++i)
        if (command_writes(commands[i], id)) return i;
    return commands.size();
}

inline bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_FRAME_DUMP");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// Which draw's submission is dumped: PALMETTO_FRAME_DUMP_AT_DRAW, else the draw the one-shot readback measures
// (PALMETTO_READBACK_AT_DRAW), else the first draw. The index is the same zero-based draw index the readback counts.
inline unsigned long long dump_at_draw() {
    static const unsigned long long at = [] {
        const char* value = std::getenv("PALMETTO_FRAME_DUMP_AT_DRAW");
        if (!value || !*value) value = std::getenv("PALMETTO_READBACK_AT_DRAW");
        return value && *value ? std::strtoull(value, nullptr, 0) : 0ull;
    }();
    return at;
}

// How many draw-bearing submissions after the two dumped ones watch for a write to the dumped draw's target.
inline unsigned tracked_window() {
    static const unsigned window = [] {
        const char* value = std::getenv("PALMETTO_FRAME_DUMP_WINDOW");
        const unsigned long long parsed = value && *value ? std::strtoull(value, nullptr, 0) : 16ull;
        return static_cast<unsigned>(std::min<unsigned long long>(parsed, 64ull));
    }();
    return window;
}

// The known-good pipeline control (docs/frame_capture.md): with PALMETTO_FRAME_DUMP_CONTROL set, the queue draws the
// project's own full-screen triangle (the test/glsl vs_tri + fs_solid pair) into the armed draw's target after the
// frame's commands, so a target that reads black can be told apart from a replay raster fault. Diagnostic only.
inline bool control_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_FRAME_DUMP_CONTROL");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// The opaque pipeline id the control draw names; 1 is never a PSO pointer and never a registered pipeline id.
inline constexpr std::uintptr_t control_pipeline_id = 1;

// The PS-only control (docs/frame_capture.md): PALMETTO_FRAME_DUMP_PS_CONTROL=1 draws the armed draw's own recorded
// vertices through the title's pipeline with only the pixel shader replaced by the project's constant-output fs_solid.
// Its target reading back the constant colour means the title's vertex stage and rasterisation produce coverage and
// the black comes from the title PS's own work; staying black means the geometry/state produces no fragment. The
// pipelines are built by the shim (`GraphicsPipeline::with_ps`); diagnostic only.
inline bool ps_control_enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_FRAME_DUMP_PS_CONTROL");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// The reserved pipeline id of the PS-only control; 2, for the same reason as control_pipeline_id.
inline constexpr std::uintptr_t ps_control_pipeline_id = 2;

// The cull-mode axis of the PS-only control: PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE=1 builds the substituted recipe
// with back-face culling off. If the control then writes colour, the documented-default cull mode was discarding the
// title's winding; if it stays black, the geometry itself produces no coverage in the window and the cull axis closes.
inline bool ps_control_cull_none() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE");
        return value && *value && value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// One submission's place in the draw sequence, for the control draw: whether it holds the draw the readback and the
// control name, and the global index of its first draw.
struct Armed {
    bool armed = false;
    unsigned long long first = 0;
    unsigned long long at = 0;
};

inline Armed armed_submission(std::span<const PreparedList> lists) {
    static std::mutex mutex;
    static unsigned long long next_draw = 0;
    Armed out;
    out.at = dump_at_draw();
    std::lock_guard lock(mutex);
    out.first = next_draw;
    std::size_t draws = 0;
    for (const auto& list : lists) draws += list.draw_count;
    next_draw += draws;
    out.armed = draws != 0 && out.first <= out.at && out.at < next_draw;
    return out;
}

// The colour target of the draw `at` in a submission whose first draw is `first`: the resource the control draws
// into and the one-shot readback copies. found is false when the submission does not hold that draw.
struct ArmedTarget {
    bool found = false;
    ResourceId id = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The armed draw's own recorded command, for the PS-only control (same views, vertex buffers, counts and root
    // arguments, with only the pipeline replaced), and its translated pipeline, alive while the lists are.
    Draw draw;
    const GraphicsPipeline* pipeline = nullptr;
};

inline ArmedTarget armed_target(std::span<const PreparedList> lists, unsigned long long first,
                                unsigned long long at) {
    ArmedTarget out;
    unsigned long long index = first;
    for (const auto& list : lists) {
        const auto commands = list.list->commands();
        for (std::size_t i = 0; i < commands.size(); ++i) {
            if (!std::holds_alternative<Draw>(commands[i])) continue;
            if (index == at) {
                const detail::Prepared& p = list.commands[i];
                if (p.draw < list.draws.size()) {
                    const detail::PreparedDraw& d = list.draws[p.draw];
                    out.draw = std::get<Draw>(commands[i]);
                    out.pipeline = d.graphics;
                    if (!d.colors.empty() && d.colors[0].id && d.colors[0].binding.image) {
                        out.found = true;
                        out.id = d.colors[0].id;
                        out.format = d.colors[0].binding.format;
                        out.width = d.colors[0].binding.width;
                        out.height = d.colors[0].binding.height;
                    }
                }
                return out;
            }
            ++index;
        }
    }
    return out;
}

// Prints one command of the list, with what the prepared entry resolved for it. Everything bounded.
inline void print_command(const PreparedList& prepared, std::size_t index, unsigned long long draw_index) {
    const auto commands = prepared.list->commands();
    const Command& command = commands[index];
    const detail::Prepared& p = prepared.commands[index];
    const auto handle = [](auto value) {
        return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(value));
    };
    std::fprintf(stderr, "[rt] replay: frame dump:  #%zu %s", index, kind_name(command));
    std::visit(
        [&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, TransitionBarrier>) {
                std::fprintf(stderr, " id 0x%llx subresource %u before %u after %u",
                             static_cast<unsigned long long>(c.resource), c.subresource, c.before, c.after);
            } else if constexpr (std::is_same_v<T, UavBarrier>) {
                std::fprintf(stderr, " id 0x%llx", static_cast<unsigned long long>(c.resource));
            } else if constexpr (std::is_same_v<T, AliasingBarrier>) {
                std::fprintf(stderr, " before 0x%llx after 0x%llx", static_cast<unsigned long long>(c.before),
                             static_cast<unsigned long long>(c.after));
            } else if constexpr (std::is_same_v<T, GlobalBarrier>) {
                std::fprintf(stderr, " sync %u->%u access %u->%u", c.sync_before, c.sync_after, c.access_before,
                             c.access_after);
            } else if constexpr (std::is_same_v<T, BufferBarrier>) {
                std::fprintf(stderr, " id 0x%llx sync %u->%u access %u->%u",
                             static_cast<unsigned long long>(c.resource), c.sync_before, c.sync_after,
                             c.access_before, c.access_after);
            } else if constexpr (std::is_same_v<T, TextureBarrier>) {
                std::fprintf(stderr,
                             " id 0x%llx layout %u->%u sync %u->%u mip %u+%u layer %u+%u plane %u+%u",
                             static_cast<unsigned long long>(c.resource), c.layout_before, c.layout_after,
                             c.sync_before, c.sync_after, c.range.index_or_first_mip, c.range.mip_count,
                             c.range.first_layer, c.range.layer_count, c.range.first_plane, c.range.plane_count);
            } else if constexpr (std::is_same_v<T, ClearRenderTarget>) {
                std::fprintf(stderr,
                             " rt id 0x%llx mip %u layers %u+%u format %d colour %.3f %.3f %.3f %.3f",
                             static_cast<unsigned long long>(c.target.resource), c.target.mip, c.target.first_layer,
                             c.target.layer_count, static_cast<int>(c.target.format),
                             static_cast<double>(c.color[0]), static_cast<double>(c.color[1]),
                             static_cast<double>(c.color[2]), static_cast<double>(c.color[3]));
                std::fprintf(stderr, " image 0x%llx %ux%u", handle(p.binding.image), p.binding.width,
                             p.binding.height);
                if (c.rects.empty()) {
                    std::fprintf(stderr, " rects whole view");
                } else {
                    std::fprintf(stderr, " rects %zu", c.rects.size());
                    for (std::size_t r = 0; r < std::min<std::size_t>(c.rects.size(), 2); ++r)
                        std::fprintf(stderr, " [%d,%d..%d,%d]", c.rects[r].left, c.rects[r].top, c.rects[r].right,
                                     c.rects[r].bottom);
                }
            } else if constexpr (std::is_same_v<T, ClearDepthStencil>) {
                std::fprintf(stderr, " depth id 0x%llx mip %u layers %u+%u flags %u depth %.3f stencil %u",
                             static_cast<unsigned long long>(c.target.resource), c.target.mip,
                             c.target.first_layer, c.target.layer_count, c.flags,
                             static_cast<double>(c.depth), unsigned(c.stencil));
                std::fprintf(stderr, " image 0x%llx %ux%u", handle(p.binding.image), p.binding.width,
                             p.binding.height);
                std::fprintf(stderr, " rects %zu", c.rects.size());
            } else if constexpr (std::is_same_v<T, ClearUnorderedAccess>) {
                std::fprintf(stderr, " uav id 0x%llx buffer %d format %u rects %zu",
                             static_cast<unsigned long long>(c.view.resource), c.view.buffer ? 1 : 0,
                             c.view.format, c.rects.size());
            } else if constexpr (std::is_same_v<T, Draw>) {
                const detail::PreparedDraw* d = p.draw < prepared.draws.size() ? &prepared.draws[p.draw] : nullptr;
                const GraphicsPipeline* g = d ? d->graphics : nullptr;
                std::fprintf(stderr, " draw %llu pipeline 0x%llx", draw_index,
                             static_cast<unsigned long long>(c.pipeline));
                if (g) {
                    std::fprintf(stderr,
                                 " ps %s %u image(s) vs %u load(s) vk 0x%llx colour(s) %zu depth format %d",
                                 g->has_pixel_stage ? "present" : "absent", g->ps_images, g->vs_loads,
                                 handle(g->pipeline), g->color_formats.size(),
                                 static_cast<int>(g->depth_format));
                }
                if (d) {
                    const auto views = c.views;
                    for (std::size_t slot = 0; slot < d->colors.size() && slot < detail::max_render_targets; ++slot) {
                        const auto& a = d->colors[slot];
                        if (!a.id) continue;
                        const std::uint32_t mip = views && slot < views->render_targets.size()
                                                      ? views->render_targets[slot].mip : 0;
                        const std::uint32_t first_layer = views && slot < views->render_targets.size()
                                                              ? views->render_targets[slot].first_layer : 0;
                        const std::uint32_t layer_count = views && slot < views->render_targets.size()
                                                              ? views->render_targets[slot].layer_count : 0;
                        const VkExtent2D extent = detail::mip_extent(a.binding, mip);
                        std::fprintf(stderr,
                                     "; rt%zu id 0x%llx image 0x%llx mip %u layers %u+%u format %d %ux%u",
                                     slot, static_cast<unsigned long long>(a.id), handle(a.binding.image), mip,
                                     first_layer, layer_count, static_cast<int>(a.binding.format), extent.width,
                                     extent.height);
                    }
                    if (d->depth.id)
                        std::fprintf(stderr, "; depth id 0x%llx format %d",
                                     static_cast<unsigned long long>(d->depth.id),
                                     static_cast<int>(d->depth.binding.format));
                    std::fprintf(stderr, "; area %ux%u layers %u", d->area.width, d->area.height, d->layers);
                }
                std::fprintf(stderr,
                             "; viewport %.1fx%.1f at %.1f,%.1f depth %.3f..%.3f; scissor %d,%d..%d,%d; "
                             "topology %u; %s %u/%u first %u",
                             static_cast<double>(c.viewport.width), static_cast<double>(c.viewport.height),
                             static_cast<double>(c.viewport.x), static_cast<double>(c.viewport.y),
                             static_cast<double>(c.viewport.min_depth), static_cast<double>(c.viewport.max_depth),
                             c.scissor.left, c.scissor.top, c.scissor.right, c.scissor.bottom, c.topology,
                             c.indexed ? "indices" : "vertices", c.count, c.instances, c.first);
                if (c.indexed)
                    std::fprintf(stderr, "; index va 0x%llx size %u format %u",
                                 static_cast<unsigned long long>(c.index.va), c.index.size, c.index.format);
                if (const auto* views = c.views.get()) {
                    unsigned printed = 0;
                    for (std::uint32_t slot = 0; slot < 32 && printed < 4; ++slot) {
                        const VertexBufferView& view = views->vertex_buffers[slot];
                        if (!view.va) continue;
                        std::fprintf(stderr, "; vb%u va 0x%llx size %u stride %u", slot,
                                     static_cast<unsigned long long>(view.va), view.size, view.stride);
                        if (d && slot < d->vertex_buffers.size())
                            std::fprintf(stderr, " buffer 0x%llx offset %llu", handle(d->vertex_buffers[slot]),
                                         static_cast<unsigned long long>(d->vertex_offsets[slot]));
                        ++printed;
                    }
                }
                std::fprintf(stderr, "; user data s0..s15");
                for (unsigned k = 0; k < 16; ++k) std::fprintf(stderr, " %08x", c.user_data[k]);
            } else if constexpr (std::is_same_v<T, Dispatch>) {
                std::fprintf(stderr, " pipeline 0x%llx groups %u,%u,%u user data s0..s15",
                             static_cast<unsigned long long>(c.pipeline), c.x, c.y, c.z);
                for (unsigned k = 0; k < 16; ++k) std::fprintf(stderr, " %08x", c.user_data[k]);
            } else if constexpr (std::is_same_v<T, CopyBuffer>) {
                std::fprintf(stderr, " dst 0x%llx+%llu src 0x%llx+%llu size %llu",
                             static_cast<unsigned long long>(c.dst), static_cast<unsigned long long>(c.dst_offset),
                             static_cast<unsigned long long>(c.src), static_cast<unsigned long long>(c.src_offset),
                             static_cast<unsigned long long>(c.size));
                if (c.tiled_resource)
                    std::fprintf(stderr, " tiled 0x%llx", static_cast<unsigned long long>(c.tiled_resource));
            } else if constexpr (std::is_same_v<T, CopyTexture>) {
                std::fprintf(stderr, " dst 0x%llx%s subresource %u offset %llu src 0x%llx%s subresource %u at %u,%u,%u",
                             static_cast<unsigned long long>(c.dst.resource), c.dst.footprint ? " footprint" : "",
                             c.dst.subresource, static_cast<unsigned long long>(c.dst.offset),
                             static_cast<unsigned long long>(c.src.resource), c.src.footprint ? " footprint" : "",
                             c.src.subresource, c.x, c.y, c.z);
                if (c.has_box)
                    std::fprintf(stderr, " box %u,%u,%u..%u,%u,%u", c.box.left, c.box.top, c.box.front, c.box.right,
                                 c.box.bottom, c.box.back);
            } else if constexpr (std::is_same_v<T, CopyResource>) {
                std::fprintf(stderr, " dst 0x%llx src 0x%llx", static_cast<unsigned long long>(c.dst),
                             static_cast<unsigned long long>(c.src));
            } else if constexpr (std::is_same_v<T, ResolveSubresource>) {
                std::fprintf(stderr, " dst 0x%llx subresource %u src 0x%llx subresource %u%s",
                             static_cast<unsigned long long>(c.dst), c.dst_subresource,
                             static_cast<unsigned long long>(c.src), c.src_subresource,
                             c.region ? " (region)" : "");
            } else if constexpr (std::is_same_v<T, Predication>) {
                std::fprintf(stderr, " buffer 0x%llx offset %llu op %u",
                             static_cast<unsigned long long>(c.buffer),
                             static_cast<unsigned long long>(c.offset), c.op);
            } else if constexpr (std::is_same_v<T, DiscardResource>) {
                std::fprintf(stderr, " id 0x%llx first %u count %u partial %d whole %d",
                             static_cast<unsigned long long>(c.resource), c.first_subresource, c.count,
                             c.partial ? 1 : 0, c.whole ? 1 : 0);
            } else if constexpr (std::is_same_v<T, WriteImmediate>) {
                std::fprintf(stderr, " address 0x%llx value 0x%08x mode %u",
                             static_cast<unsigned long long>(c.address), c.value, c.mode);
            } else if constexpr (std::is_same_v<T, AtomicCopyBuffer>) {
                std::fprintf(stderr, " dst 0x%llx+%llu src 0x%llx+%llu bytes %u dependencies %zu",
                             static_cast<unsigned long long>(c.dst), static_cast<unsigned long long>(c.dst_offset),
                             static_cast<unsigned long long>(c.src), static_cast<unsigned long long>(c.src_offset),
                             c.bytes, c.dependencies.size());
            } else if constexpr (std::is_same_v<T, BeginQuery> || std::is_same_v<T, EndQuery>) {
                std::fprintf(stderr, " heap 0x%llx type %u index %u",
                             static_cast<unsigned long long>(c.heap), c.type, c.index);
            } else if constexpr (std::is_same_v<T, ResolveQueries>) {
                std::fprintf(stderr, " heap 0x%llx type %u first %u count %u dst 0x%llx+%llu",
                             static_cast<unsigned long long>(c.heap), c.type, c.first, c.count,
                             static_cast<unsigned long long>(c.dst), static_cast<unsigned long long>(c.offset));
            } else if constexpr (std::is_same_v<T, DebugMarker>) {
                std::fprintf(stderr, " %s",
                             c.kind == DebugMarker::Kind::set ? "set"
                             : c.kind == DebugMarker::Kind::begin ? "begin" : "end");
            } else if constexpr (std::is_same_v<T, ExecuteIndirect>) {
                std::fprintf(stderr, " %s stride %u max %u args 0x%llx+%llu count 0x%llx+%llu",
                             c.kind == IndirectKind::draw ? "draw"
                             : c.kind == IndirectKind::draw_indexed ? "draw-indexed" : "dispatch",
                             c.stride, c.max_count, static_cast<unsigned long long>(c.argument_buffer),
                             static_cast<unsigned long long>(c.argument_offset),
                             static_cast<unsigned long long>(c.count_buffer),
                             static_cast<unsigned long long>(c.count_offset));
            } else if constexpr (std::is_same_v<T, BuildAccelerationStructure>) {
                std::fprintf(stderr, " %s%s dst 0x%llx src 0x%llx triangles %zu", c.is_tlas ? "tlas" : "blas",
                             c.update ? " update" : "", static_cast<unsigned long long>(c.dst_va),
                             static_cast<unsigned long long>(c.src_va), c.triangles.size());
            } else if constexpr (std::is_same_v<T, CopyAccelerationStructure>) {
                std::fprintf(stderr, " dst 0x%llx src 0x%llx mode %u",
                             static_cast<unsigned long long>(c.dst_va),
                             static_cast<unsigned long long>(c.src_va), c.mode);
            } else if constexpr (std::is_same_v<T, AccelerationStructurePostbuild>) {
                std::fprintf(stderr, " dst 0x%llx type %u sources %zu",
                             static_cast<unsigned long long>(c.dest_va), c.type, c.sources.size());
            }
        },
        command);
    std::fprintf(stderr, "\n");
}

// Prints one list's commands in order, then the ordered writers of every resource a draw writes. Returns the first
// colour target of the draw `selected_draw` (0 when it has none): the resource the one-shot readback copies. When
// that draw is not in this list, the first draw's first colour target is returned instead.
inline ResourceId print_list(const PreparedList& prepared, unsigned long long first_draw, std::size_t list_index,
                             std::size_t list_count, unsigned long long selected_draw) {
    const auto commands = prepared.list->commands();
    std::fprintf(stderr,
                 "[rt] replay: frame dump: list %zu of %zu: %zu command(s), %zu draw(s), draws %llu..%llu\n",
                 list_index, list_count, commands.size(), prepared.draw_count, first_draw,
                 first_draw + prepared.draw_count - (prepared.draw_count ? 1 : 0));
    constexpr std::size_t kMaxCommands = 256;
    ResourceId readback_target = 0;
    ResourceId first_target = 0;
    unsigned long long draw_index = first_draw;
    std::vector<ResourceId> targets;
    const auto note = [&targets](ResourceId id) {
        if (!id) return;
        if (std::find(targets.begin(), targets.end(), id) == targets.end()) targets.push_back(id);
    };
    const auto first_color = [](const Draw& draw) -> ResourceId {
        if (!draw.views) return 0;
        const std::uint32_t count = std::min<std::uint32_t>(
            draw.views->render_target_count, static_cast<std::uint32_t>(draw.views->render_targets.size()));
        for (std::uint32_t slot = 0; slot < count; ++slot)
            if (draw.views->render_targets[slot].resource) return draw.views->render_targets[slot].resource;
        return 0;
    };
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (i == kMaxCommands) {
            std::fprintf(stderr, "[rt] replay: frame dump: ... %zu more command(s) not printed (bounded)\n",
                         commands.size() - i);
            break;
        }
        const bool is_draw = std::holds_alternative<Draw>(commands[i]);
        print_command(prepared, i, draw_index);
        if (const auto* draw = std::get_if<Draw>(&commands[i])) {
            if (!first_target) first_target = first_color(*draw);
            if (draw_index == selected_draw) readback_target = first_color(*draw);
            if (draw->views) {
                const std::uint32_t count =
                    std::min<std::uint32_t>(draw->views->render_target_count,
                                            static_cast<std::uint32_t>(draw->views->render_targets.size()));
                for (std::uint32_t slot = 0; slot < count; ++slot)
                    note(draw->views->render_targets[slot].resource);
            }
            note(draw->depth.resource);
        }
        if (is_draw) ++draw_index;
    }
    // The ordered writers of each draw target: whether the last write is a draw, and whether anything writes the
    // resource after its last draw in this submission.
    for (const ResourceId target : targets) {
        const auto order = writers(commands, target);
        if (order.empty()) continue;
        std::size_t last_draw = commands.size();
        for (const std::size_t k : order)
            if (std::holds_alternative<Draw>(commands[k])) last_draw = k;
        std::fprintf(stderr, "[rt] replay: frame dump: writes to 0x%llx:",
                     static_cast<unsigned long long>(target));
        for (const std::size_t k : order) std::fprintf(stderr, " #%zu %s", k, kind_name(commands[k]));
        std::fprintf(stderr, "; last writer #%zu %s", order.back(), kind_name(commands[order.back()]));
        if (last_draw != commands.size()) {
            std::fprintf(stderr, "; last draw writing it #%zu", last_draw);
            if (const std::size_t after = first_writer_after(commands, target, last_draw);
                after != commands.size())
                std::fprintf(stderr, "; WRITTEN AFTER THE LAST DRAW by #%zu %s", after,
                             kind_name(commands[after]));
        }
        std::fprintf(stderr, "\n");
    }
    return readback_target ? readback_target : first_target;
}

// One ExecuteCommandLists: dump the submission that contains the selected draw, then the next draw-bearing
// submission, then count writes to the first draw's target over a bounded window of later submissions.
inline void submission(std::span<const PreparedList> lists) {
    if (!enabled() || lists.empty()) return;
    struct State {
        unsigned long long next_draw = 0;  // the global (zero-based) index of the next draw
        unsigned dumps = 0;
        unsigned tracked = 0;
        ResourceId target = 0;  // the target the tracking window watches
        unsigned writes[5] = {};  // draw, clear, copy, resolve, other
        unsigned reported = 0;    // non-draw writes printed
    };
    static State state;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const unsigned window = tracked_window();
    if (state.dumps >= 2 && state.tracked >= window) return;
    const unsigned long long first = state.next_draw;
    std::size_t draws = 0;
    for (const auto& list : lists) draws += list.draw_count;
    state.next_draw += draws;
    if (!draws) return;
    if (state.dumps < 2) {
        const unsigned long long at = dump_at_draw();
        const bool selected = state.dumps > 0 || (first <= at && at < state.next_draw);
        if (!selected) return;
        if (state.dumps == 0) {
            std::fprintf(stderr,
                         "[rt] replay: frame dump: submission with draws %llu..%llu (%zu list(s)); resource ids are "
                         "the D3D12 resource pointers (resource_id), as the present trace prints plane resources; "
                         "the one-shot readback copies mip 0, layer 0, origin 0,0\n",
                         first, state.next_draw - 1, lists.size());
        } else {
            std::fprintf(stderr,
                         "[rt] replay: frame dump: next submission with draws %llu..%llu (%zu list(s))\n", first,
                         state.next_draw - 1, lists.size());
        }
        unsigned long long base = first;
        for (std::size_t i = 0; i < lists.size(); ++i) {
            const ResourceId target = print_list(lists[i], base, i, lists.size(), at);
            if (state.dumps == 0 && !state.target) state.target = target;
            base += lists[i].draw_count;
        }
        ++state.dumps;
        if (state.dumps < 2) return;
        if (!state.target) {
            std::fprintf(stderr, "[rt] replay: frame dump: the dumped draw has no colour target; no tracking\n");
            state.tracked = window;
            return;
        }
        std::fprintf(stderr,
                     "[rt] replay: frame dump: watching the next %u draw submission(s) for writes to 0x%llx\n",
                     window, static_cast<unsigned long long>(state.target));
        return;
    }
    // The window after the two dumped submissions: which kinds write the dumped draw's target.
    for (const auto& list : lists) {
        for (const auto& command : list.list->commands()) {
            if (!command_writes(command, state.target)) continue;
            const char* kind = kind_name(command);
            unsigned slot = 4;
            if (std::holds_alternative<Draw>(command)) {
                slot = 0;
            } else if (std::holds_alternative<ClearRenderTarget>(command) ||
                       std::holds_alternative<ClearDepthStencil>(command) ||
                       std::holds_alternative<ClearUnorderedAccess>(command)) {
                slot = 1;
            } else if (std::holds_alternative<CopyBuffer>(command) ||
                       std::holds_alternative<CopyTexture>(command) ||
                       std::holds_alternative<CopyResource>(command)) {
                slot = 2;
            } else if (std::holds_alternative<ResolveSubresource>(command)) {
                slot = 3;
            }
            ++state.writes[slot];
            if (slot != 0 && state.reported < 8) {
                ++state.reported;
                std::fprintf(stderr,
                             "[rt] replay: frame dump: 0x%llx written at a later draw submission by %s\n",
                             static_cast<unsigned long long>(state.target), kind);
            }
        }
    }
    ++state.tracked;
    if (state.tracked >= window) {
        std::fprintf(stderr,
                     "[rt] replay: frame dump: over %u later draw submission(s), writes to 0x%llx: draw %u, clear %u, "
                     "copy %u, resolve %u, other %u\n",
                     window, static_cast<unsigned long long>(state.target), state.writes[0], state.writes[1],
                     state.writes[2], state.writes[3], state.writes[4]);
    }
}

} // namespace frame_dump

// A submission is planned against the currently published query state plus its own earlier lists.
// Keep every physical pool referenced by the recorded batch alive until that batch completes.
class QueryReplay {
public:
    // `statistics_pool` holds the list's pipeline-statistics segments (plan_statistics_segments). Without one, the
    // list's statistics queries are not split: ordinary helper commands are refused, as are resolves that dispatch.
    ReplayResult plan(PreparedList& list, const std::shared_ptr<const OcclusionSegmentPool>& pool = {},
                      std::uint32_t base = 0, bool can_sum = false,
                      const std::shared_ptr<const OcclusionSegmentPool>& statistics_pool = {},
                      std::uint32_t statistics_base = 0) {
        if (list.segment_count) {
            if (!pool) return ReplayResult::unsupported;
            retain(pool);
            list.segment_pool = pool->pool;
            list.segment_base = base;
        }
        if (list.statistics_segment_count) {
            const bool mesh = std::find(list.statistics_mesh.begin(), list.statistics_mesh.end(), 1) !=
                              list.statistics_mesh.end();
            if (statistics_pool && (!mesh || statistics_pool->mesh_pool)) {
                retain(statistics_pool);
                list.statistics_pool = statistics_pool->pool;
                list.statistics_mesh_pool = statistics_pool->mesh_pool;
                list.statistics_base = statistics_base;
            } else if (mesh) {
                // Unsplit, the query would be active around a mesh draw with counters Vulkan forbids there.
                return ReplayResult::unsupported;
            } else if (list.statistics_overlaps) {
                // Whole queries on the heap pool would keep two pipeline-statistics queries active at once.
                return ReplayResult::unsupported;
            } else {
                for (const auto& command : list.commands)
                    if (command.statistics_end != detail::PreparedQuery::no_segment)
                        return ReplayResult::unsupported;
                for (auto& query : list.queries) {
                    query.statistics_end = query.statistics_begin = detail::PreparedQuery::no_segment;
                    query.statistics_count = 0;
                }
                list.statistics_segment_count = 0;
            }
        }
        for (auto& query : list.queries) {
            const Command& command = list.list->commands()[query.command];
            if (const auto* begin = std::get_if<BeginQuery>(&command)) {
                pending_[{query.heap, begin->index}] = {};
            } else if (const auto* end = std::get_if<EndQuery>(&command)) {
                Value result{true, {}};
                if (query.result_count) result.occlusion = {pool, base + query.first_result, query.result_count};
                if (query.statistics_count)
                    result.occlusion = {statistics_pool, statistics_base + query.statistics_first,
                                        query.statistics_count};
                pending_[{query.heap, end->index}] = std::move(result);
            } else {
                const auto& resolve = std::get<ResolveQueries>(command);
                const bool binary = resolve.type == d3d12_query::binary_occlusion;
                const bool statistics = resolve.type == d3d12_query::pipeline_statistics;
                // A helper dispatch inside an unsplit statistics query would count in the application's
                // CSInvocations.
                const bool uncounted = !query.statistics_active ||
                                       query.statistics_end != detail::PreparedQuery::no_segment;
                const std::uint32_t size = query_result_size(resolve.type);
                const auto offset = list.commands[query.command].binding.buffer_offset + resolve.offset;
                for (std::uint32_t i = 0; i < resolve.count; ++i) {
                    const auto index = resolve.first + i;
                    const auto value = lookup(*query.heap, index);
                    if (binary && value.ended && !uncounted) return ReplayResult::unsupported;
                    needs_binary |= binary && value.ended;
                    detail::QueryResolveRun run{value.ended ? query.heap->pool : VK_NULL_HANDLE,
                                                index, 1, offset + VkDeviceSize(i) * size};
                    if (value.occlusion.segments) {
                        if (resolve.type != d3d12_query::occlusion && !binary && !statistics)
                            return ReplayResult::invalid_query;
                        const auto& saved = value.occlusion;
                        const std::uint8_t* kinds = saved.segments->mesh.empty() ? nullptr
                                                                                 : saved.segments->mesh.data();
                        // A mesh segment's four counters go through the sum to land in their D3D12 fields.
                        const bool any_mesh = kinds && std::find(kinds + saved.first, kinds + saved.first + saved.count,
                                                                 std::uint8_t(1)) != kinds + saved.first + saved.count;
                        run = {saved.segments->pool, saved.first, saved.count, run.offset,
                               saved.count > 1 || any_mesh, saved.segments->mesh_pool, kinds};
                        if (any_mesh && !statistics) return ReplayResult::invalid_query;
                        if (!can_sum || (run.sum && !uncounted)) return ReplayResult::unsupported;
                        if (statistics) needs_statistics_sum |= run.sum;
                        else if (binary) (run.sum ? needs_sum_binary : needs_binary) = true;
                        else needs_sum |= run.sum;
                        retain(saved.segments);
                    }
                    if (!query.resolves.empty() && !run.sum && run.count == 1) {
                        auto& last = query.resolves.back();
                        if (!last.sum && last.pool == run.pool && last.first + last.count == run.first) {
                            ++last.count;
                            continue;
                        }
                    }
                    query.resolves.push_back(run);
                }
            }
        }
        return ReplayResult::success;
    }

    void commit() {
        for (auto& [key, result] : pending_) {
            const auto [heap, index] = key;
            heap->ended[index] = result.ended;
            if (result.occlusion.segments) heap->occlusion[index] = std::move(result.occlusion);
            else heap->occlusion.erase(index);
        }
    }

    bool needs_sum = false;
    bool needs_binary = false;
    bool needs_statistics_sum = false;
    bool needs_sum_binary = false;  // a binary query whose segments must be summed before the 0/1 normalize
    std::vector<std::shared_ptr<const OcclusionSegmentPool>> retained;

private:
    struct Value {
        bool ended = false;
        OcclusionResult occlusion;
    };

    Value lookup(QueryPool& heap, std::uint32_t index) const {
        if (const auto it = pending_.find({&heap, index}); it != pending_.end()) return it->second;
        if (const auto it = heap.occlusion.find(index); it != heap.occlusion.end()) return {true, it->second};
        return {heap.ended[index], {}};
    }

    void retain(const std::shared_ptr<const OcclusionSegmentPool>& pool) {
        if (std::find(retained.begin(), retained.end(), pool) == retained.end()) retained.push_back(pool);
    }

    std::map<std::pair<QueryPool*, std::uint32_t>, Value> pending_;
};

namespace detail {

// A physical pipeline-statistics segment (plan_statistics_segments), in the pool its kind needs.
inline VkQueryPool statistics_segment_pool(const PreparedList& list, std::uint32_t segment) {
    return list.statistics_mesh[segment] ? list.statistics_mesh_pool : list.statistics_pool;
}

inline void end_statistics_segment(const PreparedList& list, std::uint32_t segment, VkCommandBuffer cb,
                                   const ReplayFunctions& vk) {
    if (segment == PreparedQuery::no_segment) return;
    vk.end_query(cb, statistics_segment_pool(list, segment), list.statistics_base + segment);
}

inline void begin_statistics_segment(const PreparedList& list, std::uint32_t segment, VkCommandBuffer cb,
                                     const ReplayFunctions& vk) {
    if (segment == PreparedQuery::no_segment) return;
    const auto pool = statistics_segment_pool(list, segment);
    vk.reset_query_pool(cb, pool, list.statistics_base + segment, 1);
    vk.begin_query(cb, pool, list.statistics_base + segment, 0);
}

}  // namespace detail

inline void emit_query(const PreparedList& list, const detail::PreparedQuery& query, VkCommandBuffer cb,
                       const ReplayFunctions& vk, const VkConditionalRenderingBeginInfoEXT& predicate) {
    constexpr auto none = detail::PreparedQuery::no_segment;
    if (query.end_segment != none || query.begin_segment != none) {
        if (query.end_segment != none) vk.end_query(cb, list.segment_pool, list.segment_base + query.end_segment);
        if (query.begin_segment != none) {
            const auto index = list.segment_base + query.begin_segment;
            vk.reset_query_pool(cb, list.segment_pool, index, 1);
            vk.begin_query(cb, list.segment_pool, index, list.precise_segments ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        }
        return;
    }
    // A split pipeline-statistics query (plan_statistics_segments): its counters run in the physical segments.
    const auto end_statistics = [&] { detail::end_statistics_segment(list, query.statistics_end, cb, vk); };
    const auto begin_statistics = [&] { detail::begin_statistics_segment(list, query.statistics_begin, cb, vk); };
    const auto& command = list.list->commands()[query.command];
    const QueryPool& heap = *query.heap;
    if (!std::holds_alternative<ResolveQueries>(command) &&
        (query.statistics_begin != none || query.statistics_end != none)) {
        end_statistics();
        begin_statistics();
        return;
    }
    // A heap without a Vulkan pool (stream-output statistics: the layer has no stream output) runs nothing for
    // BeginQuery and EndQuery; its resolves were planned as zero fills.
    if (!heap.pool && !std::holds_alternative<ResolveQueries>(command)) return;
    if (const auto* begin = std::get_if<BeginQuery>(&command)) {
        vk.reset_query_pool(cb, heap.pool, begin->index, 1);
        const bool precise = begin->type == d3d12_query::occlusion && heap.precise;
        vk.begin_query(cb, heap.pool, begin->index, precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    } else if (const auto* end = std::get_if<EndQuery>(&command)) {
        if (end->type == d3d12_query::timestamp) {
            vk.reset_query_pool(cb, heap.pool, end->index, 1);
            vk.write_timestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, heap.pool, end->index);
        } else {
            vk.end_query(cb, heap.pool, end->index);
        }
    } else {
        const auto& resolve = std::get<ResolveQueries>(command);
        const bool binary = resolve.type == d3d12_query::binary_occlusion;
        const auto size = query_result_size(resolve.type);
        const auto dst = list.commands[query.command].binding.buffer;
        // ResolveQueryData is unconditional in D3D12. Suspend conditional rendering for the whole resolve rather
        // than only before helper dispatches: on Intel's Windows Vulkan driver, vkCmdFillBuffer (used for un-ended
        // queries) is suppressed inside an active conditional rendering scope after PredicateReducer::capture's
        // compute dispatch (D3D-148).
        const bool suspend = predicate.buffer && !query.resolves.empty();
        end_statistics();
        if (suspend) vk.end_conditional_rendering(cb);
        if (resolve.type == d3d12_query::timestamp && vk.order_timestamp_copy &&
            std::any_of(query.resolves.begin(), query.resolves.end(), [](const auto& run) { return run.pool; }))
            vk.order_timestamp_copy(cb);
        for (const auto& run : query.resolves) {
            if (!run.pool) {
                vk.fill_buffer(cb, dst, run.offset, VkDeviceSize(run.count) * size, 0);
            } else if (run.sum && resolve.type == d3d12_query::pipeline_statistics) {
                vk.sum_statistics(cb, run.pool, run.mesh_pool, run.mesh, run.first, run.count, dst, run.offset);
            } else if (run.sum && binary) {
                vk.sum_binary_occlusion(cb, run.pool, run.first, run.count, dst, run.offset);
            } else if (run.sum || binary) {
                const auto& resolve_occlusion = run.sum ? vk.sum_occlusion : vk.resolve_binary_occlusion;
                resolve_occlusion(cb, run.pool, run.first, run.count, dst, run.offset);
            } else {
                vk.copy_query_pool_results(cb, run.pool, run.first, run.count, dst, run.offset, size,
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            }
        }
        if (suspend) vk.begin_conditional_rendering(cb, &predicate);
        begin_statistics();
    }
}

// Ends the debug labels a submission left open, so every command buffer is balanced as Vulkan requires.
inline void close_labels(VkCommandBuffer command_buffer, const ReplayFunctions& vk, std::uint32_t& open_labels) {
    for (; open_labels && vk.end_label; --open_labels) vk.end_label(command_buffer);
    open_labels = 0;
}

// Writes a prepared list. With a tracker, barriers start from the tracked layouts and update them.
// `open_labels` counts the debug labels begun and not yet ended in this command buffer: an EndEvent with none open
// is dropped (Vulkan requires a matching begin), and the caller ends what is still open before it ends the buffer
// (close_labels). Without it, the list's own events are balanced at its end.
inline void emit(const PreparedList& prepared, VkCommandBuffer command_buffer, const ReplayFunctions& vk,
                 LayoutTracker* tracker = nullptr, std::uint32_t* open_labels = nullptr) {
    std::uint32_t own_labels = 0;
    std::uint32_t& labels = open_labels ? *open_labels : own_labels;
    const auto commands = prepared.list->commands();
    const bool separate = vk.separate_depth_stencil_layouts;
    const VulkanAccess transfer_dst = vulkan_access(d3d12_state::copy_dest, false);
    // The extension is enabled: barriers into and out of PREDICATION also cover conditional rendering's read.
    const bool predicates = vk.begin_conditional_rendering != nullptr;
    VkConditionalRenderingBeginInfoEXT predicate{VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT};
    std::size_t next_query = 0;
    // A dispatch honours conditional rendering and a copy does not (the layer runs copies unpredicated), so the
    // conversion around a copy must run whatever the predicate says: end it for the dispatch and begin it again.
    const auto run_depth24 = [&](const Depth24Job& job) {
        if (predicate.buffer) vk.end_conditional_rendering(command_buffer);
        vk.depth24_run(command_buffer, job);
        if (predicate.buffer) vk.begin_conditional_rendering(command_buffer, &predicate);
    };

    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto& command = commands[i];
        const auto& p = prepared.commands[i];
        if (next_query < prepared.queries.size() && prepared.queries[next_query].command == i) {
            emit_query(prepared, prepared.queries[next_query++], command_buffer, vk, predicate);
            continue;
        }
        // Helpers run outside the per-command rendering scopes; both segment endpoints stay outside too.
        const auto end_statistics = [&] {
            detail::end_statistics_segment(prepared, p.statistics_end, command_buffer, vk);
        };
        const auto begin_statistics = [&] {
            detail::begin_statistics_segment(prepared, p.statistics_begin, command_buffer, vk);
        };
        // A draw of the other kind inside a statistics query starts a segment in its pool (D3D-291), before the
        // draw's own rendering scope begins.
        if (std::holds_alternative<Draw>(command) || std::holds_alternative<ExecuteIndirect>(command)) {
            end_statistics();
            begin_statistics();
        }
        if (const auto* predication = std::get_if<Predication>(&command)) {
            // Conditional rendering cannot nest, so a new predicate ends the previous one first.
            if (predicate.buffer) vk.end_conditional_rendering(command_buffer);
            predicate.buffer = predication->buffer ? p.predicate.scratch : VK_NULL_HANDLE;
            if (predicate.buffer) {
                end_statistics();
                vk.capture_predicate(command_buffer, p.predicate);
                begin_statistics();
                predicate.offset = p.predicate.offset + 8;
                // Vulkan discards the commands when the word is 0, which is EQUAL_ZERO; INVERTED gives NOT_EQUAL_ZERO.
                predicate.flags = predication->op == d3d12_predication::not_equal_zero
                                      ? VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT
                                      : 0;
                vk.begin_conditional_rendering(command_buffer, &predicate);
            }
            continue;
        }
        const ResourceId id = std::visit(
            [](const auto& c) -> ResourceId {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, ClearRenderTarget> || std::is_same_v<T, ClearDepthStencil>)
                    return c.target.resource;
                else if constexpr (std::is_same_v<T, ClearUnorderedAccess>)
                    return c.view.resource;
                else if constexpr (requires { c.resource; })
                    return c.resource;
                else
                    return 0;
            },
            command);

        if (const auto* transition = std::get_if<TransitionBarrier>(&command)) {
            const VulkanAccess from = vulkan_access(transition->before, p.binding.presentable, predicates);
            const VulkanAccess to = vulkan_access(transition->after, p.binding.presentable, predicates);
            if (!p.binding.image) {
                detail::memory_barrier(vk, command_buffer, from.access, from.stages, to.access, to.stages);
                continue;
            }
            const auto range = detail::planes_for_layout(p.binding, p.range, separate);
            const VkImageLayout new_layout = image_layout(p.binding, to.layout);
            auto pieces = detail::current(tracker, id, range, image_layout(p.binding, from.layout));
            detail::initialize(vk, command_buffer, p.binding, id, pieces, image_layout(p.binding, from.layout), tracker);
            for (const auto& piece : pieces)
                detail::image_barrier(vk, command_buffer, p.binding, piece.range, piece.layout, new_layout,
                                      from.access, from.stages, to.access, to.stages);
            if (tracker) tracker->set(id, range, new_layout);
        } else if (std::holds_alternative<UavBarrier>(command)) {
            const VulkanAccess uav = vulkan_access(d3d12_state::unordered_access, false);
            VkAccessFlags src_access = VK_ACCESS_SHADER_WRITE_BIT;
            VkAccessFlags dst_access = uav.access;
            VkPipelineStageFlags stages = uav.stages;
            if (vk.build_acceleration_structures) {
                src_access |= VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                dst_access |= VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                              VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                stages |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            }
            detail::memory_barrier(vk, command_buffer, src_access, stages, dst_access, stages);
        } else if (std::holds_alternative<AliasingBarrier>(command)) {
            detail::memory_barrier(vk, command_buffer, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        } else if (std::holds_alternative<GlobalBarrier>(command) || std::holds_alternative<BufferBarrier>(command)) {
            const auto [sync_before, sync_after, access_before, access_after] = std::visit(
                [](const auto& c) -> std::array<std::uint32_t, 4> {
                    if constexpr (requires { c.sync_before; })
                        return {c.sync_before, c.sync_after, c.access_before, c.access_after};
                    else
                        return {};
                },
                command);
            const BarrierScope src = vulkan_barrier_scope(sync_before, access_before, predicates);
            const BarrierScope dst = vulkan_barrier_scope(sync_after, access_after, predicates);
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = src.stages;
            barrier.dstStageMask = dst.stages;
            barrier.srcAccessMask = src.access;
            barrier.dstAccessMask = dst.access;
            if (barrier.srcStageMask) detail::restrict_to_queue(vk, barrier.srcStageMask, barrier.srcAccessMask);
            if (barrier.dstStageMask) detail::restrict_to_queue(vk, barrier.dstStageMask, barrier.dstAccessMask);
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vk.pipeline_barrier2(command_buffer, &dependency);
        } else if (const auto* texture = std::get_if<TextureBarrier>(&command)) {
            const auto range = detail::planes_for_layout(p.binding, p.range, separate);
            const bool discard = (texture->flags & texture_barrier_discard) ||
                                 texture->layout_before == d3d12_layout::undefined;
            const VkImageLayout new_layout =
                image_layout(p.binding, vulkan_layout(texture->layout_after, p.binding.presentable));
            LayoutPieces pieces;
            if (discard)
                pieces.push_back({range, VK_IMAGE_LAYOUT_UNDEFINED});
            else {
                const VkImageLayout before =
                    image_layout(p.binding, vulkan_layout(texture->layout_before, p.binding.presentable));
                pieces = detail::current(tracker, id, range, before);
                detail::initialize(vk, command_buffer, p.binding, id, pieces, before, tracker);
            }
            const BarrierScope src = vulkan_barrier_scope(texture->sync_before, texture->access_before, predicates);
            const BarrierScope dst = vulkan_barrier_scope(texture->sync_after, texture->access_after, predicates);
            // One barrier for each piece: nearly always one or two (LayoutPieces), which stay on the stack.
            std::array<VkImageMemoryBarrier2, 2> few;
            std::vector<VkImageMemoryBarrier2> many;
            if (pieces.size() > few.size()) many.resize(pieces.size());
            VkImageMemoryBarrier2* const barriers = many.empty() ? few.data() : many.data();
            std::uint32_t barrier_count = 0;
            for (const auto& piece : pieces) {
                VkImageMemoryBarrier2& b = barriers[barrier_count++];
                b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                b.srcStageMask = src.stages;
                b.dstStageMask = dst.stages;
                b.srcAccessMask = src.access;
                b.dstAccessMask = dst.access;
                b.oldLayout = piece.layout;
                b.newLayout = new_layout;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = p.binding.image;
                b.subresourceRange = detail::layout_range(p.binding, piece.range);
                if (b.srcStageMask) detail::restrict_to_queue(vk, b.srcStageMask, b.srcAccessMask);
                if (b.dstStageMask) detail::restrict_to_queue(vk, b.dstStageMask, b.dstAccessMask);
            }
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = barrier_count;
            dependency.pImageMemoryBarriers = barriers;
            vk.pipeline_barrier2(command_buffer, &dependency);
            if (tracker) tracker->set(id, range, new_layout);
        } else if (const auto* color_clear = std::get_if<ClearRenderTarget>(&command)) {
            if (p.skip) continue;
            const VulkanAccess state = vulkan_access(d3d12_state::render_target, false);
            auto pieces = detail::current(tracker, id, p.range, image_layout(p.binding, state.layout));
            detail::initialize(vk, command_buffer, p.binding, id, pieces, image_layout(p.binding, state.layout), tracker);
            const VkClearColorValue color = detail::render_target_clear_color(p.clear_format, color_clear->color);
            if (p.rects.empty()) {
                detail::round_trip(vk, command_buffer, p.binding, pieces, image_layout(p.binding, transfer_dst.layout),
                                   state, transfer_dst.access, transfer_dst.stages, [&] {
                    vk.clear_color_image(command_buffer, p.binding.image, image_layout(p.binding, transfer_dst.layout),
                                         &color, 1, &p.range);
                });
                continue;
            }
            const VkImageLayout attachment = image_layout(p.binding, state.layout);
            detail::round_trip(vk, command_buffer, p.binding, pieces, attachment, state, state.access,
                               state.stages, [&] {
                VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                info.imageView = p.view;
                info.imageLayout = attachment;
                info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                rendering.renderArea = {{0, 0}, detail::mip_extent(p.binding, color_clear->target.mip)};
                rendering.layerCount = color_clear->target.layer_count;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &info;
                vk.begin_rendering(command_buffer, &rendering);
                VkClearAttachment attachment_clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
                attachment_clear.clearValue.color = color;
                vk.clear_attachments(command_buffer, 1, &attachment_clear, std::uint32_t(p.rects.size()), p.rects.data());
                vk.end_rendering(command_buffer);
            });
        } else if (const auto* depth_clear = std::get_if<ClearDepthStencil>(&command)) {
            if (p.skip) continue;
            // With separate layouts only the cleared planes move; otherwise both do.
            const auto range = detail::planes_for_layout(p.binding, p.range, separate);
            const VulkanAccess state = vulkan_access(d3d12_state::depth_write, false);
            auto pieces = detail::current(tracker, id, range, image_layout(p.binding, state.layout));
            detail::initialize(vk, command_buffer, p.binding, id, pieces, image_layout(p.binding, state.layout), tracker);
            const float depth = p.binding.depth24 ? depth24_clear_value(depth_clear->depth)  // see depth24.h
                                : p.binding.format == VK_FORMAT_D16_UNORM
                                    ? detail::quantize_unorm(depth_clear->depth, 65535.0)  // see quantize_unorm
                                    : depth_clear->depth;
            const VkClearDepthStencilValue value{depth, depth_clear->stencil};
            if (p.rects.empty()) {
                detail::round_trip(vk, command_buffer, p.binding, pieces, image_layout(p.binding, transfer_dst.layout),
                                   state, transfer_dst.access, transfer_dst.stages, [&] {
                    vk.clear_depth_stencil_image(command_buffer, p.binding.image,
                                                 image_layout(p.binding, transfer_dst.layout), &value, 1, &p.range);
                });
                continue;
            }
            const VkImageLayout attachment = image_layout(p.binding, state.layout);
            detail::round_trip(vk, command_buffer, p.binding, pieces, attachment, state, state.access,
                               state.stages, [&] {
                VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                info.imageView = p.view;
                info.imageLayout = attachment;
                info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                rendering.renderArea = {{0, 0}, detail::mip_extent(p.binding, depth_clear->target.mip)};
                rendering.layerCount = depth_clear->target.layer_count;
                if (p.range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) rendering.pDepthAttachment = &info;
                if (p.range.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) rendering.pStencilAttachment = &info;
                vk.begin_rendering(command_buffer, &rendering);
                VkClearAttachment attachment_clear{p.range.aspectMask, 0, {}};
                attachment_clear.clearValue.depthStencil = value;
                vk.clear_attachments(command_buffer, 1, &attachment_clear, std::uint32_t(p.rects.size()), p.rects.data());
                vk.end_rendering(command_buffer);
            });
        } else if (const auto* uav = std::get_if<ClearUnorderedAccess>(&command)) {
            if (p.skip) continue;
            // The UAV state, widened to transfers: a later barrier from SYNC_CLEAR_UNORDERED_ACCESS_VIEW names the
            // clear stage, which is a transfer stage in the legacy masks.
            VulkanAccess state = vulkan_access(d3d12_state::unordered_access, false);
            state.access |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            state.stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            if (uav->view.buffer) {
                constexpr VkAccessFlags transfer = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
                detail::memory_barrier(vk, command_buffer, state.access, state.stages, transfer,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT);
                for (std::size_t f = 0; f < p.fills.size(); ++f) {
                    if (f)  // rectangles may overlap
                        detail::memory_barrier(vk, command_buffer, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT, transfer, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    detail::emit_buffer_fill(vk, command_buffer, p.binding.buffer, p.element, p.fills[f][0],
                                             p.fills[f][1], p.fill_scratch, p.fill_scratch_offset);
                }
                detail::memory_barrier(vk, command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       state.access, state.stages);
                continue;
            }
            // Cleared in place in GENERAL, the UAV layout, which transfer clears and attachments both accept.
            const VkImageLayout layout = image_layout(p.binding, state.layout);
            auto pieces = detail::current(tracker, id, p.range, layout);
            detail::initialize(vk, command_buffer, p.binding, id, pieces, layout, tracker);
            if (p.rects.empty()) {
                detail::round_trip(vk, command_buffer, p.binding, pieces, layout, state,
                                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, [&] {
                    vk.clear_color_image(command_buffer, p.binding.image, layout, &p.clear_color, 1, &p.range);
                });
                continue;
            }
            detail::round_trip(vk, command_buffer, p.binding, pieces, layout, state,
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, [&] {
                VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                info.imageView = p.view;
                info.imageLayout = layout;
                info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                rendering.renderArea = {{0, 0}, detail::mip_extent(p.binding, uav->view.mip)};
                rendering.layerCount = uav->view.layer_count;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &info;
                vk.begin_rendering(command_buffer, &rendering);
                VkClearAttachment attachment_clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
                attachment_clear.clearValue.color = p.clear_color;
                vk.clear_attachments(command_buffer, 1, &attachment_clear, std::uint32_t(p.rects.size()), p.rects.data());
                vk.end_rendering(command_buffer);
            });
        } else if (std::holds_alternative<CopyBuffer>(command) || std::holds_alternative<CopyTexture>(command) ||
                   std::holds_alternative<CopyResource>(command)) {
            const auto [dst_id, src_id] = copy_ids(command);
            if (!p.predicated_copy.empty()) {
                end_statistics();
                vk.emit_predicated_copy(command_buffer, p.predicated_copy);
                begin_statistics();
            } else if (!p.buffer_copies.empty()) {
                vk.copy_buffer(command_buffer, p.source.buffer, p.binding.buffer, std::uint32_t(p.buffer_copies.size()),
                               p.buffer_copies.data());
            } else if (p.staged) {
                // Depth <-> colour, or a block-compressed copy no one extent fits: the source into the scratch, then
                // the scratch into the destination. The destination's copy_layout comes after the scratch write, so
                // its barrier orders every earlier transfer write before the destination's (#860, Vonsek's
                // synchronization-validation run).
                const VkImageLayout src_layout = detail::copy_layout(
                    vk, command_buffer, p.source, src_id, detail::planes_for_layout(p.source, p.source_range, separate),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, tracker);
                vk.copy_image_to_buffer(command_buffer, p.source.image, src_layout, p.staged,
                                        std::uint32_t(p.staged_reads.size()), p.staged_reads.data());
                detail::memory_barrier(vk, command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                const VkImageLayout dst_layout = detail::copy_layout(
                    vk, command_buffer, p.binding, dst_id, detail::planes_for_layout(p.binding, p.range, separate),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, tracker);
                vk.copy_buffer_to_image(command_buffer, p.staged, p.binding.image, dst_layout,
                                        std::uint32_t(p.staged_writes.size()), p.staged_writes.data());
            } else if (!p.image_copies.empty()) {
                // A subresource can be both the source and destination only in a shared layout.
                const bool self = p.same_copy_subresource;
                const VkImageLayout src_layout = detail::copy_layout(
                    vk, command_buffer, p.source, src_id, detail::planes_for_layout(p.source, p.source_range, separate),
                    self ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    self ? VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
                    tracker);
                const VkImageLayout dst_layout = self ? src_layout : detail::copy_layout(
                    vk, command_buffer, p.binding, dst_id, detail::planes_for_layout(p.binding, p.range, separate),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, tracker);
                const auto count = std::uint32_t(p.image_copies.size());
                const auto depth_stencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                if (std::holds_alternative<CopyResource>(command) &&
                    (p.binding.aspects & depth_stencil) == depth_stencil) {
                    // prepare_resource_copy groups regions by aspect. On RX 5700 XT (Windows driver
                    // 32.0.21045.5002), copying both groups in one command corrupts stencil in multi-mip
                    // arrays, even with validation clean. Order the aspect writes explicitly; a barrier
                    // before or after the bulk copy does not fix it. Keep all mips/layers batched per aspect.
                    const auto depth_count = p.source.mip_levels;
                    vk.copy_image(command_buffer, p.source.image, src_layout, p.binding.image, dst_layout,
                                  depth_count, p.image_copies.data());
                    detail::memory_barrier(vk, command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
                                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                                           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                           VK_PIPELINE_STAGE_TRANSFER_BIT);
                    vk.copy_image(command_buffer, p.source.image, src_layout, p.binding.image, dst_layout,
                                  count - depth_count, p.image_copies.data() + depth_count);
                } else {
                    vk.copy_image(command_buffer, p.source.image, src_layout, p.binding.image, dst_layout,
                                  count, p.image_copies.data());
                }
            } else if (p.binding.image) {
                VkBuffer source = p.source.buffer;
                std::vector<VkBufferImageCopy> converted;
                const std::vector<VkBufferImageCopy>* regions = &p.buffer_image_copies;
                if (p.tile_scratch) {
                    vk.copy_buffer(command_buffer, source, p.tile_scratch, std::uint32_t(p.tile_rows.size()),
                                   p.tile_rows.data());
                    detail::memory_barrier(vk, command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                           VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    source = p.tile_scratch;
                } else if (p.has_depth24) {
                    // The words become floats in the scratch buffer, which the copy then reads (tightly packed).
                    detail::memory_barrier(vk, command_buffer, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                    end_statistics();
                    for (const auto& part : p.depth24_parts) run_depth24(part);
                    begin_statistics();
                    detail::memory_barrier(vk, command_buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                           VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    converted = p.buffer_image_copies;
                    converted[0].bufferOffset = p.depth24.scratch_offset;
                    converted[0].bufferRowLength = 0;
                    converted[0].bufferImageHeight = 0;
                    regions = &converted;
                    source = p.depth24.scratch;
                }
                const VkImageLayout dst_layout = detail::copy_layout(
                    vk, command_buffer, p.binding, dst_id, detail::planes_for_layout(p.binding, p.range, separate),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, tracker);
                vk.copy_buffer_to_image(command_buffer, source, p.binding.image, dst_layout,
                                        std::uint32_t(regions->size()), regions->data());
            } else {
                const VkImageLayout src_layout = detail::copy_layout(
                    vk, command_buffer, p.source, src_id, detail::planes_for_layout(p.source, p.source_range, separate),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, tracker);
                if (!p.has_depth24) {
                    const auto target = p.tile_scratch ? p.tile_scratch : p.binding.buffer;
                    vk.copy_image_to_buffer(command_buffer, p.source.image, src_layout, target,
                                            std::uint32_t(p.buffer_image_copies.size()), p.buffer_image_copies.data());
                    if (p.tile_scratch) {
                        detail::memory_barrier(vk, command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT);
                        vk.copy_buffer(command_buffer, p.tile_scratch, p.binding.buffer,
                                       std::uint32_t(p.tile_rows.size()), p.tile_rows.data());
                    }
                } else {
                    // The floats land tightly packed in the scratch buffer; the pass turns them into the footprint's words.
                    VkBufferImageCopy region = p.buffer_image_copies[0];
                    region.bufferOffset = p.depth24.scratch_offset;
                    region.bufferRowLength = 0;
                    region.bufferImageHeight = 0;
                    vk.copy_image_to_buffer(command_buffer, p.source.image, src_layout, p.depth24.scratch, 1, &region);
                    detail::memory_barrier(vk, command_buffer, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                    end_statistics();
                    for (const auto& part : p.depth24_parts) run_depth24(part);
                    begin_statistics();
                    detail::memory_barrier(vk, command_buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                           VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                }
            }
        } else if (std::holds_alternative<ResolveSubresource>(command)) {
            // resolve_source and resolve_dest are the transfer layouts, as copy_source and copy_dest are.
            const auto [dst_id, src_id] = copy_ids(command);
            if (p.resolve_mode != VK_RESOLVE_MODE_NONE) {
                // A rendering scope of its own, with no draws: the source plane as the attachment, the destination
                // as its resolve target. Both go to the attachment layout and back. Resolves run in the colour
                // attachment output stage with colour attachment accesses, depth and stencil ones included.
                constexpr VkAccessFlags attachment_access =
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                constexpr VkPipelineStageFlags attachment_stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                const VulkanAccess source_state = vulkan_access(d3d12_state::resolve_source, false);
                const VulkanAccess dest_state = vulkan_access(d3d12_state::resolve_dest, false);
                const VkImageLayout source_layout = image_layout(p.source, source_state.layout);
                const VkImageLayout dest_layout = image_layout(p.binding, dest_state.layout);
                auto source_pieces = detail::current(
                    tracker, src_id, detail::planes_for_layout(p.source, p.source_range, separate), source_layout);
                detail::initialize(vk, command_buffer, p.source, src_id, source_pieces, source_layout, tracker);
                auto dest_pieces = detail::current(
                    tracker, dst_id, detail::planes_for_layout(p.binding, p.range, separate), dest_layout);
                detail::initialize(vk, command_buffer, p.binding, dst_id, dest_pieces, dest_layout, tracker);
                // The texels around the rectangle, kept in `staged` across the resolve (keep_around_resolve).
                const auto keep = [&](VkImageLayout wanted, VkAccessFlags access, auto&& copy) {
                    const VkImageLayout layout = image_layout(p.binding, wanted);
                    detail::round_trip(vk, command_buffer, p.binding, dest_pieces, layout, dest_state, access,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, [&] { copy(layout); });
                };
                const auto bands = std::uint32_t(p.staged_reads.size());
                if (p.staged) {
                    keep(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, [&](VkImageLayout layout) {
                        vk.copy_image_to_buffer(command_buffer, p.binding.image, layout, p.staged, bands,
                                                p.staged_reads.data());
                    });
                    detail::memory_barrier(vk, command_buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
                                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                           VK_PIPELINE_STAGE_TRANSFER_BIT);
                }
                const VkImageLayout source_attachment =
                    image_layout(p.source, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                const VkImageLayout dest_attachment =
                    image_layout(p.binding, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                detail::round_trip(vk, command_buffer, p.source, source_pieces, source_attachment, source_state,
                                   attachment_access, attachment_stages, [&] {
                    detail::round_trip(vk, command_buffer, p.binding, dest_pieces, dest_attachment, dest_state,
                                       attachment_access, attachment_stages, [&] {
                        if (p.resolve_framebuffer) {  // D3D-417 (prepare): the same resolve as a render pass
                            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                            begin.renderPass = p.resolve_pass;
                            begin.framebuffer = p.resolve_framebuffer;
                            begin.renderArea = {{p.resolve.srcOffset.x, p.resolve.srcOffset.y},
                                                {p.resolve.extent.width, p.resolve.extent.height}};
                            vk.begin_render_pass(command_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
                            vk.end_render_pass(command_buffer);
                            return;
                        }
                        VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                        info.imageView = p.view;
                        info.imageLayout = source_attachment;
                        info.resolveMode = p.resolve_mode;
                        info.resolveImageView = p.resolve_view;
                        info.resolveImageLayout = dest_attachment;
                        info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                        info.storeOp = VK_ATTACHMENT_STORE_OP_NONE;  // the source is only read
                        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                        rendering.renderArea = {{p.resolve.srcOffset.x, p.resolve.srcOffset.y},
                                                {p.resolve.extent.width, p.resolve.extent.height}};
                        rendering.layerCount = 1;
                        // One plane only, so independentResolve(None) never applies.
                        if (p.range.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT) rendering.pDepthAttachment = &info;
                        else rendering.pStencilAttachment = &info;
                        vk.begin_rendering(command_buffer, &rendering);
                        vk.end_rendering(command_buffer);
                    });
                });
                if (p.staged) {
                    keep(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, [&](VkImageLayout layout) {
                        vk.copy_buffer_to_image(command_buffer, p.staged, p.binding.image, layout, bands,
                                                p.staged_reads.data());
                    });
                }
                continue;
            }
            const VkImageLayout src_layout = detail::copy_layout(
                vk, command_buffer, p.source, src_id, detail::planes_for_layout(p.source, p.source_range, separate),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, tracker);
            const VkImageLayout dst_layout = detail::copy_layout(
                vk, command_buffer, p.binding, dst_id, detail::planes_for_layout(p.binding, p.range, separate),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, tracker);
            vk.resolve_image(command_buffer, p.source.image, src_layout, p.binding.image, dst_layout, 1, &p.resolve);
        } else if (const auto* marker = std::get_if<DebugMarker>(&command)) {
            if (!vk.insert_label || !vk.begin_label || !vk.end_label) continue;
            VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
            label.pLabelName = marker->label.c_str();
            if (marker->kind == DebugMarker::Kind::set) {
                vk.insert_label(command_buffer, &label);
            } else if (marker->kind == DebugMarker::Kind::begin) {
                vk.begin_label(command_buffer, &label);
                ++labels;
            } else if (labels) {
                vk.end_label(command_buffer);
                --labels;
            }
        } else if (std::holds_alternative<DiscardResource>(command)) {
            // oldLayout = UNDEFINED tells the driver the contents may go; the image stays in the layout the tracker
            // has for it, so later commands see no change. Without a tracker the layout is unknown, and a part
            // never used yet is still to be zero-filled: both are left alone. Without separate depth/stencil
            // layouts, discarding one plane would also discard the other, so that is left alone too.
            constexpr VkAccessFlags any_memory = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            for (const auto& range : p.discards) {
                if (!tracker || detail::planes_for_layout(p.binding, range, separate).aspectMask != range.aspectMask)
                    continue;
                LayoutPieces pieces;
                if (!tracker->pieces(id, range, pieces)) continue;
                for (const auto& piece : pieces)
                    if (piece.layout != VK_IMAGE_LAYOUT_UNDEFINED)
                        detail::image_barrier(vk, command_buffer, p.binding, piece.range, VK_IMAGE_LAYOUT_UNDEFINED,
                                              piece.layout, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                              any_memory, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            }
        } else if (const auto* write = std::get_if<WriteImmediate>(&command)) {
            // vkCmdUpdateBuffer runs in the transfer stage. MARKER_OUT waits for every earlier command to finish
            // and sees its writes. MARKER_IN only needs them started, which Vulkan cannot express (TOP_OF_PIPE in a
            // first scope is no stage at all), so it waits for them to finish too, without their writes. DEFAULT is
            // ordered like any copy, by the application's barriers.
            if (write->mode == write_immediate_mode::marker_out)
                detail::memory_barrier(vk, command_buffer, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            else if (write->mode == write_immediate_mode::marker_in)
                detail::memory_barrier(vk, command_buffer, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            // WriteBufferImmediate is unconditional on hardware D3D12 (measured on Intel and NVIDIA D3D12; not in
            // SetPredication's predicated list). Suspend conditional rendering around vkCmdUpdateBuffer because
            // Intel's Windows Vulkan driver suppresses it after PredicateReducer::capture's compute dispatch.
            if (predicate.buffer) vk.end_conditional_rendering(command_buffer);
            vk.update_buffer(command_buffer, p.write_buffer, p.write_offset, 4, &write->value);
            if (predicate.buffer) vk.begin_conditional_rendering(command_buffer, &predicate);
        } else if (std::holds_alternative<AtomicCopyBuffer>(command)) {
            // Every earlier write, to the dependent resources among others, finishes and is visible before the copy.
            // Vulkan promises no atomicity for a transfer; that a naturally aligned 4- or 8-byte copy is one write
            // is an assumption about the hardware (docs/d3d12_vulkan.md, section 4.2).
            detail::memory_barrier(vk, command_buffer, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT);
            vk.copy_buffer(command_buffer, p.source.buffer, p.binding.buffer, 1, p.buffer_copies.data());
        } else if (const auto* ind = std::get_if<ExecuteIndirect>(&command)) {
            const VkDeviceSize arguments = p.binding.buffer_offset + ind->argument_offset;
            if (ind->count_buffer && ind->kind != IndirectKind::dispatch) {
                const VkDeviceSize count = p.source.buffer_offset + ind->count_offset;
                if (ind->kind == IndirectKind::draw) {
                    vk.draw_indirect_count(command_buffer, p.binding.buffer, arguments, p.source.buffer,
                                           count, ind->max_count, ind->stride);
                } else if (ind->kind == IndirectKind::draw_indexed) {
                    vk.draw_indexed_indirect_count(command_buffer, p.binding.buffer, arguments,
                                                   p.source.buffer, count, ind->max_count, ind->stride);
                }
            } else {
                if (ind->kind == IndirectKind::draw) {
                    vk.draw_indirect(command_buffer, p.binding.buffer, arguments, ind->max_count, ind->stride);
                } else if (ind->kind == IndirectKind::draw_indexed) {
                    vk.draw_indexed_indirect(command_buffer, p.binding.buffer, arguments, ind->max_count, ind->stride);
                } else if (ind->kind == IndirectKind::dispatch) {
                    if (ind->count_buffer) {
                        // No D3D12 query must count this internal compute pass as an application dispatch.
                        end_statistics();
                        vk.emit_counted_dispatch(command_buffer, p.counted_dispatch);
                        begin_statistics();
                    }
                    if (ind->pipeline) {
                        ShaderUserData data{};
                        std::copy(ind->user_data.begin(), ind->user_data.end(), data.s);
                        data.heap_base = vk.heap_base;
                        data.code_base = p.pipeline.code_base;
                        rebase_user_data(vk, data);
                        vk.bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline.pipeline);
                        if (p.pipeline.bindless_set) {
                            const VkDescriptorSet set0 = vk.bindless_set ? vk.bindless_set : p.pipeline.bindless_set;
                            vk.bind_descriptor_sets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline.layout, 0,
                                                    1, &set0, 0, nullptr);
                        }
                        vk.push_constants(command_buffer, p.pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(data),
                                          &data);
                    }
                    for (std::uint32_t k = 0; k < ind->max_count; ++k) {
                        vk.dispatch_indirect(command_buffer,
                            ind->count_buffer ? p.counted_dispatch.indirect : p.binding.buffer,
                            (ind->count_buffer ? p.counted_dispatch.offset + VkDeviceSize(k) * 12
                                               : arguments + VkDeviceSize(k) * ind->stride));
                    }
                }
            }
        } else if (const auto* xdxr_build = std::get_if<BuildAccelerationStructure>(&command);
                   xdxr_build && vk.xdxr_record) {
            vk.xdxr_record(command_buffer, *xdxr_build);
        } else if (const auto* copy = std::get_if<CopyAccelerationStructure>(&command)) {
            if (vk.xdxr_copy) {
                vk.xdxr_copy(command_buffer, copy->src_va, copy->dst_va, p.xdxr_bytes);
            } else {
                VkCopyAccelerationStructureInfoKHR copy_info{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
                copy_info.src = p.accel_src;
                copy_info.dst = p.accel_dst;
                copy_info.mode = copy->mode == 1 ? VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR
                                                 : VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
                vk.copy_acceleration_structure(command_buffer, &copy_info);
            }
        } else if (const auto* postbuild = std::get_if<AccelerationStructurePostbuild>(&command);
                   postbuild && !p.skip) {
            if (vk.xdxr_postbuild) {
                for (std::size_t s = 0; s < postbuild->sources.size(); ++s)
                    vk.xdxr_postbuild(command_buffer, postbuild->sources[s], postbuild->dest_va + 8 * s);
            } else if (p.khr_postbuild < prepared.khr_postbuilds.size()) {
                const auto& pb = prepared.khr_postbuilds[p.khr_postbuild];
                const auto count = static_cast<std::uint32_t>(pb.sources.size());
                // Ensure an inline BuildRaytracingAccelerationStructure (or preceding copy) is visible before query read.
                VkMemoryBarrier as_mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                as_mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                as_mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                vk.pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &as_mb, 0, nullptr,
                                    0, nullptr);
                vk.reset_query_pool(command_buffer, pb.query_pool, pb.first_query, count);
                vk.write_acceleration_structures_properties(
                    command_buffer, count, pb.sources.data(),
                    VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, pb.query_pool, pb.first_query);
                vk.copy_query_pool_results(command_buffer, pb.query_pool, pb.first_query, count, pb.dst_buffer,
                                           pb.dst_offset, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                // Make query result writes visible to D3D12 UNORDERED_ACCESS shaders and subsequent CopyBufferRegion.
                VkMemoryBarrier dst_mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                dst_mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                dst_mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vk.pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT | shader_stages, 0, 1, &dst_mb, 0, nullptr, 0,
                                    nullptr);
            }
        } else if (const auto* build = std::get_if<BuildAccelerationStructure>(&command)) {
            // The build's addresses are console VAs into the GPU address arena (heap_base is its device address).
            // A null VA (no index buffer, no transform) stays null: heap_base + 0 would name the arena's zero page.
            const VkDeviceSize heap_base = vk.heap_base;
            const auto addr = [&](std::uint64_t va) -> VkDeviceAddress { return va ? heap_base + va : 0; };
            std::vector<VkAccelerationStructureGeometryKHR> geoms;
            std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
            if (build->is_tlas) {
                VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
                g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
                g.geometry.instances.arrayOfPointers = p.khr_instances ? VK_TRUE : VK_FALSE;
                g.geometry.instances.data.deviceAddress =
                    p.khr_instances ? p.khr_instances : addr(build->instances_va);
                geoms.push_back(g);
                ranges.push_back({build->instance_count, 0, 0, 0});
            } else {
                geoms.reserve(build->triangles.size());
                ranges.reserve(build->triangles.size());
                for (const auto& t : build->triangles) {
                    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                    g.flags = t.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0u;
                    auto& tr = g.geometry.triangles;
                    tr.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                    tr.vertexFormat = static_cast<VkFormat>(t.vertex_format);
                    tr.vertexData.deviceAddress = addr(t.vertex_va);
                    tr.vertexStride = t.vertex_stride;
                    tr.maxVertex = t.vertex_count ? (t.vertex_count - 1u) : 0u;
                    tr.indexType = (t.index_type == 1)
                                       ? VK_INDEX_TYPE_UINT16
                                       : ((t.index_type == 2) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR);
                    tr.indexData.deviceAddress = addr(t.index_va);
                    tr.transformData.deviceAddress = addr(t.transform_va);
                    geoms.push_back(g);
                    const std::uint32_t prim_count =
                        (t.index_type != 0 && t.index_count >= 3) ? (t.index_count / 3u) : (t.vertex_count / 3u);
                    ranges.push_back({prim_count, 0, 0, 0});
                }
            }
            VkAccelerationStructureBuildGeometryInfoKHR info{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            info.type = build->is_tlas ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                                       : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.flags = build->build_flags;
            info.mode = build->update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                      : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.srcAccelerationStructure = build->update ? p.accel_src : VK_NULL_HANDLE;
            info.dstAccelerationStructure = p.accel_dst;
            info.geometryCount = static_cast<std::uint32_t>(geoms.size());
            info.pGeometries = geoms.data();
            info.scratchData.deviceAddress = addr(build->scratch_va);
            const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = ranges.data();
            vk.build_acceleration_structures(command_buffer, 1, &info, &range_ptr);
        } else if (const auto* dispatch = std::get_if<Dispatch>(&command)) {
            ShaderUserData data{};
            std::copy(dispatch->user_data.begin(), dispatch->user_data.end(), data.s);
            data.heap_base = vk.heap_base;
            data.code_base = p.pipeline.code_base;
            rebase_user_data(vk, data);
            vk.bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline.pipeline);
            if (p.pipeline.bindless_set) {
                const VkDescriptorSet set0 = vk.bindless_set ? vk.bindless_set : p.pipeline.bindless_set;
                vk.bind_descriptor_sets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline.layout, 0, 1,
                                        &set0, 0, nullptr);
            }
            vk.push_constants(command_buffer, p.pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(data), &data);
            vk.dispatch(command_buffer, dispatch->x, dispatch->y, dispatch->z);
        } else if (const auto* draw = std::get_if<Draw>(&command)) {
            const detail::PreparedDraw& d = prepared.draws[p.draw];
            constexpr VkAccessFlags color_access =
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            constexpr VkAccessFlags depth_access =
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            constexpr VkPipelineStageFlags depth_stages =
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            std::array<VkRenderingAttachmentInfo, detail::max_render_targets> colors;
            colors.fill({VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO});
            for (std::size_t slot = 0; slot < d.colors.size(); ++slot) {
                const auto& a = d.colors[slot];
                if (!a.id) continue;  // an unbound slot: no image view, writes are dropped
                const VkImageLayout layout = image_layout(a.binding, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                detail::to_attachment(vk, command_buffer, a, layout, color_access,
                                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, tracker, separate);
                colors[slot].imageView = a.view;
                colors[slot].imageLayout = layout;
                colors[slot].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                colors[slot].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            if (d.depth.id) {
                const VkImageLayout layout = image_layout(d.depth.binding, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                detail::to_attachment(vk, command_buffer, d.depth, layout, depth_access, depth_stages, tracker,
                                      separate);
                depth.imageView = d.depth.view;
                depth.imageLayout = layout;
                depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea = {{0, 0}, d.area};
            rendering.layerCount = d.layers;  // array views: SV_RenderTargetArrayIndex picks the layer
            rendering.colorAttachmentCount = std::uint32_t(d.colors.size());
            rendering.pColorAttachments = colors.data();
            const GraphicsPipeline& graphics = *d.graphics;
            if (d.depth.id && (graphics.depth_aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) rendering.pDepthAttachment = &depth;
            if (d.depth.id && (graphics.depth_aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) rendering.pStencilAttachment = &depth;
            vk.begin_rendering(command_buffer, &rendering);

            ShaderUserData data{};
            std::copy(draw->user_data.begin(), draw->user_data.end(), data.s);
            data.heap_base = vk.heap_base;
            data.code_base = graphics.code_base;
            const unsigned rebased = rebase_user_data(vk, data, graphics.table_windows);
            // Diagnostic (docs/vertex_path.md): the geometry fix's driver block goes in the s0/s1 pair, the two dwords
            // the translated vertex shader reads as the driver block's heap-relative address (docs/root_arguments.md).
            if (d.driver_block) {
                data.s[0] = std::uint32_t(d.driver_block);
                data.s[1] = std::uint32_t(d.driver_block >> 32);
            }
            {
                static std::atomic<unsigned> emitted{0};
                if (emitted.fetch_add(1, std::memory_order_relaxed) < 8)
                    std::fprintf(stderr,
                                 "[rt] replay: emitting a draw: %u count(s), %u instance(s), %u title VA(s) rebased in "
                                 "its user data\n",
                                 draw->count, draw->instances, rebased);
            }
            vk.bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
            if (graphics.bindless_set) {
                const VkDescriptorSet set0 = vk.bindless_set ? vk.bindless_set : graphics.bindless_set;
                vk.bind_descriptor_sets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics.layout, 0, 1,
                                        &set0, 0, nullptr);
            }
            vk.push_constants(command_buffer, graphics.layout, graphics.push_stages, 0, sizeof(data), &data);
            // Translated vertex shaders write D3D clip coordinates (y up); a negative-height viewport keeps the
            // image upright, and winding as D3D12 sees it.
            const Viewport& v = draw->viewport;
            const VkViewport viewport{v.x, v.y + v.height, v.width, -v.height, v.min_depth, v.max_depth};
            vk.set_viewport(command_buffer, 0, 1, &viewport);
            const Rect& s = draw->scissor;
            const VkRect2D scissor{{std::max(0, s.left), std::max(0, s.top)},
                                   {std::uint32_t(std::max(0, s.right - std::max(0, s.left))),
                                    std::uint32_t(std::max(0, s.bottom - std::max(0, s.top)))}};
            vk.set_scissor(command_buffer, 0, 1, &scissor);
            // A mesh pipeline has no input assembly: its topology comes from the shader, and neither the topology
            // nor primitive restart is dynamic state there (VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-07065).
            if (!draw->mesh) {
                const VkPrimitiveTopology topology = detail::vulkan_topology(draw->topology);
                vk.set_primitive_topology(command_buffer, topology);
                const bool restart = draw->indexed && detail::strip_topology(topology) &&
                                     ((graphics.strip_cut == 1 && d.index_type == VK_INDEX_TYPE_UINT16) ||
                                      (graphics.strip_cut == 2 && d.index_type == VK_INDEX_TYPE_UINT32));
                vk.set_primitive_restart_enable(command_buffer, restart ? VK_TRUE : VK_FALSE);
            }
            vk.set_stencil_reference(command_buffer, VK_STENCIL_FACE_FRONT_BIT, draw->stencil_front);
            vk.set_stencil_reference(command_buffer, VK_STENCIL_FACE_BACK_BIT, draw->stencil_back);
            vk.set_blend_constants(command_buffer, draw->blend_factor.data());
            for (std::uint32_t slot = 0; slot < d.vertex_buffers.size(); ++slot) {
                if (!d.vertex_buffers[slot]) continue;
                vk.bind_vertex_buffers2(command_buffer, slot, 1, &d.vertex_buffers[slot], &d.vertex_offsets[slot],
                                        &d.vertex_sizes[slot], &d.vertex_strides[slot]);
            }
            if (draw->mesh) {
                vk.draw_mesh_tasks(command_buffer, draw->count, draw->instances, draw->first);
            } else if (draw->indexed) {
                vk.bind_index_buffer(command_buffer, d.index_buffer, d.index_offset, d.index_type);
                vk.draw_indexed(command_buffer, draw->count, draw->instances, draw->first, draw->base_vertex,
                                draw->first_instance);
            } else {
                vk.draw(command_buffer, draw->count, draw->instances, draw->first, draw->first_instance);
            }
            vk.end_rendering(command_buffer);
        }
    }
    // Predication does not carry over to the next command list.
    if (predicate.buffer) vk.end_conditional_rendering(command_buffer);
    if (!open_labels) close_labels(command_buffer, vk, own_labels);
}

// prepare + emit for one list. A failed replay leaves the command buffer and the tracker untouched.
// With `keep` and vk.create_segment_pool (and the sum helpers) set, overlapping occlusion or statistics queries get
// segment pools as in the batched path; the caller holds `keep` until the command buffer has completed.
template<typename Resolve>
ReplayResult replay(const CommandList& list, VkCommandBuffer command_buffer, Resolve&& resolve,
                    const ReplayFunctions& vk, LayoutTracker* tracker = nullptr,
                    std::vector<std::shared_ptr<const OcclusionSegmentPool>>* keep = nullptr) {
    if (tracker && tracker->separate_depth_stencil_layouts() != vk.separate_depth_stencil_layouts)
        return ReplayResult::unsupported;
    const bool pools = keep && vk.create_segment_pool && vk.sum_occlusion && vk.resolve_binary_occlusion &&
                       vk.sum_statistics && vk.sum_binary_occlusion;
    PreparedList prepared;
    if (const auto result = prepare(list, resolve, vk, prepared, pools); result != ReplayResult::success)
        return result;
    if (tracker && zero_clear_unavailable(prepared, vk, *tracker)) return ReplayResult::unsupported;
    std::shared_ptr<const OcclusionSegmentPool> physical, statistics;
    if (pools && prepared.segment_count &&
        !(physical = vk.create_segment_pool(VK_QUERY_TYPE_OCCLUSION, prepared.segment_count)))
        return ReplayResult::unsupported;
    if (pools && prepared.statistics_segment_count &&
        !(statistics = vk.create_segment_pool(VK_QUERY_TYPE_PIPELINE_STATISTICS, prepared.statistics_segment_count)))
        return ReplayResult::unsupported;
    QueryReplay queries;
    if (const auto result = queries.plan(prepared, physical, 0, pools, statistics, 0); result != ReplayResult::success)
        return result;
    emit(prepared, command_buffer, vk, tracker);
    queries.commit();  // the caller owns submission of the legacy command buffer
    if (keep) keep->insert(keep->end(), queries.retained.begin(), queries.retained.end());
    return ReplayResult::success;
}

} // namespace palmetto
