// Replay with vkCmdClearColorImage / vkCmdClearDepthStencilImage unset: a list that needs one of them, as a
// whole-view clear or as the implicit zero-clear of a never-used (UNDEFINED) image, is refused as unsupported
// instead of calling a null pointer (#634 finding by @bobcatchris). Also pins the strict multi-count ExecuteIndirect
// viewport-state readiness gate and its unchanged off-path record. No Vulkan loader, GPU, shaders or game data.
#include "../src/vulkan_replay.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
unsigned clears = 0, attachment_clears = 0;
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
    std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*,
    std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL clear_color(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*,
    std::uint32_t, const VkImageSubresourceRange*) { ++clears; }
VKAPI_ATTR void VKAPI_CALL clear_depth(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue*,
    std::uint32_t, const VkImageSubresourceRange*) { ++clears; }
VKAPI_ATTR void VKAPI_CALL begin(VkCommandBuffer, const VkRenderingInfo*) {}
VKAPI_ATTR void VKAPI_CALL end(VkCommandBuffer) {}
VKAPI_ATTR void VKAPI_CALL attachments(VkCommandBuffer, std::uint32_t, const VkClearAttachment*, std::uint32_t,
    const VkClearRect*) { ++attachment_clears; }

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

palmetto::ResourceBinding image(VkFormat format, VkImageAspectFlags aspects) {
    palmetto::ResourceBinding b{};
    b.image = reinterpret_cast<VkImage>(std::uintptr_t{1});
    b.format = format;
    b.aspects = aspects;
    b.width = b.height = 4;
    b.depth = b.mip_levels = b.array_layers = b.samples = 1;
    b.color_attachment = aspects == VK_IMAGE_ASPECT_COLOR_BIT;
    return b;
}

// Everything a clear can use, minus the two image clears; each case adds back what it needs.
palmetto::ReplayFunctions functions() {
    palmetto::ReplayFunctions vk{};
    vk.pipeline_barrier = barrier;
    vk.begin_rendering = begin;
    vk.end_rendering = end;
    vk.clear_attachments = attachments;
    vk.image_view = [](const auto&, const auto&) { return reinterpret_cast<VkImageView>(std::uintptr_t{1}); };
    return vk;
}

palmetto::ReplayResult run(const palmetto::CommandList& list, const palmetto::ResourceBinding& binding,
                          const palmetto::ReplayFunctions& vk, palmetto::LayoutTracker* tracker = nullptr) {
    clears = attachment_clears = 0;
    return palmetto::replay(list, VK_NULL_HANDLE, [&](auto) { return &binding; }, vk, tracker);
}
void indirect_viewport_readiness() {
    namespace r = palmetto::indirect_viewport_replay_readiness;
    using r::Disposition;

    const auto off = r::assess(2, 2, 2, false);
    expect(off.disposition == Disposition::legacy_unchanged && off.refusal == r::Refusal::none,
           "strict switch off leaves multi-count indirect viewport behavior unchanged");

    const auto refused = r::assess(2, 2, 2, true);
    expect(refused.disposition == Disposition::refuse &&
               std::strcmp(r::refusal_name(refused.refusal),
                           "indirect_multi_count_viewport_state_not_captured") == 0,
           "strict multi-count indirect draw with arrays has its exact named refusal");
    const auto scissor_refused = r::assess(3, 1, 2, true);
    expect(scissor_refused.disposition == Disposition::refuse &&
               scissor_refused.refusal == r::Refusal::multi_count_state_not_captured,
           "a multi-count indirect draw with multiple scissors is refused too");
    expect(r::assess(2, 1, 1, true).disposition == Disposition::ready,
           "multi-count indirect with scalar viewport/scissor state is outside this refusal");
    expect(r::assess(0, 2, 2, true).disposition == Disposition::ready,
           "zero-command ExecuteIndirect needs no captured graphics state");

    palmetto::ExecuteIndirect command{};
    command.kind = palmetto::IndirectKind::draw_indexed;
    command.stride = 24;
    command.max_count = 2;
    command.argument_buffer = 41;
    command.argument_offset = 32;
    command.count_buffer = 42;
    command.count_offset = 12;

    palmetto::CommandList legacy;
    expect(legacy.execute_indirect(command) && legacy.close(),
           "switch-off command-list recording accepts the existing indirect payload");
    const auto commands = legacy.commands();
    const auto* captured = commands.size() == 1 ? std::get_if<palmetto::ExecuteIndirect>(&commands.front()) : nullptr;
    expect(captured && captured->kind == command.kind && captured->stride == command.stride &&
               captured->max_count == command.max_count && captured->argument_buffer == command.argument_buffer &&
               captured->argument_offset == command.argument_offset && captured->count_buffer == command.count_buffer &&
               captured->count_offset == command.count_offset,
           "switch-off record preserves the complete multi-count indexed indirect tuple");

}
} // namespace

int main() {
    using palmetto::ReplayResult;
    indirect_viewport_readiness();
    const auto color = image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    const auto depth = image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    const palmetto::Rect rect{1, 1, 3, 3};

    // A whole-view render-target clear runs vkCmdClearColorImage.
    palmetto::CommandList whole_color;
    expect(whole_color.clear_render_target({1, 0, 0, 1}, {0.25f, 0.5f, 0.75f, 1}) && whole_color.close(),
           "record a whole-view colour clear");
    auto vk = functions();
    expect(run(whole_color, color, vk) == ReplayResult::unsupported, "whole-view colour clear without clear_color_image");
    vk.clear_color_image = clear_color;
    expect(run(whole_color, color, vk) == ReplayResult::success && clears == 1, "whole-view colour clear with it");

    // A whole-view depth clear runs vkCmdClearDepthStencilImage.
    palmetto::CommandList whole_depth;
    expect(whole_depth.clear_depth_stencil({1, 0, 0, 1}, palmetto::clear_depth, 1.0f, 0) && whole_depth.close(),
           "record a whole-view depth clear");
    vk = functions();
    expect(run(whole_depth, depth, vk) == ReplayResult::unsupported,
           "whole-view depth clear without clear_depth_stencil_image");
    vk.clear_depth_stencil_image = clear_depth;
    expect(run(whole_depth, depth, vk) == ReplayResult::success && clears == 1, "whole-view depth clear with it");

    // A rectangle clear goes through vkCmdClearAttachments, but on a never-used image the zero-clear that
    // initializes it first needs vkCmdClearColorImage (the case that crashed in detail::initialize).
    palmetto::CommandList rect_color;
    expect(rect_color.clear_render_target({1, 0, 0, 1}, {1, 0, 0, 1}, std::span<const palmetto::Rect>(&rect, 1)) &&
           rect_color.close(), "record a rectangle colour clear");
    vk = functions();
    expect(run(rect_color, color, vk) == ReplayResult::success && attachment_clears == 1,
           "rectangle clear without a tracker needs no image clear");
    palmetto::LayoutTracker tracker;
    tracker.track(1, color, VK_IMAGE_LAYOUT_UNDEFINED);
    expect(run(rect_color, color, vk, &tracker) == ReplayResult::unsupported,
           "rectangle clear on an UNDEFINED image without clear_color_image");
    expect(tracker.layout(1, 0) == VK_IMAGE_LAYOUT_UNDEFINED, "a refused list leaves the tracker untouched");
    vk.clear_color_image = clear_color;
    expect(run(rect_color, color, vk, &tracker) == ReplayResult::success && clears == 1 && attachment_clears == 1,
           "rectangle clear on an UNDEFINED image: zero-clear, then the rectangle");
    // Once initialized, the image no longer needs the zero-clear.
    vk.clear_color_image = nullptr;
    expect(run(rect_color, color, vk, &tracker) == ReplayResult::success && attachment_clears == 1,
           "rectangle clear on an initialized image needs no image clear");

    // not_zeroed images (D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) are only moved out of UNDEFINED, never cleared.
    auto not_zeroed = color;
    not_zeroed.not_zeroed = true;
    palmetto::LayoutTracker fresh;
    fresh.track(1, not_zeroed, VK_IMAGE_LAYOUT_UNDEFINED);
    expect(run(rect_color, not_zeroed, vk, &fresh) == ReplayResult::success && clears == 0,
           "not_zeroed image: no zero-clear needed");

    if (failures) return 1;
    std::printf("replay_clear_gate_test: clear gates and strict multi-count indirect viewport readiness passed\n");
    return 0;
}
