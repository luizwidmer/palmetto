#pragma once

// The execution side of the D3D12 layer, without COM: the per-device Vulkan context (resources, layout tracker,
// image views), queues that execute command lists, and timeline-semaphore fences. The COM objects in
// shims/d3d12_com.cpp are thin wrappers over these. Needs Vulkan 1.2 timeline semaphores; rectangle clears and
// enhanced barriers need the Vulkan 1.3 functions (dynamicRendering, synchronization2) to be enabled, as core
// features or through their KHR extensions on a 1.2 device (vulkan13_features.h).

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "binary_occlusion_spirv.h"
#include "buffer_va_index.h"
#include "console_va_map.h"
#include "deferred_compile.h"
#include "occlusion_sum_spirv.h"
#include "queue_holds.h"
#include "retire_tickets.h"
#include "statistics_sum_spirv.h"
#include "vulkan13_features.h"
#include "vulkan_depth24.h"
#include "vulkan_indirect_dispatch_count.h"
#include "vulkan_scratch.h"
#include "vulkan_replay.h"
#include "upscale_standin_spv.h"
#include "queue_families.h"
#include "resource_tiling.h"
#include "title_pull_windows.h"  // docs/title_memory_model.md: the same-submission title-page pull coalescer
#include "xdxr_build.h"
#include "../shims/xmem.h"  // docs/card_copies.md: the guard-page write tracker the mirror gate polls

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

// The host-memory probe above reads the same VirtualQuery result create_host_buffer does (docs/vertex_buffer_va.md).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace palmetto {

// Diagnostic (docs/vertex_buffer_va.md): what a VA the replay could not resolve actually is, for the vertex-buffer
// measurement. It answers the one question the refusal cannot: is the VA a page range the CPU can read (the title's own
// memory, which a copy into an arena stand-in can serve) or nothing at all? Prints the containing host allocation and,
// when the pages are committed and readable, how many bytes of the requested span are readable and how many non-zero
// bytes the first 64 KiB hold. No bytes are printed; only shapes. Off unless PALMETTO_REPLAY_MISS=2 (the measurement
// arm of the switch docs/dev-tools.md already documents).
inline void probe_host_range(std::uint64_t va, std::uint64_t bytes) {
    if (!va) return;
#if defined(_WIN32)
    constexpr std::uint64_t kScanCap = 4ull << 20;  // 4 MiB: enough to tell a vertex buffer from a stray pointer
    const std::uint64_t span = std::min(bytes ? bytes : 1, kScanCap);
    std::uint64_t readable = 0, committed = 0;
    std::uint64_t first_unreadable = 0;
    std::uint32_t page_size = 4096;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    if (system_info.dwPageSize) page_size = system_info.dwPageSize;
    for (std::uint64_t at = 0; at < span; at += page_size) {
        MEMORY_BASIC_INFORMATION page{};
        if (!VirtualQuery(reinterpret_cast<const void*>(va + at), &page, sizeof(page)) ||
            page.State != MEM_COMMIT) {
            if (!first_unreadable) first_unreadable = at;
            continue;
        }
        ++committed;
        const DWORD readable_protect = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (page.Protect & (PAGE_NOACCESS | PAGE_GUARD) || !(page.Protect & readable_protect)) {
            if (!first_unreadable) first_unreadable = at;
            continue;
        }
        ++readable;
    }
    MEMORY_BASIC_INFORMATION info{};
    const bool queried = VirtualQuery(reinterpret_cast<const void*>(va), &info, sizeof(info)) == sizeof(info);
    const std::uint64_t base = queried ? reinterpret_cast<std::uint64_t>(info.AllocationBase) : 0;
    const std::uint64_t alloc_offset = base ? va - base : 0;
    const std::uint64_t nonzero = readable ? std::count_if(
                                                  reinterpret_cast<const std::uint8_t*>(va),
                                                  reinterpret_cast<const std::uint8_t*>(va) +
                                                      std::min<std::uint64_t>(span, 0x10000),
                                                  [](std::uint8_t b) { return b != 0; })
                                          : 0;
    std::fprintf(stderr,
                 "[rt] replay: miss probe: VA 0x%llx size 0x%llx is host state %s protect 0x%x allocation base 0x%llx "
                 "offset 0x%llx region 0x%llx; of the first 0x%llx bytes %llu page(s) committed/%llu readable, first "
                 "unreadable at 0x%llx, %llu non-zero byte(s) in the first 64 KiB\n",
                 static_cast<unsigned long long>(va), static_cast<unsigned long long>(bytes),
                 queried ? (info.State == MEM_COMMIT ? "commit" : (info.State == MEM_RESERVE ? "reserve" : "free"))
                         : "unqueried",
                 queried ? static_cast<unsigned>(info.Protect) : 0u, static_cast<unsigned long long>(base),
                 static_cast<unsigned long long>(alloc_offset),
                 queried ? static_cast<unsigned long long>(info.RegionSize) : 0ull,
                 static_cast<unsigned long long>(span), static_cast<unsigned long long>(committed),
                 static_cast<unsigned long long>(readable), static_cast<unsigned long long>(first_unreadable),
                 static_cast<unsigned long long>(nonzero));
#else
    (void)va;
    (void)bytes;
#endif
}

// Diagnostic: where the miss VA sits relative to the console-VA ranges the layer already aliased, so "no mapping" is
// printed together with "nearest range below/above", which says whether the VA is a range the layer has not seen or a
// small offset outside one.
inline void probe_console_ranges(const std::vector<ConsoleVaRange>& ranges, std::uint64_t va) {
    const ConsoleVaRange* below = nullptr;
    const ConsoleVaRange* above = nullptr;
    bool holds = false;
    for (const auto& range : ranges) {
        if (range.holds(va)) holds = true;
        if (range.title <= va && (!below || range.title > below->title)) below = &range;
        if (range.title > va && (!above || range.title < above->title)) above = &range;
    }
    std::fprintf(stderr, "[rt] replay: miss probe: %zu title range(s) aliased, none holds the VA: %d", ranges.size(),
                 int(holds));
    if (below)
        std::fprintf(stderr, "; nearest below: 0x%llx + 0x%llx (offset 0x%llx, %s)",
                     static_cast<unsigned long long>(below->title), static_cast<unsigned long long>(below->size),
                     static_cast<unsigned long long>(va - below->title), console_va_kind_name(below->kind));
    if (above)
        std::fprintf(stderr, "; nearest above: 0x%llx + 0x%llx (%s)",
                     static_cast<unsigned long long>(above->title), static_cast<unsigned long long>(above->size),
                     console_va_kind_name(above->kind));
    std::fprintf(stderr, "\n");
}

// A CPU-written Vulkan-address instance pointer array, kept until the submission that builds with it completes.
// Completed stages are pooled per queue and reused, so a per-frame TLAS build does not allocate memory each frame.
struct KhrInstanceStage {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    std::uint64_t size = 0;  // bytes available in buffer, so a larger later list can be refused for a new stage
};

// One per-queue query pool of VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, retained on the
// CommandSlot until its submission completes and then recycled to the queue pool.
struct KhrAsQueryPool {
    VkQueryPool pool = VK_NULL_HANDLE;
    std::uint32_t capacity = 0;
    std::uint32_t used = 0;
};

// VK_EXT_debug_utils labels: an instance extension, so a device has them only when its instance enabled it.
struct DebugLabelFunctions {
    PFN_vkCmdInsertDebugUtilsLabelEXT cmd_insert = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT cmd_begin = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT cmd_end = nullptr;
    PFN_vkQueueInsertDebugUtilsLabelEXT queue_insert = nullptr;
    PFN_vkQueueBeginDebugUtilsLabelEXT queue_begin = nullptr;
    PFN_vkQueueEndDebugUtilsLabelEXT queue_end = nullptr;

    bool complete() const { return cmd_insert && cmd_begin && cmd_end && queue_insert && queue_begin && queue_end; }
};

struct VulkanDeviceFunctions {
#define PALMETTO_VK_DEVICE_FUNCTIONS(X)                                                                         \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer)         \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateSemaphore)     \
    X(vkDestroySemaphore) X(vkGetSemaphoreCounterValue) X(vkWaitSemaphores) X(vkSignalSemaphore)               \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCmdPipelineBarrier) X(vkCmdPipelineBarrier2)                \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdBeginRendering) X(vkCmdEndRendering)         \
    X(vkCmdClearAttachments) X(vkDeviceWaitIdle) X(vkCreateImage) X(vkDestroyImage)                         \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer)                  \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory)  \
    X(vkUnmapMemory) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage)  \
    X(vkCmdResolveImage) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCreateSampler) X(vkDestroySampler)        \
    X(vkCmdBeginConditionalRenderingEXT) X(vkCmdEndConditionalRenderingEXT) X(vkCreateQueryPool)               \
    X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdWriteTimestamp)    \
    X(vkCmdCopyQueryPoolResults) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDrawIndirectCount)    \
    X(vkCmdDrawIndexedIndirectCount) X(vkCmdDispatchIndirect) X(vkGetBufferDeviceAddress)                      \
    X(vkGetAccelerationStructureBuildSizesKHR) X(vkCreateAccelerationStructureKHR)                             \
    X(vkDestroyAccelerationStructureKHR) X(vkGetAccelerationStructureDeviceAddressKHR)                         \
    X(vkCmdBuildAccelerationStructuresKHR) X(vkCmdCopyAccelerationStructureKHR)                                \
    X(vkCmdWriteAccelerationStructuresPropertiesKHR)                                                           \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)                   \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)                          \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateShaderModule) X(vkDestroyShaderModule)      \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets)            \
    X(vkCmdPushConstants) X(vkCmdDispatch)                                                                     \
    X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdSetViewport) X(vkCmdSetScissor)                  \
    X(vkCmdSetViewportWithCount) X(vkCmdSetScissorWithCount)                                                    \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetStencilReference)                          \
    X(vkCmdSetBlendConstants) X(vkCmdDraw)                                                                              \
    X(vkCmdDrawIndexed) X(vkCreateGraphicsPipelines) X(vkQueueBindSparse) X(vkGetImageSparseMemoryRequirements)   \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData)                                      \
    X(vkCreateEvent) X(vkDestroyEvent) X(vkResetEvent) X(vkCmdSetEvent) X(vkCmdWaitEvents) X(vkCmdDrawMeshTasksEXT) \
    X(vkCreateRenderPass2) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)               \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)
#define PALMETTO_VK_MEMBER(name) PFN_##name name = nullptr;
    PALMETTO_VK_DEVICE_FUNCTIONS(PALMETTO_VK_MEMBER)
#undef PALMETTO_VK_MEMBER

    // The Vulkan 1.3 entry points may be null on older devices, the extension ones (conditional rendering, mesh
    // shaders) when the extension is not enabled, and the query functions are optional (a list that uses queries
    // without them is unsupported). The compute and descriptor functions are needed for normalized or summed
    // occlusion results.
    bool load(PFN_vkGetDeviceProcAddr get, VkDevice device) {
#define PALMETTO_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(palmetto::device_proc_or_khr(get, device, #name));
        PALMETTO_VK_DEVICE_FUNCTIONS(PALMETTO_VK_LOAD)
#undef PALMETTO_VK_LOAD
        return vkCreateCommandPool && vkDestroyCommandPool && vkAllocateCommandBuffers && vkResetCommandBuffer &&
               vkBeginCommandBuffer && vkEndCommandBuffer && vkQueueSubmit && vkQueueWaitIdle && vkCreateSemaphore &&
               vkDestroySemaphore && vkGetSemaphoreCounterValue && vkWaitSemaphores && vkSignalSemaphore &&
               vkCreateImageView && vkDestroyImageView && vkCmdPipelineBarrier && vkCmdClearColorImage &&
               vkCmdClearDepthStencilImage && vkCmdClearAttachments && vkDeviceWaitIdle && vkCreateImage &&
               vkDestroyImage && vkGetImageMemoryRequirements && vkBindImageMemory && vkCreateBuffer &&
               vkDestroyBuffer && vkGetBufferMemoryRequirements && vkBindBufferMemory && vkAllocateMemory &&
               vkFreeMemory && vkMapMemory && vkUnmapMemory && vkCmdCopyBuffer && vkCmdCopyBufferToImage &&
               vkCmdCopyImageToBuffer && vkCmdCopyImage && vkCmdResolveImage && vkCmdFillBuffer &&
               vkCmdUpdateBuffer && vkCmdBindPipeline && vkCmdPushConstants && vkCmdDispatch &&
               vkCreateShaderModule && vkDestroyShaderModule && vkCreatePipelineLayout &&
               vkDestroyPipelineLayout && vkCreateComputePipelines && vkDestroyPipeline &&
               vkGetBufferDeviceAddress && vkCmdBindIndexBuffer && vkCmdSetViewport && vkCmdSetScissor &&
               vkCmdSetStencilReference && vkCmdSetBlendConstants && vkCmdDraw && vkCmdDrawIndexed &&
               vkCreateGraphicsPipelines;
    }

    // Graphics pipelines also need these Vulkan 1.3 entry points (dynamic rendering, extended dynamic state).
    bool graphics_complete() const {
        return vkCmdBeginRendering && vkCmdEndRendering && vkCmdBindVertexBuffers2 && vkCmdSetPrimitiveTopology &&
               vkCmdSetPrimitiveRestartEnable;
    }
};

// Both kinds of compute resolve use the same storage buffer, descriptor set and pipeline layout.
// The destination needs transfer usage only. Pipelines are created on first use.
class OcclusionResolve {
public:
    OcclusionResolve(const VulkanDeviceFunctions& vk, VkDevice device) : vk_(vk), device_(device) {}
    OcclusionResolve(const OcclusionResolve&) = delete;
    OcclusionResolve& operator=(const OcclusionResolve&) = delete;

    ~OcclusionResolve() {
        if (sum_pipeline_) vk_.vkDestroyPipeline(device_, sum_pipeline_, nullptr);
        if (binary_pipeline_) vk_.vkDestroyPipeline(device_, binary_pipeline_, nullptr);
        if (statistics_pipeline_) vk_.vkDestroyPipeline(device_, statistics_pipeline_, nullptr);
        if (layout_) vk_.vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (pool_) vk_.vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (set_layout_) vk_.vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        if (scratch_) vk_.vkDestroyBuffer(device_, scratch_, nullptr);
        if (memory_) vk_.vkFreeMemory(device_, memory_, nullptr);
    }

    bool initialize(const VkPhysicalDeviceMemoryProperties& memory) {
        if (!vk_.vkCreateDescriptorSetLayout || !vk_.vkDestroyDescriptorSetLayout ||
            !vk_.vkCreateDescriptorPool || !vk_.vkDestroyDescriptorPool || !vk_.vkAllocateDescriptorSets ||
            !vk_.vkUpdateDescriptorSets || !vk_.vkCreatePipelineLayout || !vk_.vkDestroyPipelineLayout ||
            !vk_.vkCreateShaderModule || !vk_.vkDestroyShaderModule || !vk_.vkCreateComputePipelines ||
            !vk_.vkDestroyPipeline || !vk_.vkCmdBindPipeline || !vk_.vkCmdBindDescriptorSets ||
            !vk_.vkCmdPushConstants || !vk_.vkCmdDispatch)
            return false;

        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = scratch_size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (vk_.vkCreateBuffer(device_, &buffer, nullptr, &scratch_) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vk_.vkGetBufferMemoryRequirements(device_, scratch_, &requirements);
        auto type = memory.memoryTypeCount;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if (!(requirements.memoryTypeBits & (1u << i))) continue;
            if (type == memory.memoryTypeCount) type = i;
            if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                type = i;
                break;
            }
        }
        if (type == memory.memoryTypeCount) return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (vk_.vkAllocateMemory(device_, &allocation, nullptr, &memory_) != VK_SUCCESS ||
            vk_.vkBindBufferMemory(device_, scratch_, memory_, 0) != VK_SUCCESS)
            return false;

        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_layout.bindingCount = 1;
        set_layout.pBindings = &binding;
        if (vk_.vkCreateDescriptorSetLayout(device_, &set_layout, nullptr, &set_layout_) != VK_SUCCESS) return false;
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        if (vk_.vkCreateDescriptorPool(device_, &pool, nullptr, &pool_) != VK_SUCCESS) return false;
        VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set.descriptorPool = pool_;
        set.descriptorSetCount = 1;
        set.pSetLayouts = &set_layout_;
        if (vk_.vkAllocateDescriptorSets(device_, &set, &set_) != VK_SUCCESS) return false;
        const VkDescriptorBufferInfo storage{scratch_, 0, scratch_size};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set_;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &storage;
        vk_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

        // The count, and for statistics_sum.comp whether the segments are mesh-pool results.
        const VkPushConstantRange count{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(std::uint32_t)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &set_layout_;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &count;
        return vk_.vkCreatePipelineLayout(device_, &layout, nullptr, &layout_) == VK_SUCCESS;
    }

    bool prepare(bool needs_sum, bool needs_binary, bool needs_statistics_sum = false,
                 bool needs_sum_binary = false) {
        // The composite (sum then normalize, D3D-252) uses both pipelines.
        needs_sum = needs_sum || needs_sum_binary;
        needs_binary = needs_binary || needs_sum_binary;
        return (!needs_sum || sum_pipeline_ || create_pipeline(occlusion_sum_spirv, sum_pipeline_)) &&
               (!needs_binary || binary_pipeline_ || create_pipeline(binary_occlusion_spirv, binary_pipeline_)) &&
               (!needs_statistics_sum || statistics_pipeline_ ||
                create_pipeline(statistics_sum_spirv, statistics_pipeline_));
    }

    // Adds `count` pipeline-statistics results into one D3D12_QUERY_DATA_PIPELINE_STATISTICS at dst + offset: the
    // segments of a statistics query that was split around helper dispatches (D3D-162) or mesh draws (D3D-291).
    // Segment i is in `mesh_queries` when mesh[i] is set (four counters: clipping, fragment and compute; see
    // statistics_sum.comp), otherwise in `queries` (all eleven).
    void sum_statistics(VkCommandBuffer cb, VkQueryPool queries, VkQueryPool mesh_queries, const std::uint8_t* mesh,
                        std::uint32_t first, std::uint32_t count, VkBuffer dst, VkDeviceSize offset) const {
        constexpr VkDeviceSize result = statistics_counters * word_size;
        constexpr VkDeviceSize mesh_slot = 5 * word_size;  // CInvocations, where the mesh pool's first counter goes
        begin_scratch_write(cb);
        vk_.vkCmdFillBuffer(cb, scratch_, 0, result, 0);
        bind(cb, statistics_pipeline_);
        while (count) {
            const bool in_mesh = mesh && mesh[first];
            std::uint32_t chunk = 1;
            while (chunk < std::min(count, statistics_capacity) && bool(mesh && mesh[first + chunk]) == in_mesh)
                ++chunk;
            vk_.vkCmdCopyQueryPoolResults(cb, in_mesh ? mesh_queries : queries, first, chunk, scratch_,
                                          result + (in_mesh ? mesh_slot : 0), result,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            barrier(cb, transfer | compute, compute, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    shader_access);
            dispatch(cb, chunk, 1, in_mesh);
            first += chunk;
            count -= chunk;
            if (count) barrier(cb, compute, transfer, shader_access, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        copy_result(cb, dst, offset, result);
    }

    void sum(VkCommandBuffer cb, VkQueryPool queries, std::uint32_t first, std::uint32_t count,
             VkBuffer dst, VkDeviceSize offset) const {
        begin_scratch_write(cb);
        vk_.vkCmdFillBuffer(cb, scratch_, 0, word_size, 0);
        bind(cb, sum_pipeline_);
        while (count) {
            const auto chunk = std::min(count, sum_capacity);
            vk_.vkCmdCopyQueryPoolResults(cb, queries, first, chunk, scratch_, word_size, word_size,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            barrier(cb, transfer | compute, compute, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    shader_access);
            dispatch(cb, chunk, 1);
            first += chunk;
            count -= chunk;
            if (count) barrier(cb, compute, transfer, shader_access, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        copy_result(cb, dst, offset, word_size);
    }

    // Sums `count` binary-occlusion segments into one value and normalizes that total in place to 0 or 1: the
    // result of a binary query whose interval spans more than one physical segment (overlapping queries, D3D-252).
    void sum_binary(VkCommandBuffer cb, VkQueryPool queries, std::uint32_t first, std::uint32_t count,
                    VkBuffer dst, VkDeviceSize offset) const {
        begin_scratch_write(cb);
        vk_.vkCmdFillBuffer(cb, scratch_, 0, word_size, 0);
        bind(cb, sum_pipeline_);
        auto tail = first, left = count;
        while (left) {
            const auto chunk = std::min(left, sum_capacity);
            vk_.vkCmdCopyQueryPoolResults(cb, queries, tail, chunk, scratch_, word_size, word_size,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            barrier(cb, transfer | compute, compute, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    shader_access);
            dispatch(cb, chunk, 1);
            tail += chunk;
            left -= chunk;
            if (left) barrier(cb, compute, transfer, shader_access, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        barrier(cb, compute, compute, VK_ACCESS_SHADER_WRITE_BIT, shader_access);
        bind(cb, binary_pipeline_);
        dispatch(cb, 1, 1);
        copy_result(cb, dst, offset, word_size);
    }

    void normalize(VkCommandBuffer cb, VkQueryPool queries, std::uint32_t first, std::uint32_t count,
                   VkBuffer dst, VkDeviceSize offset) const {
        bind(cb, binary_pipeline_);
        while (count) {
            const auto chunk = std::min(count, binary_capacity);
            const VkDeviceSize bytes = VkDeviceSize(chunk) * word_size;
            begin_scratch_write(cb);
            vk_.vkCmdCopyQueryPoolResults(cb, queries, first, chunk, scratch_, 0, word_size,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            barrier(cb, transfer, compute, VK_ACCESS_TRANSFER_WRITE_BIT, shader_access);
            dispatch(cb, chunk, (chunk + binary_group_size - 1) / binary_group_size);
            copy_result(cb, dst, offset, bytes);
            first += chunk;
            count -= chunk;
            offset += bytes;
        }
    }

private:
    bool create_pipeline(std::span<const std::uint32_t> code, VkPipeline& out) const {
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = code.size_bytes();
        module.pCode = code.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vk_.vkCreateShaderModule(device_, &module, nullptr, &shader) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                          VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
        pipeline.layout = layout_;
        VkPipeline handle = VK_NULL_HANDLE;
        const VkResult result = vk_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &handle);
        vk_.vkDestroyShaderModule(device_, shader, nullptr);
        if (result == VK_SUCCESS) out = handle;
        else if (handle) vk_.vkDestroyPipeline(device_, handle, nullptr);
        return result == VK_SUCCESS;
    }

    void bind(VkCommandBuffer cb, VkPipeline pipeline) const {
        vk_.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vk_.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
    }

    void dispatch(VkCommandBuffer cb, std::uint32_t chunk, std::uint32_t groups, bool mesh = false) const {
        const std::uint32_t range[2] = {chunk, mesh ? 1u : 0u};
        vk_.vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(range), range);
        vk_.vkCmdDispatch(cb, groups, 1, 1);
    }

    void begin_scratch_write(VkCommandBuffer cb) const {
        // A sum or a normalization in an earlier submission may still use this buffer.
        barrier(cb, transfer | compute, transfer,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | shader_access,
                VK_ACCESS_TRANSFER_WRITE_BIT);
    }

    void copy_result(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize bytes) const {
        barrier(cb, compute, transfer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy copy{0, offset, bytes};
        vk_.vkCmdCopyBuffer(cb, scratch_, dst, 1, &copy);
    }

    void barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                 VkAccessFlags before, VkAccessFlags after) const {
        VkBufferMemoryBarrier memory{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        memory.srcAccessMask = before;
        memory.dstAccessMask = after;
        memory.srcQueueFamilyIndex = memory.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memory.buffer = scratch_;
        memory.size = scratch_size;
        vk_.vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 1, &memory, 0, nullptr);
    }

    static constexpr auto transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    static constexpr auto compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    static constexpr VkAccessFlags shader_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    static constexpr std::uint32_t sum_capacity = 256;  // one more word holds the running sum
    static constexpr std::uint32_t binary_capacity = 1024;
    static constexpr std::uint32_t binary_group_size = 64;  // local_size_x in binary_occlusion.comp
    static constexpr std::uint32_t statistics_counters = 11;  // local_size_x in statistics_sum.comp
    static constexpr std::uint32_t statistics_capacity = 64;  // segments per pass; 11 more words hold the sums
    static constexpr VkDeviceSize word_size = sizeof(std::uint64_t);
    static constexpr VkDeviceSize scratch_size =
        std::max({sum_capacity + 1, binary_capacity, (statistics_capacity + 1) * statistics_counters}) * word_size;
    const VulkanDeviceFunctions& vk_;
    VkDevice device_;
    VkBuffer scratch_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline sum_pipeline_ = VK_NULL_HANDLE;
    VkPipeline binary_pipeline_ = VK_NULL_HANDLE;
    VkPipeline statistics_pipeline_ = VK_NULL_HANDLE;
};

struct SparseResource;

// Zero-allocation O(1) Ring Buffer deferred deletion queue tied to the device timeline semaphore.
class DeferredDeletionQueue {
public:
    enum class Kind : std::uint8_t {
        bindless_slot,
        image_view,
        image,
        sparse_resource,
        buffer,
        device_memory,
        // An image's memory is a range of a pooled block (image_memory_pool.h): the handle is a heap-allocated
        // std::function that drops the range, which hands its bytes back to the block. It runs when the submissions
        // that used the image have retired, like the image's own destruction above it in the queue.
        pooled_image_memory,
        acceleration_structure,
        framebuffer,
        // A VkPipeline the device's live-pipeline budget evicted (pipeline_budget.h, d3d12_vulkan_device.cpp): the
        // pipeline state keeps its recipe and rebuilds on the next bind, so the old object only has to outlive the
        // submissions already given to the GPU.
        pipeline,
    };

    struct Item {
        std::uint64_t ticket = 0;
        std::uint64_t handle = 0;  // for bindless_slot: (uint64_t(dimension) << 32) | index
        Kind kind = Kind::image_view;
    };

    using BindlessReleaseFn = void (*)(void* user, std::uint32_t dimension, std::uint32_t index);

    explicit DeferredDeletionQueue(std::size_t initial_capacity = 256) : pending_(next_pow2(initial_capacity)) {}

    void set_bindless_release(BindlessReleaseFn fn, void* user) {
        std::lock_guard lock(mutex_);
        bindless_release_fn_ = fn;
        bindless_release_user_ = user;
    }

    void push(std::uint64_t retire_ticket, Kind kind, std::uint64_t handle) {
        if (!handle && kind != Kind::bindless_slot) return;
        std::lock_guard lock(mutex_);
        if (count_ == pending_.size()) {
            const std::size_t old_cap = pending_.size();
            std::vector<Item> grown(old_cap * 2);
            const std::size_t first = old_cap - head_;
            std::memcpy(grown.data(), pending_.data() + head_, first * sizeof(Item));
            if (head_ > 0) std::memcpy(grown.data() + first, pending_.data(), head_ * sizeof(Item));
            pending_ = std::move(grown);
            head_ = 0;
            tail_ = count_;
        }
        pending_[tail_] = {retire_ticket, handle, kind};
        tail_ = (tail_ + 1) & (pending_.size() - 1);
        ++count_;
    }

    // Drains all items with ticket <= current_ticket in O(k), invoking vkDestroy* outside the lock.
    // Uses a stack-local batch vector so concurrent drain() calls from multiple queues never race.
    void drain(std::uint64_t current_ticket, VkDevice device, const VulkanDeviceFunctions& vk) {
        BindlessReleaseFn release_fn = nullptr;
        void* release_user = nullptr;
        std::vector<Item> to_destroy;
        {
            std::lock_guard lock(mutex_);
            if (count_ == 0 || pending_[head_].ticket > current_ticket) return;
            release_fn = bindless_release_fn_;
            release_user = bindless_release_user_;
            const std::size_t mask = pending_.size() - 1;
            while (count_ > 0 && pending_[head_].ticket <= current_ticket) {
                to_destroy.push_back(pending_[head_]);
                head_ = (head_ + 1) & mask;
                --count_;
            }
            if (count_ == 0) {
                head_ = 0;
                tail_ = 0;
            }
        }

        for (const Item& item : to_destroy) {
            switch (item.kind) {
            case Kind::bindless_slot:
                if (release_fn) {
                    release_fn(release_user, static_cast<std::uint32_t>(item.handle >> 32),
                               static_cast<std::uint32_t>(item.handle & 0xFFFFFFFFu));
                }
                break;
            case Kind::image_view:
                vk.vkDestroyImageView(device, reinterpret_cast<VkImageView>(item.handle), nullptr);
                break;
            case Kind::image:
                vk.vkDestroyImage(device, reinterpret_cast<VkImage>(item.handle), nullptr);
                break;
            case Kind::sparse_resource:
                // Releases the resource and its page backing together after outstanding submissions.
                delete reinterpret_cast<std::shared_ptr<SparseResource>*>(item.handle);
                break;
            case Kind::buffer:
                vk.vkDestroyBuffer(device, reinterpret_cast<VkBuffer>(item.handle), nullptr);
                break;
            case Kind::device_memory:
                vk.vkFreeMemory(device, reinterpret_cast<VkDeviceMemory>(item.handle), nullptr);
                break;
            case Kind::pooled_image_memory:
                delete reinterpret_cast<std::function<void()>*>(item.handle);
                break;
            case Kind::acceleration_structure:
                if (vk.vkDestroyAccelerationStructureKHR)
                    vk.vkDestroyAccelerationStructureKHR(
                        device, reinterpret_cast<VkAccelerationStructureKHR>(item.handle), nullptr);
                break;
            case Kind::framebuffer:
                vk.vkDestroyFramebuffer(device, reinterpret_cast<VkFramebuffer>(item.handle), nullptr);
                break;
            case Kind::pipeline:
                vk.vkDestroyPipeline(device, reinterpret_cast<VkPipeline>(item.handle), nullptr);
                break;
            }
        }
    }

private:
    static std::size_t next_pow2(std::size_t n) {
        std::size_t p = 16;
        while (p < n) p <<= 1;
        return p;
    }

    std::mutex mutex_;
    std::vector<Item> pending_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
    BindlessReleaseFn bindless_release_fn_ = nullptr;
    void* bindless_release_user_ = nullptr;
};

// Centralized single-thread fence dispatcher using Vulkan 1.2 vkWaitSemaphores(VK_SEMAPHORE_WAIT_ANY_BIT).
// Eliminates per-fence OS threads and 10ms polling: a synthetic timeline semaphore (wakeup_sem_) at index 0
// wakes vkWaitSemaphores immediately whenever a new wait, a released fence, or shutdown arrives while waiting_ == true.
class FenceWaitDispatcher {
public:
    struct Entry {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        std::uint64_t value = 0;
        void* event_handle = nullptr;
        std::function<void()> callback;
    };

    // A dispatch runs outside mutex_. Each entry in a ready batch gets its own slot: a callback may re-entrantly
    // call cancel() for a later entry in the same batch, which marks that entry cancelled so it is skipped before
    // it can use the semaphore. A callback running on another thread must still be waited for.
    // A cancelled entry still in pending_ is dispatched by cancel() itself (callback and bookkeeping both run);
    // one already in a ready batch is skipped and only its event is signalled. The held Signal op's count is
    // therefore not decremented for a skipped batch entry. That is safe because only ~Fence cancels a wait, but
    // keep it in mind if cancel() gains other callers.
    struct DispatchSlot {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        std::thread::id owner;
        bool active = false;     // guarded by mutex_
        bool cancelled = false;  // guarded by mutex_
    };

    using SignalEventFn = void (*)(void* event_handle);

    // Longest single vkWaitSemaphores call in run(): bounds how long the thread can sit in an uninterruptible wait.
    static constexpr std::uint64_t kWaitSliceNs = 100'000'000;  // 100 ms

    FenceWaitDispatcher(VkDevice device, const VulkanDeviceFunctions& vk, std::atomic<bool>& device_lost)
        : device_(device), vk_(vk), device_lost_(device_lost) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        if (vk_.vkCreateSemaphore(device_, &info, nullptr, &wakeup_sem_) != VK_SUCCESS)
            wakeup_sem_ = VK_NULL_HANDLE;
        semaphores_.reserve(64);
        values_.reserve(64);
    }

    ~FenceWaitDispatcher() {
        stop();
        if (wakeup_sem_) vk_.vkDestroySemaphore(device_, wakeup_sem_, nullptr);
    }

    FenceWaitDispatcher(const FenceWaitDispatcher&) = delete;
    FenceWaitDispatcher& operator=(const FenceWaitDispatcher&) = delete;

    void set_signal_event_fn(SignalEventFn fn) {
        std::lock_guard lock(mutex_);
        signal_event_ = fn;
    }

    void enqueue(VkSemaphore sem, std::uint64_t value, void* event_handle) {
        if (!sem || !event_handle) return;
        SignalEventFn immediate_fn = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (stop_ || device_lost_.load(std::memory_order_acquire)) {
                immediate_fn = signal_event_;
            } else {
                pending_.push_back({sem, value, event_handle, {}});
                if (!thread_.joinable() && wakeup_sem_) thread_ = std::thread([this] { run(); });
                signal_wakeup_locked();
            }
        }
        if (immediate_fn) {
            immediate_fn(event_handle);
            return;
        }
        cv_.notify_one();
    }

    void enqueue_callback(VkSemaphore sem, std::uint64_t value, std::function<void()> cb) {
        if (!sem || !cb) return;
        std::function<void()> immediate_cb;
        {
            std::lock_guard lock(mutex_);
            if (stop_ || device_lost_.load(std::memory_order_acquire)) {
                immediate_cb = std::move(cb);
            } else {
                pending_.push_back({sem, value, nullptr, std::move(cb)});
                if (!thread_.joinable() && wakeup_sem_) thread_ = std::thread([this] { run(); });
                signal_wakeup_locked();
            }
        }
        if (immediate_cb) {
            immediate_cb();
            return;
        }
        cv_.notify_one();
    }

    // Wakes run() when another path marks the device lost so pending waits complete and stop()/cancel() never hang.
    void notify_device_lost() {
        {
            std::lock_guard lock(mutex_);
            signal_wakeup_locked();
        }
        cv_.notify_all();
    }

    // Completes the pending waits for `sem` (a released D3D12 fence counts as reached, as on the PC runtime) and
    // blocks until both a vkWaitSemaphores on `sem` and any callback already dispatched for it have finished, before
    // the caller destroys the VkSemaphore.
    void cancel(VkSemaphore sem) {
        if (!sem) return;
        std::vector<Entry> cancelled;
        SignalEventFn fn = nullptr;
        {
            std::unique_lock lock(mutex_);
            fn = signal_event_;
            std::size_t write = 0;
            for (std::size_t i = 0; i < pending_.size(); ++i) {
                if (pending_[i].semaphore == sem) {
                    cancelled.push_back(std::move(pending_[i]));
                } else {
                    if (write != i) pending_[write] = std::move(pending_[i]);
                    ++write;
                }
            }
            pending_.resize(write);
            // Entries already taken out of pending_ but not yet started belong to a live ready batch. Mark them
            // cancelled so dispatch skips them; this is how a re-entrant cancel() from another callback in the
            // same batch is completed without waiting on the callback that is already on this thread's stack.
            for (const auto& slot : dispatching_) {
                if (slot->semaphore == sem && !slot->active) slot->cancelled = true;
            }
            if (thread_.joinable() && (waiting_ || callback_on_other_thread(sem))) {
                const std::uint64_t target_epoch = epoch_ + 1;
                signal_wakeup_locked();
                epoch_cv_.wait(lock, [this, sem, target_epoch] {
                    return (!waiting_ || epoch_ >= target_epoch) && !callback_on_other_thread(sem);
                });
            }
        }
        dispatch_entries(cancelled, fn);
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            if (stop_) return;
            stop_ = true;
            signal_wakeup_locked();
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    static void dispatch_entries(std::vector<Entry>& entries, SignalEventFn signal_event) {
        for (Entry& e : entries) {
            if (e.callback) e.callback();
            else if (signal_event && e.event_handle) signal_event(e.event_handle);
        }
    }

    // True only for a callback already running on another thread. A slot that has not started yet is cancelled by
    // cancel() and skipped, so it never uses a destroyed semaphore and must not hold up the cancelling caller.
    bool callback_on_other_thread(VkSemaphore sem) const {
        const std::thread::id me = std::this_thread::get_id();
        return std::any_of(dispatching_.begin(), dispatching_.end(), [&](const auto& d) {
            return d->semaphore == sem && d->active && d->owner != me;
        });
    }

    // mutex_ is held on entry and again on return. Recording every entry before unlocking closes the window where
    // run() has taken them out of pending_ but has not started the callback, yet cancel() can return and let the
    // caller destroy the semaphore. Each entry has its own slot so a cancel() for a later entry in the same batch
    // skips that entry instead of being mistaken for the callback already on the cancelling thread's stack.
    void dispatch_outside_lock(std::unique_lock<std::mutex>& lock, std::vector<Entry>& entries, SignalEventFn fn) {
        const std::thread::id me = std::this_thread::get_id();
        std::vector<std::shared_ptr<DispatchSlot>> slots;
        slots.reserve(entries.size());
        for (const Entry& e : entries) {
            auto slot = std::make_shared<DispatchSlot>();
            slot->semaphore = e.semaphore;
            slot->owner = me;
            slots.push_back(slot);
            dispatching_.push_back(slot);
        }
        lock.unlock();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            Entry& e = entries[i];
            DispatchSlot& slot = *slots[i];
            bool skip = false;
            {
                std::lock_guard guard(mutex_);
                skip = slot.cancelled;
                if (!skip) slot.active = true;
            }
            if (skip) {
                // A re-entrant cancel() removed this wait; still complete its event so no waiter is left behind.
                if (fn && e.event_handle) fn(e.event_handle);
                continue;
            }
            if (e.callback) e.callback();
            else if (fn && e.event_handle) fn(e.event_handle);
            {
                std::lock_guard guard(mutex_);
                slot.active = false;
                const auto it = std::find(dispatching_.begin(), dispatching_.end(), slots[i]);
                if (it != dispatching_.end()) dispatching_.erase(it);
            }
            epoch_cv_.notify_all();
        }
        lock.lock();
        for (const auto& slot : slots) {
            const auto it = std::find(dispatching_.begin(), dispatching_.end(), slot);
            if (it != dispatching_.end()) dispatching_.erase(it);
        }
        epoch_cv_.notify_all();
    }

    // Only signals wakeup_sem_ when run() is actively waiting (waiting_ == true) on wakeup_val_ + 1,
    // eliminating any lost-wakeup race between snapshot construction and vkWaitSemaphores. Keep signaling even if
    // device_lost_ is already set so a blocked vkWaitSemaphores wakes cleanly for cancel()/stop().
    void signal_wakeup_locked() {
        if (!waiting_ || !wakeup_sem_) return;
        const std::uint64_t next = ++wakeup_val_;
        VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        si.semaphore = wakeup_sem_;
        si.value = next;
        vk_.vkSignalSemaphore(device_, &si);
    }

    void run() {
        std::unique_lock lock(mutex_);
        while (true) {
            cv_.wait(lock, [this] { return stop_ || !pending_.empty() || device_lost_.load(std::memory_order_acquire); });
            if (device_lost_.load(std::memory_order_acquire)) {
                std::vector<Entry> drained = std::move(pending_);
                pending_.clear();
                dispatch_outside_lock(lock, drained, signal_event_);
                return;
            }
            if (stop_) return;

            semaphores_.clear();
            values_.clear();
            semaphores_.push_back(wakeup_sem_);
            values_.push_back(wakeup_val_ + 1);
            for (const Entry& e : pending_) {
                semaphores_.push_back(e.semaphore);
                values_.push_back(e.value);
            }

            waiting_ = true;
            lock.unlock();

            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.flags = VK_SEMAPHORE_WAIT_ANY_BIT;
            wi.semaphoreCount = static_cast<std::uint32_t>(semaphores_.size());
            wi.pSemaphores = semaphores_.data();
            wi.pValues = values_.data();
            // A bounded slice, not UINT64_MAX. Under GPU paravirtualization (measured in Windows Sandbox on an
            // RTX 5060 Ti) this wait is a kernel wait the OS cannot interrupt: a process that exits while a wait
            // is pending (d3d12_queue_wait_timeout's _Exit, or quitting with a fence outstanding) never finishes
            // exiting, and TerminateProcess cannot end it either. A timeout returns here and the loop re-arms the
            // same wait; completions still wake it at once through the ANY wait.
            const VkResult res = vk_.vkWaitSemaphores(device_, &wi, kWaitSliceNs);

            lock.lock();
            waiting_ = false;
            ++epoch_;
            epoch_cv_.notify_all();

            if (res == VK_ERROR_DEVICE_LOST || device_lost_.load(std::memory_order_acquire)) {
                device_lost_.store(true, std::memory_order_release);
                std::vector<Entry> drained = std::move(pending_);
                pending_.clear();
                dispatch_outside_lock(lock, drained, signal_event_);
                return;
            }
            if (stop_) return;

            std::vector<Entry> ready;
            bool lost = false;
            std::size_t write = 0;
            for (std::size_t i = 0; i < pending_.size(); ++i) {
                std::uint64_t done = 0;
                const VkResult cr = vk_.vkGetSemaphoreCounterValue(device_, pending_[i].semaphore, &done);
                if (cr == VK_ERROR_DEVICE_LOST) {
                    lost = true;
                    for (std::size_t j = i; j < pending_.size(); ++j) ready.push_back(std::move(pending_[j]));
                    break;
                }
                if (cr == VK_SUCCESS && done >= pending_[i].value) {
                    ready.push_back(std::move(pending_[i]));
                } else {
                    if (write != i) pending_[write] = std::move(pending_[i]);
                    ++write;
                }
            }
            if (lost) {
                pending_.resize(write);
                for (Entry& e : pending_) ready.push_back(std::move(e));
                pending_.clear();
                device_lost_.store(true, std::memory_order_release);
                dispatch_outside_lock(lock, ready, signal_event_);
                return;
            }
            pending_.resize(write);
            if (!ready.empty()) {
                dispatch_outside_lock(lock, ready, signal_event_);
                if (stop_) return;
            }
        }
    }

    VkDevice device_;
    const VulkanDeviceFunctions& vk_;
    std::atomic<bool>& device_lost_;
    SignalEventFn signal_event_ = nullptr;
    VkSemaphore wakeup_sem_ = VK_NULL_HANDLE;
    std::uint64_t wakeup_val_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable epoch_cv_;
    std::vector<Entry> pending_;
    std::vector<std::shared_ptr<DispatchSlot>> dispatching_;  // ready-batch callbacks outside mutex_
    std::vector<VkSemaphore> semaphores_;
    std::vector<std::uint64_t> values_;
    std::uint64_t epoch_ = 0;
    bool waiting_ = false;
    bool stop_ = false;
    std::thread thread_;
};

// A range of a D3D12 heap allocation. Mappings and in-flight unbinds keep the arena range alive.
struct TileHeapAllocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    std::uint32_t memory_type = 0;
};

struct TileMapping {
    std::shared_ptr<const TileHeapAllocation> heap;
    VkDeviceSize offset = 0;
    bool operator==(const TileMapping&) const = default;
};

struct SparseResource {
    SparseResource(VkDevice device, const VulkanDeviceFunctions& vk) : device(device), vk(vk) {}
    SparseResource(const SparseResource&) = delete;
    SparseResource& operator=(const SparseResource&) = delete;
    ~SparseResource() {
        if (image) vk.vkDestroyImage(device, image, nullptr);
        if (buffer) vk.vkDestroyBuffer(device, buffer, nullptr);
    }

    VkDevice device;
    const VulkanDeviceFunctions& vk;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkExtent2D image_extent{};
    std::uint64_t tile_count = 0;
    std::uint32_t memory_types = 0;
    std::map<std::uint64_t, TileMapping> mapped;
};

struct TileRegion {
    std::uint64_t first = 0;
    std::uint64_t count = 0;
};

enum class TileRangeMode { sequential, unmap, skip, reuse };

struct TileRange {
    TileRangeMode mode;
    std::uint64_t first;
    std::uint64_t count;
};

// Diagnostic (docs/frame_capture.md): one sampled-texture readback armed for a submission - a resolved sampled T#'s
// layer image, copied in GENERAL layout both ways after the submission.
struct SampleCapture {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    palmetto::ResourceId resource = 0;  // the D3D12 resource behind the layer image (for the id comparison)
};

// Diagnostic (docs/frame_capture.md): one armed colour-target readback. Several can be armed for the same submission
// (the frame's even intermediate pass and its odd six-image pass), so each records the draw index and the resource id it
// copied; the readback prints one labelled line pair (window + write control) per capture.
struct AttachmentCapture {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    palmetto::ResourceId resource = 0;
    std::uint64_t draw = 0;
};

// ---- Content-frame readback window (PALMETTO_READBACK_AFTER) ------------------------------------------------
// docs/vertex_buffer_va.md section 13. The section-10 trigger arms the submission whose lists imported title pages at
// first use plus the next two, because the content came in two submissions in every measured boot. To answer whether
// the black frame is the first transition or the content, the window is a switch: PALMETTO_READBACK_AFTER=<n> arms the
// import submission and the next n-1, so later stage-3 content submissions are read back and written out as windows.
// The pure mapping is tested (port/test/frame_dump_test.cpp) so the window can be pinned without a device. Default 3:
// the measured arm and the default path are unchanged.
inline unsigned title_import_capture_window_from(const char* value) {
    constexpr unsigned kDefaultWindow = 3;
    constexpr unsigned kMaxWindow = 512;
    if (!value || !*value) return kDefaultWindow;
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    if (parsed < 1) return kDefaultWindow;
    return parsed > kMaxWindow ? kMaxWindow : static_cast<unsigned>(parsed);
}

inline unsigned title_import_capture_window() {
    static const unsigned window = title_import_capture_window_from(std::getenv("PALMETTO_READBACK_AFTER"));
    return window;
}

// Per device: what the queues share. Resources are registered by their stable ID (the COM resource pointer).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // intentional alignas(64) cache-line padding to prevent false sharing
#endif
class VulkanContext {
public:
    // conditional_rendering: VK_EXT_conditional_rendering is enabled on `device` (SetPredication).
    // ray_tracing: VK_KHR_acceleration_structure + VK_KHR_ray_query are enabled on `device`.
    // xdxr: acceleration-structure builds write XDXR BVH4 records (xdxr_build.h) instead; needs bufferDeviceAddress.
    VulkanContext(PFN_vkGetDeviceProcAddr get, VkDevice device, bool separate_depth_stencil_layouts,
                  bool conditional_rendering = false, bool ray_tracing = false, bool xdxr = false,
                  bool viewport_scissor_with_count_opt_in = false, bool multi_viewport = false,
                  std::uint32_t max_viewport_count = 1)
        : device_(device), tracker_(separate_depth_stencil_layouts), ray_tracing_(ray_tracing) {
        // docs/card_copies.md: the per-submission title-memory mirror's switches, read once per process. With every one
        // unset the context behaves exactly as before this slice (no clock read, no tracker call, no skip).
        const auto switch_on = [](const char* name) {
            const char* value = std::getenv(name);
            return value && *value && *value != '0';
        };
        card_copy_trace_ = switch_on("PALMETTO_CARD_COPY_TRACE");
        card_copy_skip_no_shader_ = switch_on("PALMETTO_CARD_COPY_SKIP_NO_SHADER");
        card_copy_watch_ = switch_on("PALMETTO_CARD_COPY_WATCH");
        card_copy_gate_ = card_copy_watch_ && switch_on("PALMETTO_CARD_COPY_GATE");
        if (const char* refresh = std::getenv("PALMETTO_CARD_COPY_GATE_REFRESH"))
            card_copy_gate_refresh_ = static_cast<unsigned>(std::strtoul(refresh, nullptr, 10));
        // docs/title_memory_model.md: the same-submission title-page pull coalescer. TRACE is the measurement arm (the
        // counters only, the copy path unchanged); COALESCE makes the decision the same counters describe.
        title_pull_trace_ = switch_on("PALMETTO_TITLE_PULL_TRACE");
        title_pull_coalesce_ = switch_on("PALMETTO_TITLE_PULL_COALESCE");
        if (title_pull_trace_ || title_pull_coalesce_)
            std::fprintf(stderr,
                         "[rt] replay: title pulls: trace %d, coalesce repeated same-submission ranges %d\n",
                         int(title_pull_trace_), int(title_pull_coalesce_));
        if (card_copy_trace_ || card_copy_watch_)
            std::fprintf(stderr,
                         "[rt] replay: card copies: trace %d, skip no-shader submission %d, write tracker %d, gate %d "
                         "(refresh every %u), tracker hooks %s\n",
                         int(card_copy_trace_), int(card_copy_skip_no_shader_), int(card_copy_watch_),
                         int(card_copy_gate_), card_copy_gate_refresh_, console_watch_available() ? "present" : "null");
        loaded_ = vk.load(get, device);
        replay_.pipeline_barrier = vk.vkCmdPipelineBarrier;
        replay_.clear_color_image = vk.vkCmdClearColorImage;
        replay_.clear_depth_stencil_image = vk.vkCmdClearDepthStencilImage;
        replay_.pipeline_barrier2 = vk.vkCmdPipelineBarrier2;
        replay_.begin_rendering = vk.vkCmdBeginRendering;
        replay_.end_rendering = vk.vkCmdEndRendering;
        replay_.clear_attachments = vk.vkCmdClearAttachments;
        replay_.copy_buffer = vk.vkCmdCopyBuffer;
        replay_.copy_buffer_to_image = vk.vkCmdCopyBufferToImage;
        replay_.copy_image_to_buffer = vk.vkCmdCopyImageToBuffer;
        replay_.copy_image = vk.vkCmdCopyImage;
        replay_.resolve_image = vk.vkCmdResolveImage;
        replay_.fill_buffer = vk.vkCmdFillBuffer;
        replay_.update_buffer = vk.vkCmdUpdateBuffer;
        replay_.draw_indirect = vk.vkCmdDrawIndirect;
        replay_.draw_indexed_indirect = vk.vkCmdDrawIndexedIndirect;
        replay_.draw_indirect_count = vk.vkCmdDrawIndirectCount;
        replay_.draw_indexed_indirect_count = vk.vkCmdDrawIndexedIndirectCount;
        replay_.dispatch_indirect = vk.vkCmdDispatchIndirect;
        if (conditional_rendering) {  // a driver may hand out these pointers even when the extension is off
            replay_.begin_conditional_rendering = vk.vkCmdBeginConditionalRenderingEXT;
            replay_.end_conditional_rendering = vk.vkCmdEndConditionalRenderingEXT;
        }
        replay_.reset_query_pool = vk.vkCmdResetQueryPool;
        replay_.begin_query = vk.vkCmdBeginQuery;
        replay_.end_query = vk.vkCmdEndQuery;
        replay_.write_timestamp = vk.vkCmdWriteTimestamp;
        replay_.copy_query_pool_results = vk.vkCmdCopyQueryPoolResults;
        replay_.bind_pipeline = vk.vkCmdBindPipeline;
        replay_.push_constants = vk.vkCmdPushConstants;
        replay_.bind_descriptor_sets = vk.vkCmdBindDescriptorSets;
        replay_.dispatch = vk.vkCmdDispatch;
        replay_.compute_pipeline = [this](std::uintptr_t id) -> const ComputePipeline* {
            const auto it = pipelines_.find(id);
            if (it == pipelines_.end()) {
                static std::atomic<unsigned> misses{0};
                if (misses.fetch_add(1, std::memory_order_relaxed) < 6)
                    std::fprintf(stderr, "[rt] replay: compute pipeline map miss for id 0x%llx (%zu registered)\n",
                                 static_cast<unsigned long long>(id), pipelines_.size());
            }
            return it == pipelines_.end() ? nullptr : &it->second;
        };
        replay_.bind_vertex_buffers2 = vk.vkCmdBindVertexBuffers2;
        replay_.bind_index_buffer = vk.vkCmdBindIndexBuffer;
        replay_.set_viewport = vk.vkCmdSetViewport;
        replay_.set_scissor = vk.vkCmdSetScissor;
        replay_.multi_viewport_enabled = multi_viewport;
        replay_.max_viewport_count = max_viewport_count;
        viewport_scissor_count_enabled_ = viewport_scissor_with_count_opt_in && multi_viewport &&
                                          max_viewport_count > 1 && vk.vkCmdSetViewportWithCount &&
                                          vk.vkCmdSetScissorWithCount;
        if (viewport_scissor_count_enabled_) {
            replay_.set_viewport_with_count = vk.vkCmdSetViewportWithCount;
            replay_.set_scissor_with_count = vk.vkCmdSetScissorWithCount;
        }
        replay_.set_primitive_topology = vk.vkCmdSetPrimitiveTopology;
        replay_.set_primitive_restart_enable = vk.vkCmdSetPrimitiveRestartEnable;
        replay_.set_stencil_reference = vk.vkCmdSetStencilReference;
        replay_.set_blend_constants = vk.vkCmdSetBlendConstants;
        replay_.draw = vk.vkCmdDraw;
        replay_.draw_mesh_tasks = vk.vkCmdDrawMeshTasksEXT;  // null unless VK_EXT_mesh_shader is enabled
        replay_.draw_indexed = vk.vkCmdDrawIndexed;
        replay_.graphics_pipeline = [this](std::uintptr_t id) -> const GraphicsPipeline* {
            // The frame dump's control draws (docs/frame_capture.md) name these reserved ids; they are not in the map.
            if (id == frame_dump::control_pipeline_id) return &frame_dump_control_pipeline_;
            if (id == frame_dump::ps_control_pipeline_id) return &frame_dump_ps_control_pipeline_;
            // The upscale stand-in's own draw (docs/upscale_standin.md); the layer's pipeline, never a title one.
            if (id == upscale_standin::pipeline_id) return &upscale_standin_pipeline_;
            const auto it = graphics_pipelines_.find(id);
            if (it == graphics_pipelines_.end()) {
                static std::atomic<unsigned> misses{0};
                if (misses.fetch_add(1, std::memory_order_relaxed) < 6)
                    std::fprintf(stderr,
                                 "[rt] replay: graphics pipeline map miss for id 0x%llx (%zu registered)\n",
                                 static_cast<unsigned long long>(id), graphics_pipelines_.size());
            }
            return it == graphics_pipelines_.end() ? nullptr : &it->second;
        };
        // Called with resource_mutex_ held. A binary search of the buffers' VA ranges (buffer_va_index.h), kept in step
        // with resources_ by set_resource_locked and remove_resource.
        // A range that runs past its buffer is cut at the buffer's end (`size` is updated): D3D12 reads past a view's
        // buffer as out of bounds, which is no reason to drop the submission.
        replay_.buffer_at = [this](std::uint64_t va, std::uint64_t& size, VkBuffer& buffer, VkDeviceSize& offset) {
            // The opt-in outside-arena path resolves the raw title VA to the actual device-address window before the
            // arena alias translation below. Window writes are not pulled back to title memory, so any command batch
            // naming the same bytes through WriteBufferImmediate refuses this lookup by name.
            if (title_va_window_buffer_resolver_) {
                VkBuffer window_buffer = VK_NULL_HANDLE;
                VkDeviceSize window_offset = 0;
                if (title_va_window_buffer_resolver_(va, size, window_buffer, window_offset)) {
                    bool write_conflict = title_va_window_write_overflow_;
                    if (!write_conflict && va && size && size <= UINT64_MAX - va) {
                        const std::uint64_t end = va + size;
                        for (std::size_t i = 0; i < title_va_window_write_count_; ++i) {
                            const auto [write_va, write_bytes] = title_va_window_write_ranges_[i];
                            if (write_va && write_bytes && write_bytes <= UINT64_MAX - write_va &&
                                va < write_va + write_bytes && write_va < end) {
                                write_conflict = true;
                                break;
                            }
                        }
                    }
                    if (write_conflict) {
                        note_title_window_refusal(va, size, title_va_window_write_overflow_
                                                                        ? "write_range_cap"
                                                                        : "write_buffer_immediate_unsupported");
                        return false;
                    }
                    buffer = window_buffer;
                    offset = window_offset;
                    return true;
                }
            }
            // A command argument that names a title-placed buffer by the VA the title chose: the title VA is an alias
            // for the arena range that stands in for it (docs/page_address_space.md), so the view resolves to the
            // stand-in's bytes through the same lookup as every other buffer.
            std::uint64_t rebased = 0;
            if (console_vas_.rebase(va, rebased, palmetto::ConsoleVaKind::buffer)) va = rebased;
            BufferVaIndex<ResourceId>::Hit hit{};
            if (!buffer_vas_.find(va, hit, size)) {
                // A title VA this layer imported over the title's own pages at first use (docs/vertex_buffer_va.md):
                // the command argument names those pages by the VA the title placed them at.
                if (imported_title_range(va, size, buffer, offset)) {
                    imported_title_hits_.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                static std::atomic<unsigned> misses{0};
                if (misses.fetch_add(1, std::memory_order_relaxed) < 6)
                    std::fprintf(stderr, "[rt] replay: buffer_at miss for VA 0x%llx size 0x%llx\n",
                                 static_cast<unsigned long long>(va), static_cast<unsigned long long>(size));
                // Diagnostic (docs/vertex_buffer_va.md): the miss measurement: is the VA readable host memory (the
                // title's pages), and where does it sit relative to the title ranges the layer already aliased? The
                // first few misses only, so the probe cannot dominate a boot.
                if (miss_dump::enabled() && misses.load(std::memory_order_relaxed) <= 6) {
                    probe_host_range(va, size);
                    probe_console_ranges(console_vas_.snapshot(), va);
                }
                return false;  // prefers an aliasing buffer that holds all of it
            }
            const ResourceBinding* b = resolve(hit.id);
            if (!b) {
                static std::atomic<unsigned> misses{0};
                if (misses.fetch_add(1, std::memory_order_relaxed) < 6)
                    std::fprintf(stderr, "[rt] replay: buffer_at hit has no binding for id 0x%llx\n",
                                 static_cast<unsigned long long>(hit.id));
                return false;
            }
            buffer = b->buffer;
            offset = b->buffer_offset + (va - b->va);
            size = std::min<std::uint64_t>(size, b->size - (va - b->va));
            return true;
        };
        // Host reads of small CPU-written inputs. Unlike buffer_at, the whole range must be in one CPU-writable
        // mapping: a partial read would see bytes the application has not written through the D3D12 API. The
        // binding's host pointer is null on DEFAULT and READBACK heaps, whose mapped bytes the GPU can write.
        replay_.host_at = [this](std::uint64_t va, std::uint64_t bytes, const std::uint8_t*& host) {
            std::uint64_t rebased = 0;
            if (console_vas_.rebase(va, rebased, palmetto::ConsoleVaKind::buffer)) va = rebased;
            BufferVaIndex<ResourceId>::Hit hit{};
            if (!buffer_vas_.find(va, hit)) return HostReadStatus::absent;
            const ResourceBinding* b = resolve(hit.id);
            if (!b) return HostReadStatus::absent;
            if (!b->host) return HostReadStatus::not_cpu_written;
            const std::uint64_t offset = va - b->va;
            if (offset > b->size || bytes > b->size - offset) return HostReadStatus::absent;
            host = b->host + offset;
            return HostReadStatus::ok;
        };
        // The title's own VAs (console_va_map.h, docs/page_address_space.md): a value inside a range the layer aliased
        // names that range's arena stand-in. `descriptor_range` says which of two overlapping ranges the site means: a
        // descriptor table's VA is in a console descriptor heap, a root descriptor's VA names a placed buffer. Rebase
        // every shader-visible copy before it reaches a shader, and refuse draws while a title range the title can name
        // is not aliased (console_va_ready).
        replay_.rebase_console_va = [this](std::uint64_t va, std::uint64_t& rebased, bool descriptor_range) {
            if (!console_vas_.rebase(va, rebased,
                                     descriptor_range ? ConsoleVaKind::descriptors : ConsoleVaKind::buffer))
                return false;
            console_vas_.count_rebase();
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr,
                             "[rt] replay: title VA 0x%llx (%s) rebased to arena VA 0x%llx (heap_base + VA is its "
                             "stand-in)\n",
                             static_cast<unsigned long long>(va),
                             descriptor_range ? "descriptor heap" : "placed buffer",
                             static_cast<unsigned long long>(rebased));
            return true;
        };
        replay_.console_va_ready = [this] { return !console_vas_.incomplete(); };
        replay_.sync_console_heaps = [this] {
            if (console_heap_sync_) console_heap_sync_();
        };
        // The title's page content (docs/page_content.md): the device copies the bytes behind a title VA into its arena
        // stand-in, and the data a descriptor table's V#s name, before the submission reads them.
        // docs/title_memory_model.md: a draw's vertex/index/root-descriptor pull first asks this submission's
        // coalescing window; a range this submission already copied is not copied again (COALESCE), and the counters
        // report how often that decision is available (TRACE).
        replay_.pull_title_pages = [this](std::uint64_t va, std::uint64_t bytes) {
            if (title_pull_decide(va, bytes)) return;
            std::uint64_t microseconds = 0;
            if (title_pull_trace_ || title_pull_coalesce_) {
                const auto started = std::chrono::steady_clock::now();
                if (console_page_pull_) console_page_pull_(va, bytes);
                microseconds = static_cast<std::uint64_t>(std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - started).count());
            } else if (console_page_pull_) {
                console_page_pull_(va, bytes);
            }
            title_pull_copied(microseconds);
        };
        replay_.pull_title_table = [this](std::uint64_t table_va, std::uint32_t max_descriptors) {
            if (console_table_pull_) console_table_pull_(table_va, max_descriptors);
        };
        replay_.pull_title_arena = [this](std::uint64_t arena_va, std::uint64_t bytes) {
            if (console_arena_pull_) console_arena_pull_(arena_va, bytes);
        };
        replay_.capture_attachment = [this](VkImage image, VkFormat format, VkExtent2D extent,
                                            palmetto::ResourceId resource, std::uint64_t draw) {
            arm_attachment_capture(image, format, extent, resource, draw);
        };
        // Diagnostic (docs/vertex_buffer_va.md section 10): the content frame's readback trigger. True while the
        // submission being prepared is one whose lists named title memory at first use.
        replay_.title_import_capture = [this] {
            return title_import_capture_active_.load(std::memory_order_relaxed);
        };
        // Diagnostic (docs/frame_capture.md): the sampled-texture readback's copy. Off unless the frame dump's sample
        // mode arms it, and it only ever copies.
        replay_.capture_sample = [this](VkImage image, VkFormat format, VkExtent2D extent,
                                        palmetto::ResourceId resource) {
            arm_sample_capture(image, format, extent, resource);
        };
        if (xdxr && loaded_) {
            xdxr_ = std::make_unique<XdxrBuilder>(get, device);
            if (xdxr_->loaded()) {
                replay_.xdxr_check = [](const BuildAccelerationStructure& b) {
                    XdxrLayout layout;
                    // An update needs ALLOW_UPDATE (D3D12 requires it on the build and the update): a structure
                    // built without it may have paired leaves, which a refit would rewrite as wrong geometry.
                    return xdxr_inputs(b, nullptr, 0) && layout.compute(xdxr_elements(b), b.is_tlas, xdxr_box16(b)) &&
                           b.dst_va && b.scratch_va && (!b.is_tlas || !b.instance_count || b.instances_va) &&
                           (!b.update || (b.build_flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) != 0);
                };
                replay_.xdxr_record = [this](VkCommandBuffer cb, const BuildAccelerationStructure& b) {
                    // Every address is a console VA in the GPU address arena: the device address is heap_base + VA,
                    // and the BLAS addresses in instance descs get the same bias.
                    const std::uint64_t base = replay_.heap_base;
                    XdxrLayout layout;
                    layout.compute(xdxr_elements(b), b.is_tlas, xdxr_box16(b));
                    // PERFORM_UPDATE refits the source (the destination itself when no source is given, as D3D12
                    // allows for an in-place update); a full build otherwise.
                    const std::uint64_t from = b.update ? base + (b.src_va ? b.src_va : b.dst_va) : 0;
                    if (b.is_tlas) {
                        xdxr_->record_tlas(cb, layout, b.instances_va ? base + b.instances_va : 0, base + b.dst_va,
                                           base + b.scratch_va, base, from);
                    } else {
                        std::vector<XdxrTriangles> geometries;
                        xdxr_inputs(b, &geometries, base);
                        xdxr_->record_blas(cb, layout, geometries, base + b.dst_va, base + b.scratch_va, from,
                                           xdxr_pair(b));
                    }
                };
                replay_.xdxr_copy = [this](VkCommandBuffer cb, std::uint64_t src, std::uint64_t dst, std::uint64_t bytes) {
                    const std::uint64_t base = replay_.heap_base;
                    xdxr_->record_copy(cb, base + src, base + dst, bytes);
                };
                replay_.xdxr_postbuild = [this](VkCommandBuffer cb, std::uint64_t src, std::uint64_t dest) {
                    const std::uint64_t base = replay_.heap_base;
                    xdxr_->record_postbuild(cb, base + src, base + dest);
                };
            } else {
                xdxr_.reset();
            }
        }
        if (!xdxr_ && ray_tracing_ && vk.vkCmdBuildAccelerationStructuresKHR) {
            replay_.build_acceleration_structures = vk.vkCmdBuildAccelerationStructuresKHR;
            replay_.copy_acceleration_structure = vk.vkCmdCopyAccelerationStructureKHR;
            replay_.write_acceleration_structures_properties = vk.vkCmdWriteAccelerationStructuresPropertiesKHR;
            replay_.resolve_acceleration_structure = [this](std::uint64_t va) {
                const auto it = acceleration_structures_.find(va);
                return it == acceleration_structures_.end() ? VK_NULL_HANDLE : it->second;
            };
        }
        replay_.image_view = [this](const ResourceBinding& r, const VkImageSubresourceRange& range) {
            return view(r, range);
        };
        if (vk.vkCreateRenderPass2 && vk.vkDestroyRenderPass && vk.vkCreateFramebuffer && vk.vkDestroyFramebuffer &&
            vk.vkCmdBeginRenderPass && vk.vkCmdEndRenderPass) {
            replay_.begin_render_pass = vk.vkCmdBeginRenderPass;
            replay_.end_render_pass = vk.vkCmdEndRenderPass;
            replay_.depth_resolve_framebuffer = [this](const ResourceBinding& source, VkImageView from,
                                                       VkImageLayout from_layout, const ResourceBinding& target,
                                                       VkImageView to, VkImageLayout to_layout,
                                                       VkResolveModeFlagBits mode, VkExtent2D extent,
                                                       VkRenderPass& pass) {
                return depth_resolve_framebuffer(source, from, from_layout, target, to, to_layout, mode, extent, pass);
            };
        }
        replay_.query_pool = [this](QueryHeapId id) -> QueryPool* {  // called with resource_mutex_ held
            const auto it = query_pools_.find(id);
            return it == query_pools_.end() ? nullptr : &it->second;
        };
        replay_.separate_depth_stencil_layouts = separate_depth_stencil_layouts;
        if (loaded_) {
            fence_dispatcher_ = std::make_unique<FenceWaitDispatcher>(device_, vk, device_lost_);
        }
    }

    ~VulkanContext() {
        fence_dispatcher_.reset();
        deletion_queue_.drain(UINT64_MAX, device_, vk);
        // The frame dump's control pipeline (docs/frame_capture.md), when the diagnostic created one.
        if (frame_dump_control_pipeline_.pipeline) vk.vkDestroyPipeline(device_, frame_dump_control_pipeline_.pipeline, nullptr);
        if (frame_dump_control_pipeline_.layout) vk.vkDestroyPipelineLayout(device_, frame_dump_control_pipeline_.layout, nullptr);
        if (frame_dump_control_vs_) vk.vkDestroyShaderModule(device_, frame_dump_control_vs_, nullptr);
        if (frame_dump_control_fs_) vk.vkDestroyShaderModule(device_, frame_dump_control_fs_, nullptr);
        // The upscale stand-in's own pipeline and views (docs/upscale_standin.md), when the switch created them.
        if (upscale_standin_pipeline_.pipeline) vk.vkDestroyPipeline(device_, upscale_standin_pipeline_.pipeline, nullptr);
        if (upscale_standin_layout_) vk.vkDestroyPipelineLayout(device_, upscale_standin_layout_, nullptr);
        if (upscale_standin_vs_) vk.vkDestroyShaderModule(device_, upscale_standin_vs_, nullptr);
        if (upscale_standin_ps_) vk.vkDestroyShaderModule(device_, upscale_standin_ps_, nullptr);
        if (upscale_standin_view_) vk.vkDestroyImageView(device_, upscale_standin_view_, nullptr);
        if (upscale_standin_pool_) vk.vkDestroyDescriptorPool(device_, upscale_standin_pool_, nullptr);
        if (upscale_standin_set_layout_)
            vk.vkDestroyDescriptorSetLayout(device_, upscale_standin_set_layout_, nullptr);
        for (const auto& [key, framebuffer] : resolve_framebuffers_)
            vk.vkDestroyFramebuffer(device_, framebuffer, nullptr);
        for (const auto& [key, pass] : resolve_passes_) vk.vkDestroyRenderPass(device_, pass, nullptr);
        for (const auto& [key, view] : views_) vk.vkDestroyImageView(device_, view, nullptr);
    }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool loaded() const { return loaded_; }
    bool viewport_scissor_count_enabled() const { return viewport_scissor_count_enabled_; }
    void configure_predication(PFN_vkGetDeviceProcAddr get, const VkPhysicalDeviceMemoryProperties& memory,
                               VkDeviceSize max_range, std::uint32_t family) {
        std::unique_lock lock(resource_mutex_);
        predicate_family_ = family;
        predicate_reducer_ = std::make_unique<PredicateReducer>(get, device_, memory, max_range);
    }
    // Configure before submitting work, only when storageBuffer8BitAccess is enabled on the device.
    void configure_predicated_copy(PFN_vkGetDeviceProcAddr get, const VkPhysicalDeviceLimits& limits) {
        std::unique_lock lock(resource_mutex_);
        predicated_copier_ = std::make_unique<PredicatedBufferCopier>(get, device_, limits);
    }
    // Counts for DISPATCH-only ExecuteIndirect are read by a helper compute shader from GPU memory.
    // Requires Vulkan bufferDeviceAddress, already enabled for the D3D12 address arena.
    void configure_counted_dispatch(PFN_vkGetDeviceProcAddr get, const VkPhysicalDeviceMemoryProperties& memory,
                                    const VkPhysicalDeviceLimits& limits) {
        std::unique_lock lock(resource_mutex_);
        counted_dispatch_ = std::make_unique<IndirectDispatchCounter>(get, device_, memory, limits);
    }
    VkDevice device() const { return device_; }

    // Debug markers become labels from here on; call before any queue or list uses the context. Incomplete sets
    // are ignored, so markers stay no-ops.
    void enable_debug_labels(const DebugLabelFunctions& labels) {
        if (!labels.complete()) return;
        labels_ = labels;
        replay_.insert_label = labels.cmd_insert;
        replay_.begin_label = labels.cmd_begin;
        replay_.end_label = labels.cmd_end;
    }
    bool debug_labels() const { return labels_.complete(); }

    // Registers a resource in its D3D12 initial state (a legacy state; images get the matching layout).
    // The image must already be in that layout, e.g. transitioned from UNDEFINED at creation.
    void add_resource(ResourceId id, const ResourceBinding& binding) {
        std::unique_lock lock(resource_mutex_);
        set_resource_locked(id, binding);
    }
    void add_resource(ResourceId id, const ResourceBinding& binding, VkImageLayout current_layout) {
        std::unique_lock rlock(resource_mutex_);
        set_resource_locked(id, binding);
        if (binding.image) {
            std::lock_guard tlock(tracker_mutex_);
            tracker_.track(id, binding, current_layout);
        }
    }

    // A shader can access a storage image or an initially unbound sparse image before any command names it.
    // The first shader submission moves it to GENERAL, the descriptor's layout, ahead of its lists.
    void note_shader_image(ResourceId id) {
        std::lock_guard lock(storage_mutex_);
        storage_pending_.insert(id);
    }

    // A device-local buffer D3D12 says starts zeroed (D3D-71): the host cannot write it, so the next submission on
    // any queue fills it with zeros ahead of its lists. That submission publishes the same initialisation semaphore as
    // the storage images above, so a list on another queue that uses the buffer first waits for the fill.
    void zero_on_next_submit(ResourceId id, VkBuffer buffer) {
        std::lock_guard lock(storage_mutex_);
        zero_pending_.push_back({id, buffer});
    }

    void add_sparse_resource(ResourceId id, const ResourceBinding& binding, std::shared_ptr<SparseResource> sparse) {
        std::unique_lock lock(resource_mutex_);
        if (!binding.image) {
            auto [it, inserted] = sparse_resources_.try_emplace(id, std::move(sparse));
            try {
                set_resource_locked(id, binding);
            } catch (...) {
                if (inserted) sparse_resources_.erase(it);
                throw;
            }
            return;
        }
        set_resource_locked(id, binding);
        sparse_resources_.emplace(id, std::move(sparse));
        if (binding.image) {
            std::lock_guard tlock(tracker_mutex_);
            tracker_.track(id, binding, VK_IMAGE_LAYOUT_UNDEFINED);
        }
    }

    void remove_resource(ResourceId id) {
        VkImage img = VK_NULL_HANDLE;
        std::shared_ptr<SparseResource> sparse_resource;
        {
            std::unique_lock rlock(resource_mutex_);
            const auto it = resources_.find(id);
            if (it == resources_.end()) return;
            img = it->second.image;
            unindex_buffer(id, it->second);
            resources_.erase(it);
            if (auto sparse = sparse_resources_.find(id); sparse != sparse_resources_.end()) {
                sparse_resource = std::move(sparse->second);
                sparse_resources_.erase(sparse);
            }
        }
        {
            std::lock_guard lock(storage_mutex_);  // a buffer destroyed before any submission is not filled
            std::erase_if(zero_pending_, [id](const PendingZero& zero) { return zero.id == id; });
        }
        if (img) {
            std::lock_guard vlock(views_mutex_);
            for (auto f = resolve_framebuffers_.begin(); f != resolve_framebuffers_.end();) {
                if (std::get<0>(f->first) == img || std::get<1>(f->first) == img) {
                    defer_destroy(DeferredDeletionQueue::Kind::framebuffer, f->second);
                    f = resolve_framebuffers_.erase(f);
                } else {
                    ++f;
                }
            }
            for (auto v = views_.begin(); v != views_.end();) {
                if (std::get<0>(v->first) == img) {
                    defer_destroy(DeferredDeletionQueue::Kind::image_view, v->second);
                    v = views_.erase(v);
                } else {
                    ++v;
                }
            }
        }
        if (sparse_resource)
            defer_destroy(DeferredDeletionQueue::Kind::sparse_resource,
                          new std::shared_ptr<SparseResource>(std::move(sparse_resource)));
        std::lock_guard tlock(tracker_mutex_);
        tracker_.forget(id);
    }

    // A query heap, by its stable ID (the COM query heap pointer). The pool stays the caller's to destroy, after
    // remove_query_pool.
    void add_query_pool(QueryHeapId id, const QueryPool& pool) {
        std::unique_lock lock(resource_mutex_);
        query_pools_[id] = pool;
    }
    void remove_query_pool(QueryHeapId id) {
        // Lock hierarchy: resource_mutex_ -> tracker_mutex_. Holding tracker_mutex_ before erasing keeps the
        // QueryPool node alive until any in-flight execute() that unlocked resource_mutex_ finishes QueryReplay::commit().
        std::unique_lock lock(resource_mutex_);
        std::lock_guard tlock(tracker_mutex_);
        query_pools_.erase(id);
    }
    // The heap's d3d12_query_heap type, or ~0u for a heap that is not registered. Called from the COM queue's timestamp
    // check before execute(), never from inside execute()'s shared lock (heap_base() explains why that matters).
    std::uint32_t query_heap_type(QueryHeapId id) {
        std::shared_lock lock(resource_mutex_);   // query_pools_ is guarded by resource_mutex_, like add/remove
        const auto it = query_pools_.find(id);
        return it == query_pools_.end() ? ~0u : it->second.heap_type;
    }

    // VkPhysicalDeviceLimits::timestampPeriod (nanoseconds per tick), or 0 when the queue writes no timestamps
    // (timestampValidBits 0). Set by the device before any queue uses it.
    double timestamp_period() const { return timestamp_period_; }
    void set_timestamp_period(double period) { timestamp_period_ = period; }
    // True where a timestamp resolve must be ordered explicitly after the timestamps it copies (MoltenVK on Apple
    // GPUs, docs/d3d12_vulkan.md section 4.6). Set by the device before any queue uses it.
    bool order_timestamp_copies() const { return order_timestamp_copies_; }
    void set_order_timestamp_copies(bool order) { order_timestamp_copies_ = order; }

    // The device address of the GPU address arena: console VAs are offsets from it (docs section 3.3).
    // execute() copies replay_ under a shared resource_mutex_, so writers hold it uniquely. Called once, by the device,
    // when the arena is created, which is inside the DeviceState constructor and therefore before any queue exists
    // (queues are made by a later CreateCommandQueue, and no execute can precede one) - that is what makes the
    // replay-side accessor heap_base() able to read the value without taking resource_mutex_ (see there).
    void set_heap_base(std::uint64_t heap_base) {
        std::unique_lock lock(resource_mutex_);
        replay_.heap_base = heap_base;
        heap_base_.store(heap_base, std::memory_order_release);
    }

    // Zeroed memory for unbound vertex and index buffers to read (ReplayFunctions::zero_buffer).
    void set_zero_page(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
        std::unique_lock lock(resource_mutex_);
        replay_.zero_buffer = buffer;
        replay_.zero_offset = offset;
        replay_.zero_size = size;
    }

    // The title's own console VAs (console_va_map.h, docs/page_address_space.md). A placed buffer or a console
    // descriptor heap gets a range in the arena that stands in for it: `title` is the VA the title names it by, `size`
    // its bytes and `arena` the arena VA of the stand-in. The title VA stays an alias for command arguments through
    // buffer_at; every shader-visible copy of it is rebased (replay_.rebase_console_va) before a shader sees it.
    bool add_console_va_range(std::uint64_t title, std::uint64_t size, std::uint64_t arena, ConsoleVaKind kind,
                              bool outside_arena = false) {
        if (console_vas_.add(title, size, arena, kind)) {
            if (kind == ConsoleVaKind::descriptors) note_console_mirror_added(title, size);
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                if (outside_arena)
                    std::fprintf(stderr,
                                 "[rt] replay: title VA range 0x%llx + 0x%llx (%s) maps to outside-arena console VA "
                                 "0x%llx\n",
                                 static_cast<unsigned long long>(title), static_cast<unsigned long long>(size),
                                 console_va_kind_name(kind), static_cast<unsigned long long>(arena));
                else
                    std::fprintf(stderr,
                                 "[rt] replay: title VA range 0x%llx + 0x%llx (%s) is arena VA 0x%llx\n",
                                 static_cast<unsigned long long>(title), static_cast<unsigned long long>(size),
                                 console_va_kind_name(kind), static_cast<unsigned long long>(arena));
            }
            return true;
        }
        // Two stand-ins for one VA is a range the layer cannot decide; refuse rather than execute a guess.
        console_vas_.mark_incomplete();
        std::fprintf(stderr,
                     "[rt] replay: title VA range 0x%llx + 0x%llx (%s) overlaps one already aliased; draws stay "
                     "refused\n",
                     static_cast<unsigned long long>(title), static_cast<unsigned long long>(size),
                     console_va_kind_name(kind));
        return false;
    }
    void remove_console_va_range(std::uint64_t title) {
        note_console_mirror_removed(title);
        console_vas_.remove(title);
    }
    // Draws stay refused while a title range the title can name as a shader-visible VA has no stand-in, and while a
    // title descriptor names a page range this layer imported at first use (the shader would add heap_base to the
    // title VA). docs/vertex_buffer_va.md.
    bool console_va_ready() const {
        return !console_vas_.incomplete() && !import_conflict_.load(std::memory_order_relaxed);
    }
    // The boot's one-line report of the title-VA work: ranges aliased, shader-visible VAs rebased, V# bases patched.
    std::size_t console_va_ranges() const { return console_vas_.size(); }
    std::uint64_t console_va_rebased_values() const { return console_vas_.rebased_values(); }
    std::uint64_t console_va_rebased_fields() const { return console_vas_.rebased_fields(); }
    // A title range the title can name as a shader-visible VA has no arena stand-in: every draw stays refused.
    void mark_console_va_incomplete() { console_vas_.mark_incomplete(); }
    // Rewrites the V# base fields in a console descriptor heap's arena mirror that name a title-placed buffer, so the
    // descriptors the title wrote name the buffer's arena range (console_va_map.h). Returns how many were rewritten.
    std::size_t patch_console_descriptor_bases(std::uint8_t* bytes, std::size_t size) {
        const std::size_t patched = console_vas_.patch_descriptor_bases(bytes, size);
        if (patched) console_vas_.count_fields(patched);
        note_imported_title_descriptors(bytes, size);
        return patched;
    }
    // ---- docs/card_copies.md: the per-submission title-memory mirror -------------------------------------------------
    // The measured copy path (docs/page_address_space.md section 4): before every submission, sync_console_heaps()
    // copies the title's CPU descriptor-heap bytes into their GPU-visible arena mirrors, a full compare pass over the
    // measured 38-40 MB heap that costs 3.4-4.4 ms - most of the ~3.8 ms between submissions. This slice quantifies
    // that path and readies the two ways of avoiding it; every switch is off by default, so the default path is
    // exactly the tree before the slice.
    //
    //   PALMETTO_CARD_COPY_TRACE=1            count the refresh per submission (calls, skipped, microseconds, max, the
    //                                         bytes it covers) and print the totals at the submission milestones.
    //                                         Behaviour unchanged: the measurement arm.
    //   PALMETTO_CARD_COPY_SKIP_NO_SHADER=1   skip the refresh for a submission that records no command a shader can
    //                                         read the mirror from (barriers, clears, copies, queries only). The
    //                                         mirror can only be read by a shader, so no submission that records none
    //                                         needs it; a later draw submission refreshes before it is recorded.
    //   PALMETTO_CARD_COPY_WATCH=1            arm the guard-page write tracker (xmem.h, docs/card_copies.md) on the
    //                                         mirrored title ranges and poll it every submission, counts only.
    //   PALMETTO_CARD_COPY_GATE=1             with WATCH, skip the refresh when no watched page was written since the
    //                                         last one. READIED, NOT ENABLED: the safety condition needs the device's
    //                                         console-heap census (in-arena heaps are not mirrored and their
    //                                         title-written V# bases are patched by the same refresh, so an
    //                                         unmirrored heap must force the full refresh) - see the page's
    //                                         "what remains" section for the exact pending hook.
    //   PALMETTO_CARD_COPY_GATE_REFRESH=<n>   with GATE, force one refresh every n submissions: the bound on how long
    //                                         a write that landed while the guards were suspended can stay
    //                                         unmirrored. 0 (default) means only a changed page refreshes.
    bool card_copy_trace() const { return card_copy_trace_; }
    bool card_copy_watch() const { return card_copy_watch_; }
    bool card_copy_gate() const { return card_copy_gate_; }
    // The measured mirror: how many ranges the layer mirrors and their bytes, and how many the write tracker guards.
    std::size_t console_mirror_ranges() const {
        std::lock_guard lock(console_mirrors_mutex_);
        return console_mirrors_.size();
    }
    std::uint64_t console_mirror_bytes() const { return console_mirror_bytes_.load(std::memory_order_relaxed); }
    std::uint32_t console_watch_ranges() const { return console_watch_ranges_.load(std::memory_order_relaxed); }
    bool console_watch_available() const { return rt::xmem::page_watch_arm && rt::xmem::page_watch_forget && rt::xmem::page_watch_poll; }
    std::uint64_t console_watch_pages() const { return rt::xmem::page_watch_pages ? rt::xmem::page_watch_pages() : 0; }
    std::uint64_t page_write_hits() const { return rt::xmem::page_watch_write_trips ? rt::xmem::page_watch_write_trips() : 0; }
    std::uint64_t page_read_hits() const { return rt::xmem::page_watch_read_trips ? rt::xmem::page_watch_read_trips() : 0; }

    // A console descriptor heap's title range is now mirrored (add_console_va_range): record its size and, with the
    // write tracker on, arm its pages. A range the tracker cannot guard stays `armed = false`, which keeps the gate's
    // coverage condition false and so keeps the full refresh running.
    void note_console_mirror_added(std::uint64_t title, std::uint64_t size) {
        // Only PALMETTO_CARD_COPY_WATCH arms a range: the trace arm must not change the refresh path's cost.
        const bool arm = card_copy_watch_ && rt::xmem::page_watch_arm &&
                         rt::xmem::page_watch_arm(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(title)), size);
        std::lock_guard lock(console_mirrors_mutex_);
        console_mirrors_.push_back({title, size, arm});
        console_mirror_bytes_.fetch_add(size, std::memory_order_relaxed);
        if (arm) console_watch_ranges_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_console_mirror_removed(std::uint64_t title) {
        std::lock_guard lock(console_mirrors_mutex_);
        for (auto it = console_mirrors_.begin(); it != console_mirrors_.end(); ++it) {
            if (it->title != title) continue;
            if (it->armed && rt::xmem::page_watch_forget)
                rt::xmem::page_watch_forget(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(it->title)));
            if (it->armed) console_watch_ranges_.fetch_sub(1, std::memory_order_relaxed);
            console_mirror_bytes_.fetch_sub(it->size, std::memory_order_relaxed);
            console_mirrors_.erase(it);
            return;
        }
    }
    // The gate's cover condition: every mirrored range is guarded (no unmirrored console heap is visible here - the
    // device census hook named in docs/card_copies.md is what proves that, which is why GATE is readied, not enabled).
    bool watch_covers_mirrors() const {
        std::lock_guard lock(console_mirrors_mutex_);
        if (console_mirrors_.empty()) return false;
        for (const auto& mirror : console_mirrors_)
            if (!mirror.armed) return false;
        return console_watch_ranges() == console_mirrors_.size();
    }
    // The gate's staleness bound (PALMETTO_CARD_COPY_GATE_REFRESH): the n-th submission since the last refresh always
    // refreshes, so a write that landed while the guards were suspended cannot stay unmirrored longer than n
    // submissions.
    bool gate_allows_refresh_skip() {
        const std::uint64_t since =
            gate_submissions_since_refresh_.fetch_add(1, std::memory_order_relaxed) + 1;
        return card_copy_gate_refresh_ == 0 || since < card_copy_gate_refresh_;
    }

    // The queue's per-submission mirror step (docs/card_copies.md). `shader_submission` says whether any list of the
    // submission records a command that launches a shader - the only commands that can read the mirror. With every
    // switch off this is exactly sync_console_heaps(), with no clock read and no tracker call. Returns the measured
    // microseconds the refresh took, 0 when it was skipped.
    std::uint64_t sync_console_heaps_measured(bool shader_submission) {
        title_pull_begin_submission();  // docs/title_memory_model.md: a new submission's coalescing window
        const bool watch = card_copy_watch_ && console_watch_available() && console_watch_ranges() > 0;
        bool refresh = true;
        bool unchanged = false;
        if (watch) {
            const auto verdict = rt::xmem::page_watch_poll();
            unchanged = verdict == rt::xmem::WriteWatchPoll::unchanged;
            if (card_copy_gate_ && unchanged && watch_covers_mirrors() && gate_allows_refresh_skip()) refresh = false;
        } else if (card_copy_trace_ && console_watch_available() && console_watch_ranges() > 0) {
            // The measurement arm also keeps the tracker's guard state coherent if a caller armed it without WATCH.
            (void)rt::xmem::page_watch_poll();
        }
        const bool no_shader_skip = refresh && shader_submission == false && card_copy_skip_no_shader_;
        if (no_shader_skip) refresh = false;
        if (!refresh) {
            heap_refresh_skipped_.fetch_add(1, std::memory_order_relaxed);
            if (unchanged) heap_refresh_skipped_unchanged_.fetch_add(1, std::memory_order_relaxed);
            else heap_refresh_skipped_no_shader_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        // A full refresh reads every mirrored page, and every guarded page would trip: take the guards off around it.
        if (watch && rt::xmem::page_watch_suspend) rt::xmem::page_watch_suspend();
        std::uint64_t microseconds = 0;
        if (card_copy_trace_) {
            const auto started = std::chrono::steady_clock::now();
            sync_console_heaps();
            microseconds = static_cast<std::uint64_t>(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
        } else {
            sync_console_heaps();
        }
        if (watch && rt::xmem::page_watch_resume) rt::xmem::page_watch_resume();
        heap_refresh_calls_.fetch_add(1, std::memory_order_relaxed);
        heap_refresh_us_.fetch_add(microseconds, std::memory_order_relaxed);
        std::uint64_t max = heap_refresh_max_us_.load(std::memory_order_relaxed);
        while (microseconds > max && !heap_refresh_max_us_.compare_exchange_weak(max, microseconds)) {
        }
        gate_submissions_since_refresh_.store(0, std::memory_order_relaxed);
        return microseconds;
    }
    // The boot's milestone report of the mirror path (docs/card_copies.md section 4), empty when no switch is on.
    std::string card_copy_report() const {
        if (!card_copy_trace_ && !card_copy_watch_) return {};
        const std::uint64_t calls = heap_refresh_calls_.load(std::memory_order_relaxed);
        const std::uint64_t us = heap_refresh_us_.load(std::memory_order_relaxed);
        const std::uint64_t skipped = heap_refresh_skipped_.load(std::memory_order_relaxed);
        char text[512];
        std::snprintf(text, sizeof text,
                      "mirror %zu range(s) %.1f MB, tracker %u range(s)/%.1f MB | refreshed %llu, skipped %llu "
                      "(unchanged %llu, no-shader %llu) | %.0f us/refresh, %.1f ms total, max %.0f us | page watch "
                      "changed %llu, read-trip %llu",
                      console_mirror_ranges(), double(console_mirror_bytes()) / (1 << 20),
                      console_watch_ranges(), double(console_watch_pages()) * 4096.0 / (1 << 20),
                      static_cast<unsigned long long>(calls), static_cast<unsigned long long>(skipped),
                      static_cast<unsigned long long>(heap_refresh_skipped_unchanged_.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(heap_refresh_skipped_no_shader_.load(std::memory_order_relaxed)),
                      calls ? double(us) / double(calls) : 0.0, double(us) / 1000.0,
                      double(heap_refresh_max_us_.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(page_write_hits()), static_cast<unsigned long long>(page_read_hits()));
        return text;
    }

    // ---- docs/title_memory_model.md: the same-submission title-page pull coalescer ---------------------------------
    // The draw's page pull (docs/page_content.md) copies the title's placed-buffer bytes into the arena stand-in before
    // the submission reads them: measured 131,072 pulls / 273.0 MiB over a 300 s boot, about 2 KiB per draw at the
    // frontier this slice measured. The frame records its two draws in one submission and both name the same full-screen
    // geometry, so the second draw's pull can repeat ranges the first already copied. Skipping that repeat is the
    // measured first step of the peers' copy model (docs/title_memory_model.md section 3). The safe rule is deliberately
    // narrow: a pull is skipped only when *this same submission* already copied every byte of it (TitlePullWindows, the
    // include-only policy above), so nothing here has to know whether a page changed - no write watch, no guard pages,
    // no cross-submission state, and every switch is off by default.
    //   PALMETTO_TITLE_PULL_TRACE=1     the measurement arm: counts requests, copied bytes, copies, the ranges COALESCE
    //                                   would skip, and the ranges the previous submission also asked for (the
    //                                   cross-submission locality the guard-page cache would need). Copy path unchanged.
    //   PALMETTO_TITLE_PULL_COALESCE=1  the implemented step: skip a pull this submission already copied. Values change
    //                                   only when the frame's draws really name the same bytes.
    bool title_pull_trace() const { return title_pull_trace_; }
    bool title_pull_coalesce() const { return title_pull_coalesce_; }
    // Starts a submission's coalescing window: the previous submission's windows are kept for the TRACE line, the rest
    // restarts empty. Called once per submission before the lists are recorded; with both switches off it is one
    // branch and no state change.
    void title_pull_begin_submission() {
        if (!title_pull_trace_ && !title_pull_coalesce_) return;
        {
            std::lock_guard lock(title_pull_mutex_);
            title_pull_previous_ = title_pull_windows_;
            title_pull_windows_.reset();
        }
        const std::uint64_t submission = title_pull_submissions_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (submission % 2048 == 0) std::fprintf(stderr, "[rt] replay: %s\n", title_pull_report().c_str());
    }
    // The decision for one pull. False: copy it (and the range joins this submission's window). True: every byte is
    // already copied in this submission and COALESCE is on, so the copy is skipped. With only TRACE on it never answers
    // true; the counters say what COALESCE would have saved.
    bool title_pull_decide(std::uint64_t va, std::uint64_t bytes) {
        if (!title_pull_trace_ && !title_pull_coalesce_) return false;
        title_pull_requests_.fetch_add(1, std::memory_order_relaxed);
        title_pull_requested_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        std::lock_guard lock(title_pull_mutex_);
        if (title_pull_previous_.covers(va, bytes)) {
            title_pull_repeat_previous_.fetch_add(1, std::memory_order_relaxed);
            title_pull_repeat_previous_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        }
        if (title_pull_windows_.covers(va, bytes)) {
            // The measurement arm counts the repeat; the coalescing arm takes it.
            title_pull_repeats_.fetch_add(1, std::memory_order_relaxed);
            title_pull_repeat_bytes_.fetch_add(bytes, std::memory_order_relaxed);
            if (title_pull_coalesce_) {
                title_pull_skips_.fetch_add(1, std::memory_order_relaxed);
                title_pull_skipped_bytes_.fetch_add(bytes, std::memory_order_relaxed);
                return true;
            }
            return false;
        }
        title_pull_windows_.add(va, bytes);
        return false;
    }
    // The copy path's cost, so the A/B can report microseconds per copied pull next to the counts.
    void title_pull_copied(std::uint64_t microseconds) {
        if (!title_pull_trace_ && !title_pull_coalesce_) return;
        title_pull_copies_.fetch_add(1, std::memory_order_relaxed);
        title_pull_us_.fetch_add(microseconds, std::memory_order_relaxed);
        std::uint64_t max = title_pull_max_us_.load(std::memory_order_relaxed);
        while (microseconds > max && !title_pull_max_us_.compare_exchange_weak(max, microseconds)) {
        }
    }
    // The milestone report of the pull path (docs/title_memory_model.md section 4), empty with both switches off.
    std::string title_pull_report() const {
        if (!title_pull_trace_ && !title_pull_coalesce_) return {};
        const std::uint64_t requests = title_pull_requests_.load(std::memory_order_relaxed);
        const std::uint64_t requested = title_pull_requested_bytes_.load(std::memory_order_relaxed);
        const std::uint64_t copies = title_pull_copies_.load(std::memory_order_relaxed);
        const std::uint64_t skips = title_pull_skips_.load(std::memory_order_relaxed);
        const std::uint64_t skipped = title_pull_skipped_bytes_.load(std::memory_order_relaxed);
        const std::uint64_t repeats = title_pull_repeats_.load(std::memory_order_relaxed);
        const std::uint64_t repeat_bytes = title_pull_repeat_bytes_.load(std::memory_order_relaxed);
        const std::uint64_t previous = title_pull_repeat_previous_.load(std::memory_order_relaxed);
        const std::uint64_t previous_bytes = title_pull_repeat_previous_bytes_.load(std::memory_order_relaxed);
        const std::uint64_t us = title_pull_us_.load(std::memory_order_relaxed);
        char text[512];
        std::snprintf(text, sizeof text,
                      "title pulls: %llu request(s)/%.1f MiB, copied %llu, skipped %llu/%.1f MiB, repeated in "
                      "submission %llu/%.1f MiB, previous-submission repeat %llu/%.1f MiB | %.0f us/copy, %.1f ms "
                      "total, max %.0f us | coalesce %d",
                      static_cast<unsigned long long>(requests), double(requested) / (1 << 20),
                      static_cast<unsigned long long>(copies), static_cast<unsigned long long>(skips),
                      double(skipped) / (1 << 20), static_cast<unsigned long long>(repeats),
                      double(repeat_bytes) / (1 << 20), static_cast<unsigned long long>(previous),
                      double(previous_bytes) / (1 << 20), copies ? double(us) / double(copies) : 0.0,
                      double(us) / 1000.0, double(title_pull_max_us_.load(std::memory_order_relaxed)),
                      int(title_pull_coalesce_));
        return text;
    }

    // Copies the title's shader-visible console descriptor heaps into their arena mirrors and rebases the V# bases the
    // title wrote there. Set by the device; called before a queue records a submission's lists.
    void set_console_heap_sync(std::function<void()> sync) {
        std::unique_lock lock(resource_mutex_);
        console_heap_sync_ = std::move(sync);
    }
    void sync_console_heaps() {
        if (console_heap_sync_) console_heap_sync_();
    }
    // The title's page content (docs/page_content.md): the replay asks the device to copy the bytes behind a title VA
    // into the arena range that stands in for it, to pull the buffer data a descriptor table's V#s name, and to pull a
    // copy's source range by its arena VA (the byte range a copy is about to read), before the GPU reads them.
    void set_console_page_pull(std::function<void(std::uint64_t, std::uint64_t)> pages,
                               std::function<void(std::uint64_t, std::uint32_t)> table,
                               std::function<void(std::uint64_t, std::uint64_t)> arena = {}) {
        std::unique_lock lock(resource_mutex_);
        console_page_pull_ = std::move(pages);
        console_table_pull_ = std::move(table);
        console_arena_pull_ = std::move(arena);
    }

    // ---- Title pages a command names directly (docs/vertex_buffer_va.md) ---------------------------------------
    // The stage-3 renderer streams vertex and index data into committed pages of its own and binds the ranges by the VA
    // it placed them at. There is no D3D12 resource for those pages, so no creation path registered them and the
    // arena's title-VA map does not know the range. Measured on the refused draw: the vertex view's VA is MEM_COMMIT,
    // PAGE_READWRITE, all 288 pages of its 0x120000 bytes readable, inside a 0x44a00000 title allocation; by that point
    // the GPU address arena is full (a 36 MiB slot-133 stand-in was refused with E_OUTOFMEMORY in the same boot), so an
    // arena stand-in cannot serve the span. The measured answer is the import the slot-133 path already uses whenever
    // its pages can be imported: a VkBuffer over the title's own pages at the title VA the command names, so the CPU's
    // writes are the GPU's reads and no arena room is needed. Only the device can make that import (host-pointer
    // alignment, Vulkan handles); this side decides which spans to ask for and resolves the command arguments to it.
    struct ImportedTitleBuffer {
        std::uint64_t va = 0;     // first title VA of the span
        std::uint64_t size = 0;   // bytes
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;  // byte offset in `buffer` of the byte at `va`
    };
    using TitlePageImporter =
        std::function<bool(std::uint64_t va, std::uint64_t bytes, VkBuffer& buffer, VkDeviceSize& offset,
                           const char*& why)>;
    using TitleVaWindowAllocator = TitlePageImporter;
    using TitleVaWindowBufferResolver =
        std::function<bool(std::uint64_t va, std::uint64_t bytes, VkBuffer& buffer, VkDeviceSize& offset)>;
    void set_title_page_importer(TitlePageImporter importer) {
        std::unique_lock lock(resource_mutex_);
        title_page_importer_ = std::move(importer);
    }
    // Opt-in device-address windows are distinct from host-pointer imports: the device creates a copy-backed buffer and
    // registers its raw title-VA alias before returning. `buffer_at` uses the resolver for direct command views, while
    // ConsoleVaMap rewrites root/table/V# addresses to the returned heap-relative BDA.
    void set_title_va_window_allocator(TitleVaWindowAllocator allocator) {
        std::unique_lock lock(resource_mutex_);
        title_va_window_allocator_ = std::move(allocator);
    }
    void set_title_va_window_buffer_resolver(TitleVaWindowBufferResolver resolver) {
        std::unique_lock lock(resource_mutex_);
        title_va_window_buffer_resolver_ = std::move(resolver);
    }
    // Called by the submitting queue thread once per submission, before the lists are prepared (and before the shared
    // resource lock is taken): every unresolved buffer VA is offered either to the legacy host-pointer importer or to
    // the opt-in outside-arena window allocator. Window requests coalesce overlapping views, but remain bounded by the
    // policy table; unsupported immediate writes are left unresolved rather than writing only the mirror.
    std::size_t register_title_command_buffers(std::span<const CommandList* const> lists) {
        const bool window_mode = bool(title_va_window_allocator_);
        if (!window_mode && !title_page_importer_) return 0;

        std::vector<std::pair<std::uint64_t, std::uint64_t>> requests;
        requests.reserve(16);
        std::array<std::pair<std::uint64_t, std::uint64_t>, title_buffers::kMaxRanges> window_writes{};
        std::size_t window_write_count = 0;
        bool window_write_overflow = false;
        for (const CommandList* list : lists) {
            if (!list) continue;
            if (requests.size() < title_buffers::kMaxRanges) title_buffers::collect_requests(*list, requests);
            if (!window_mode) continue;
            for (const Command& command : list->commands()) {
                const auto* write = std::get_if<WriteImmediate>(&command);
                if (!write) continue;
                if (window_write_count < window_writes.size())
                    window_writes[window_write_count++] = {write->address, 4};
                else
                    window_write_overflow = true;
            }
        }

        if (window_mode) {
            // One alias must name one unambiguous byte mapping. Merge exact/overlapping views into their smallest union;
            // a view that would need more than the policy's 4 MiB window is refused by the allocator, not guessed.
            std::sort(requests.begin(), requests.end());
            std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
            merged.reserve(requests.size());
            for (const auto& request : requests) {
                const auto [va, bytes] = request;
                const bool valid = va && bytes && bytes <= UINT64_MAX - va;
                bool joined = false;
                if (valid && !merged.empty()) {
                    auto& previous = merged.back();
                    const bool previous_valid = previous.first && previous.second &&
                                                previous.second <= UINT64_MAX - previous.first;
                    if (previous_valid) {
                        const std::uint64_t previous_end = previous.first + previous.second;
                        const std::uint64_t request_end = va + bytes;
                        if (va <= previous_end) {
                            previous.second = std::max(previous_end, request_end) - previous.first;
                            joined = true;
                        }
                    }
                }
                if (!joined) merged.push_back(request);
            }
            requests.swap(merged);
        }
        if (!window_mode && requests.empty()) return 0;

        std::unique_lock lock(resource_mutex_);
        if (window_mode) {
            title_va_window_write_ranges_ = window_writes;
            title_va_window_write_count_ = window_write_count;
            title_va_window_write_overflow_ = window_write_overflow;
        }
        if (requests.empty()) return 0;

        std::size_t imported = 0;
        for (const auto& [va, bytes] : requests) {
            {
                // An earlier submission (or an earlier command in this one) already imported a range holding this VA:
                // buffer_at resolves it from that range, so there is nothing to do.
                std::uint64_t imported_size = bytes;
                VkBuffer imported_buffer = VK_NULL_HANDLE;
                VkDeviceSize imported_offset = 0;
                if (imported_title_range(va, imported_size, imported_buffer, imported_offset)) continue;
            }
            std::uint64_t rebased = 0;
            if (window_mode) {
                std::uint64_t available = 0;
                if (console_vas_.rebase_span(va, palmetto::ConsoleVaKind::buffer, rebased, available)) {
                    if (bytes > available) note_title_window_refusal(va, bytes, "alias_view_out_of_bounds");
                    continue;  // a registered title range; never create an overlapping second stand-in
                }
            } else if (console_vas_.rebase(va, rebased, palmetto::ConsoleVaKind::buffer)) {
                continue;  // a registered title range
            }
            BufferVaIndex<ResourceId>::Hit hit{};
            if (buffer_vas_.find(va, hit)) continue;  // an ordinary or arena buffer, or an earlier import

            if (window_mode) {
                bool write_overlap = window_write_overflow;
                const bool valid = va && bytes && bytes <= UINT64_MAX - va;
                if (!write_overlap && valid) {
                    const std::uint64_t end = va + bytes;
                    for (std::size_t i = 0; i < window_write_count; ++i) {
                        const auto [write_va, write_bytes] = window_writes[i];
                        if (write_va && write_bytes && write_bytes <= UINT64_MAX - write_va &&
                            va < write_va + write_bytes && write_va < end) {
                            write_overlap = true;
                            break;
                        }
                    }
                }
                if (write_overlap) {
                    note_title_window_refusal(va, bytes, window_write_overflow ? "write_range_cap" :
                                                                          "write_buffer_immediate_unsupported");
                    continue;
                }
                if (!valid) {
                    note_title_window_refusal(va, bytes, "range_overflow");
                    continue;
                }
                VkBuffer buffer = VK_NULL_HANDLE;
                VkDeviceSize offset = 0;
                const char* why = nullptr;
                if (!title_va_window_allocator_(va, bytes, buffer, offset, why) || buffer == VK_NULL_HANDLE) {
                    note_title_window_refusal(va, bytes, why && *why ? why : "window_allocator_refused");
                    continue;
                }
                ++imported;
                static std::atomic<unsigned> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                    std::fprintf(stderr,
                                 "[rt] replay: title VA window registered: raw VA 0x%llx + 0x%llx (outside-arena "
                                 "device backing; root/table/V# addresses use its heap-relative BDA)\n",
                                 static_cast<unsigned long long>(va), static_cast<unsigned long long>(bytes));
                continue;
            }

            if (const char* why = title_buffers::refusal(va, bytes, imported_title_ranges(),
                                                          imported_title_bytes())) {
                note_title_import_refusal(va, bytes, why);
                continue;
            }
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            const char* why = nullptr;
            if (!title_page_importer_(va, bytes, buffer, offset, why) || buffer == VK_NULL_HANDLE) {
                note_title_import_refusal(va, bytes, why && *why ? why : "the host import refused the pages");
                continue;
            }
            {
                std::lock_guard lock(imported_title_mutex_);
                imported_title_buffers_.push_back({va, bytes, buffer, offset});
                imported_title_bytes_.fetch_add(bytes, std::memory_order_relaxed);
            }
            ++imported;
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr,
                             "[rt] replay: title pages imported at first use: VA 0x%llx + 0x%llx (a command names the "
                             "title's own memory; the buffer is those pages, no arena stand-in)\n",
                             static_cast<unsigned long long>(va), static_cast<unsigned long long>(bytes));
        }
        return imported;
    }
    // Called by a queue once per submission, right after register_title_command_buffers (docs/vertex_buffer_va.md
    // section 10): when that call imported title pages at first use, this submission holds the stage-3 content draws,
    // and the next two submissions are armed with it (the content is submitted over several frames). While armed, the
    // replay arms a colour-target readback on every draw the submission holds; the presenter capture's fixed schedule
    // misses the presents that follow the content submits, so this readback is how the content frame is read.
    // PALMETTO_READBACK_AFTER widens the window (section 13) so later stage-3 content frames are read too.
    void title_import_capture_tick(std::size_t imported) {
        if (imported) title_import_capture_pending_.store(title_import_capture_window(), std::memory_order_relaxed);
        const unsigned pending = title_import_capture_pending_.load(std::memory_order_relaxed);
        title_import_capture_active_.store(pending != 0, std::memory_order_relaxed);
        if (pending) title_import_capture_pending_.fetch_sub(1, std::memory_order_relaxed);
    }
    // True while the submission being prepared is inside the content-capture window (section 13's narrow pacing arm).
    bool title_import_capture_active() const {
        return title_import_capture_active_.load(std::memory_order_relaxed);
    }
    // The byte range of an imported title buffer that holds `va`, cut to the range's end.
    bool imported_title_range(std::uint64_t va, std::uint64_t& size, VkBuffer& buffer, VkDeviceSize& offset) const {
        std::lock_guard lock(imported_title_mutex_);
        for (const auto& range : imported_title_buffers_) {
            if (va < range.va || va - range.va >= range.size) continue;
            const std::uint64_t at = va - range.va;
            buffer = range.buffer;
            offset = range.offset + at;
            size = std::min<std::uint64_t>(size, range.size - at);
            return true;
        }
        return false;
    }
    void note_title_import_refusal(std::uint64_t va, std::uint64_t bytes, const char* why) {
        if (title_import_refusals_.fetch_add(1, std::memory_order_relaxed) < 8)
            std::fprintf(stderr,
                         "[rt] replay: title pages a command names stay refused: VA 0x%llx + 0x%llx (%s)\n",
                         static_cast<unsigned long long>(va), static_cast<unsigned long long>(bytes), why);
    }
    void note_title_window_refusal(std::uint64_t va, std::uint64_t bytes, const char* why) {
        if (title_window_refusals_.fetch_add(1, std::memory_order_relaxed) < 8)
            std::fprintf(stderr,
                         "[rt] replay: outside-arena title VA window refused: VA 0x%llx + 0x%llx (%s)\n",
                         static_cast<unsigned long long>(va), static_cast<unsigned long long>(bytes), why);
    }
    // Called with a console descriptor heap's mirror (a shader-visible V# the title wrote can name a title VA): a base
    // inside an imported range would be added to heap_base by the shader and reach a wild device address, so the draws
    // stay refused rather than fault. Only the ranges this slice imported are scanned; the console-VA map's own patch
    // already ran.
    void note_imported_title_descriptors(const std::uint8_t* bytes, std::size_t size) {
        if (!bytes || size < console_va_descriptor_size) return;
        std::lock_guard lock(imported_title_mutex_);
        if (imported_title_buffers_.empty()) return;
        for (std::size_t at = 0; at + console_va_descriptor_size <= size; at += console_va_descriptor_size) {
            const auto* dwords = reinterpret_cast<const std::uint32_t*>(bytes + at);
            const std::uint64_t base =
                std::uint64_t(dwords[0]) | (std::uint64_t(dwords[1] & 0xffffu) << 32);
            if (base < title_buffers::kArenaWindowEnd) continue;
            for (const auto& range : imported_title_buffers_) {
                if (base < range.va || base - range.va >= range.size) continue;
                import_conflict_.store(true, std::memory_order_relaxed);
                static std::atomic<unsigned> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 3)
                    std::fprintf(stderr,
                                 "[rt] replay: a title descriptor names imported title pages (VA 0x%llx); draws stay "
                                 "refused rather than add heap_base to a title VA\n",
                                 static_cast<unsigned long long>(base));
                return;
            }
        }
    }
    std::size_t imported_title_ranges() const {
        std::lock_guard lock(imported_title_mutex_);
        return imported_title_buffers_.size();
    }
    std::uint64_t imported_title_bytes() const { return imported_title_bytes_.load(std::memory_order_relaxed); }
    std::uint64_t imported_title_hits() const { return imported_title_hits_.load(std::memory_order_relaxed); }

    // Diagnostic (docs/page_content.md): read the first draw's colour target back after its submission, so the frame's
    // own bytes can be compared with what the present blits. Armed once per boot; the image's layout is restored.
    // Diagnostic (docs/frame_capture.md): arm one colour-target readback. Up to four can be armed for one submission, so
    // the frame's even intermediate pass and its odd six-image pass are copied together and printed with their draw
    // indices and resource ids.
    void arm_attachment_capture(VkImage image, VkFormat format, VkExtent2D extent, ResourceId resource,
                                std::uint64_t draw) {
        if (image == VK_NULL_HANDLE || attachment_capture_count_ >= std::size(attachment_captures_)) return;
        AttachmentCapture& capture = attachment_captures_[attachment_capture_count_++];
        capture.image = image;
        capture.format = format;
        capture.extent = extent;
        capture.resource = resource;
        capture.draw = draw;
        capture_armed_.store(true, std::memory_order_relaxed);
    }
    // Diagnostic (docs/frame_capture.md): with PALMETTO_FRAME_DUMP_CONTROL set, the queue draws the project's own
    // known-good full-screen triangle (the test/glsl vs_tri + fs_solid pair, compiled to the SPIR-V files the env
    // names) into the armed draw's colour target, after the frame's commands and before the one-shot readback copies
    // that target. The target reading back in the triangle's colour proves the raster, target binding, viewport,
    // subresource and readback path for it; reading back black points at the replay's raster path instead. Creates the
    // pipeline once, for `format`; false when the files cannot be loaded or the pipeline cannot be created.
    bool frame_dump_control_pipeline(VkFormat format) {
        std::lock_guard lock(frame_dump_control_mutex_);
        if (frame_dump_control_pipeline_.pipeline) {
            if (frame_dump_control_pipeline_.color_formats.size() == 1 &&
                frame_dump_control_pipeline_.color_formats[0] == format)
                return true;
            std::fprintf(stderr,
                         "[rt] replay: frame dump control: target format %d differs from the pipeline's %d; not drawn\n",
                         static_cast<int>(format),
                         frame_dump_control_pipeline_.color_formats.empty()
                             ? -1
                             : static_cast<int>(frame_dump_control_pipeline_.color_formats[0]));
            return false;
        }
        if (frame_dump_control_failed_) return false;
        const auto load = [](const char* name, std::vector<std::uint32_t>& words) {
            const char* path = std::getenv(name);
            if (!path || !*path) return false;
            std::FILE* file = std::fopen(path, "rb");
            if (!file) return false;
            std::uint32_t word = 0;
            while (std::fread(&word, sizeof(word), 1, file) == 1) words.push_back(word);
            std::fclose(file);
            return words.size() >= 5 && words[0] == 0x07230203u;  // SPIR-V magic
        };
        std::vector<std::uint32_t> vs_words, fs_words;
        if (!load("PALMETTO_FRAME_DUMP_CONTROL_VS", vs_words) ||
            !load("PALMETTO_FRAME_DUMP_CONTROL_PS", fs_words)) {
            frame_dump_control_failed_ = true;
            std::fprintf(stderr,
                         "[rt] replay: frame dump control: PALMETTO_FRAME_DUMP_CONTROL_VS/PS cannot be read (the "
                         "build's vs_tri.spv/fs_solid.spv); no control draw\n");
            return false;
        }
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = vs_words.size() * sizeof(std::uint32_t);
        module_info.pCode = vs_words.data();
        if (vk.vkCreateShaderModule(device_, &module_info, nullptr, &frame_dump_control_vs_) != VK_SUCCESS) {
            frame_dump_control_failed_ = true;
            return false;
        }
        module_info.codeSize = fs_words.size() * sizeof(std::uint32_t);
        module_info.pCode = fs_words.data();
        if (vk.vkCreateShaderModule(device_, &module_info, nullptr, &frame_dump_control_fs_) != VK_SUCCESS) {
            frame_dump_control_failed_ = true;
            return false;
        }
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(ShaderUserData)};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vk.vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout) != VK_SUCCESS) {
            frame_dump_control_failed_ = true;
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = frame_dump_control_vs_;
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frame_dump_control_fs_;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;  // the triangle's winding is not what this control measures
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        depth.stencilTestEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable = VK_FALSE;
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        // Everything the replay's draw path sets dynamically (replay::emit), so the pipeline accepts those calls.
        constexpr VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,  VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamic_states));
        dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport_state;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vk.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            vk.vkDestroyPipelineLayout(device_, layout, nullptr);
            frame_dump_control_failed_ = true;
            std::fprintf(stderr, "[rt] replay: frame dump control: the control pipeline could not be created\n");
            return false;
        }
        frame_dump_control_pipeline_.pipeline = pipeline;
        frame_dump_control_pipeline_.layout = layout;
        frame_dump_control_pipeline_.push_stages = push.stageFlags;
        frame_dump_control_pipeline_.color_formats = {format};
        frame_dump_control_pipeline_.samples = 1;
        frame_dump_control_pipeline_.vertex_slots = 0;
        frame_dump_control_pipeline_.has_pixel_stage = true;
        std::fprintf(stderr,
                     "[rt] replay: frame dump control: built the vs_tri/fs_solid pipeline for format %d; its draws "
                     "write 32 160 240\n",
                     static_cast<int>(format));
        return true;
    }
    // Diagnostic (docs/frame_capture.md): the shim's descriptor-table probe for the frame dump's sampled-texture
    // readback (a draw's table keys and the layer image of its first T#). Registered once by the device; read only by
    // the env-gated diagnostic.
    void set_sampled_table_probe(std::function<bool(std::uint64_t, std::uint32_t, SampledTable&)> probe) {
        replay_.sampled_table = std::move(probe);
    }
    // Diagnostic (docs/frame_capture.md): the shim's reader of the title's own memory, for the frame dump's constants
    // read-through. Reads only; registered once by the device.
    void set_title_memory_reader(
        std::function<bool(std::uint64_t, std::uint32_t, std::uint8_t*, std::uint32_t&)> reader) {
        replay_.read_title_memory = std::move(reader);
    }
    // Diagnostic (docs/vertex_path.md): the shim's reader for a device address a translated shader builds
    // (heap_base + a push-constant value), so the fetch-slot dump can show what each descriptor fetch lands on. Reads
    // only.
    void set_device_memory_reader(
        std::function<bool(std::uint64_t, std::uint32_t, std::uint8_t*, std::uint32_t&)> reader) {
        replay_.read_device_memory = std::move(reader);
    }
    // Diagnostic (docs/vertex_path.md): the shim's writer for the console driver-block V# the geometry fix places in the
    // arena mirror. Opt-in only (PALMETTO_VERTEX_STREAM_DESCRIPTOR).
    void set_device_memory_writer(
        std::function<bool(std::uint64_t, std::uint32_t, const std::uint8_t*)> writer) {
        replay_.write_device_memory = std::move(writer);
    }
    // Diagnostic (docs/vertex_path.md): the shim's arena allocator for the per-pipeline driver block the geometry fix
    // emits in s0/s1. Opt-in only.
    void set_driver_block_allocator(std::function<bool(std::uint32_t, std::uint64_t&)> allocator) {
        replay_.allocate_driver_block = std::move(allocator);
    }
    // The heap-relative address of a buffer resolved by buffer_at (docs/vertex_buffer_va.md section 18.3): the arena
    // offset for the GPU address arena itself, false for any other buffer (a title-pages import at a device address the
    // shader's heap_base + value model cannot name).
    void set_buffer_arena_address(std::function<bool(VkBuffer, VkDeviceSize, std::uint64_t&)> address) {
        replay_.buffer_arena_address = std::move(address);
    }
    // A cached arena staging copy of a span of the title's own host memory, so a translated shader can fetch a vertex
    // stream the layer imported over the title's pages at first use (docs/vertex_buffer_va.md section 18.3).
    void set_title_span_stager(std::function<bool(std::uint64_t, std::uint64_t, std::uint64_t&)> stager) {
        replay_.stage_title_span = std::move(stager);
    }
    // Diagnostic (docs/frame_capture.md, the colour path): binds layer images for the T#s a shader fetches itself out of
    // its published matrix block. Opt-in (PALMETTO_TEXTURE_BIND_FETCHED).
    void set_fetched_texture_binder(std::function<void(std::uint64_t)> binder) {
        replay_.bind_fetched_textures = std::move(binder);
    }
    // Diagnostic (docs/frame_capture.md): the project's constant-output pixel shader (fs_solid), created once from the
    // SPIR-V file PALMETTO_FRAME_DUMP_CONTROL_PS names. Null when it cannot be read; the caller logs and skips.
    VkShaderModule frame_dump_solid_module() {
        std::lock_guard lock(frame_dump_control_mutex_);
        if (frame_dump_control_fs_) return frame_dump_control_fs_;
        if (frame_dump_control_failed_) return VK_NULL_HANDLE;
        const char* path = std::getenv("PALMETTO_FRAME_DUMP_CONTROL_PS");
        std::vector<std::uint32_t> words;
        if (path && *path) {
            std::FILE* file = std::fopen(path, "rb");
            if (file) {
                std::uint32_t word = 0;
                while (std::fread(&word, sizeof(word), 1, file) == 1) words.push_back(word);
                std::fclose(file);
            }
        }
        if (words.size() < 5 || words[0] != 0x07230203u) {
            frame_dump_control_failed_ = true;
            std::fprintf(stderr,
                         "[rt] replay: frame dump control: PALMETTO_FRAME_DUMP_CONTROL_PS cannot be read (the build's "
                         "fs_solid.spv); no PS-only control\n");
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = words.size() * sizeof(std::uint32_t);
        module_info.pCode = words.data();
        if (vk.vkCreateShaderModule(device_, &module_info, nullptr, &frame_dump_control_fs_) != VK_SUCCESS) {
            frame_dump_control_failed_ = true;
            return VK_NULL_HANDLE;
        }
        return frame_dump_control_fs_;
    }
    // Diagnostic (docs/frame_capture.md): the PS-only control's pipeline: `title` with only its pixel shader replaced
    // by fs_solid. False when the shim offers no with_ps, the module cannot be read, or the title recipe has no
    // fragment stage; the caller logs and skips the control.
    bool frame_dump_ps_pipeline(const GraphicsPipeline& title) {
        const VkShaderModule ps = frame_dump_solid_module();
        if (!ps) return false;
        if (!title.with_ps) {
            std::fprintf(stderr, "[rt] replay: frame dump PS control: the shim offers no substituted-shader pipeline\n");
            return false;
        }
        const VkPipeline pipeline = title.with_ps(ps, frame_dump::ps_control_cull_none());
        if (!pipeline) {
            std::fprintf(stderr,
                         "[rt] replay: frame dump PS control: the pipeline could not be built (no fragment stage?)\n");
            return false;
        }
        std::lock_guard lock(frame_dump_control_mutex_);
        GraphicsPipeline control = title;
        control.pipeline = pipeline;
        control.variant = nullptr;   // a control draw never needs the unbound-slot variants
        control.with_ps = nullptr;
        control.with_cull = nullptr;  // docs/vertex_buffer_va.md section 22: a control draw is never re-substituted
        frame_dump_ps_control_pipeline_ = std::move(control);
        return true;
    }
    // Diagnostic (docs/frame_capture.md): arm one of the sampled-texture readbacks (the armed draw's resolved sampled
    // images, up to max_sampled_keys). Copies only; the title's textures are never modified.
    void arm_sample_capture(VkImage image, VkFormat format, VkExtent2D extent, ResourceId resource) {
        if (sample_capture_count_ >= max_sampled_keys || image == VK_NULL_HANDLE) return;
        SampleCapture& capture = sample_captures_[sample_capture_count_++];
        capture.image = image;
        capture.format = format;
        capture.extent = extent;
        capture.resource = resource;
    }
    // The upscale stand-in's own graphics pipeline and descriptor set (docs/upscale_standin.md): a full-screen triangle
    // whose pixel stage texel-fetches the armed upscale draw's resolved colour image (set 0 binding 0) and writes the
    // armed draw's presented target, with the geometry in the first 64 bytes of the replay's push-constant block
    // (upscale_standin::Push). It needs no title shader, pipeline state or descriptor table, so it can present the
    // sampled colour even when the console upscaler's pipelines cannot be made. Diagnostic and opt-in: created once,
    // for the armed draw's target format; `colour_layout` is the layout the colour image is in for the draw (the layer
    // transitions it there before the draw and back after, so the tracker's state is unchanged). False (with one named
    // line) when the device cannot build it or a second, different colour image is handed to it.
    bool upscale_standin_ready(VkFormat target_format, VkImage colour, VkFormat colour_format,
                               VkImageLayout colour_layout) {
        std::lock_guard lock(upscale_standin_mutex_);
        if (upscale_standin_failed_) return false;
        if (colour == VK_NULL_HANDLE || colour_format == VK_FORMAT_UNDEFINED ||
            target_format == VK_FORMAT_UNDEFINED) {
            upscale_standin_failed_ = true;
            std::fprintf(stderr,
                         "[rt] replay: upscale stand-in: refused: colour image 0x%llx format %d, target format %d\n",
                         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(colour)),
                         static_cast<int>(colour_format), static_cast<int>(target_format));
            return false;
        }
        if (upscale_standin_set_ != VK_NULL_HANDLE) {
            if (upscale_standin_colour_image_ != colour || upscale_standin_colour_format_ != colour_format) {
                std::fprintf(stderr,
                             "[rt] replay: upscale stand-in: refused: the stand-in already samples image 0x%llx "
                             "(format %d); the armed draw names a different one\n",
                             static_cast<unsigned long long>(
                                 reinterpret_cast<std::uintptr_t>(upscale_standin_colour_image_)),
                             static_cast<int>(upscale_standin_colour_format_));
                return false;
            }
        } else {
            const VkDevice device = device_;
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = colour;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = colour_format;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vk.vkCreateImageView(device, &view_info, nullptr, &upscale_standin_view_) != VK_SUCCESS) {
                upscale_standin_failed_ = true;
                std::fprintf(stderr, "[rt] replay: upscale stand-in: the colour image's view could not be created\n");
                return false;
            }
            upscale_standin_colour_image_ = colour;
            upscale_standin_colour_format_ = colour_format;
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = 0;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            set_info.bindingCount = 1;
            set_info.pBindings = &binding;
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1};
            VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool_info.maxSets = 1;
            pool_info.poolSizeCount = 1;
            pool_info.pPoolSizes = &size;
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocate.descriptorSetCount = 1;
            if (vk.vkCreateDescriptorSetLayout(device, &set_info, nullptr, &upscale_standin_set_layout_) != VK_SUCCESS ||
                vk.vkCreateDescriptorPool(device, &pool_info, nullptr, &upscale_standin_pool_) != VK_SUCCESS) {
                upscale_standin_failed_ = true;
                std::fprintf(stderr, "[rt] replay: upscale stand-in: the descriptor set layout/pool failed\n");
                return false;
            }
            allocate.descriptorPool = upscale_standin_pool_;
            allocate.pSetLayouts = &upscale_standin_set_layout_;
            if (vk.vkAllocateDescriptorSets(device, &allocate, &upscale_standin_set_) != VK_SUCCESS) {
                upscale_standin_failed_ = true;
                std::fprintf(stderr, "[rt] replay: upscale stand-in: the descriptor set could not be allocated\n");
                return false;
            }
            const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                           sizeof(ShaderUserData)};
            VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout_info.setLayoutCount = 1;
            layout_info.pSetLayouts = &upscale_standin_set_layout_;
            layout_info.pushConstantRangeCount = 1;
            layout_info.pPushConstantRanges = &push;
            if (vk.vkCreatePipelineLayout(device, &layout_info, nullptr, &upscale_standin_layout_) != VK_SUCCESS) {
                upscale_standin_failed_ = true;
                std::fprintf(stderr, "[rt] replay: upscale stand-in: the pipeline layout could not be created\n");
                return false;
            }
            VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module_info.codeSize = sizeof(upscale_standin_vs_spirv);
            module_info.pCode = upscale_standin_vs_spirv;
            if (vk.vkCreateShaderModule(device, &module_info, nullptr, &upscale_standin_vs_) != VK_SUCCESS) {
                upscale_standin_failed_ = true;
                std::fprintf(stderr, "[rt] replay: upscale stand-in: the vertex module could not be created\n");
                return false;
            }
            module_info.codeSize = sizeof(upscale_standin_ps_spirv);
            module_info.pCode = upscale_standin_ps_spirv;
            if (vk.vkCreateShaderModule(device, &module_info, nullptr, &upscale_standin_ps_) != VK_SUCCESS) {
                upscale_standin_failed_ = true;
                std::fprintf(stderr, "[rt] replay: upscale stand-in: the pixel module could not be created\n");
                return false;
            }
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = upscale_standin_vs_;
            stages[0].pName = "main";
            stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = upscale_standin_ps_;
            stages[1].pName = "main";
            VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport_state.viewportCount = 1;
            viewport_state.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;  // a stand-in full-screen triangle covers whichever way it winds
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depth.depthTestEnable = VK_FALSE;
            depth.depthWriteEnable = VK_FALSE;
            depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            depth.stencilTestEnable = VK_FALSE;
            VkPipelineColorBlendAttachmentState blend_attachment{};
            blend_attachment.blendEnable = VK_FALSE;
            blend_attachment.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1;
            blend.pAttachments = &blend_attachment;
            constexpr VkDynamicState dynamic_states[] = {
                VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
                VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
                VK_DYNAMIC_STATE_STENCIL_REFERENCE,  VK_DYNAMIC_STATE_BLEND_CONSTANTS};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamic_states));
            dynamic.pDynamicStates = dynamic_states;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &target_format;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
            info.stageCount = 2;
            info.pStages = stages;
            info.pVertexInputState = &vertex_input;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewport_state;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &multisample;
            info.pDepthStencilState = &depth;
            info.pColorBlendState = &blend;
            info.pDynamicState = &dynamic;
            info.layout = upscale_standin_layout_;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vk.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
                upscale_standin_failed_ = true;
                std::fprintf(stderr, "[rt] replay: upscale stand-in: the pipeline could not be created for format %d\n",
                             static_cast<int>(target_format));
                return false;
            }
            upscale_standin_pipeline_.pipeline = pipeline;
            upscale_standin_pipeline_.layout = upscale_standin_layout_;
            upscale_standin_pipeline_.push_stages = push.stageFlags;
            upscale_standin_pipeline_.code_base = 0;
            upscale_standin_pipeline_.bindless_set = upscale_standin_set_;  // the marker emit binds set 0 for
            upscale_standin_pipeline_.color_formats = {target_format};
            upscale_standin_pipeline_.samples = 1;
            upscale_standin_pipeline_.vertex_slots = 0;
            upscale_standin_pipeline_.has_pixel_stage = true;
        }
        VkDescriptorImageInfo image_info{};
        image_info.imageView = upscale_standin_view_;
        image_info.imageLayout = colour_layout;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = upscale_standin_set_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &image_info;
        vk.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        // One line with the shape the boot reads: the image, its format, the target format and the sampling layout.
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 3)
            std::fprintf(stderr,
                         "[rt] replay: upscale stand-in: built the layer's stretch pipeline (target format %d) over "
                         "colour image 0x%llx (format %d) in layout %d\n",
                         static_cast<int>(target_format),
                         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(colour)),
                         static_cast<int>(colour_format), static_cast<int>(colour_layout));
        return true;
    }
    // The stand-in draw's descriptor set (set 0 of its own layout), for the queue's emit call.
    VkDescriptorSet upscale_standin_set() const { return upscale_standin_set_; }
    // A title-placed texture's page import (docs/texture_import.md): the device registers this, and every queue calls
    // it before recording a submission, so the image holds the title's bytes before anything can sample it.
    void set_placed_texture_import(std::function<void()> import) {
        std::unique_lock lock(resource_mutex_);
        placed_texture_import_ = std::move(import);
    }
    void import_placed_textures() {
        if (placed_texture_import_) placed_texture_import_();
    }
    // Deferred image backing (docs/streamed_image_backing.md): the device registers a hook that binds the memory of
    // any placed/streamed image still waiting for its first write or GPU use. Called before a submission's lists are
    // prepared and recorded, so an image is bound before anything can name it; a false answer refuses the submission
    // rather than recording a command that uses an image with no memory.
    void set_deferred_image_binder(std::function<bool()> binder) {
        std::unique_lock lock(resource_mutex_);
        deferred_image_binder_ = std::move(binder);
    }
    bool bind_deferred_images() {
        if (!deferred_image_binder_) return true;
        return deferred_image_binder_();
    }
    // Diagnostic (docs/texture_source.md): the page-writer watch for sampled placed textures, called once per
    // submission (the callback itself scans every 64th). Registered by the device; a no-op when empty.
    void set_page_watch_tick(std::function<void()> tick) {
        std::unique_lock lock(resource_mutex_);
        page_watch_tick_ = std::move(tick);
    }
    void tick_page_watch() {
        if (page_watch_tick_) page_watch_tick_();
    }
    // Deferred pipeline compilation (docs/deferred_compile.md): the device registers this; each queue calls it from
    // execute_now before the lists are prepared/recorded, so a pipeline a submission's dispatch names is ready before
    // the replay resolves its id. Called without resource_mutex_ held (the hook is where a waiting submission may
    // block, never the recording thread); a no-op when empty.
    void set_prepare_pipelines(std::function<void(std::span<const CommandList* const>)> prepare) {
        std::unique_lock lock(resource_mutex_);
        prepare_pipelines_ = std::move(prepare);
    }
    void prepare_pipelines(std::span<const CommandList* const> lists) {
        if (prepare_pipelines_) prepare_pipelines_(lists);
    }
    // ---- non-blocking submission hold (docs/peer_port_streaming.md) ----------------------------------------------
    //
    // The measured problem: the deferred compile moves the driver build off the title's threads, but the submission
    // that first names a still-unbuilt pipeline waits for it inside prepare_pipelines above - on the thread that
    // submits, a title thread. The peer audit ranks "never wait for a pipeline build on a title thread" first. The
    // queue's own fix is here: when the switch is on and a listed dispatch names a pipeline the compile queue has not
    // finished, the submission is held (the queue's held_ deque, with the lists copied and their pipeline states
    // pinned) instead of executed; it is replayed by the next submit/present on the submitting thread or by the
    // device's hold flusher as soon as every pipeline it names has finished, in submission order. `collect` answers
    // "does this batch name an unfinished deferred pipeline, and which ids"; `finished` answers "have all these ids
    // finished"; `wake` wakes the device's flusher. With the switch off nothing is registered and every call below is
    // one false branch: the default path is unchanged.
    using PipelineHoldCollectFn =
        std::function<bool(std::span<const CommandList* const>, std::vector<std::uintptr_t>&)>;
    using PipelineHoldFinishedFn = std::function<bool(std::span<const std::uintptr_t>)>;
    void set_pipeline_hold(PipelineHoldCollectFn collect, PipelineHoldFinishedFn finished, std::function<void()> wake,
                           const PipelineHoldSettings& settings) {
        std::unique_lock lock(resource_mutex_);
        pipeline_hold_collect_ = std::move(collect);
        pipeline_hold_finished_ = std::move(finished);
        pipeline_hold_wake_ = std::move(wake);
        pipeline_hold_.configure(settings);
        pipeline_hold_enabled_ = pipeline_hold_.enabled() && pipeline_hold_collect_ && pipeline_hold_finished_;
    }
    bool pipeline_hold_enabled() const { return pipeline_hold_enabled_; }
    // True when `lists` name at least one deferred pipeline the compile workers have not finished; `out` gets those ids.
    bool collect_pending_pipelines(std::span<const CommandList* const> lists, std::vector<std::uintptr_t>& out) const {
        if (!pipeline_hold_enabled_) return false;
        return pipeline_hold_collect_(lists, out);
    }
    // True when every one of `ids` has finished (built, failed or cancelled); an id the device does not know finished.
    bool pipelines_finished(std::span<const std::uintptr_t> ids) const {
        return !pipeline_hold_enabled_ || !pipeline_hold_finished_ || pipeline_hold_finished_(ids);
    }
    PipelineHoldPolicy& pipeline_hold() { return pipeline_hold_; }
    const PipelineHoldPolicy& pipeline_hold() const { return pipeline_hold_; }
    void wake_pipeline_hold() {
        if (pipeline_hold_enabled_ && pipeline_hold_wake_) pipeline_hold_wake_();
    }
    // The boot report's hold line; empty unless the switch is on.
    std::string pipeline_hold_report() const { return pipeline_hold_enabled_ ? pipeline_hold_.report() : std::string(); }
    // The arena span that stands in for a title span, and the reverse (arena span -> title span); used by the device's
    // page pull to find source and destination.
    bool console_va_span(std::uint64_t title, ConsoleVaKind kind, std::uint64_t& arena,
                         std::uint64_t& available) const {
        return console_vas_.rebase_span(title, kind, arena, available);
    }
    bool console_arena_span(std::uint64_t arena, std::uint64_t& title, std::uint64_t& available) const {
        return console_vas_.title_span(arena, title, available);
    }
    void count_console_pages(std::uint64_t bytes) { console_vas_.count_pulled(bytes); }
    std::uint64_t console_pulled_calls() const { return console_vas_.pulled_calls(); }
    std::uint64_t console_pulled_bytes() const { return console_vas_.pulled_bytes(); }

    // Physical-device COLOR_ATTACHMENT format check for reinterpreted UAV clears.
    void set_color_attachment_format(std::function<bool(VkFormat)> check) {
        std::unique_lock lock(resource_mutex_);
        replay_.color_attachment_format = std::move(check);
    }
    // The arena's device address, read by the device's replay callbacks (the device-memory reader/writer that
    // d3d12_vulkan_device.cpp hands to the replay). Those callbacks run from prepare, i.e. inside execute()'s shared
    // resource lock, so this read must take no lock of its own: a nested shared acquire on the same thread blocks behind
    // any queued exclusive writer (Windows SRW locks are writer-preferring), and that writer is waiting for the outer
    // shared lock - the whole-process deadlock docs/memory_growth.md records. Lock-free is also the honest statement of
    // the invariant: set_heap_base() runs once from device init, before any queue or submission exists, so there is one
    // writer before every reader.
    std::uint64_t heap_base() const { return heap_base_.load(std::memory_order_acquire); }

    // Pipelines are registered by an opaque id (the PSO object) for as long as the PSO lives.
    void add_pipeline(std::uintptr_t id, const ComputePipeline& pipeline) {
        std::unique_lock lock(resource_mutex_);
        pipelines_[id] = pipeline;
    }
    void remove_pipeline(std::uintptr_t id) {
        std::unique_lock lock(resource_mutex_);
        pipelines_.erase(id);
        graphics_pipelines_.erase(id);
    }
    // The live-pipeline budget (pipeline_budget.h) rebuilds a pipeline state's VkPipeline on a bind after an eviction;
    // the replay reads the handle from its own copy of the entry, so the rebuilt handle is written back here.
    void set_pipeline_handle(std::uintptr_t id, VkPipeline pipeline, bool graphics_pipeline) {
        std::unique_lock lock(resource_mutex_);
        if (graphics_pipeline) {
            if (const auto it = graphics_pipelines_.find(id); it != graphics_pipelines_.end())
                it->second.pipeline = pipeline;
        } else if (const auto it = pipelines_.find(id); it != pipelines_.end()) {
            it->second.pipeline = pipeline;
        }
    }
    // Diagnostic (docs/memory_growth.md): a one-line census of the live-pipeline budget, printed at the bounded
    // submission milestones. Unset: the milestones print exactly what they printed before.
    void set_pipeline_census(std::function<std::string()> census) { pipeline_census_ = std::move(census); }
    std::string pipeline_census() const { return pipeline_census_ ? pipeline_census_() : std::string(); }
    void add_graphics_pipeline(std::uintptr_t id, const GraphicsPipeline& pipeline) {
        {
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr,
                             "[rt] replay: graphics pipeline registered id 0x%llx (%zu colours, depth %d)\n",
                             static_cast<unsigned long long>(id),
                             static_cast<unsigned long long>(pipeline.color_formats.size()),
                             pipeline.depth_format != VK_FORMAT_UNDEFINED ? 1 : 0);
        }
        std::unique_lock lock(resource_mutex_);
        graphics_pipelines_[id] = pipeline;
    }
    // Configure before queue creation, when the selected queue family supports compute.
    void set_occlusion_resolve_memory(const VkPhysicalDeviceMemoryProperties& memory) {
        occlusion_resolve_memory_ = memory;
    }

    // For tests and traces.
    VkImageLayout layout(ResourceId id, std::uint32_t subresource) {
        std::lock_guard lock(tracker_mutex_);
        return tracker_.layout(id, subresource);
    }

    // Called from a COM/title thread (view creation), never from a replay callback: execute() already holds the shared
    // resource lock there, so a nested shared acquire would block behind a queued writer (heap_base()).
    bool has_resource(ResourceId id) {
        std::shared_lock lock(resource_mutex_);
        return resources_.find(id) != resources_.end();
    }

    // For tests (port/test/pipeline_recipe_cache_test.cpp's replay-accessor lock-order section): run `body` while the
    // resource lock is held shared, the way execute() holds it for a whole replay. A test that calls a replay-side
    // accessor from `body` while another thread queues an exclusive writer proves the accessor takes no lock of its own -
    // the pattern that deadlocked the default boot (docs/memory_growth.md). Not used by the runtime.
    template<typename Body> void with_resource_shared_for_test(Body&& body) {
        std::shared_lock lock(resource_mutex_);
        body();
    }

    bool ray_tracing_supported() const { return ray_tracing_ && vk.vkCmdBuildAccelerationStructuresKHR != nullptr; }
    bool xdxr_acceleration_structures() const { return xdxr_ != nullptr; }

    // Whether a build's inner nodes are BOX16 (64 bytes, half boxes rounded outward) instead of BOX32: bottom-level
    // builds with MINIMIZE_MEMORY (VK LOW_MEMORY). A top-level build keeps BOX32: it is in world space, where half
    // precision would make boxes metres too large a few kilometres from the origin. The flags of an update must match
    // its build's (D3D12), so prebuild info, the build and its refits agree. WARNING: an update that breaks that rule
    // (MINIMIZE_MEMORY on one and not the other) is refitted with the wrong layout and writes past the structure; it
    // cannot be detected when the command is recorded.
    static bool xdxr_box16(const BuildAccelerationStructure& b) {
        return !b.is_tlas && (b.build_flags & VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR) != 0;
    }
    // Whether a bottom-level build pairs sibling triangles into TRI0/TRI1 leaves: not when the structure may be
    // updated (ALLOW_UPDATE), since a refit rewrites the leaves one triangle each and would leave every TRI1 child
    // reading wrong vertices. xdxr_check refuses an update without ALLOW_UPDATE for that reason. Pairing changes
    // neither the layout nor the tree, so prebuild info does not depend on it.
    static bool xdxr_pair(const BuildAccelerationStructure& b) {
        return !b.is_tlas && !b.update && (b.build_flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) == 0;
    }
    // Triangles (BLAS) or instances (TLAS) a build has: what XdxrLayout is computed from, for the build and for
    // GetRaytracingAccelerationStructurePrebuildInfo.
    static std::uint32_t xdxr_elements(const BuildAccelerationStructure& b) {
        if (b.is_tlas) return b.instance_count;
        std::uint64_t n = 0;
        for (const auto& t : b.triangles) n += (t.index_type ? t.index_count : t.vertex_count) / 3;
        return n > 0xffffffffu ? 0xffffffffu : std::uint32_t(n);
    }
    // The geometries as the builder reads them, with `base` added to their VAs; false for an input it cannot read
    // (a vertex format other than R32G32B32_FLOAT or R32G32_FLOAT, an index type other than none/R16/R32).
    static bool xdxr_inputs(const BuildAccelerationStructure& b, std::vector<XdxrTriangles>* out, std::uint64_t base) {
        if (b.is_tlas) return true;
        for (const auto& t : b.triangles) {
            if (t.vertex_format != VK_FORMAT_R32G32B32_SFLOAT && t.vertex_format != VK_FORMAT_R32G32_SFLOAT) return false;
            if (t.index_type > 2) return false;
            const std::uint32_t count = (t.index_type ? t.index_count : t.vertex_count) / 3;
            if (count && !t.vertex_va) return false;
            if (count && t.index_type && !t.index_va) return false;
            if (!out) continue;
            XdxrTriangles g;
            g.vertices = t.vertex_va ? base + t.vertex_va : 0;
            g.vertex_stride = t.vertex_stride;
            g.xy_only = t.vertex_format == VK_FORMAT_R32G32_SFLOAT;
            g.indices = t.index_type && t.index_va ? base + t.index_va : 0;
            g.index_type = t.index_type;
            g.triangle_count = count;
            g.transform = t.transform_va ? base + t.transform_va : 0;
            g.flags = t.flags | (t.opaque ? 1u : 0u);
            out->push_back(g);
        }
        return true;
    }

    void register_acceleration_structure(std::uint64_t va, VkAccelerationStructureKHR as) {
        if (!va || !as) return;
        std::unique_lock lock(resource_mutex_);
        acceleration_structures_[va] = as;
    }

    void unregister_acceleration_structure(std::uint64_t va) {
        if (!va) return;
        std::unique_lock lock(resource_mutex_);
        acceleration_structures_.erase(va);
    }

    // Called from a COM/title thread (a resource returning its structure handle), never from a replay callback: the
    // replay's own lookup (replay_.resolve_acceleration_structure) reads the map lock-free under execute()'s shared lock.
    VkAccelerationStructureKHR resolve_acceleration_structure(std::uint64_t va) {
        std::shared_lock lock(resource_mutex_);
        const auto it = acceleration_structures_.find(va);
        return it == acceleration_structures_.end() ? VK_NULL_HANDLE : it->second;
    }

    // `prepare` is called at the start of the command buffer of every ExecuteCommandLists, before the lists are
    // written: the device brings the bindless set 0 of the translated shaders up to date there (vulkan_bindless.h)
    // and records what the set needs first. When it fails, nothing of that call is executed.
    void set_transfer_only_family(std::uint32_t family) { transfer_only_family_ = family; }

    void set_bindless_prepare(std::function<bool(VkCommandBuffer)> prepare) {
        std::unique_lock lock(resource_mutex_);
        bindless_prepare_ = std::move(prepare);
    }

    // D24_UNORM_S8_UINT stored as D32: copies of its depth plane run a compute pass (vulkan_depth24.h). `allocate`
    // gives the scratch buffers device-local memory. Without this such a copy is refused as unsupported.
    void enable_depth24(PFN_vkGetDeviceProcAddr get, Depth24Converter::Allocate allocate,
                       std::uint64_t max_storage_range = 1u << 27) {
        std::unique_lock lock(resource_mutex_);
        depth24_ = std::make_unique<Depth24Converter>(get, device_, std::move(allocate), max_storage_range);
        if (!depth24_->loaded()) {
            depth24_.reset();
            return;
        }
        replay_.depth24_prepare = [this](Depth24Job& job, std::vector<Depth24Job>& parts) { return depth24_->prepare(job, parts); };
        replay_.depth24_run = [this](VkCommandBuffer command_buffer, const Depth24Job& job) {
            depth24_->run(command_buffer, job);
        };
    }

    // Copies the replay stages through a buffer (depth <-> colour, vulkan_replay.h) take device-local scratch from
    // `allocate`. Without this such a copy is refused as unsupported.
    void enable_copy_scratch(PFN_vkGetDeviceProcAddr get, ScratchBuffers::Allocate allocate) {
        std::unique_lock lock(resource_mutex_);
        scratch_ = std::make_unique<ScratchBuffers>(get, device_, std::move(allocate));
        if (!scratch_->loaded()) scratch_.reset();
    }

    // Called after that command buffer's submit: true when it was accepted, false when vkQueueSubmit failed. The
    // set keeps its upload pending in the second case, because the copy never reached the GPU.
    void set_bindless_commit(std::function<void(bool)> commit) {
        std::unique_lock lock(resource_mutex_);
        bindless_commit_ = std::move(commit);
    }

    // The set 0 the commands of this ExecuteCommandLists bind, read after `prepare` succeeded and under the same
    // locks (a flush may have made a new set). Without it the pipelines' own set is bound.
    void set_bindless_current(std::function<VkDescriptorSet()> current) {
        std::unique_lock lock(resource_mutex_);
        bindless_current_ = std::move(current);
    }

    void set_bindless_release(DeferredDeletionQueue::BindlessReleaseFn fn, void* user) {
        deletion_queue_.set_bindless_release(fn, user);
    }

    // The ticket of the latest submission: everything submitted so far is at or below it, and a resource that any
    // of that work may use can be freed once completed_retire_ticket() has reached it. Tickets run over all queues,
    // each recorded with its queue's timeline value, so this waits for every queue's timeline to pass what was
    // submitted to it, and never makes a queue wait.
    std::uint64_t submitted_retire_ticket() const { return submitted_ticket_.load(std::memory_order_acquire); }

    template<typename VkHandle>
    void defer_destroy(DeferredDeletionQueue::Kind kind, VkHandle handle) {
        const auto raw = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
        if (!raw) return;
        std::lock_guard lock(submit_mutex_);
        const std::uint64_t safe_ticket = submitted_ticket_.load(std::memory_order_acquire) + 1;
        deletion_queue_.push(safe_ticket, kind, raw);
    }

    void defer_destroy(DeferredDeletionQueue::Kind kind, std::uint64_t raw_handle) {
        if (!raw_handle && kind != DeferredDeletionQueue::Kind::bindless_slot) return;
        std::lock_guard lock(submit_mutex_);
        const std::uint64_t safe_ticket = submitted_ticket_.load(std::memory_order_acquire) + 1;
        deletion_queue_.push(safe_ticket, kind, raw_handle);
    }

    // Waits (not for the device: for the queues' timelines) until every submission up to `ticket` is done, then
    // returns the completed ticket. For a caller whose list of retired objects has grown past what it will hold.
    std::uint64_t wait_retire_ticket(std::uint64_t ticket) {
        std::vector<std::pair<VkSemaphore, std::uint64_t>> pending;
        {
            std::lock_guard lock(submit_mutex_);
            if (is_device_lost()) return 0;
            pending = retire_tickets_.pending_through(ticket);
        }
        if (!pending.empty()) {
            std::vector<VkSemaphore> semaphores;
            std::vector<std::uint64_t> values;
            for (const auto& [semaphore, value] : pending) {
                semaphores.push_back(semaphore);
                values.push_back(value);
            }
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = std::uint32_t(semaphores.size());
            wait.pSemaphores = semaphores.data();
            wait.pValues = values.data();
            vk.vkWaitSemaphores(device_, &wait, UINT64_MAX);  // outside submit_mutex_: a signal may need it
        }
        return completed_retire_ticket();
    }

    // The bindless flush of this ExecuteCommandLists started a new set generation, whose table upload is in its
    // command buffer: called from the `prepare` callback (tracker_mutex_ held), so execute() makes every other
    // queue's next submission wait for this one, which binds the new set.
    void note_bindless_upload() { bindless_upload_pending_ = true; }

    std::uint64_t completed_retire_ticket() {
        std::lock_guard lock(submit_mutex_);
        if (is_device_lost()) return 0;
        return retire_tickets_.completed([this](VkSemaphore semaphore, std::uint64_t value) {
            std::uint64_t done = 0;
            return vk.vkGetSemaphoreCounterValue(device_, semaphore, &done) == VK_SUCCESS && done >= value;
        });
    }

    void drain_deleted_resources(std::uint64_t current_ticket) {
        deletion_queue_.drain(current_ticket, device_, vk);
    }

    // Both methods are called with submit_mutex_ held. A queue's own progress timeline is
    // monotonic even when another queue completes first; global tickets are retired in order.
    void record_submit_locked(VkSemaphore progress, std::uint64_t value) {
        const auto ticket = submitted_ticket_.fetch_add(1, std::memory_order_acq_rel) + 1;
        retire_tickets_.record(ticket, progress, value);
    }
    void retire_queue_idle_locked(VkSemaphore progress) {
        retire_tickets_.queue_idle(progress);
    }

    VulkanDeviceFunctions vk;

    // Vulkan requires external synchronisation of a VkQueue; every D3D12 queue of the device may share one.
    std::mutex& submit_mutex() { return submit_mutex_; }

    void mark_device_lost() noexcept {
        device_lost_.store(true, std::memory_order_release);
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 3)
            std::fprintf(stderr, "[rt] replay: the Vulkan device was lost\n");
        if (fence_dispatcher_) {
            try {
                fence_dispatcher_->notify_device_lost();
            } catch (...) {
            }
        }
    }
    bool is_device_lost() const noexcept { return device_lost_.load(std::memory_order_acquire); }

    void set_signal_event_fn(FenceWaitDispatcher::SignalEventFn fn) {
        if (fence_dispatcher_) fence_dispatcher_->set_signal_event_fn(fn);
    }
    void enqueue_fence_wait(VkSemaphore sem, std::uint64_t value, void* event_handle) {
        if (fence_dispatcher_) fence_dispatcher_->enqueue(sem, value, event_handle);
    }
    void enqueue_fence_callback(VkSemaphore sem, std::uint64_t value, std::function<void()> cb) {
        if (fence_dispatcher_) fence_dispatcher_->enqueue_callback(sem, value, std::move(cb));
    }
    void cancel_fence_waits(VkSemaphore sem) {
        if (fence_dispatcher_) fence_dispatcher_->cancel(sem);
    }

    // Cross-queue Wait-before-Signal: per fence, the highest value a Signal was submitted for, and the queues that
    // hold batches until one is (queue_holds.h).
    QueueHolds<VkSemaphore>& holds() { return holds_; }

private:
    friend class VulkanQueue;
    friend class VulkanPresenter;

    // buffer_at finds a buffer by VA: one with no image, a VkBuffer, a VA and bytes (what the linear search it replaced
    // accepted). Both are called with resource_mutex_ held uniquely.
    static bool addressable(const ResourceBinding& b) { return !b.image && b.buffer && b.va && b.size; }
    void unindex_buffer(ResourceId id, const ResourceBinding& b) {
        if (addressable(b)) buffer_vas_.erase(b.va, id);
    }
    // resources_[id] = binding, with the VA index kept in step (a re-registered ID drops its old range first).
    void set_resource_locked(ResourceId id, const ResourceBinding& binding) {
        auto [it, inserted] = resources_.try_emplace(id, binding);
        if (addressable(binding)) {
            try {
                buffer_vas_.insert(binding.va, binding.size, id);
            } catch (...) {
                if (inserted) resources_.erase(it);
                throw;
            }
        }
        if (!inserted) {
            // A same-VA insert updates its size in place; removing the old key would remove the new range.
            if (addressable(it->second) && (!addressable(binding) || it->second.va != binding.va))
                unindex_buffer(id, it->second);
            it->second = binding;
        }
    }

    const ResourceBinding* resolve(ResourceId id) const {
        const auto it = resources_.find(id);
        return it == resources_.end() ? nullptr : &it->second;
    }

    // Called from prepare; protected by views_mutex_ so concurrent shared_lock readers are safe.
    VkImageView view(const ResourceBinding& r, const VkImageSubresourceRange& range) {
        std::lock_guard lock(views_mutex_);
        const auto key = std::make_tuple(r.image, range.aspectMask, range.baseMipLevel, range.baseArrayLayer,
                                         range.layerCount, r.format);
        if (const auto it = views_.find(key); it != views_.end()) return it->second;
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = r.image;
        info.viewType = r.line ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY;  // 1D images take no 2D view
        info.format = r.format;
        info.subresourceRange = range;
        // Attachment views need only color-attachment usage. Inheriting STORAGE usage from a mutable/extended
        // resource (typeless views such as RGBA8 sRGB, or R32_UINT-backed R9G9B9E5 UAV images) would require unrelated usages.
        VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (r.typeless || (range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && r.color_attachment))
            info.pNext = &usage;
        VkImageView view = VK_NULL_HANDLE;
        if (vk.vkCreateImageView(device_, &info, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
        views_[key] = view;
        return view;
    }

    // The render pass and framebuffer that resolve one layer of a depth-only multisampled image into one layer of
    // another: views `from` and `to` (from view()), in the layouts given. Used where the dynamic-rendering resolve
    // reads the wrong source layer (D3D-417; vulkan_replay.h says when). Both objects are cached: a pass by what it
    // describes, a framebuffer by its images and views, until remove_resource retires either image.
    VkFramebuffer depth_resolve_framebuffer(const ResourceBinding& source, VkImageView from, VkImageLayout from_layout,
                                            const ResourceBinding& target, VkImageView to, VkImageLayout to_layout,
                                            VkResolveModeFlagBits mode, VkExtent2D extent, VkRenderPass& pass) {
        std::lock_guard lock(views_mutex_);
        const auto pass_key = std::make_tuple(source.format, source.samples, from_layout, to_layout, mode);
        if (const auto it = resolve_passes_.find(pass_key); it != resolve_passes_.end()) {
            pass = it->second;
        } else {
            VkAttachmentDescription2 attachments[2]{};
            for (auto& attachment : attachments) {
                attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
                attachment.format = source.format;
                attachment.samples = VK_SAMPLE_COUNT_1_BIT;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;      // the source is read; the destination keeps
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // what is outside the render area
                attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // depth-only formats
                attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            }
            attachments[0].samples = static_cast<VkSampleCountFlagBits>(source.samples);
            attachments[0].initialLayout = attachments[0].finalLayout = from_layout;
            attachments[1].initialLayout = attachments[1].finalLayout = to_layout;
            const VkAttachmentReference2 from_reference{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0,
                                                        from_layout, VK_IMAGE_ASPECT_DEPTH_BIT};
            const VkAttachmentReference2 to_reference{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, to_layout,
                                                      VK_IMAGE_ASPECT_DEPTH_BIT};
            VkSubpassDescriptionDepthStencilResolve resolve{
                VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
            resolve.depthResolveMode = mode;
            resolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
            resolve.pDepthStencilResolveAttachment = &to_reference;
            VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
            subpass.pNext = &resolve;
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.pDepthStencilAttachment = &from_reference;
            VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
            info.attachmentCount = 2;
            info.pAttachments = attachments;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            if (vk.vkCreateRenderPass2(device_, &info, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
            resolve_passes_[pass_key] = pass;
        }
        const auto key = std::make_tuple(source.image, target.image, from, to, pass);
        if (const auto it = resolve_framebuffers_.find(key); it != resolve_framebuffers_.end()) return it->second;
        const VkImageView views[] = {from, to};
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = pass;
        info.attachmentCount = 2;
        info.pAttachments = views;
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (vk.vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) return VK_NULL_HANDLE;
        resolve_framebuffers_[key] = framebuffer;
        return framebuffer;
    }

    VkDevice device_;
    bool loaded_ = false;
    bool viewport_scissor_count_enabled_ = false;
    bool ray_tracing_ = false;
    std::unique_ptr<XdxrBuilder> xdxr_;   // set: acceleration structures are XDXR BVH4 records
    std::atomic<bool> device_lost_{false};
    // The arena's device address, as a lock-free copy of replay_.heap_base. The replay's device-memory callbacks read it
    // from inside execute()'s shared resource lock, so it cannot go through resource_mutex_ (heap_base()).
    std::atomic<std::uint64_t> heap_base_{0};
    alignas(64) std::shared_mutex resource_mutex_;  // shared for prepare lookups, unique for add/remove
    alignas(64) std::mutex tracker_mutex_;
    alignas(64) std::mutex views_mutex_;
    alignas(64) std::mutex submit_mutex_;
    RetireTickets<VkSemaphore> retire_tickets_;
    std::atomic<std::uint64_t> submitted_ticket_{0};
    DeferredDeletionQueue deletion_queue_;
    std::unique_ptr<FenceWaitDispatcher> fence_dispatcher_;
    QueueHolds<VkSemaphore> holds_;
    std::unordered_map<ResourceId, ResourceBinding> resources_;
    BufferVaIndex<ResourceId> buffer_vas_;  // the addressable buffers of resources_, by VA range (buffer_at)
    // The title's own VAs and the arena ranges that stand in for them (console_va_map.h, docs/page_address_space.md).
    // A resource or heap adds its range when it is created and removes it when it is destroyed.
    ConsoleVaMap console_vas_;
    std::function<void()> console_heap_sync_;  // the device's copy of its console heaps' bytes into their mirrors
    // docs/card_copies.md: the mirrored console descriptor-heap title ranges (registered with add_console_va_range),
    // how many the write tracker guards, and the refresh counters the milestone report prints.
    struct ConsoleMirror {
        std::uint64_t title = 0;
        std::uint64_t size = 0;
        bool armed = false;
    };
    mutable std::mutex console_mirrors_mutex_;
    std::vector<ConsoleMirror> console_mirrors_;
    std::atomic<std::uint64_t> console_mirror_bytes_{0};
    std::atomic<std::uint32_t> console_watch_ranges_{0};
    bool card_copy_trace_ = false;          // PALMETTO_CARD_COPY_TRACE: measure the refresh (behaviour unchanged)
    bool card_copy_skip_no_shader_ = false; // PALMETTO_CARD_COPY_SKIP_NO_SHADER: no refresh for a shader-free submission
    bool card_copy_watch_ = false;          // PALMETTO_CARD_COPY_WATCH: arm and poll the guard-page write tracker
    bool card_copy_gate_ = false;           // PALMETTO_CARD_COPY_GATE: skip when the tracker says unchanged
    unsigned card_copy_gate_refresh_ = 0;   // PALMETTO_CARD_COPY_GATE_REFRESH: force a refresh every n submissions
    std::atomic<std::uint64_t> gate_submissions_since_refresh_{0};
    std::atomic<std::uint64_t> heap_refresh_calls_{0}, heap_refresh_skipped_{0}, heap_refresh_skipped_unchanged_{0},
        heap_refresh_skipped_no_shader_{0}, heap_refresh_us_{0}, heap_refresh_max_us_{0};
    // docs/title_memory_model.md: the same-submission title-page pull coalescer. The switches are read once per process;
    // the windows are per submission and written from the replay (under a leaf mutex: two queues may prepare at once).
    std::mutex title_pull_mutex_;
    TitlePullWindows title_pull_windows_;   // ranges this submission has copied
    TitlePullWindows title_pull_previous_;  // the previous submission's, for the TRACE line only
    bool title_pull_trace_ = false;         // PALMETTO_TITLE_PULL_TRACE: count the pulls (copy path unchanged)
    bool title_pull_coalesce_ = false;      // PALMETTO_TITLE_PULL_COALESCE: skip a range this submission already copied
    std::atomic<std::uint64_t> title_pull_submissions_{0}, title_pull_requests_{0}, title_pull_requested_bytes_{0},
        title_pull_copies_{0}, title_pull_skips_{0}, title_pull_skipped_bytes_{0}, title_pull_repeats_{0},
        title_pull_repeat_bytes_{0}, title_pull_repeat_previous_{0}, title_pull_repeat_previous_bytes_{0},
        title_pull_us_{0}, title_pull_max_us_{0};
    std::function<void(std::uint64_t, std::uint64_t)> console_page_pull_;   // title pages -> arena stand-in
    std::function<void(std::uint64_t, std::uint32_t)> console_table_pull_;  // a table's V# data -> arena stand-in
    std::function<void()> placed_texture_import_;                           // title-placed texture pages -> image
    std::function<bool()> deferred_image_binder_;  // deferred image backing, before a submission records
    std::function<void()> page_watch_tick_;  // docs/texture_source.md: the page-writer watch, once per submission
    std::function<void(std::span<const CommandList* const>)> prepare_pipelines_;  // docs/deferred_compile.md
    // Non-blocking submission hold (docs/peer_port_streaming.md): registered only when PALMETTO_PIPELINE_HOLD is set.
    // The collect/finished hooks read the device's deferred-compile task map; the wake hook starts the device's hold
    // flusher so a held submission is completed off the title's thread as soon as its pipelines are built.
    PipelineHoldCollectFn pipeline_hold_collect_;
    PipelineHoldFinishedFn pipeline_hold_finished_;
    std::function<void()> pipeline_hold_wake_;
    mutable PipelineHoldPolicy pipeline_hold_;
    bool pipeline_hold_enabled_ = false;
    std::function<std::string()> pipeline_census_;  // docs/memory_growth.md: the live-pipeline budget's counters
    std::function<void(std::uint64_t, std::uint64_t)> console_arena_pull_;  // a copy's source range -> arena stand-in
    // Title pages a command names directly (docs/vertex_buffer_va.md): the device imports the title's own pages as a
    // VkBuffer at first use; these are those buffers and their byte counts. `imported_title_mutex_` is taken after
    // resource_mutex_ (registration) and alone from the descriptor patch, so the lock order is consistent.
    TitlePageImporter title_page_importer_;
    TitleVaWindowAllocator title_va_window_allocator_;
    TitleVaWindowBufferResolver title_va_window_buffer_resolver_;
    std::array<std::pair<std::uint64_t, std::uint64_t>, title_buffers::kMaxRanges> title_va_window_write_ranges_{};
    std::size_t title_va_window_write_count_ = 0;
    bool title_va_window_write_overflow_ = false;
    mutable std::mutex imported_title_mutex_;
    std::vector<ImportedTitleBuffer> imported_title_buffers_;
    std::atomic<std::uint64_t> imported_title_bytes_{0};
    std::atomic<std::uint64_t> imported_title_hits_{0};
    std::atomic<unsigned> title_import_refusals_{0};
    std::atomic<unsigned> title_window_refusals_{0};
    std::atomic<bool> import_conflict_{false};  // a title descriptor named an imported range: draws stay refused
    // The content-frame readback trigger (docs/vertex_buffer_va.md section 10): set for the submission whose lists
    // imported title pages at first use and the two after it, so the replay's draw readback arms on the content draws.
    std::atomic<unsigned> title_import_capture_pending_{0};
    std::atomic<bool> title_import_capture_active_{false};
    // The one-shot colour-target readbacks of docs/page_content.md (up to four per submission, docs/frame_capture.md):
    // armed by the draws PALMETTO_READBACK_AT_DRAW lists and taken after that submission completes.
    AttachmentCapture attachment_captures_[4];
    unsigned attachment_capture_count_ = 0;
    std::atomic<bool> capture_armed_{false};
    // The frame dump's known-good pipeline control (docs/frame_capture.md): the project's vs_tri + fs_solid pipeline,
    // its modules and its own leaf mutex. Created once, for the armed draw's target format; diagnostic only.
    GraphicsPipeline frame_dump_control_pipeline_;
    VkShaderModule frame_dump_control_vs_ = VK_NULL_HANDLE;
    VkShaderModule frame_dump_control_fs_ = VK_NULL_HANDLE;
    bool frame_dump_control_failed_ = false;
    std::mutex frame_dump_control_mutex_;
    // The PS-only control (docs/frame_capture.md): the armed title pipeline with fs_solid as its pixel shader, and the
    // sampled-texture readback's one-shot arm. Both diagnostic only.
    GraphicsPipeline frame_dump_ps_control_pipeline_;
    // The upscale stand-in (docs/upscale_standin.md): the layer's own stretch pipeline, its shader modules, its image
    // view and its descriptor set. Created once when the switch is on and an upscale draw is measured.
    GraphicsPipeline upscale_standin_pipeline_;
    VkShaderModule upscale_standin_vs_ = VK_NULL_HANDLE;
    VkShaderModule upscale_standin_ps_ = VK_NULL_HANDLE;
    VkImageView upscale_standin_view_ = VK_NULL_HANDLE;
    VkImage upscale_standin_colour_image_ = VK_NULL_HANDLE;
    VkFormat upscale_standin_colour_format_ = VK_FORMAT_UNDEFINED;
    VkDescriptorSetLayout upscale_standin_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool upscale_standin_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet upscale_standin_set_ = VK_NULL_HANDLE;
    VkPipelineLayout upscale_standin_layout_ = VK_NULL_HANDLE;
    bool upscale_standin_failed_ = false;
    std::mutex upscale_standin_mutex_;
    // The sampled-texture readbacks (docs/frame_capture.md): up to max_sampled_keys one-shot copies, armed per resolved
    // sampled T# while the armed submission is prepared and consumed in that same submission.
    SampleCapture sample_captures_[max_sampled_keys];
    unsigned sample_capture_count_ = 0;
    std::unordered_map<ResourceId, std::shared_ptr<SparseResource>> sparse_resources_;
    std::unordered_map<std::uintptr_t, ComputePipeline> pipelines_;
    std::unordered_map<std::uintptr_t, GraphicsPipeline> graphics_pipelines_;
    std::unordered_map<std::uint64_t, VkAccelerationStructureKHR> acceleration_structures_;
    LayoutTracker tracker_;
    std::mutex storage_mutex_;  // a leaf lock: guards storage_pending_ and zero_pending_ only
    std::unordered_set<ResourceId> storage_pending_;
    struct PendingZero {
        ResourceId id;
        VkBuffer buffer;
    };
    std::vector<PendingZero> zero_pending_;  // zero_on_next_submit
    // The last submission that initialized storage images (its queue's progress semaphore and ticket) and a counter
    // that says which one: every other queue's next submission waits for it, so no queue uses such an image before
    // the zero-fill that put it in GENERAL has run.
    VkSemaphore storage_init_semaphore_ = VK_NULL_HANDLE;
    std::uint64_t storage_init_value_ = 0;
    std::uint64_t storage_init_version_ = 0;
    std::map<std::tuple<VkImage, VkImageAspectFlags, std::uint32_t, std::uint32_t, std::uint32_t, VkFormat>, VkImageView> views_;
    // depth_resolve_framebuffer's caches, under views_mutex_ like views_.
    std::map<std::tuple<VkFormat, std::uint32_t, VkImageLayout, VkImageLayout, VkResolveModeFlagBits>, VkRenderPass>
        resolve_passes_;
    std::map<std::tuple<VkImage, VkImage, VkImageView, VkImageView, VkRenderPass>, VkFramebuffer> resolve_framebuffers_;
    std::function<bool(VkCommandBuffer)> bindless_prepare_;
    std::function<VkDescriptorSet()> bindless_current_;
    bool bindless_upload_pending_ = false;  // tracker_mutex_
    // The last submission that carried a new generation's table upload (its queue's progress semaphore and value), and
    // a counter that says which one: every other queue's next submission waits for it (storage_mutex_).
    VkSemaphore bindless_upload_semaphore_ = VK_NULL_HANDLE;
    std::uint64_t bindless_upload_value_ = 0;
    std::uint64_t bindless_upload_version_ = 0;
    std::unique_ptr<Depth24Converter> depth24_;
    std::unique_ptr<ScratchBuffers> scratch_;  // copies staged through a buffer (depth <-> colour)
    std::function<void(bool)> bindless_commit_;
    std::unordered_map<QueryHeapId, QueryPool> query_pools_;  // element pointers stay valid across inserts
    double timestamp_period_ = 0;
    bool order_timestamp_copies_ = false;
    VkPhysicalDeviceMemoryProperties occlusion_resolve_memory_{};
    ReplayFunctions replay_;
    std::unique_ptr<PredicateReducer> predicate_reducer_;
    std::unique_ptr<PredicatedBufferCopier> predicated_copier_;
    std::unique_ptr<IndirectDispatchCounter> counted_dispatch_;
    std::uint32_t predicate_family_ = UINT32_MAX;
    std::uint32_t transfer_only_family_ = UINT32_MAX;
    DebugLabelFunctions labels_;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// A Vulkan timeline semaphore with D3D12 fence semantics for the host side.
class VulkanFence {
public:
    VulkanFence(VulkanContext& context, std::uint64_t initial) : context_(context) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = initial;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        if (context.vk.vkCreateSemaphore(context.device(), &info, nullptr, &semaphore_) != VK_SUCCESS)
            semaphore_ = VK_NULL_HANDLE;
        if (semaphore_) context.holds().add_fence(semaphore_, initial);
    }
    ~VulkanFence() {
        if (!semaphore_) return;
        context_.holds().remove_fence(semaphore_);  // batches held on it can never run: dropped and reported
        context_.vk.vkDestroySemaphore(context_.device(), semaphore_, nullptr);
    }
    VulkanFence(const VulkanFence&) = delete;
    VulkanFence& operator=(const VulkanFence&) = delete;

    VkSemaphore semaphore() const { return semaphore_; }

    std::uint64_t completed() const {
        if (context_.is_device_lost()) return UINT64_MAX;
        std::uint64_t value = 0;
        const VkResult result = context_.vk.vkGetSemaphoreCounterValue(context_.device(), semaphore_, &value);
        if (result == VK_ERROR_DEVICE_LOST) {
            context_.mark_device_lost();
            return UINT64_MAX;
        }
        if (result != VK_SUCCESS) return 0;
        return value;
    }

    // ID3D12Fence::Signal from the CPU. Vulkan timelines only increase: a value at or below the current one is
    // ignored (D3D12 would set it), and the caller is told.
    bool signal(std::uint64_t value) {
        // The dispatcher may be testing a failed counter query. Do not call completed() here:
        // it marks the context lost on a query error and prevents the dispatcher from waking.
        if (context_.is_device_lost()) return false;
        std::uint64_t current = 0;
        if (context_.vk.vkGetSemaphoreCounterValue(context_.device(), semaphore_, &current) == VK_SUCCESS &&
            value <= current)
            return false;
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = semaphore_;
        info.value = value;
        const VkResult result = context_.vk.vkSignalSemaphore(context_.device(), &info);
        if (result == VK_ERROR_DEVICE_LOST) context_.mark_device_lost();
        if (result != VK_SUCCESS) return false;
        timeline::line("cpu fence signal v=%llu", static_cast<unsigned long long>(value));
        context_.holds().signaled(semaphore_, value);  // queues that held a batch for this value run it now
        context_.holds().pump_ready();
        return true;
    }

    // true when the value was reached within the timeout.
    bool wait(std::uint64_t value, std::uint64_t timeout_ns) const {
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1;
        info.pSemaphores = &semaphore_;
        info.pValues = &value;
        const double started = timeline::seconds();
        const VkResult result = context_.vk.vkWaitSemaphores(context_.device(), &info, timeout_ns);
        if (result == VK_ERROR_DEVICE_LOST) context_.mark_device_lost();
        timeline::line("fence wait v=%llu ok=%d us=%.0f", static_cast<unsigned long long>(value),
                       result == VK_SUCCESS ? 1 : 0, (timeline::seconds() - started) * 1e6);
        return result == VK_SUCCESS;
    }

private:
    VulkanContext& context_;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
};

// docs/card_copies.md: whether any list of a submission records a command that can launch a shader. Only a shader can
// read a console descriptor heap through its arena mirror, so a submission of barriers, clears, copies, queries and
// markers only cannot read it and does not need the per-submission title-memory refresh. Draw, Dispatch and
// ExecuteIndirect cover every draw/dispatch path (indirect ones launch their shaders from the argument buffer), and a
// raytracing build reads descriptors it was handed, so it counts too.
inline bool submission_launches_a_shader(std::span<const CommandList* const> lists) {
    for (const CommandList* list : lists) {
        if (!list) continue;
        for (const Command& command : list->commands())
            if (std::holds_alternative<Draw>(command) || std::holds_alternative<Dispatch>(command) ||
                std::holds_alternative<ExecuteIndirect>(command) ||
                std::holds_alternative<BuildAccelerationStructure>(command))
                return true;
    }
    return false;
}

// ---- Submission pacing knobs (PALMETTO_PACE_IDLE / PALMETTO_PACE_DELAY_US / PALMETTO_PACE_CONTENT) --------------
// docs/vertex_buffer_va.md section 12: the renderer stop is a CPU-side lifetime race in the title's job/command
// graph, not a layer answer, so the measurement question is whether the reuse is timing-induced. These knobs slow the
// layer's own submission path on purpose. All are off by default and the default path is unchanged:
// PALMETTO_PACE_IDLE drains the queue after each accepted ExecuteCommandLists (vkQueueWaitIdle, so the submitting
// thread does not run ahead of the GPU; emulation-only - a console submission is asynchronous), and
// PALMETTO_PACE_DELAY_US sleeps that many microseconds after the submission, spacing the title's producer thread.
// Section 13's narrow arm is PALMETTO_PACE_CONTENT=1: drain only after a submission inside the content-capture window
// (the stage-3 command-graph boundary the title's own console semantics would allow at a graph boundary), not after
// every submission.
struct SubmissionPace {
    bool idle_after_execute = false;
    bool idle_at_content_boundary = false;
    std::uint64_t delay_us = 0;
};

// The pure mapping of the three values; the device reads it once at startup. Kept separate so a test can pin the
// truth table without an environment or a device (port/test/frame_dump_test.cpp).
inline SubmissionPace submission_pace_from(const char* idle, const char* delay, const char* content = nullptr) {
    SubmissionPace pace;
    pace.idle_after_execute = idle && *idle && *idle != '0';
    pace.idle_at_content_boundary = content && *content && *content != '0';
    if (delay && *delay) pace.delay_us = std::strtoull(delay, nullptr, 10);
    return pace;
}

inline const SubmissionPace& submission_pace() {
    static const SubmissionPace pace =
        submission_pace_from(std::getenv("PALMETTO_PACE_IDLE"), std::getenv("PALMETTO_PACE_DELAY_US"),
                             std::getenv("PALMETTO_PACE_CONTENT"));
    return pace;
}

// One D3D12 command queue on one VkQueue. D3D12 lets one thread use a queue at a time per call and the COM wrapper
// serialises its calls; state_mutex_ additionally serialises them against the thread that releases a held batch
// (Wait-before-Signal, queue_holds.h), the only other thread that touches a queue.
//
// A Wait on a fence value no Signal has been submitted for yet holds the next batch (ExecuteCommandLists or Signal)
// on the CPU, with everything the queue is asked to do after it, instead of replaying it against layouts from before
// the work it waits for: Vulkan itself would accept the wait (timeline semaphores allow wait-before-signal), but the
// layout tracker follows replay order. Holding on the CPU also keeps a Wait and the Signal that satisfies it from
// landing on one VkQueue in the wrong order, which would never make progress on the GPU.
class VulkanQueue : private QueueHolds<VkSemaphore>::Holder {
public:
    enum class QueueKind : std::uint8_t { direct, compute, copy };

    // The switch-off command-buffer ceiling is byte-for-byte the historical fixed window. When opted in, the pure
    // settings in queue_holds.h can widen that window independently for direct/compute and the existing COPY lane, but
    // never beyond kMaximumSubmissionFlightBound. At the selected bound wait_for_room() still waits for the oldest GPU
    // ticket in finite slices; a device loss makes execute() submit nothing.
    static constexpr std::size_t kMaxQueuedCommandBuffers = SubmissionFlightSettings::kLegacyBound;
    static constexpr std::size_t kMaximumSubmissionFlightBound = SubmissionFlightSettings::kMaximumBound;
    static constexpr std::uint64_t kQueuedWaitNs = 5'000'000'000;

    static constexpr VkPipelineStageFlags2 queue_supported_stages(QueueKind kind) noexcept {
        switch (kind) {
        case QueueKind::copy:
            return VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        case QueueKind::compute:
            return VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case QueueKind::direct:
        default:
            return VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        }
    }

    static constexpr SubmissionLane submission_lane(QueueKind kind) noexcept {
        switch (kind) {
        case QueueKind::copy: return SubmissionLane::copy;
        case QueueKind::compute: return SubmissionLane::compute;
        case QueueKind::direct:
        default: return SubmissionLane::direct;
        }
    }

    VulkanQueue(VulkanContext& context, VkQueue queue, std::uint32_t family, QueueKind kind = QueueKind::direct,
                SubmissionFlightSettings flight_settings = default_submission_flight_settings())
        : context_(context), queue_(queue), kind_(kind), family_(family),
          flight_bound_(flight_settings.bound(submission_lane(kind))) {
        if (flight_settings.enabled && submission_trace_enabled())
            std::fprintf(stderr, "[rt] submit-flight: enabled lane=%s bound=%u hard-cap=%zu\n", queue_kind_name(kind_),
                         flight_bound_, kMaximumSubmissionFlightBound);
        auto& vk = context.vk;
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family;
        ok_ = vk.vkCreateCommandPool(context.device(), &pool, nullptr, &pool_) == VK_SUCCESS;
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        ok_ = ok_ && vk.vkCreateSemaphore(context.device(), &info, nullptr, &progress_) == VK_SUCCESS;
        try {
            context_.holds().attach(this);
        } catch (...) {
            // A failed registration leaves no queue to run the destructor for these Vulkan handles.
            if (progress_) vk.vkDestroySemaphore(context.device(), progress_, nullptr);
            if (pool_) vk.vkDestroyCommandPool(context.device(), pool_, nullptr);
            throw;
        }
    }

    ~VulkanQueue() {
        auto& vk = context_.vk;
        context_.holds().detach(this);  // no thread releases into this queue from here on
        report_held_at_teardown();
        context_.holds().pump_ready();  // presents queued behind what was just dropped can run
        if (queue_) {
            std::lock_guard lock(context_.submit_mutex());
            for (; open_labels_; --open_labels_) context_.labels_.queue_end(queue_);
            const VkResult idle_result = vk.vkQueueWaitIdle(queue_);
            if (idle_result == VK_SUCCESS) {
                if (progress_) context_.retire_queue_idle_locked(progress_);
            } else {
                context_.mark_device_lost();
            }
        }
        if (submission_trace_enabled() && room_wait_count_)
            std::fprintf(stderr,
                         "[rt] submit-flight: summary lane=%s bound=%u submissions=%llu room_waits=%llu "
                         "wait_us=%llu max_wait_us=%llu pool=%zu\n",
                         queue_kind_name(kind_), flight_bound_, static_cast<unsigned long long>(submitted_),
                         static_cast<unsigned long long>(room_wait_count_),
                         static_cast<unsigned long long>(room_wait_total_us_),
                         static_cast<unsigned long long>(room_wait_max_us_), buffers_.size());
        if (progress_) {
            std::lock_guard lock(context_.tracker_mutex_);
            if (context_.depth24_) context_.depth24_->forget(progress_);  // its scratch is idle now
            if (context_.scratch_) context_.scratch_->forget(progress_);
        }
        if (progress_) {
            // This queue is idle, so whatever initialisation it published (storage images, zero-filled buffers) has
            // finished: the next submission on another queue must not wait on a semaphore that is about to go.
            std::lock_guard lock(context_.storage_mutex_);
            if (context_.storage_init_semaphore_ == progress_) context_.storage_init_semaphore_ = VK_NULL_HANDLE;
        }
        if (pool_) vk.vkDestroyCommandPool(context_.device(), pool_, nullptr);
        if (progress_) vk.vkDestroySemaphore(context_.device(), progress_, nullptr);
        destroy_khr_stages(khr_stage_pool_);
        destroy_khr_as_queries(khr_as_query_pool_);
        for (auto& slot : buffers_) {
            destroy_khr_stages(slot.khr_instances);
            destroy_khr_as_queries(slot.khr_as_queries);
            free_events_.insert(free_events_.end(), slot.events.begin(), slot.events.end());
        }
        for (VkEvent event : free_events_) vk.vkDestroyEvent(context_.device(), event, nullptr);
    }

    VulkanQueue(const VulkanQueue&) = delete;
    VulkanQueue& operator=(const VulkanQueue&) = delete;

    bool ok() const { return ok_; }
    std::uint32_t submission_flight_bound() const { return flight_bound_; }

    // Entries kept between executions. Execute only uses the first lists.size(); a larger count is retained storage,
    // not work the next submission will replay (vk_queue_test checks both halves of that contract).
    std::size_t prepared_entries() const { return prepared_.size(); }

    template<typename GetHeap>
    bool update_tiles(ResourceId id, std::span<const TileRegion> regions,
                      std::span<const TileRange> ranges, GetHeap&& get_heap) {
        std::lock_guard state(state_mutex_);  // outermost: held batches are released from other threads
        std::unique_lock lock(context_.resource_mutex_);
        const auto it = context_.sparse_resources_.find(id);
        if (it == context_.sparse_resources_.end()) return false;
        const auto& sparse = it->second;
        for (const auto& region : regions)
            if (!valid_tiles(*sparse, region)) return false;
        const auto heap = get_heap(sparse->image != VK_NULL_HANDLE, sparse->memory_types);

        std::map<std::uint64_t, TileMapping> changes;
        std::size_t region_index = 0;
        std::uint64_t consumed = 0;
        for (const auto& range : ranges) {
            const bool backed = range.mode == TileRangeMode::sequential || range.mode == TileRangeMode::reuse;
            const std::uint64_t heap_count = heap ? heap->size / tile_size_in_bytes : 0;
            const std::uint64_t required = range.mode == TileRangeMode::reuse ? 1 : range.count;
            if (backed && (!heap || range.first > heap_count || required > heap_count - range.first)) return false;
            for (std::uint64_t i = 0; i < range.count;) {
                if (region_index == regions.size()) return false;
                const auto& region = regions[region_index];
                const auto count = std::min(range.count - i, region.count - consumed);
                const auto first = region.first + consumed;
                if (range.mode == TileRangeMode::unmap) {
                    const auto end = first + count;
                    for (auto old = sparse->mapped.lower_bound(first); old != sparse->mapped.end() && old->first < end; ++old)
                        changes[old->first] = {};
                    for (auto change = changes.lower_bound(first); change != changes.end() && change->first < end; ++change)
                        change->second = {};
                } else if (backed) {
                    for (std::uint64_t j = 0; j < count; ++j) {
                        const auto page = range.first + (range.mode == TileRangeMode::sequential ? i + j : 0);
                        changes[first + j] = {heap, heap->offset + page * tile_size_in_bytes};
                    }
                }
                i += count;
                consumed += count;
                if (consumed == region.count) {
                    ++region_index;
                    consumed = 0;
                }
            }
        }
        return region_index == regions.size() && bind_tiles(id, sparse, changes);
    }

    bool copy_tile_mappings(ResourceId dst, std::span<const TileRegion> dst_regions, ResourceId src,
                            std::span<const TileRegion> src_regions) {
        std::lock_guard state(state_mutex_);
        std::unique_lock lock(context_.resource_mutex_);
        const auto source = context_.sparse_resources_.find(src), target = context_.sparse_resources_.find(dst);
        if (source == context_.sparse_resources_.end() || target == context_.sparse_resources_.end() ||
            bool(source->second->image) != bool(target->second->image) || src_regions.size() != dst_regions.size())
            return false;
        // Read the complete source mapping before changing any destination slot, including copies within one resource.
        std::map<std::uint64_t, TileMapping> changes;
        for (std::size_t row = 0; row < src_regions.size(); ++row) {
            const auto from = src_regions[row], to = dst_regions[row];
            if (!valid_tiles(*source->second, from) || !valid_tiles(*target->second, to) || from.count != to.count)
                return false;
            for (std::uint64_t i = 0; i < from.count; ++i) {
                const auto page = source->second->mapped.find(from.first + i);
                changes.emplace(to.first + i, page == source->second->mapped.end() ? TileMapping{} : page->second);
            }
        }
        return bind_tiles(dst, target->second, changes);
    }

    // ExecuteCommandLists: every list is checked before any is written, so a failure submits nothing and leaves
    // the layout tracker as it was. Pending Wait()s are attached to this submission. When one of them (or an
    // earlier held operation) waits for a fence value no Signal has been submitted for, the lists are copied and
    // held instead, and the call succeeds: the batch is replayed by the thread that submits the Signal (or the CPU
    // Signal), which is also where its errors are reported (stderr, and take_held_failure()).
    //
    // `pin`, called only when the batch is held, returns what has to stay alive until the batch has run: the COM layer
    // passes the pipeline states and root signatures the lists named (the application may release them and Reset
    // the lists as soon as the call returns).
    using PinFn = std::function<std::shared_ptr<void>()>;

    ReplayResult execute(std::span<const CommandList* const> lists, const PinFn& pin = {}) {
        ReplayResult result = ReplayResult::success;
        bool held = false;
        bool pipeline_held = false;
        std::vector<std::shared_ptr<void>> retired;  // destroyed after state_mutex_ is released
        timeline::line("exec entry n=%u", static_cast<unsigned>(lists.size()));
        const double started = timeline::seconds();
        {
            std::lock_guard state(state_mutex_);
            // Non-blocking pipeline hold (docs/peer_port_streaming.md): when the switch is on and a listed dispatch
            // names a deferred pipeline the compile queue has not finished, hold the submission instead of waiting for
            // the build on this (a title) thread. It is replayed in order, by the next submit/present or by the
            // device's hold flusher, once every pipeline it names has finished. The budget bounds the hold: at the
            // bound the oldest pipeline holds are completed by the old synchronous wait here, and if the gate still
            // refuses this submission it takes the old wait itself - the declared fallback - and is held for order
            // only, as a batch behind held work already is.
            std::vector<std::uintptr_t> pending;
            const bool wants_pipeline_hold =
                context_.pipeline_hold_enabled() && !context_.is_device_lost() &&
                context_.collect_pending_pipelines(lists, pending);
            if (wants_pipeline_hold) {
                if (context_.pipeline_hold().at_bound()) force_oldest_pipeline_holds_locked();
                const std::uint64_t token = context_.holds().next_seq();
                if (context_.pipeline_hold().hold(token, pending, std::chrono::steady_clock::now())) {
                    Deferred op;
                    op.kind = Deferred::Kind::batch;
                    op.hold_token = token;
                    op.lists.reserve(lists.size());
                    for (const CommandList* list : lists) op.lists.push_back(*list);
                    if (pin) op.pins.push_back(pin());
                    hold(std::move(op));
                    held = true;
                    pipeline_held = true;
                    // A build may have finished between the collect above and this hold: the completion notification
                    // that would have marked it is already past, so mark it now (the check reads the device's live
                    // task state) and wake the flusher, which releases the batch if nothing is pending any more.
                    for (const std::uintptr_t id : pending)
                        if (context_.pipelines_finished(std::span<const std::uintptr_t>(&id, 1)))
                            context_.pipeline_hold().finished(id);
                    context_.wake_pipeline_hold();
                } else {
                    context_.pipeline_hold().note_fallback();
                    context_.prepare_pipelines(lists);  // the previous behaviour, on this thread
                }
            }
            if (!pipeline_held) {
                if (!must_hold()) {
                    result = execute_now(lists);
                } else if (context_.is_device_lost()) {
                    result = ReplayResult::unsupported;
                } else {
                    Deferred op;
                    op.kind = Deferred::Kind::batch;
                    op.lists.reserve(lists.size());
                    for (const CommandList* list : lists) op.lists.push_back(*list);
                    if (pin) op.pins.push_back(pin());
                    // Held behind earlier work, but it may name unfinished pipelines too: gate its replay on them, so
                    // the thread that releases it never has to wait for a build either.
                    if (context_.pipeline_hold_enabled()) {
                        std::vector<std::uintptr_t> also;
                        if (context_.collect_pending_pipelines(lists, also)) {
                            const std::uint64_t token = context_.holds().next_seq();
                            context_.pipeline_hold().hold(token, std::move(also), std::chrono::steady_clock::now());
                            op.hold_token = token;
                        }
                    }
                    hold(std::move(op));
                    held = true;
                }
            }
            drain_pins(retired);
        }
        retired.clear();
        const double execution_us = (timeline::seconds() - started) * 1e6;
        timeline::line("exec return n=%u result=%d held=%d us=%.0f", static_cast<unsigned>(lists.size()),
                       static_cast<int>(result), held ? 1 : 0, execution_us);
        if (submission_trace_enabled()) {
            static std::atomic<std::uint64_t> calls{0};
            static std::atomic<std::uint64_t> reports{0};
            const std::uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
            if (call <= 16 || execution_us >= 1000.0) {
                const std::uint64_t report = reports.fetch_add(1, std::memory_order_relaxed);
                if (report < 128)
                    std::fprintf(stderr,
                                 "[rt] submit-flight: execute lane=%s tid=%llu call=%llu lists=%zu held=%d result=%d us=%.0f\n",
                                 queue_kind_name(kind_), static_cast<unsigned long long>(submission_thread_id()),
                                 static_cast<unsigned long long>(call), lists.size(), held ? 1 : 0,
                                 static_cast<int>(result), execution_us);
            }
        }
        // The pacing knobs (see SubmissionPace; docs/vertex_buffer_va.md section 12). Off by default: with none set
        // this block is two atomic-free loads and changes no call.
        if (!held && result == ReplayResult::success) {
            const SubmissionPace& pace = submission_pace();
            if (pace.delay_us) std::this_thread::sleep_for(std::chrono::microseconds(pace.delay_us));
            if (pace.idle_after_execute ||
                (pace.idle_at_content_boundary && context_.title_import_capture_active())) {
                const double idle_started = timeline::seconds();
                const VkResult idle = context_.vk.vkQueueWaitIdle(queue_);
                timeline::line("pace idle ok=%d us=%.0f", idle == VK_SUCCESS ? 1 : 0,
                               (timeline::seconds() - idle_started) * 1e6);
                if (idle == VK_ERROR_DEVICE_LOST) context_.mark_device_lost();
            }
        }
        if (held) {
            context_.holds().pump_ready();  // a Signal may have been submitted since the check
            if (pipeline_held) context_.wake_pipeline_hold();
        }
        return result;
    }

    // ID3D12CommandQueue::Signal: the fence reaches `value` after all work submitted so far. Held behind earlier
    // held operations, so the fence does not run ahead of the work it follows. `pin` (called only when the Signal is
    // held) keeps the fence object alive until it has been submitted, as D3D12 does.
    bool signal(VkSemaphore fence, std::uint64_t value, const PinFn& pin = {}) {
        bool ok = true;
        bool held = false;
        std::vector<std::shared_ptr<void>> retired;
        timeline::line("queue signal entry v=%llu", static_cast<unsigned long long>(value));
        {
            std::lock_guard state(state_mutex_);
            if (must_hold()) {
                held = true;
                Deferred op;
                op.kind = Deferred::Kind::signal;
                op.fence = fence;
                op.value = value;
                if (pin) op.pins.push_back(pin());
                hold(std::move(op));
            } else {
                ok = submit(VK_NULL_HANDLE, fence, value);
            }
            drain_pins(retired);
        }
        retired.clear();
        timeline::line("queue signal return v=%llu ok=%d", static_cast<unsigned long long>(value), ok ? 1 : 0);
        if (submission_trace_enabled()) {
            static std::atomic<std::uint64_t> reports{0};
            if (reports.fetch_add(1, std::memory_order_relaxed) < 128)
                std::fprintf(stderr,
                             "[rt] submit-flight: queue-signal lane=%s tid=%llu value=%llu deferred=%d ok=%d\n",
                             queue_kind_name(kind_), static_cast<unsigned long long>(submission_thread_id()),
                             static_cast<unsigned long long>(value), held ? 1 : 0, ok ? 1 : 0);
        }
        context_.holds().pump_ready();  // this Signal, or the hold above, may have made a held batch ready
        return ok;
    }

    // ID3D12CommandQueue::Wait: later submissions wait for the fence. A wait in a Vulkan batch only holds back that
    // batch, so it is kept and attached to the next submission (held on the CPU if no Signal reaches it yet).
    // `pin` returns what keeps the fence object alive until then (a D3D12 queue holds its fences).
    void wait(VkSemaphore fence, std::uint64_t value, const PinFn& pin = {}) {
        std::vector<std::shared_ptr<void>> retired;  // fences whose submission has completed: released unlocked
        timeline::line("queue wait v=%llu", static_cast<unsigned long long>(value));
        const bool trace = submission_trace_enabled();
        const bool pending_signal = trace && context_.holds().pending(fence, value);
        {
            std::lock_guard state(state_mutex_);
            waits_.push_back(fence);
            wait_values_.push_back(value);
            if (pin) wait_pins_.push_back(pin());
            drain_pins(retired);
        }
        if (trace) {
            static std::atomic<std::uint64_t> reports{0};
            if (reports.fetch_add(1, std::memory_order_relaxed) < 128)
                std::fprintf(stderr,
                             "[rt] submit-flight: queue-wait lane=%s tid=%llu value=%llu pending-signal=%d\n",
                             queue_kind_name(kind_), static_cast<unsigned long long>(submission_thread_id()),
                             static_cast<unsigned long long>(value), pending_signal ? 1 : 0);
        }
    }

    // The first failure of a held ExecuteCommandLists since the last call (what the call would have returned had it
    // not been held), or success. A Signal behind a failed batch still signals, as when the failure is not held: the
    // work did not run, the application is told here and on stderr, and nothing waits forever.
    ReplayResult take_held_failure() {
        return static_cast<ReplayResult>(held_failure_.exchange(static_cast<int>(ReplayResult::success)));
    }

    // ID3D12CommandQueue::SetMarker/BeginEvent/EndEvent: queue labels, between the submissions around them. An end
    // with no begin open on this queue is dropped, as Vulkan requires a matching begin. No-ops without the extension.
    void marker(DebugMarker::Kind kind, const char* label) {
        std::vector<std::shared_ptr<void>> retired;
        std::lock_guard state(state_mutex_);
        drain_pins(retired);  // destroyed after the lock (reverse declaration order)
        if (!held_.empty()) {  // behind the held work, so behind it in the queue's labels too
            Deferred op;
            op.kind = Deferred::Kind::marker;
            op.marker_kind = kind;
            op.label = label ? label : "";
            const std::size_t label_bytes = op.label.size();
            hold(std::move(op));
            if (submission_trace_enabled()) {
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1, std::memory_order_relaxed) < 128)
                    std::fprintf(stderr,
                                 "[rt] submit-flight: marker-held lane=%s tid=%llu kind=%u label-bytes=%zu\n",
                                 queue_kind_name(kind_), static_cast<unsigned long long>(submission_thread_id()),
                                 static_cast<unsigned>(kind), label_bytes);
            }
            return;
        }
        marker_now(kind, label);
    }

    bool idle() {
        std::lock_guard lock(context_.submit_mutex());
        return context_.vk.vkQueueWaitIdle(queue_) == VK_SUCCESS;
    }

private:
    static bool submission_trace_enabled() {
        static const bool enabled =
            SubmissionFlightSettings::switch_on(std::getenv("PALMETTO_SUBMIT_TRACE"));
        return enabled;
    }

    static const char* queue_kind_name(QueueKind kind) noexcept {
        switch (kind) {
        case QueueKind::compute: return "compute";
        case QueueKind::copy: return "copy";
        case QueueKind::direct:
        default: return "direct";
        }
    }

    static std::uint64_t submission_thread_id() noexcept {
#if defined(_WIN32)
        return static_cast<std::uint64_t>(GetCurrentThreadId());
#else
        return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    }

    // Completed stages, reused by later ARRAY_OF_POINTERS builds. Only execute_now() touches the pool, and it holds
    // state_mutex_ (including the deferred/held release paths), so no extra lock is needed.
    std::vector<KhrInstanceStage> khr_stage_pool_;
    std::vector<KhrAsQueryPool> khr_as_query_pool_;

    void destroy_khr_stages(std::vector<KhrInstanceStage>& stages) {
        for (const auto& stage : stages) {
            if (stage.memory) context_.vk.vkFreeMemory(context_.device(), stage.memory, nullptr);
            if (stage.buffer) context_.vk.vkDestroyBuffer(context_.device(), stage.buffer, nullptr);
        }
        stages.clear();
    }

    // A stage returns to the pool only after its submission has completed, so rewriting it cannot race the GPU.
    void recycle_khr_stages(std::vector<KhrInstanceStage>& stages) {
        for (auto& stage : stages) khr_stage_pool_.push_back(std::move(stage));
        stages.clear();
    }

    void destroy_khr_as_queries(std::vector<KhrAsQueryPool>& pools) {
        if (context_.vk.vkDestroyQueryPool) {
            for (const auto& entry : pools) {
                if (entry.pool) context_.vk.vkDestroyQueryPool(context_.device(), entry.pool, nullptr);
            }
        }
        pools.clear();
    }

    void recycle_khr_as_queries(std::vector<KhrAsQueryPool>& pools) {
        for (auto& entry : pools) {
            entry.used = 0;
            khr_as_query_pool_.push_back(entry);
        }
        pools.clear();
    }

    bool allocate_khr_as_queries(std::uint32_t count, std::vector<KhrAsQueryPool>& active, VkQueryPool& out_pool,
                                 std::uint32_t& out_first) {
        if (!count || !context_.vk.vkCreateQueryPool || !context_.vk.vkDestroyQueryPool) return false;
        if (!active.empty()) {
            auto& back = active.back();
            if (back.used <= back.capacity && count <= back.capacity - back.used) {
                out_pool = back.pool;
                out_first = back.used;
                back.used += count;
                return true;
            }
        }
        for (auto it = khr_as_query_pool_.begin(); it != khr_as_query_pool_.end(); ++it) {
            if (it->capacity >= count) {
                KhrAsQueryPool entry = *it;
                khr_as_query_pool_.erase(it);
                entry.used = count;
                active.push_back(entry);
                out_pool = entry.pool;
                out_first = 0;
                return true;
            }
        }
        const std::uint32_t capacity = std::max<std::uint32_t>(count, 16u);
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        info.queryCount = capacity;
        VkQueryPool pool = VK_NULL_HANDLE;
        if (context_.vk.vkCreateQueryPool(context_.device(), &info, nullptr, &pool) != VK_SUCCESS || !pool)
            return false;
        active.push_back({pool, capacity, count});
        out_pool = pool;
        out_first = 0;
        return true;
    }

    // A KHR TLAS built with D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS holds console VAs in its pointer list; Vulkan
    // reads those 8-byte entries as device addresses. Stage a translated copy in our own host-visible buffer instead
    // of editing the guest's input. The caller keeps the stage until the submission that builds with it completes.
    ReplayResult stage_khr_instances(const ReplayFunctions& vk, const BuildAccelerationStructure& build,
                                     std::vector<KhrInstanceStage>& stages, VkDeviceAddress& address) {
        const auto& funcs = context_.vk;
        if (!funcs.vkCreateBuffer || !funcs.vkDestroyBuffer || !funcs.vkGetBufferMemoryRequirements ||
            !funcs.vkAllocateMemory || !funcs.vkFreeMemory || !funcs.vkBindBufferMemory || !funcs.vkMapMemory ||
            !funcs.vkUnmapMemory || !funcs.vkGetBufferDeviceAddress || !vk.host_at || !vk.buffer_at ||
            !build.instance_count)
            return ReplayResult::unsupported;
        const std::uint64_t bytes = 8ull * build.instance_count;
        const std::uint8_t* guest = nullptr;
        const HostReadStatus status = vk.host_at(build.instances_va, bytes, guest);
        if (status == HostReadStatus::not_cpu_written) return ReplayResult::unsupported;
        if (status != HostReadStatus::ok) return ReplayResult::invalid_copy;

        KhrInstanceStage stage{};
        const auto discard = [&] {
            if (stage.memory) funcs.vkFreeMemory(context_.device(), stage.memory, nullptr);
            if (stage.buffer) funcs.vkDestroyBuffer(context_.device(), stage.buffer, nullptr);
        };
        // Reuse a completed stage that is already large enough. A per-frame TLAS rebuild then reuses the same buffer
        // and memory instead of making a new vkAllocateMemory call every frame.
        for (auto it = khr_stage_pool_.begin(); it != khr_stage_pool_.end(); ++it) {
            if (it->size >= bytes) {
                stage = std::move(*it);
                khr_stage_pool_.erase(it);
                break;
            }
        }
        if (!stage.buffer) {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = bytes;
            info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
            if (funcs.vkCreateBuffer(context_.device(), &info, nullptr, &stage.buffer) != VK_SUCCESS)
                return ReplayResult::unsupported;
            VkMemoryRequirements requirements{};
            funcs.vkGetBufferMemoryRequirements(context_.device(), stage.buffer, &requirements);
            const auto& memory = context_.occlusion_resolve_memory_;
            constexpr VkMemoryPropertyFlags host_flags =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            std::uint32_t type = memory.memoryTypeCount;
            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                if (!(requirements.memoryTypeBits & (1u << i))) continue;
                if ((memory.memoryTypes[i].propertyFlags & host_flags) == host_flags) {
                    type = i;
                    break;
                }
            }
            if (type == memory.memoryTypeCount) {
                discard();
                return ReplayResult::unsupported;
            }
            VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
            flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = type;
            if (funcs.vkAllocateMemory(context_.device(), &allocation, nullptr, &stage.memory) != VK_SUCCESS ||
                funcs.vkBindBufferMemory(context_.device(), stage.buffer, stage.memory, 0) != VK_SUCCESS) {
                discard();
                return ReplayResult::unsupported;
            }
            stage.size = bytes;
        }

        void* mapped = nullptr;
        if (funcs.vkMapMemory(context_.device(), stage.memory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
            discard();
            return ReplayResult::unsupported;
        }
        // Validate and translate in one pass straight into the staging buffer; no second vector of addresses.
        for (std::uint32_t i = 0; i < build.instance_count; ++i) {
            std::uint64_t va = 0;
            std::memcpy(&va, guest + 8ull * i, sizeof(va));
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            std::uint64_t room = 64;  // one D3D12_RAYTRACING_INSTANCE_DESC
            // No overflow check on heap_base + va: a buffer outside the arena whose device address lies below the
            // arena's (an UPLOAD heap in system memory without Resizable BAR) has a VA that wraps (address - base),
            // and the unsigned sum wraps back to its device address, as the replay's other VA translations do.
            if (!va || !vk.buffer_at(va, room, buffer, offset) || room < 64) {
                funcs.vkUnmapMemory(context_.device(), stage.memory);
                discard();
                return ReplayResult::invalid_copy;
            }
            const std::uint64_t device_address = vk.heap_base + va;
            std::memcpy(static_cast<std::uint8_t*>(mapped) + 8ull * i, &device_address, sizeof(device_address));
        }
        funcs.vkUnmapMemory(context_.device(), stage.memory);
        VkBufferDeviceAddressInfo device_address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        device_address.buffer = stage.buffer;
        stage.address = funcs.vkGetBufferDeviceAddress(context_.device(), &device_address);
        if (!stage.address) {
            discard();
            return ReplayResult::unsupported;
        }
        stages.push_back(std::move(stage));
        address = stages.back().address;
        return ReplayResult::success;
    }

    ReplayResult execute_now(std::span<const CommandList* const> lists) {
        if (context_.is_device_lost() || !wait_for_room()) return ReplayResult::unsupported;
        // Title pages a command names directly (docs/vertex_buffer_va.md): a vertex/index view or immediate-write VA
        // that no mapping resolves is imported over the title's own pages at first use, before the lists below are
        // prepared, so prepare_draw finds a VkBuffer for it. The measured stage-3 range is the title's committed memory
        // and the arena is full, so the import is the one answer that needs no arena room; a span the device cannot
        // import stays unresolved and keeps the replay's named refusal.
        const std::size_t imported_title_spans = context_.register_title_command_buffers(lists);
        // Diagnostic (docs/vertex_buffer_va.md section 10): arm the draw readback on the content submissions, whose
        // presents the presenter capture misses. A no-op unless that registration imported something or an armed
        // submission is pending.
        context_.title_import_capture_tick(imported_title_spans);
        // Diagnostic (docs/vertex_buffer_va.md section 17): the content window this submission is in, read once here so
        // the content probe's command dump and the PS-only control can arm on the submission itself (the content draws'
        // global indices vary per boot).
        const bool content_window = context_.title_import_capture_active();
        // Diagnostic (docs/frame_capture.md): which control drew after the frame's commands. The one-shot readback below
        // copies the target after them, so its line is labelled as the control's readback; that axis measures coverage
        // independently of the title pixel shader.
        bool ps_control_drew = false;
        bool control_drew = false;
        // The title's shader-visible console descriptor heaps are CPU memory; their GPU-visible mirrors are refreshed
        // here, before anything that reads them is recorded (console_va_map.h, docs/page_address_space.md). Copying
        // the title's own bytes is what a console GPU would read, so a title that rewrites descriptors still in flight
        // keeps the same race it would have on the console. docs/card_copies.md measures this step and the switches
        // that avoid it; with every switch unset the call below is the bare refresh, as before the slice.
        context_.sync_console_heaps_measured(submission_launches_a_shader(lists));
        // Deferred image backing (docs/streamed_image_backing.md): a placed/streamed image whose memory was not
        // allocated at creation gets it here, before the lists below are prepared or recorded, so the first command
        // that could name it finds a bound image. A no-op unless the device registered the hook; an allocation that
        // fails refuses the submission instead of recording a use of an image without memory.
        if (!context_.bind_deferred_images()) return ReplayResult::unsupported;
        // A title-placed texture's pages (docs/texture_import.md) are imported before the submission that could sample
        // them: the copy runs on the staging path and waits, so the bytes are in the image when the replay records it.
        context_.import_placed_textures();
        // Diagnostic (docs/texture_source.md): the page-writer watch for sampled placed textures, ticked once per
        // submission; it scans every 64th. A no-op unless the device registered it and the switch is on.
        context_.tick_page_watch();
        // Deferred pipeline compilation (docs/deferred_compile.md): make every pipeline this submission names ready
        // before the lists below are prepared and recorded.
        context_.prepare_pipelines(lists);
        // Bounded progress diagnostic (counts only): how fast submissions come, so a clean boot's draw count can be
        // told apart from a stalled or slowed title.
        {
            static std::atomic<std::uint64_t> executed{0};
            static const auto started = std::chrono::steady_clock::now();
            const std::uint64_t count = executed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count == 16 || count == 256 || count == 1024 ||
                (count % 4096) == 0) {
                const double seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                std::fprintf(stderr, "[rt] replay: submission %llu at %.1f s\n",
                             static_cast<unsigned long long>(count), seconds);
                // docs/memory_growth.md: the live-pipeline budget's counters ride the same bounded milestone.
                const std::string census = context_.pipeline_census();
                if (!census.empty()) std::fprintf(stderr, "[rt] replay: pipeline budget: %s\n", census.c_str());
                // docs/card_copies.md: the title-memory mirror's counters ride the same milestone.
                const std::string mirror = context_.card_copy_report();
                if (!mirror.empty()) std::fprintf(stderr, "[rt] replay: card copies: %s\n", mirror.c_str());
            }
        }
        if (kind_ != QueueKind::direct)
            for (const auto* list : lists)
                for (const auto& command : list->commands())
                    if (std::holds_alternative<ClearRenderTarget>(command) ||
                        std::holds_alternative<ClearDepthStencil>(command))
                        return ReplayResult::unsupported;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        struct KhrGuard {
            VulkanQueue* queue;
            std::vector<KhrInstanceStage> stages;
            std::vector<KhrAsQueryPool> as_queries;
            ~KhrGuard() {
                queue->recycle_khr_stages(stages);
                queue->recycle_khr_as_queries(as_queries);
            }
        } khr_guard{this};
        std::function<void(bool)> bindless_commit;
        // Lock hierarchy: resource_mutex_ -> tracker_mutex_ -> submit_mutex_.
        // Shared read-lock allows concurrent prepare() across queues; tracker_mutex_ is acquired lazily during
        // prepare() only if a list actually converts a depth24_as_float copy, and otherwise right before
        // bindless_prepare_/emit() through submit() so bindless flushes, layout emits, and submissions stay in order.
        std::shared_lock resource_lock(context_.resource_mutex_);
        std::unique_lock tracker_lock(context_.tracker_mutex_, std::defer_lock);
        // Declared after tracker_lock so any early return after depth24_->prepare() retires uncommitted scratch/pools
        // with ticket 0 while tracker_lock is still held.
        struct Depth24Guard {
            Depth24Converter* d24 = nullptr;
            VkSemaphore progress = VK_NULL_HANDLE;
            bool active = false;
            ~Depth24Guard() {
                if (active && d24) d24->retire(progress, 0);
            }
        } depth24_guard{context_.depth24_.get(), progress_, false};
        struct ScratchGuard {  // the same for the copy scratch
            ScratchBuffers* scratch = nullptr;
            VkSemaphore progress = VK_NULL_HANDLE;
            bool active = false;
            ~ScratchGuard() {
                if (active && scratch) scratch->retire(progress, 0);
            }
        } scratch_guard{context_.scratch_.get(), progress_, false};
        const bool transfer_only = family_ == context_.transfer_only_family_;
        if (!transfer_only) bindless_commit = context_.bindless_commit_;
        // Kept between executions: prepare refills each list's entries in the storage the last execution left, where
        // making them fresh for every command was about half the layer's CPU cost per command (replay_cpu_bench).
        auto& prepared = prepared_;
        // Keep the storage from a larger batch. resize() down would destroy the unused entries and their vectors, so
        // a queue that sees 8 lists and then 1 in every frame would rebuild them on each execution. The loops below
        // use only the first lists.size() entries, so stale entries are never prepared, queried or emitted.
        if (prepared.size() < lists.size()) prepared.resize(lists.size());
        ReplayFunctions replay = context_.replay_;
        replay.queue_stages = kind_ == QueueKind::copy      ? detail::queue_stages_copy
                              : kind_ == QueueKind::compute ? detail::queue_stages_compute
                                                            : 0;
        replay.prepare_khr_instances = [this, &replay, &khr_guard](const BuildAccelerationStructure& build,
                                                                   VkDeviceAddress& address) {
            return stage_khr_instances(replay, build, khr_guard.stages, address);
        };
        if (replay.write_acceleration_structures_properties) {
            replay.prepare_khr_postbuild = [this, &khr_guard](std::uint32_t count, VkQueryPool& pool,
                                                              std::uint32_t& first) {
                return allocate_khr_as_queries(count, khr_guard.as_queries, pool, first);
            };
        }
        std::uint64_t captures = 0;
        for (const auto* list : lists)
            for (const auto& command : list->commands())
                if (const auto* p = std::get_if<Predication>(&command); p && p->buffer) ++captures;
        std::unique_ptr<PredicateReducer::Batch> batch;
        std::unique_ptr<PredicatedBufferCopier::Batch> copy_batch;
        std::unique_ptr<IndirectDispatchCounter::Batch> counted_batch;
        if (captures) {
            if (kind_ == QueueKind::copy || !context_.predicate_reducer_ || family_ != context_.predicate_family_)
                return ReplayResult::unsupported;
            // The helper's lazy pipeline is shared by queues. Serialize preparation with the tracker lock,
            // retaining main's resource -> tracker -> submit lock order through emit and submission.
            if (!tracker_lock.owns_lock()) tracker_lock.lock();
            batch = context_.predicate_reducer_->batch(captures);
            if (!batch) return ReplayResult::unsupported;
            replay.prepare_predicate = [&](VkBuffer buffer, VkDeviceSize offset, PredicateCapture& out) {
                return batch->prepare(buffer, offset, out);
            };
            replay.capture_predicate = [&](VkCommandBuffer cb, const PredicateCapture& p) {
                context_.predicate_reducer_->capture(cb, p);
            };
            if (context_.predicated_copier_) {
                copy_batch = context_.predicated_copier_->batch();
                replay.prepare_predicated_copy = [&](VkBuffer source, VkBuffer destination, VkDeviceSize src,
                                                     VkDeviceSize dst, VkDeviceSize size,
                                                     std::vector<PredicatedCopyDispatch>& out) {
                    return copy_batch->prepare(source, destination, src, dst, size, out);
                };
                replay.emit_predicated_copy = [&](VkCommandBuffer cb, const std::vector<PredicatedCopyDispatch>& jobs) {
                    context_.predicated_copier_->emit(cb, jobs);
                };
            }
        }
        if (context_.scratch_) {
            replay.copy_scratch = [this, &tracker_lock, &scratch_guard](VkDeviceSize bytes, VkBuffer& buffer,
                                                                        VkDeviceSize& offset) {
                if (!tracker_lock.owns_lock()) tracker_lock.lock();
                if (!scratch_guard.active) {
                    context_.scratch_->collect();
                    scratch_guard.active = true;
                }
                return context_.scratch_->get(bytes, buffer, offset);
            };
        }
        if (kind_ != QueueKind::copy && context_.counted_dispatch_) {
            replay.prepare_counted_dispatch = [this, &tracker_lock, &counted_batch](
                VkBuffer args, VkDeviceSize args_offset, VkBuffer count, VkDeviceSize count_offset,
                std::uint32_t stride, std::uint32_t maximum, CountedDispatch& out) {
                // Pipeline creation is lazy and shared across queues; serialize it with other replay preparation.
                if (!tracker_lock.owns_lock()) tracker_lock.lock();
                if (!counted_batch) counted_batch = context_.counted_dispatch_->batch();
                return counted_batch->prepare(args, args_offset, count, count_offset, stride, maximum, out);
            };
            replay.emit_counted_dispatch = [this](VkCommandBuffer cb, const CountedDispatch& job) {
                context_.counted_dispatch_->emit(cb, job);
            };
        }
        if (context_.depth24_ && !transfer_only) {
            replay.depth24_prepare = [this, &tracker_lock, &depth24_guard](Depth24Job& job,
                                                                           std::vector<Depth24Job>& parts) {
                if (!tracker_lock.owns_lock()) tracker_lock.lock();
                if (!depth24_guard.active) {
                    context_.depth24_->collect();
                    depth24_guard.active = true;
                }
                return context_.depth24_->prepare(job, parts);
            };
        }
        const auto& occlusion_memory = context_.occlusion_resolve_memory_;
        const bool can_resolve_occlusion = occlusion_memory.memoryTypeCount != 0;
        if (can_resolve_occlusion) {
            replay.sum_occlusion = [this](VkCommandBuffer cb, VkQueryPool pool, std::uint32_t first,
                                          std::uint32_t count, VkBuffer dst, VkDeviceSize offset) {
                occlusion_resolve_->sum(cb, pool, first, count, dst, offset);
            };
            replay.resolve_binary_occlusion = [this](VkCommandBuffer cb, VkQueryPool pool, std::uint32_t first,
                                                      std::uint32_t count, VkBuffer dst, VkDeviceSize offset) {
                occlusion_resolve_->normalize(cb, pool, first, count, dst, offset);
            };
            replay.sum_statistics = [this](VkCommandBuffer cb, VkQueryPool pool, VkQueryPool mesh_pool,
                                           const std::uint8_t* mesh, std::uint32_t first, std::uint32_t count,
                                           VkBuffer dst, VkDeviceSize offset) {
                occlusion_resolve_->sum_statistics(cb, pool, mesh_pool, mesh, first, count, dst, offset);
            };
            replay.sum_binary_occlusion = [this](VkCommandBuffer cb, VkQueryPool pool, std::uint32_t first,
                                                 std::uint32_t count, VkBuffer dst, VkDeviceSize offset) {
                occlusion_resolve_->sum_binary(cb, pool, first, count, dst, offset);
            };
        }
        const auto resolve = [this](ResourceId id) {
            const ResourceBinding* binding = context_.resolve(id);
            if (!binding && id) {
                static std::atomic<unsigned> misses{0};
                if (misses.fetch_add(1, std::memory_order_relaxed) < 12)
                    std::fprintf(stderr, "[rt] replay: resource map miss for id 0x%llx\n",
                                 static_cast<unsigned long long>(id));
            }
            return binding;
        };
        std::uint32_t segments = 0, statistics_segments = 0;
        bool has_queries = false;
        for (std::size_t i = 0; i < lists.size(); ++i) {
            const auto result = prepare(*lists[i], resolve, replay, prepared[i], can_resolve_occlusion);
            if (result != ReplayResult::success) {
                static std::atomic<unsigned> refusals{0};
                if (refusals.fetch_add(1, std::memory_order_relaxed) < 8)
                    std::fprintf(stderr,
                                 "[rt] replay: list %zu of %zu refused in prepare: result %d, %zu command(s)\n", i,
                                 lists.size(), int(result), lists[i]->commands().size());
                // Diagnostic (docs/vertex_buffer_va.md): every bound resource of the refused list, as recorded.
                if (miss_dump::enabled()) miss_dump::refused_list(*lists[i], i, lists.size(), int(result));
                return result;
            }
            if (prepared[i].segment_count > UINT32_MAX - segments ||
                prepared[i].statistics_segment_count > UINT32_MAX - statistics_segments)
                return ReplayResult::unsupported;
            segments += prepared[i].segment_count;
            statistics_segments += prepared[i].statistics_segment_count;
            has_queries = has_queries || !prepared[i].queries.empty();
        }
        // Diagnostic (docs/frame_capture.md): with PALMETTO_FRAME_DUMP set, print this submission's prepared
        // commands in order with the resource ids each one writes, so the resource the present blits has its last
        // writer measured. Off, this is one false branch; the recorded commands are unchanged either way.
        if (frame_dump::enabled())
            frame_dump::submission(std::span<const PreparedList>(prepared.data(), lists.size()));
        // Diagnostic (docs/vertex_buffer_va.md section 17): the content probe's command table for the content submits.
        // A no-op unless PALMETTO_CONTENT_PROBE is set; independent of PALMETTO_FRAME_DUMP.
        frame_dump::content_submission(std::span<const PreparedList>(prepared.data(), lists.size()), content_window);
        // Diagnostic (docs/frame_capture.md): the controls. Find whether this is the submission that holds the draw
        // PALMETTO_FRAME_DUMP_AT_DRAW names, and that draw's colour target and recorded command, so the project-built
        // full-screen triangle (PALMETTO_FRAME_DUMP_CONTROL) or the title's own vertices with a substituted pixel
        // shader (PALMETTO_FRAME_DUMP_PS_CONTROL) can be drawn after the frame's commands.
        bool control_armed = false;
        bool ps_control_armed = false;
        frame_dump::Armed armed;  // the dump/readback-armed draw's place in this submission, once per submission
        frame_dump::ArmedTarget control_target;
        if (frame_dump::control_enabled() || frame_dump::ps_control_enabled() || upscale_standin::enabled()) {
            const std::span<const PreparedList> submission(prepared.data(), lists.size());
            // Diagnostic (docs/vertex_buffer_va.md section 17): in the content window the first draw of the submission is
            // the armed one, so the PS-only control measures the content draw's own geometry/raster state.
            armed = frame_dump::armed_submission(submission, content_window && frame_dump::content_probe_enabled());
            if (armed.armed) {
                control_target = frame_dump::armed_target(submission, armed.first, armed.at);
                control_armed = control_target.found;
                ps_control_armed = control_target.found && control_target.pipeline != nullptr;
            }
            if (armed.armed && !control_armed)
                std::fprintf(stderr,
                             "[rt] replay: frame dump control: the armed draw has no resolvable colour target\n");
        }
        const auto segment_pool = [this](VkQueryType type, std::uint32_t count,
                                         std::shared_ptr<OcclusionSegmentPool>& out) {
            const auto& vk = context_.vk;
            if (!vk.vkCreateQueryPool || !vk.vkDestroyQueryPool) return false;
            out = std::make_shared<OcclusionSegmentPool>();
            out->device = context_.device();
            out->destroy = vk.vkDestroyQueryPool;
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = type;
            info.queryCount = count;
            // The counters of a D3D12 pipeline-statistics heap, in D3D12_QUERY_DATA_PIPELINE_STATISTICS order.
            if (type == VK_QUERY_TYPE_PIPELINE_STATISTICS) info.pipelineStatistics = 0x7ff;
            return vk.vkCreateQueryPool(out->device, &info, nullptr, &out->pool) == VK_SUCCESS;
        };
        std::shared_ptr<OcclusionSegmentPool> physical, statistics;
        if (segments && !segment_pool(VK_QUERY_TYPE_OCCLUSION, segments, physical)) return ReplayResult::unsupported;
        // Without the compute helper a split query could not be summed: leave statistics queries whole, as before
        // (QueryReplay::plan refuses a list whose statistics query holds a mesh draw then).
        if (statistics_segments && can_resolve_occlusion) {
            if (!segment_pool(VK_QUERY_TYPE_PIPELINE_STATISTICS, statistics_segments, statistics))
                return ReplayResult::unsupported;
            for (const auto& list : prepared)
                statistics->mesh.insert(statistics->mesh.end(), list.statistics_mesh.begin(),
                                        list.statistics_mesh.end());
            if (std::find(statistics->mesh.begin(), statistics->mesh.end(), 1) == statistics->mesh.end()) {
                statistics->mesh.clear();
            } else {
                // The counters a mesh draw allows (VUID-vkCmdDrawMeshTasksEXT-pipelineStatistics-07076): clipping
                // invocations and primitives, fragment and compute invocations (D3D-291).
                VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
                info.queryCount = statistics_segments;
                info.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                                          VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                          VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                                          VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
                if (context_.vk.vkCreateQueryPool(statistics->device, &info, nullptr, &statistics->mesh_pool) !=
                    VK_SUCCESS)
                    return ReplayResult::unsupported;
            }
        }
        // QueryReplay::plan() reads QueryPool::ended / occlusion and QueryReplay::commit() writes them after
        // resource_lock is unlocked. Hold tracker_mutex_ across plan -> emit -> submit -> commit so concurrent queues
        // see submission-order query state and remove_query_pool() waits until commit finishes.
        if (has_queries && !tracker_lock.owns_lock()) tracker_lock.lock();
        QueryReplay queries;
        std::uint32_t base = 0, statistics_base = 0;
        for (std::size_t i = 0; i < lists.size(); ++i) {
            auto& list = prepared[i];
            const auto result = queries.plan(list, physical, base, can_resolve_occlusion, statistics, statistics_base);
            if (result != ReplayResult::success) return result;
            base += list.segment_count;
            statistics_base += list.statistics_segment_count;
        }
        if (queries.needs_sum || queries.needs_binary || queries.needs_statistics_sum || queries.needs_sum_binary) {
            if (!occlusion_resolve_) {
                auto helper = std::make_unique<OcclusionResolve>(context_.vk, context_.device());
                if (!helper->initialize(occlusion_memory)) return ReplayResult::unsupported;
                occlusion_resolve_ = std::move(helper);
            }
            if (!occlusion_resolve_->prepare(queries.needs_sum, queries.needs_binary, queries.needs_statistics_sum,
                                             queries.needs_sum_binary))
                return ReplayResult::unsupported;
        }

        CommandSlot* slot = acquire();
        if (!slot) return ReplayResult::unsupported;
        slot->khr_instances = std::move(khr_guard.stages);  // freed when this slot is reused
        khr_guard.stages.clear();
        slot->khr_as_queries = std::move(khr_guard.as_queries);
        khr_guard.as_queries.clear();
        command_buffer = slot->buffer;
        // MoltenVK writes a timestamp at the end of a Metal encoder and resolves it with a blit that nothing orders
        // after that write, so a copy in the same or the next command buffer reads the old value (docs section 4.6).
        // An event set and waited for in this command buffer orders it after this submission's timestamps; a wait for
        // this queue's previous submission orders it after earlier ones still running.
        bool ordered_timestamps = false;
        const auto& events = context_.vk;
        if (context_.order_timestamp_copies() && events.vkCreateEvent && events.vkDestroyEvent && events.vkResetEvent &&
            events.vkCmdSetEvent && events.vkCmdWaitEvents)
            replay.order_timestamp_copy = [this, slot, &ordered_timestamps](VkCommandBuffer cb) {
                const VkEvent event = take_event();
                if (!event) return;
                slot->events.push_back(event);
                const auto& vk = context_.vk;
                vk.vkCmdSetEvent(cb, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                vk.vkCmdWaitEvents(cb, 1, &event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
                ordered_timestamps = true;
            };
        const auto discard = [&] {
            if (context_.vk.vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS) slot->ticket = 0;
        };
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (context_.vk.vkBeginCommandBuffer(command_buffer, &begin) != VK_SUCCESS) {
            discard();
            return ReplayResult::unsupported;
        }
        if (!tracker_lock.owns_lock()) tracker_lock.lock();
        if (!transfer_only && context_.bindless_prepare_ && !context_.bindless_prepare_(command_buffer)) {
            // The bindless set's flush degrades a table generation that cannot be made (it keeps the last uploaded
            // table and returns true, vulkan_bindless.h), so a false here is a prepare case that could serve
            // nothing at all: the allocation source may be gone rather than short of table memory. Refuse the
            // submission by name, bounded, instead of recording commands that would bind an unprepared set.
            static std::atomic<unsigned> bindless_refusals{0};
            if (bindless_refusals.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr,
                             "[rt] replay: bindless set 0 could not be prepared for this submission; it is refused "
                             "(%u so far, %zu command list(s))\n",
                             bindless_refusals.load(std::memory_order_relaxed), lists.size());
            const bool ended = context_.vk.vkEndCommandBuffer(command_buffer) == VK_SUCCESS;
            resource_lock.unlock();
            const bool submitted = ended && submit(command_buffer, VK_NULL_HANDLE, 0);
            if (!submitted) discard();
            if (depth24_guard.active) {
                context_.depth24_->retire(progress_, 0);
                depth24_guard.active = false;
            }
            if (scratch_guard.active) {
                context_.scratch_->retire(progress_, 0);
                scratch_guard.active = false;
            }
            if (bindless_commit) bindless_commit(submitted);
            tracker_lock.unlock();
            return ReplayResult::unsupported;
        }
        replay.bindless_set = context_.bindless_current_ ? context_.bindless_current_() : VK_NULL_HANDLE;
        const bool bindless_uploaded = std::exchange(context_.bindless_upload_pending_, false);
        auto next_tracker = context_.tracker_;
        // Bindless images that may already be visible to shaders are initialized on a submission that runs shaders.
        // initialize() zeroes committed storage images and only transitions sparse images.
        bool runs_shaders = false;
        for (const CommandList* list : lists)
            for (const auto& command : list->commands())
                runs_shaders |= std::holds_alternative<Dispatch>(command) || std::holds_alternative<Draw>(command) ||
                                std::holds_alternative<ExecuteIndirect>(command);
        std::vector<ResourceId> storage_images;
        std::vector<VulkanContext::PendingZero> zero_buffers;
        {
            std::lock_guard lock(context_.storage_mutex_);
            if (context_.storage_init_semaphore_ && context_.storage_init_semaphore_ != progress_ &&
                context_.storage_init_version_ != storage_init_seen_) {
                waits_.push_back(context_.storage_init_semaphore_);
                wait_values_.push_back(context_.storage_init_value_);
                storage_init_seen_ = context_.storage_init_version_;
            }
            // The set this submission binds may be a new generation whose table another queue's submission uploads:
            // wait for that one, as for the storage images (the table's buffers are used by one queue family only:
            // every D3D12 queue of the device is made in info.queue_family).
            if (context_.bindless_upload_semaphore_ && context_.bindless_upload_semaphore_ != progress_ &&
                context_.bindless_upload_version_ != bindless_upload_seen_) {
                waits_.push_back(context_.bindless_upload_semaphore_);
                wait_values_.push_back(context_.bindless_upload_value_);
            }
            bindless_upload_seen_ = context_.bindless_upload_version_;
            if (kind_ != QueueKind::copy && runs_shaders)
                storage_images.assign(context_.storage_pending_.begin(), context_.storage_pending_.end());
            if (!storage_images.empty()) context_.storage_pending_.clear();
            zero_buffers.swap(context_.zero_pending_);  // any queue can fill a buffer
        }
        for (const auto& zero : zero_buffers)
            context_.vk.vkCmdFillBuffer(command_buffer, zero.buffer, 0, VK_WHOLE_SIZE, 0);
        if (!zero_buffers.empty()) {
            VkMemoryBarrier filled{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            filled.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &filled, 0, nullptr, 0, nullptr);
        }
        for (const ResourceId id : storage_images) {
            const ResourceBinding* image = context_.resolve(id);
            if (!image || !image->image) continue;
            const VkImageSubresourceRange range{image->aspects, 0, image->mip_levels, 0, image->array_layers};
            auto pieces = detail::current(&next_tracker, id, range, VK_IMAGE_LAYOUT_UNDEFINED);
            detail::initialize(replay, command_buffer, *image, id, pieces, VK_IMAGE_LAYOUT_GENERAL, &next_tracker);
        }
        // Diagnostic (docs/frame_capture.md): the mid-submission pre-draw copy. The sampled textures of a readback-armed
        // draw are copied right after that draw's list, before any later list of the same submission can paint them, so
        // the pre-draw window can be compared with the post-submission readback.
        VkBuffer pre_buffer = VK_NULL_HANDLE;
        VkDeviceMemory pre_memory = VK_NULL_HANDLE;
        VkImage pre_images[2] = {};
        VkExtent2D pre_regions[2] = {};
        unsigned pre_texels[2] = {};
        ResourceId pre_resources[2] = {};
        unsigned pre_count = 0;
        bool pre_recorded = false;
        static std::atomic<std::uint64_t> submission_sequence{0};
        const std::uint64_t submission_index = submission_sequence.fetch_add(1, std::memory_order_relaxed);
        bool wrote_readback = false;
        VkPipelineStageFlags2 readback_stages = 0;
        std::uint32_t open_labels = 0;  // events may span the lists of one ExecuteCommandLists
        for (std::size_t i = 0; i < lists.size(); ++i) {
            const auto& list = prepared[i];
            emit(list, command_buffer, replay, &next_tracker, &open_labels);
            wrote_readback |= list.wrote_readback;
            readback_stages |= list.readback_src_stages;
            // The readback-armed draw's list: copy its sampled textures here, before any later list of this submission can
            // paint them (docs/frame_capture.md, the pre-draw capture).
            bool armed_list = false;
            std::uint32_t armed_draw = 0;
            for (std::size_t d = 0; d < list.draw_count && d < list.draws.size(); ++d) {
                if (!list.draws[d].armed_readback) continue;
                armed_list = true;
                armed_draw = static_cast<std::uint32_t>(d);
            }
            if (armed_list)
                std::fprintf(stderr,
                             "[rt] replay: order: submission %llu list %zu of %zu: %zu command(s), %zu draw(s)%s\n",
                             static_cast<unsigned long long>(submission_index), i, lists.size(),
                             lists[i]->commands().size(), list.draw_count,
                             armed_list ? ", the readback-armed draw is here" : "");
            if (armed_list) {
                for (std::size_t d = 0; d < list.draw_count && d < list.draws.size(); ++d) {
                    const detail::PreparedDraw& draw = list.draws[d];
                    const ResourceId target = draw.colors.empty() ? 0 : draw.colors[0].id;
                    std::fprintf(stderr,
                                 "[rt] replay: order:   draw %zu: pipeline 0x%llx target resource 0x%llx%s\n", d,
                                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(draw.graphics)),
                                 static_cast<unsigned long long>(target), d == armed_draw ? " (armed)" : "");
                }
                for (std::size_t l = 0; l < lists.size(); ++l) {
                    std::size_t clears = 0;
                    for (const Command& command : lists[l]->commands())
                        if (std::holds_alternative<ClearRenderTarget>(command)) ++clears;
                    if (clears)
                        std::fprintf(stderr,
                                     "[rt] replay: order:   list %zu has %zu render-target clear(s)\n", l, clears);
                }
                if (!pre_recorded && context_.vk.vkCmdCopyImageToBuffer) {
                    pre_recorded = true;
                    const unsigned armed = std::min<unsigned>(context_.sample_capture_count_, 2);
                    VkDeviceSize total = 0;
                    for (unsigned k = 0; k < armed; ++k) {
                        const SampleCapture& capture = context_.sample_captures_[k];
                        if (capture.image == VK_NULL_HANDLE) continue;
                        unsigned texel = 4;
                        switch (capture.format) {
                        case VK_FORMAT_R16G16B16A16_UNORM:
                        case VK_FORMAT_R16G16B16A16_SFLOAT: texel = 8; break;
                        case VK_FORMAT_R32G32B32A32_SFLOAT: texel = 16; break;
                        case VK_FORMAT_R32G32_UINT:
                        case VK_FORMAT_R32G32_SINT:
                        case VK_FORMAT_R32G32_SFLOAT: texel = 8; break;
                        default: texel = 4; break;
                        }
                        pre_images[k] = capture.image;
                        pre_texels[k] = texel;
                        pre_regions[k].width = std::min(128u, capture.extent.width);
                        pre_regions[k].height = std::min(128u, capture.extent.height);
                        pre_resources[k] = capture.resource;
                        pre_count = k + 1;
                        total += VkDeviceSize(pre_regions[k].width) * pre_regions[k].height * texel;
                    }
                    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                    buffer_info.size = total;
                    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                    if (pre_count != 0 && total != 0 &&
                        context_.vk.vkCreateBuffer(context_.device(), &buffer_info, nullptr, &pre_buffer) == VK_SUCCESS) {
                        VkMemoryRequirements requirements{};
                        context_.vk.vkGetBufferMemoryRequirements(context_.device(), pre_buffer, &requirements);
                        const auto& memory = context_.occlusion_resolve_memory_;
                        std::uint32_t type = memory.memoryTypeCount;
                        for (std::uint32_t t = 0; t < memory.memoryTypeCount; ++t) {
                            if (!(requirements.memoryTypeBits & (1u << t))) continue;
                            const VkMemoryPropertyFlags flags = memory.memoryTypes[t].propertyFlags;
                            if (type == memory.memoryTypeCount) type = t;
                            if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                                type = t;
                                break;
                            }
                        }
                        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                        allocation.allocationSize = requirements.size;
                        allocation.memoryTypeIndex = type;
                        if (type < memory.memoryTypeCount &&
                            context_.vk.vkAllocateMemory(context_.device(), &allocation, nullptr, &pre_memory) ==
                                VK_SUCCESS &&
                            context_.vk.vkBindBufferMemory(context_.device(), pre_buffer, pre_memory, 0) == VK_SUCCESS) {
                            VkDeviceSize base = 0;
                            for (unsigned k = 0; k < pre_count; ++k) {
                                VkImageMemoryBarrier to{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                                to.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                                to.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                                to.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                to.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                to.image = pre_images[k];
                                to.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                                to.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                                to.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                                context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                                                 nullptr, 1, &to);
                                VkBufferImageCopy region{};
                                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                region.imageExtent = {pre_regions[k].width, pre_regions[k].height, 1};
                                region.bufferOffset = base;
                                context_.vk.vkCmdCopyImageToBuffer(command_buffer, pre_images[k],
                                                                   VK_IMAGE_LAYOUT_GENERAL, pre_buffer, 1, &region);
                                base += VkDeviceSize(pre_regions[k].width) * pre_regions[k].height * pre_texels[k];
                            }
                        } else {
                            context_.vk.vkDestroyBuffer(context_.device(), pre_buffer, nullptr);
                            pre_buffer = VK_NULL_HANDLE;
                            if (pre_memory) context_.vk.vkFreeMemory(context_.device(), pre_memory, nullptr);
                            pre_memory = VK_NULL_HANDLE;
                        }
                    }
                }
            }

        }
        // Diagnostic (docs/frame_capture.md): the PS-only control. Draw the armed draw's OWN recorded command (same
        // render target, vertex/index buffers, counts, viewport, scissor and root arguments) through the title's
        // pipeline with only the pixel shader replaced by the project's constant-output fs_solid. The readback below
        // copies the target, so 32 160 240 means the title's vertex stage, input layout and rasterisation produce
        // coverage and the black comes from the title PS's own data; black means the geometry or state produces none.
        if (frame_dump::ps_control_enabled() && ps_control_armed && context_.frame_dump_ps_pipeline(*control_target.pipeline)) {
            CommandList ps_control;
            Draw ps_draw = control_target.draw;
            ps_draw.pipeline = frame_dump::ps_control_pipeline_id;
            PreparedList ps_prepared;
            const ReplayResult ps_result =
                ps_control.draw(std::move(ps_draw)) && ps_control.close()
                    ? prepare(ps_control, resolve, replay, ps_prepared)
                    : ReplayResult::unsupported;
            if (ps_result == ReplayResult::success) {
                emit(ps_prepared, command_buffer, replay, &next_tracker, &open_labels);
                ps_control_drew = true;
                std::fprintf(stderr,
                             "[rt] replay: frame dump PS control: drew the armed draw's own vertices through the title "
                             "pipeline (0x%llx) with fs_solid as the pixel shader, into 0x%llx; the readback below "
                             "copies it\n",
                             static_cast<unsigned long long>(
                                 reinterpret_cast<std::uintptr_t>(control_target.pipeline->pipeline)),
                             static_cast<unsigned long long>(control_target.id));
            } else {
                std::fprintf(stderr,
                             "[rt] replay: frame dump PS control: the control list was refused (result %d); the target "
                             "is left as the frame wrote it\n",
                             static_cast<int>(ps_result));
            }
        }
        // Diagnostic (docs/frame_capture.md): draw the project's own known-good full-screen triangle into the armed
        // draw's colour target, after the frame's commands and before the one-shot readback copies it. Its target
        // reading back 32 160 240 says this target's raster, binding, viewport, subresource and readback path all
        // write; reading back black says the replay's raster path for this target is what to chase.
        if (frame_dump::control_enabled() && control_armed && context_.frame_dump_control_pipeline(control_target.format)) {
            CommandList control;
            Draw control_draw;
            control_draw.pipeline = frame_dump::control_pipeline_id;
            control_draw.count = 3;     // vs_tri builds one full-screen triangle from gl_VertexIndex
            control_draw.instances = 1;
            control_draw.topology = 4;  // D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
            control_draw.viewport = Viewport{0.0f, 0.0f, static_cast<float>(control_target.width),
                                             static_cast<float>(control_target.height), 0.0f, 1.0f};
            control_draw.scissor = Rect{0, 0, static_cast<std::int32_t>(control_target.width),
                                        static_cast<std::int32_t>(control_target.height)};
            auto control_views = std::make_shared<DrawViews>();
            control_views->render_target_count = 1;
            control_views->render_targets[0] = TargetView{control_target.id, 0, 0, 1, ColorViewFormat::resource};
            control_draw.views = std::move(control_views);
            PreparedList control_prepared;
            const ReplayResult control_result = control.draw(std::move(control_draw)) && control.close()
                                                    ? prepare(control, resolve, replay, control_prepared)
                                                    : ReplayResult::unsupported;
            if (control_result == ReplayResult::success) {
                emit(control_prepared, command_buffer, replay, &next_tracker, &open_labels);
                control_drew = true;
                std::fprintf(stderr,
                             "[rt] replay: frame dump control: drew the vs_tri/fs_solid triangle into 0x%llx "
                             "(%ux%u, format %d) after the frame's commands; the readback below copies it\n",
                             static_cast<unsigned long long>(control_target.id), control_target.width,
                             control_target.height, static_cast<int>(control_target.format));
            } else {
                std::fprintf(stderr,
                             "[rt] replay: frame dump control: the control list was refused (result %d); the target "
                             "is left as the frame wrote it\n",
                             static_cast<int>(control_result));
            }
        }
        // Diagnostic (docs/upscale_standin.md): with PALMETTO_UPSCALE_STANDIN set, the layer presents the colour image
        // its measured upscale (compose) draw samples, by its own stretch pipeline, into the presented target the draw
        // writes - the stand-in for the console upscaler's pipelines when those cannot be made. The colour image is the
        // draw's own resolved T# (docs/frame_capture.md), stretched from its whole texel rectangle to the target's whole
        // pixel rectangle; nothing is colourised. With the colour window zero by design (docs/colour_path.md) the result
        // is black like the title's own compose output; PALMETTO_UPSCALE_STANDIN_CONTROL adds the project's coordinate
        // ramp as a second draw so the boot can prove the rectangle arithmetic. Opt-in, bounded, one line each.
        if (upscale_standin::enabled()) {
            static std::atomic<unsigned> standin_logged{0};
            const std::span<const PreparedList> submission(prepared.data(), lists.size());
            const frame_dump::ArmedTarget compose = frame_dump::compose_target(submission);
            // The stand-in executes at the dump/readback-armed draw: one measured execution per boot. The title's
            // colour image rotates through a ring (docs/colour_path.md sampled three positions), so a per-frame
            // substitution needs a per-image pipeline cache; that is the next step (docs/upscale_standin.md limits).
            // compose.index is the measured upscale draw's index inside this submission; armed.at is its global index.
            const bool at_armed_upscale_draw =
                armed.armed && compose.found && compose.pipeline != nullptr &&
                (armed.first + compose.index) == armed.at;
            if (!at_armed_upscale_draw) {
                if (standin_logged.fetch_add(1, std::memory_order_relaxed) < 4)
                    std::fprintf(stderr,
                                 "[rt] replay: upscale stand-in: no draw: the armed draw is %s, the measured upscale "
                                 "draw is %s (armed %llu, measured global %llu, measured index %llu)\n",
                                 armed.armed ? "in this submission" : "not in this submission",
                                 compose.found ? "in this submission" : "not in this submission",
                                 armed.armed ? armed.at : 0ull, compose.found ? armed.first + compose.index : 0ull,
                                 compose.found ? compose.index : 0ull);
            } else {
            // The colour image the upscale draw samples: the first T# the shim resolved to a layer image, read from the
            // draw's own recorded descriptor tables (the same resolution the sampled-texture readback uses).
            SampledKey colour;
            bool colour_found = false;
            if (compose.found && compose.pipeline && replay.sampled_table) {
                for (const std::uint64_t table_va :
                     frame_dump::descriptor_tables(compose.draw, compose.pipeline->table_windows)) {
                    SampledTable table;
                    if (!replay.sampled_table(table_va, 64, table)) continue;
                    for (std::uint32_t k = 0; k < table.keys; ++k) {
                        if (!table.key[k].resolved || table.key[k].image_handle == VK_NULL_HANDLE) continue;
                        colour = table.key[k];
                        colour_found = true;
                        break;
                    }
                    if (colour_found) break;
                }
            }
            upscale_standin::Inputs inputs;
            inputs.switch_on = true;
            inputs.has_pixel_stage = compose.found && compose.pipeline && compose.pipeline->has_pixel_stage;
            inputs.pixel_image_samples = compose.found && compose.pipeline ? compose.pipeline->ps_images : 0;
            inputs.colour_resolved = colour_found;
            inputs.colour_width = colour.extent.width;
            inputs.colour_height = colour.extent.height;
            inputs.target_width = compose.width;
            inputs.target_height = compose.height;
            const upscale_standin::Plan plan = upscale_standin::plan(inputs);
            if (!plan.draw) {
                if (standin_logged.fetch_add(1, std::memory_order_relaxed) < 4)
                    std::fprintf(stderr,
                                 "[rt] replay: upscale stand-in: no draw: %s (ps %u image sample(s), colour image %s, "
                                 "target %ux%u)\n",
                                 upscale_standin::refusal_name(plan.refusal), inputs.pixel_image_samples,
                                 colour_found ? "resolved" : "unresolved", compose.width, compose.height);
            } else {
                // The layout the colour image is in for this draw: the constant pass wrote it as a render target in
                // this submission, so the tracker names it. Sample it in SHADER_READ_ONLY (GENERAL stays GENERAL, the
                // layer's sampled_in_general mode); the stand-in transitions it there and back, so the tracker's
                // state is unchanged for the commands after this one.
                const VkImageLayout current = next_tracker.layout(colour.resource, 0);
                const VkImageLayout sampled = current == VK_IMAGE_LAYOUT_GENERAL
                                                  ? VK_IMAGE_LAYOUT_GENERAL
                                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                bool ready = current != VK_IMAGE_LAYOUT_UNDEFINED &&
                            context_.upscale_standin_ready(compose.format, colour.image_handle, colour.format, sampled);
                if (!ready) {
                    if (standin_logged.fetch_add(1, std::memory_order_relaxed) < 4)
                        std::fprintf(stderr,
                                     "[rt] replay: upscale stand-in: refused: the colour image's layout is %s\n",
                                     current == VK_IMAGE_LAYOUT_UNDEFINED ? "untracked" : "usable");
                } else {
                    const auto layout_barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                                                    VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                                                    VkPipelineStageFlags dst_stage) {
                        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        barrier.srcAccessMask = src_access;
                        barrier.dstAccessMask = dst_access;
                        barrier.oldLayout = from;
                        barrier.newLayout = to;
                        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.image = colour.image_handle;
                        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        replay.pipeline_barrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                                                &barrier);
                    };
                    constexpr VkAccessFlags kWriteAccess =
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    if (sampled != current &&
                        current != VK_IMAGE_LAYOUT_UNDEFINED)
                        layout_barrier(current, sampled, kWriteAccess, VK_ACCESS_SHADER_READ_BIT,
                                       VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    // Two draws: the shipping stretch of the title's colour image, then (when switched) the project's
                    // mapping control over the same target, which a readback can measure while the colour is black.
                    const bool control = upscale_standin::mapping_control_enabled();
                    const upscale_standin::Plan control_plan = upscale_standin::mapping_control(plan);
                    for (unsigned arm = 0; arm < (control ? 2u : 1u); ++arm) {
                        const upscale_standin::Push& push = arm ? control_plan.push : plan.push;
                        CommandList standin_list;
                        Draw standin_draw;
                        standin_draw.pipeline = upscale_standin::pipeline_id;
                        standin_draw.count = 3;  // the direction (vertex stage) builds one full-screen triangle
                        standin_draw.instances = 1;
                        standin_draw.topology = 4;  // D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                        standin_draw.viewport = Viewport{0.0f, 0.0f, static_cast<float>(compose.width),
                                                         static_cast<float>(compose.height), 0.0f, 1.0f};
                        standin_draw.scissor = Rect{0, 0, static_cast<std::int32_t>(compose.width),
                                                    static_cast<std::int32_t>(compose.height)};
                        auto standin_views = std::make_shared<DrawViews>();
                        standin_views->render_target_count = 1;
                        standin_views->render_targets[0] = TargetView{compose.id, 0, 0, 1, ColorViewFormat::resource};
                        standin_draw.views = std::move(standin_views);
                        upscale_standin::write_push(push, standin_draw.user_data.data());
                        ReplayFunctions standin_replay = replay;
                        standin_replay.bindless_set = context_.upscale_standin_set();  // our own set 0, bound by emit
                        PreparedList standin_prepared;
                        const ReplayResult standin_result =
                            standin_list.draw(std::move(standin_draw)) && standin_list.close()
                                ? prepare(standin_list, resolve, replay, standin_prepared)
                                : ReplayResult::unsupported;
                        if (standin_result != ReplayResult::success) {
                            if (standin_logged.fetch_add(1, std::memory_order_relaxed) < 4)
                                std::fprintf(stderr,
                                             "[rt] replay: upscale stand-in: the %s draw was refused in prepare (result "
                                             "%d)\n",
                                             arm ? "mapping-control" : "sampling", static_cast<int>(standin_result));
                            continue;
                        }
                        emit(standin_prepared, command_buffer, standin_replay, &next_tracker, &open_labels);
                        if (standin_logged.fetch_add(1, std::memory_order_relaxed) < 8)
                            std::fprintf(stderr,
                                         "[rt] replay: upscale stand-in: drew the %s full-screen triangle into 0x%llx "
                                         "(%ux%u, format %d): source %.0fx%.0f -> destination %.0fx%.0f, colour image "
                                         "0x%llx format %d in layout %d\n",
                                         arm ? "mapping-control" : "sampling",
                                         static_cast<unsigned long long>(compose.id), compose.width, compose.height,
                                         static_cast<int>(compose.format), push.src[2] - push.src[0],
                                         push.src[3] - push.src[1], push.dst[2] - push.dst[0],
                                         push.dst[3] - push.dst[1],
                                         static_cast<unsigned long long>(
                                             reinterpret_cast<std::uintptr_t>(colour.image_handle)),
                                         static_cast<int>(colour.format), static_cast<int>(sampled));
                    }
                    if (sampled != current && current != VK_IMAGE_LAYOUT_UNDEFINED)
                        layout_barrier(sampled, current, VK_ACCESS_SHADER_READ_BIT, kWriteAccess,
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                       VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                }
            }
            }  // at_armed_upscale_draw
        }
        close_labels(command_buffer, replay, open_labels);
        // Only flush GPU writes to the host domain when this batch actually wrote to a readback buffer.
        if (wrote_readback) {
            const VkPipelineStageFlags2 allowed = queue_supported_stages(kind_);
            VkPipelineStageFlags2 src_stages = readback_stages & allowed;
            if (!src_stages) src_stages = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            const bool copy_only =
                (src_stages & ~(VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT)) == 0;
            const VkAccessFlags2 src_access =
                copy_only ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_MEMORY_WRITE_BIT;

            if (context_.vk.vkCmdPipelineBarrier2) {
                VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                host.srcStageMask = src_stages;
                host.srcAccessMask = src_access;
                host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &host;
                context_.vk.vkCmdPipelineBarrier2(command_buffer, &dep);
            } else {
                const VkPipelineStageFlags legacy_src =
                    copy_only ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                host.srcAccessMask = copy_only ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_MEMORY_WRITE_BIT;
                host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                context_.vk.vkCmdPipelineBarrier(command_buffer, legacy_src, VK_PIPELINE_STAGE_HOST_BIT,
                                                 0, 1, &host, 0, nullptr, 0, nullptr);
            }
        }
        // Diagnostic readback (docs/page_content.md, docs/frame_capture.md): copy a 128x128 window of every armed draw's
        // colour target into host memory, then read them after this submission completes. Up to four draws can be armed
        // together (PALMETTO_READBACK_AT_DRAW=20000,20001), each window followed by a write control (the same target
        // cleared by us to a known colour). The images are put back in the layout the tracker expects.
        constexpr unsigned kMaxAttachmentCaptures = 4;
        AttachmentCapture attachment_captures[kMaxAttachmentCaptures] = {};
        unsigned attachment_count = 0;
        VkExtent2D attachment_regions[kMaxAttachmentCaptures] = {};
        VkOffset2D attachment_offsets[kMaxAttachmentCaptures] = {};
        unsigned attachment_texels[kMaxAttachmentCaptures] = {};
        bool attachment_control_recorded[kMaxAttachmentCaptures] = {};
        VkDeviceSize attachment_windows[kMaxAttachmentCaptures] = {};
        VkBuffer capture_buffer = VK_NULL_HANDLE;
        VkDeviceMemory capture_memory = VK_NULL_HANDLE;
        if (context_.capture_armed_.exchange(false) && context_.vk.vkCmdCopyImageToBuffer) {
            attachment_count = std::min<unsigned>(context_.attachment_capture_count_, kMaxAttachmentCaptures);
            context_.attachment_capture_count_ = 0;
            for (unsigned k = 0; k < attachment_count; ++k)
                attachment_captures[k] = context_.attachment_captures_[k];
            const bool full_target = frame_dump::full_target_enabled();
            VkDeviceSize total = 0;
            for (unsigned k = 0; k < attachment_count; ++k) {
                const AttachmentCapture& capture = attachment_captures[k];
                unsigned texel = 4;
                switch (capture.format) {
                case VK_FORMAT_R16G16B16A16_UNORM:
                case VK_FORMAT_R16G16B16A16_SFLOAT: texel = 8; break;
                case VK_FORMAT_R32G32B32A32_SFLOAT: texel = 16; break;
                case VK_FORMAT_R32G32_UINT:
                case VK_FORMAT_R32G32_SINT:
                case VK_FORMAT_R32G32_SFLOAT: texel = 8; break;
                default: texel = 4; break;
                }
                attachment_texels[k] = texel;
                attachment_regions[k].width = full_target ? capture.extent.width
                                                          : std::min(128u, capture.extent.width);
                attachment_regions[k].height = full_target ? capture.extent.height
                                                           : std::min(128u, capture.extent.height);
                attachment_offsets[k] = {0, 0};
                // Diagnostic (docs/frame_capture.md): PALMETTO_READBACK_CENTRE=1 copies a window at the target's centre
                // instead of its origin, so a region that is only black in one corner is not mistaken for an empty target.
                if (const char* centre = std::getenv("PALMETTO_READBACK_CENTRE");
                    centre && *centre && *centre != '0' && *centre != 'n')
                    attachment_offsets[k] = {
                        static_cast<std::int32_t>((capture.extent.width - attachment_regions[k].width) / 2),
                        static_cast<std::int32_t>((capture.extent.height - attachment_regions[k].height) / 2)};
                attachment_windows[k] =
                    VkDeviceSize(attachment_regions[k].width) * attachment_regions[k].height * texel;
                total += (full_target ? 1 : 2) * attachment_windows[k];
            }
            VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            // Two windows per capture: the frame's own content, then a write control (the same target cleared by us to a
            // known colour), so an all-zero readback cannot be an artefact of the copy or the host mapping.
            buffer_info.size = total;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (total && context_.vk.vkCreateBuffer(context_.device(), &buffer_info, nullptr, &capture_buffer) ==
                              VK_SUCCESS) {
                VkMemoryRequirements requirements{};
                context_.vk.vkGetBufferMemoryRequirements(context_.device(), capture_buffer, &requirements);
                const auto& memory = context_.occlusion_resolve_memory_;
                std::uint32_t type = memory.memoryTypeCount;
                for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                    if (!(requirements.memoryTypeBits & (1u << i))) continue;
                    const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
                    if (type == memory.memoryTypeCount) type = i;
                    if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                        type = i;
                        break;
                    }
                }
                VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocation.allocationSize = requirements.size;
                allocation.memoryTypeIndex = type;
                if (type < memory.memoryTypeCount &&
                    context_.vk.vkAllocateMemory(context_.device(), &allocation, nullptr, &capture_memory) ==
                        VK_SUCCESS &&
                    context_.vk.vkBindBufferMemory(context_.device(), capture_buffer, capture_memory, 0) == VK_SUCCESS) {
                    VkDeviceSize base = 0;
                    for (unsigned k = 0; k < attachment_count; ++k) {
                        const AttachmentCapture& capture = attachment_captures[k];
                        VkImageMemoryBarrier to{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        to.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        to.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        to.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        to.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        to.image = capture.image;
                        to.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        to.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                        to.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                                         &to);
                        VkBufferImageCopy region{};
                        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                        region.imageOffset = {attachment_offsets[k].x, attachment_offsets[k].y, 0};
                        region.imageExtent = {attachment_regions[k].width, attachment_regions[k].height, 1};
                        region.bufferOffset = base;
                        context_.vk.vkCmdCopyImageToBuffer(command_buffer, capture.image,
                                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_buffer, 1,
                                                           &region);
                        // Write control: clear the same target to a known colour, then copy it into the second window.
                        // Skipped when this target is also an armed sampled input (the frame's own sampled image can be
                        // one of its render targets): clearing it would corrupt the sampled readback, which is measured
                        // separately.
                        bool sampled_input = false;
                        for (unsigned s = 0; s < std::min(context_.sample_capture_count_, max_sampled_keys); ++s)
                            if (context_.sample_captures_[s].image == capture.image) sampled_input = true;
                        if (!full_target && context_.vk.vkCmdClearColorImage && !sampled_input) {
                            attachment_control_recorded[k] = true;
                            to.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                            to.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            to.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                            to.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                                             &to);
                            const VkClearColorValue control_colour{{0.2f, 0.4f, 0.8f, 1.0f}};
                            context_.vk.vkCmdClearColorImage(command_buffer, capture.image,
                                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &control_colour, 1,
                                                             &to.subresourceRange);
                            to.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            to.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                            to.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            to.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                            context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                                             &to);
                            VkBufferImageCopy control = region;
                            control.bufferOffset = base + attachment_windows[k];
                            context_.vk.vkCmdCopyImageToBuffer(command_buffer, capture.image,
                                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_buffer, 1,
                                                               &control);
                        }
                        to.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        to.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        to.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        to.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
                        context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                                                         0, nullptr, 1, &to);
                        base += (full_target ? 1 : 2) * attachment_windows[k];
                    }
                } else {
                    context_.vk.vkDestroyBuffer(context_.device(), capture_buffer, nullptr);
                    capture_buffer = VK_NULL_HANDLE;
                    if (capture_memory) context_.vk.vkFreeMemory(context_.device(), capture_memory, nullptr);
                    capture_memory = VK_NULL_HANDLE;
                }
            }
        }
        // Diagnostic (docs/frame_capture.md): the sampled-texture readbacks. Copies a 128x128 window of every resolved
        // sampled image into host memory after the submission, in GENERAL layout both ways (set 0 reads every image in
        // GENERAL). No write control: these are the title's textures and the diagnostic must not modify them.
        VkBuffer sample_buffers[max_sampled_keys] = {};
        VkDeviceMemory sample_memories[max_sampled_keys] = {};
        VkImage sample_images[max_sampled_keys] = {};
        VkExtent2D sample_regions[max_sampled_keys] = {};
        unsigned sample_texels[max_sampled_keys] = {};
        unsigned samples = 0;
        if (context_.vk.vkCmdCopyImageToBuffer) {
            const unsigned armed = std::min(context_.sample_capture_count_, max_sampled_keys);
            context_.sample_capture_count_ = 0;
            for (unsigned k = 0; k < armed && samples < max_sampled_keys; ++k) {
                const SampleCapture& capture = context_.sample_captures_[k];
                if (capture.image == VK_NULL_HANDLE) continue;
                unsigned texel = 4;
                switch (capture.format) {
                case VK_FORMAT_R16G16B16A16_UNORM:
                case VK_FORMAT_R16G16B16A16_SFLOAT: texel = 8; break;
                case VK_FORMAT_R32G32B32A32_SFLOAT: texel = 16; break;
                case VK_FORMAT_R32G32_UINT:
                case VK_FORMAT_R32G32_SINT:
                case VK_FORMAT_R32G32_SFLOAT: texel = 8; break;
                default: texel = 4; break;
                }
                const VkExtent2D region{std::min(128u, std::max(1u, capture.extent.width)),
                                        std::min(128u, std::max(1u, capture.extent.height))};
                VkBuffer buffer = VK_NULL_HANDLE;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                VkBufferCreateInfo sample_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                sample_info.size = VkDeviceSize(region.width) * region.height * texel;
                sample_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                if (context_.vk.vkCreateBuffer(context_.device(), &sample_info, nullptr, &buffer) != VK_SUCCESS)
                    continue;
                VkMemoryRequirements requirements{};
                context_.vk.vkGetBufferMemoryRequirements(context_.device(), buffer, &requirements);
                const auto& memory_properties = context_.occlusion_resolve_memory_;
                std::uint32_t type = memory_properties.memoryTypeCount;
                for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
                    if (!(requirements.memoryTypeBits & (1u << i))) continue;
                    const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
                    if (type == memory_properties.memoryTypeCount) type = i;
                    if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                        type = i;
                        break;
                    }
                }
                VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocation.allocationSize = requirements.size;
                allocation.memoryTypeIndex = type;
                const bool usable =
                    type < memory_properties.memoryTypeCount &&
                    context_.vk.vkAllocateMemory(context_.device(), &allocation, nullptr, &memory) == VK_SUCCESS &&
                    context_.vk.vkBindBufferMemory(context_.device(), buffer, memory, 0) == VK_SUCCESS;
                if (!usable) {
                    context_.vk.vkDestroyBuffer(context_.device(), buffer, nullptr);
                    if (memory) context_.vk.vkFreeMemory(context_.device(), memory, nullptr);
                    continue;
                }
                VkImageMemoryBarrier to{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                to.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                to.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                to.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to.image = capture.image;
                to.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                to.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
                to.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {region.width, region.height, 1};
                context_.vk.vkCmdCopyImageToBuffer(command_buffer, capture.image,
                                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
                to.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                to.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                to.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                to.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
                context_.vk.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                                 &to);
                sample_buffers[samples] = buffer;
                sample_memories[samples] = memory;
                sample_images[samples] = capture.image;
                sample_regions[samples] = region;
                sample_texels[samples] = texel;
                ++samples;
            }
        }
        const bool ended = context_.vk.vkEndCommandBuffer(command_buffer) == VK_SUCCESS;
        resource_lock.unlock();  // release resource_mutex_ before vkQueueSubmit while keeping tracker_lock held
        const bool wait_previous = ordered_timestamps && submitted_;
        if (wait_previous) {
            waits_.push_back(progress_);
            wait_values_.push_back(submitted_);
        }
        const bool submitted = ended && submit(command_buffer, VK_NULL_HANDLE, 0);
        if (!submitted && (!storage_images.empty() || !zero_buffers.empty())) {
            // not initialized after all: try again with the next submission
            std::lock_guard lock(context_.storage_mutex_);
            context_.storage_pending_.insert(storage_images.begin(), storage_images.end());
            context_.zero_pending_.insert(context_.zero_pending_.end(), zero_buffers.begin(), zero_buffers.end());
        }
        if (submitted && (!storage_images.empty() || !zero_buffers.empty())) {
            std::lock_guard lock(context_.storage_mutex_);
            context_.storage_init_semaphore_ = progress_;
            context_.storage_init_value_ = submitted_;
            ++context_.storage_init_version_;
            storage_init_seen_ = context_.storage_init_version_;
        }
        if (submitted && bindless_uploaded) {
            std::lock_guard lock(context_.storage_mutex_);
            context_.bindless_upload_semaphore_ = progress_;
            context_.bindless_upload_value_ = submitted_;
            ++context_.bindless_upload_version_;
            bindless_upload_seen_ = context_.bindless_upload_version_;
        }
        if (wait_previous && !submitted && !waits_.empty()) {  // a failed submit keeps the waits for the next one
            waits_.pop_back();
            wait_values_.pop_back();
        }
        if (submitted) {
            context_.tracker_ = std::move(next_tracker);
            slot->predicate = std::move(batch);
            slot->predicated_copy = std::move(copy_batch);
            slot->counted_dispatch = std::move(counted_batch);
            queries.commit();
            slot->segments = std::move(queries.retained);
        } else {
            discard();
        }
        if (depth24_guard.active) {
            context_.depth24_->retire(progress_, submitted ? submitted_ : 0);
            depth24_guard.active = false;
        }
        if (scratch_guard.active) {
            context_.scratch_->retire(progress_, submitted ? submitted_ : 0);
            scratch_guard.active = false;
        }
        // Under tracker_mutex_, so no other flush comes between this submit and the set's note of whether it reached
        // the queue (it keeps a single upload obligation).
        if (bindless_commit) bindless_commit(submitted);
        // The one-shot readbacks (docs/frame_capture.md): drain this queue, read every copied window and print one line
        // pair per armed draw - its colour target's content and the write control - labelled with the draw index and the
        // resource id, so the frame's even (intermediate) and odd (display) draws can be compared in one boot.
        if (submitted && capture_memory) {
            if (context_.vk.vkQueueWaitIdle(queue_) == VK_SUCCESS) {
                void* mapped = nullptr;
                if (context_.vk.vkMapMemory(context_.device(), capture_memory, 0, VK_WHOLE_SIZE, 0, &mapped) ==
                    VK_SUCCESS) {
                    const auto* attachment_bytes = static_cast<const std::uint8_t*>(mapped);
                    const bool full_target = frame_dump::full_target_enabled();
                    VkDeviceSize base = 0;
                    for (unsigned k = 0; k < attachment_count; ++k) {
                        const AttachmentCapture& capture = attachment_captures[k];
                        const unsigned capture_texel = attachment_texels[k];
                        const std::uint64_t pixels = std::uint64_t(attachment_regions[k].width) *
                                                     attachment_regions[k].height;
                        const auto* bytes = attachment_bytes + base;
                        std::uint64_t sums[4] = {0, 0, 0, 0}, non_black = 0, peak = 0, alpha_non_black = 0,
                                     alpha_peak = 0;
                        for (std::uint64_t i = 0; i < pixels; ++i) {
                            const std::uint8_t* texel = bytes + i * std::size_t(capture_texel);
                            const unsigned r = texel[0];
                            const unsigned g = capture_texel > 1 ? texel[1] : 0;
                            const unsigned b = capture_texel > 2 ? texel[2] : 0;
                            const unsigned a = capture_texel > 3 ? texel[3] : 0;
                            sums[0] += r;
                            sums[1] += g;
                            sums[2] += b;
                            sums[3] += a;
                            if (r || g || b) ++non_black;
                            if (a) ++alpha_non_black;
                            peak = std::max<std::uint64_t>(peak, std::max(r, std::max(g, b)));
                            alpha_peak = std::max<std::uint64_t>(alpha_peak, a);
                        }
                        if (full_target) {
                            const unsigned tile_w = (attachment_regions[k].width + 7) / 8;
                            const unsigned tile_h = (attachment_regions[k].height + 7) / 8;
                            std::uint64_t tile_non_black[64] = {};
                            for (std::uint64_t i = 0; i < pixels; ++i) {
                                const std::uint8_t* texel = bytes + i * std::size_t(capture_texel);
                                const unsigned r = texel[0];
                                const unsigned g = capture_texel > 1 ? texel[1] : 0;
                                const unsigned b = capture_texel > 2 ? texel[2] : 0;
                                if (!(r || g || b)) continue;
                                const std::uint64_t x = i % attachment_regions[k].width;
                                const std::uint64_t y = i / attachment_regions[k].width;
                                const unsigned tx = std::min<unsigned>(7, unsigned(x) / (tile_w ? tile_w : 1));
                                const unsigned ty = std::min<unsigned>(7, unsigned(y) / (tile_h ? tile_h : 1));
                                ++tile_non_black[ty * 8 + tx];
                            }
                            std::fprintf(stderr,
                                         "[rt] replay: full-target tile map [draw %llu target resource 0x%llx]: "
                                         "8x8 non-black counts\n",
                                         static_cast<unsigned long long>(capture.draw),
                                         static_cast<unsigned long long>(capture.resource));
                            for (unsigned ty = 0; ty < 8; ++ty) {
                                std::fprintf(stderr, "[rt] replay: full-target tile row %u:", ty);
                                for (unsigned tx = 0; tx < 8; ++tx)
                                    std::fprintf(stderr, " %llu",
                                                 static_cast<unsigned long long>(tile_non_black[ty * 8 + tx]));
                                std::fprintf(stderr, "\n");
                            }
                        }
                        // Diagnostic (docs/frame_capture.md): the copy runs after the frame's commands AND after any
                        // control draw, so when a control drew the window holds the control's output. Say which line this
                        // is: the PS-only control's readback is the coverage axis measured independently of the title PS.
                        const char* readback_label = ps_control_drew ? "frame dump PS control readback"
                                                      : control_drew ? "frame dump control readback"
                                                                     : "render-target readback";
                        std::fprintf(stderr,
                                     "[rt] replay: %s [draw %llu target resource 0x%llx image 0x%llx]: %ux%u at the "
                                     "target's offset of %u,%u of %ux%u, %u-byte texel: mean %llu %llu %llu, non-black "
                                     "%llu/%llu, peak %llu, alpha mean %llu, alpha non-black %llu/%llu, alpha peak "
                                     "%llu%s\n",
                                     readback_label, static_cast<unsigned long long>(capture.draw),
                                     static_cast<unsigned long long>(capture.resource),
                                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(capture.image)),
                                     attachment_regions[k].width, attachment_regions[k].height,
                                     attachment_offsets[k].x, attachment_offsets[k].y, capture.extent.width,
                                     capture.extent.height, capture_texel,
                                     static_cast<unsigned long long>(sums[0] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(sums[1] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(sums[2] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(non_black),
                                     static_cast<unsigned long long>(pixels),
                                     static_cast<unsigned long long>(peak),
                                     static_cast<unsigned long long>(sums[3] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(alpha_non_black),
                                     static_cast<unsigned long long>(pixels),
                                     static_cast<unsigned long long>(alpha_peak),
                                     (ps_control_drew || control_drew)
                                         ? "; the control drew after the frame's commands and this window is its output"
                                         : "");
                        // The write control: the same target, cleared by us to a known colour after the frame's commands.
                        if (attachment_control_recorded[k]) {
                        const std::uint8_t* control = bytes + pixels * capture_texel;
                        std::uint64_t control_sums[3] = {0, 0, 0}, control_non_black = 0, control_peak = 0;
                        for (std::uint64_t i = 0; i < pixels; ++i) {
                            const std::uint8_t* texel = control + i * std::size_t(capture_texel);
                            const unsigned r = texel[0];
                            const unsigned g = capture_texel > 1 ? texel[1] : 0;
                            const unsigned b = capture_texel > 2 ? texel[2] : 0;
                            control_sums[0] += r;
                            control_sums[1] += g;
                            control_sums[2] += b;
                            if (r || g || b) ++control_non_black;
                            control_peak = std::max<std::uint64_t>(control_peak, std::max(r, std::max(g, b)));
                        }
                        std::fprintf(stderr,
                                     "[rt] replay: write control [draw %llu target resource 0x%llx]: mean %llu %llu "
                                     "%llu, non-black %llu/%llu, peak %llu\n",
                                     static_cast<unsigned long long>(capture.draw),
                                     static_cast<unsigned long long>(capture.resource),
                                     static_cast<unsigned long long>(control_sums[0] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(control_sums[1] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(control_sums[2] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(control_non_black),
                                     static_cast<unsigned long long>(pixels),
                                     static_cast<unsigned long long>(control_peak));
                        } else {
                            std::fprintf(stderr,
                                         "[rt] replay: write control [draw %llu target resource 0x%llx]: skipped, the "
                                         "image is an armed sampled input\n",
                                         static_cast<unsigned long long>(capture.draw),
                                         static_cast<unsigned long long>(capture.resource));
                        }
                        // Diagnostic (docs/vertex_buffer_va.md section 10): write the copied window out as a binary PPM,
                        // so the content frame the title draws can be inspected as pixels rather than averages. Off
                        // unless PALMETTO_READBACK_PPM names an existing directory; bounded by
                        // PALMETTO_READBACK_PPM_LIMIT (default 4). 4-byte texels are written as their R,G,B bytes (the
                        // display targets are VK_FORMAT_R8G8B8A8_*); other texel sizes are skipped and named.
                        if (const char* ppm_dir = std::getenv("PALMETTO_READBACK_PPM"); ppm_dir && *ppm_dir) {
                            static std::atomic<unsigned> ppm_written{0};
                            unsigned ppm_limit = 4;
                            if (const char* limit_text = std::getenv("PALMETTO_READBACK_PPM_LIMIT"))
                                ppm_limit = static_cast<unsigned>(std::max(0, std::atoi(limit_text)));
                            if (ppm_written.load(std::memory_order_relaxed) < ppm_limit) {
                                if (capture_texel != 4) {
                                    std::fprintf(stderr,
                                                 "[rt] replay: readback ppm [draw %llu]: skipped, a %u-byte texel is "
                                                 "not written\n",
                                                 static_cast<unsigned long long>(capture.draw), capture_texel);
                                } else {
                                    char path[1024];
                                    std::snprintf(path, sizeof path,
                                                  "%s\\readback_draw%llu_res%llx_%ux%u.ppm", ppm_dir,
                                                  static_cast<unsigned long long>(capture.draw),
                                                  static_cast<unsigned long long>(capture.resource),
                                                  attachment_regions[k].width, attachment_regions[k].height);
                                    if (FILE* file = std::fopen(path, "wb")) {
                                        std::fprintf(file, "P6\n%u %u\n255\n", attachment_regions[k].width,
                                                     attachment_regions[k].height);
                                        std::vector<std::uint8_t> row(std::size_t(attachment_regions[k].width) * 3);
                                        for (unsigned y = 0; y < attachment_regions[k].height; ++y) {
                                            const std::uint8_t* texel =
                                                bytes + std::size_t(y) * attachment_regions[k].width * capture_texel;
                                            for (unsigned x = 0; x < attachment_regions[k].width; ++x) {
                                                row[std::size_t(x) * 3 + 0] = texel[0];
                                                row[std::size_t(x) * 3 + 1] = texel[1];
                                                row[std::size_t(x) * 3 + 2] = texel[2];
                                                texel += capture_texel;
                                            }
                                            std::fwrite(row.data(), 1, row.size(), file);
                                        }
                                        std::fclose(file);
                                        ppm_written.fetch_add(1, std::memory_order_relaxed);
                                        std::fprintf(stderr,
                                                     "[rt] replay: readback ppm [draw %llu target resource 0x%llx]: "
                                                     "%ux%u written to %s\n",
                                                     static_cast<unsigned long long>(capture.draw),
                                                     static_cast<unsigned long long>(capture.resource),
                                                     attachment_regions[k].width, attachment_regions[k].height, path);
                                    }
                                }
                            }
                        }
                        base += (full_target ? 1 : 2) * attachment_windows[k];
                    }
                    context_.vk.vkUnmapMemory(context_.device(), capture_memory);
                }
            }
        }
        // Diagnostic (docs/frame_capture.md): the id comparison. Each sampled T#'s layer image is compared with every
        // armed target of the frame (handle and D3D12 resource id), so "the six-image pass samples the intermediate
        // target, the display buffer or a texture of its own" is a printed identity, not an inference.
        for (unsigned k = 0; k < samples; ++k) {
            const ResourceId sampled_resource = k < max_sampled_keys ? context_.sample_captures_[k].resource : 0;
            bool matched = false;
            for (unsigned a = 0; a < attachment_count; ++a) {
                const bool same_image = sample_images[k] != VK_NULL_HANDLE &&
                                        sample_images[k] == attachment_captures[a].image;
                const bool same_resource = sampled_resource && sampled_resource == attachment_captures[a].resource;
                if (!same_image && !same_resource) continue;
                matched = true;
                std::fprintf(stderr,
                             "[rt] replay: colour path id match: sampled image 0x%llx (resource 0x%llx) is draw %llu's "
                             "target (image 0x%llx, resource 0x%llx, %s)\n",
                             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(sample_images[k])),
                             static_cast<unsigned long long>(sampled_resource),
                             static_cast<unsigned long long>(attachment_captures[a].draw),
                             static_cast<unsigned long long>(
                                 reinterpret_cast<std::uintptr_t>(attachment_captures[a].image)),
                             static_cast<unsigned long long>(attachment_captures[a].resource),
                             same_image ? "same layer image" : "same D3D12 resource");
            }
            if (!matched)
                std::fprintf(stderr,
                             "[rt] replay: colour path id match: sampled image 0x%llx (resource 0x%llx) matches no armed "
                             "frame target\n",
                             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(sample_images[k])),
                             static_cast<unsigned long long>(sampled_resource));
        }
        // The sampled-texture windows (docs/frame_capture.md): what each resolved sampled image holds, so "the textures
        // are black" can be told from "the textures are fine and the pixel shader computes black".
        if (submitted) {
            for (unsigned k = 0; k < samples; ++k) {
                if (sample_memories[k] == VK_NULL_HANDLE) continue;
                if (context_.vk.vkQueueWaitIdle(queue_) != VK_SUCCESS) break;
                void* mapped = nullptr;
                if (context_.vk.vkMapMemory(context_.device(), sample_memories[k], 0, VK_WHOLE_SIZE, 0, &mapped) !=
                    VK_SUCCESS)
                    continue;
                const auto* bytes = static_cast<const std::uint8_t*>(mapped);
                const std::uint64_t pixels = std::uint64_t(sample_regions[k].width) * sample_regions[k].height;
                const unsigned texel_bytes = sample_texels[k];
                std::uint64_t sums[4] = {0, 0, 0, 0}, non_black = 0, peak = 0, alpha_non_black = 0, alpha_peak = 0;
                for (std::uint64_t i = 0; i < pixels; ++i) {
                    const std::uint8_t* texel = bytes + i * std::size_t(texel_bytes);
                    const unsigned r = texel[0];
                    const unsigned g = texel_bytes > 1 ? texel[1] : 0;
                    const unsigned b = texel_bytes > 2 ? texel[2] : 0;
                    const unsigned a = texel_bytes > 3 ? texel[3] : 0;
                    sums[0] += r;
                    sums[1] += g;
                    sums[2] += b;
                    sums[3] += a;
                    if (r || g || b) ++non_black;
                    if (a) ++alpha_non_black;
                    peak = std::max<std::uint64_t>(peak, std::max(r, std::max(g, b)));
                    alpha_peak = std::max<std::uint64_t>(alpha_peak, a);
                }
                std::fprintf(stderr,
                             "[rt] replay: frame dump sample readback [%u/%u] image 0x%llx: %ux%u window, %u-byte "
                             "texel: mean %llu %llu %llu, non-black %llu/%llu, peak %llu, alpha mean %llu, alpha "
                             "non-black %llu/%llu, alpha peak %llu\n",
                             k + 1, samples,
                             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(sample_images[k])),
                             sample_regions[k].width, sample_regions[k].height, texel_bytes,
                             static_cast<unsigned long long>(sums[0] / (pixels ? pixels : 1)),
                             static_cast<unsigned long long>(sums[1] / (pixels ? pixels : 1)),
                             static_cast<unsigned long long>(sums[2] / (pixels ? pixels : 1)),
                             static_cast<unsigned long long>(non_black),
                             static_cast<unsigned long long>(pixels),
                             static_cast<unsigned long long>(peak),
                             static_cast<unsigned long long>(sums[3] / (pixels ? pixels : 1)),
                             static_cast<unsigned long long>(alpha_non_black),
                             static_cast<unsigned long long>(pixels),
                             static_cast<unsigned long long>(alpha_peak));
                context_.vk.vkUnmapMemory(context_.device(), sample_memories[k]);
            }
        }
        for (unsigned k = 0; k < samples; ++k) {
            if (sample_buffers[k]) context_.vk.vkDestroyBuffer(context_.device(), sample_buffers[k], nullptr);
            if (sample_memories[k]) context_.vk.vkFreeMemory(context_.device(), sample_memories[k], nullptr);
        }
        // Diagnostic (docs/frame_capture.md): the pre-draw window, copied mid-submission, printed next to the
        // post-submission sample readback so the two can be compared directly.
        if (submitted && pre_memory && pre_count) {
            if (context_.vk.vkQueueWaitIdle(queue_) == VK_SUCCESS) {
                void* mapped = nullptr;
                if (context_.vk.vkMapMemory(context_.device(), pre_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
                    const auto* bytes = static_cast<const std::uint8_t*>(mapped);
                    VkDeviceSize base = 0;
                    for (unsigned k = 0; k < pre_count; ++k) {
                        const unsigned texel_bytes = pre_texels[k];
                        const std::uint64_t pixels =
                            std::uint64_t(pre_regions[k].width) * pre_regions[k].height;
                        std::uint64_t sums[4] = {0, 0, 0, 0}, non_black = 0, peak = 0, alpha_non_black = 0,
                                     alpha_peak = 0;
                        for (std::uint64_t i = 0; i < pixels; ++i) {
                            const std::uint8_t* texel = bytes + base + i * std::size_t(texel_bytes);
                            const unsigned r = texel[0];
                            const unsigned g = texel_bytes > 1 ? texel[1] : 0;
                            const unsigned b = texel_bytes > 2 ? texel[2] : 0;
                            const unsigned a = texel_bytes > 3 ? texel[3] : 0;
                            sums[0] += r;
                            sums[1] += g;
                            sums[2] += b;
                            sums[3] += a;
                            if (r || g || b) ++non_black;
                            if (a) ++alpha_non_black;
                            peak = std::max<std::uint64_t>(peak, std::max(r, std::max(g, b)));
                            alpha_peak = std::max<std::uint64_t>(alpha_peak, a);
                        }
                        std::fprintf(stderr,
                                     "[rt] replay: pre-draw sample readback [%u/%u] image 0x%llx (resource 0x%llx): "
                                     "%ux%u window, %u-byte texel: mean %llu %llu %llu, non-black %llu/%llu, peak "
                                     "%llu, alpha mean %llu, alpha non-black %llu/%llu, alpha peak %llu\n",
                                     k + 1, pre_count,
                                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pre_images[k])),
                                     static_cast<unsigned long long>(pre_resources[k]), pre_regions[k].width,
                                     pre_regions[k].height, texel_bytes,
                                     static_cast<unsigned long long>(sums[0] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(sums[1] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(sums[2] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(non_black),
                                     static_cast<unsigned long long>(pixels),
                                     static_cast<unsigned long long>(peak),
                                     static_cast<unsigned long long>(sums[3] / (pixels ? pixels : 1)),
                                     static_cast<unsigned long long>(alpha_non_black),
                                     static_cast<unsigned long long>(pixels),
                                     static_cast<unsigned long long>(alpha_peak));
                        base += VkDeviceSize(pre_regions[k].width) * pre_regions[k].height * texel_bytes;
                    }
                    context_.vk.vkUnmapMemory(context_.device(), pre_memory);
                }
            }
        }
        if (pre_buffer) context_.vk.vkDestroyBuffer(context_.device(), pre_buffer, nullptr);
        if (pre_memory) context_.vk.vkFreeMemory(context_.device(), pre_memory, nullptr);
        if (capture_buffer) context_.vk.vkDestroyBuffer(context_.device(), capture_buffer, nullptr);
        if (capture_memory) context_.vk.vkFreeMemory(context_.device(), capture_memory, nullptr);
        tracker_lock.unlock();
        return submitted ? ReplayResult::success : ReplayResult::unsupported;
    }

    void marker_now(DebugMarker::Kind kind, const char* label) {
        const DebugLabelFunctions& f = context_.labels_;
        if (!f.complete()) return;
        if (kind == DebugMarker::Kind::end && !open_labels_) return;
        VkDebugUtilsLabelEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        info.pLabelName = label ? label : "";
        std::lock_guard lock(context_.submit_mutex());
        if (kind == DebugMarker::Kind::set) {
            f.queue_insert(queue_, &info);
        } else if (kind == DebugMarker::Kind::begin) {
            f.queue_begin(queue_, &info);
            ++open_labels_;
        } else {
            f.queue_end(queue_);
            --open_labels_;
        }
    }

    // ---- Wait-before-Signal: operations held on the CPU (queue_holds.h) ----

    // One held operation, with the waits that were pending when it was asked for.
    struct Deferred {
        enum class Kind : std::uint8_t { batch, signal, marker };
        Kind kind = Kind::batch;
        std::uint64_t seq = 0;  // QueueHolds::next_seq(): the order operations were held in, across queues
        // Nonzero: this batch is also held for the deferred pipelines the hold policy's entry for this token names
        // (docs/peer_port_streaming.md). 0: held only for order and/or a pending Wait.
        std::uint64_t hold_token = 0;
        std::vector<VkSemaphore> waits;
        std::vector<std::uint64_t> wait_values;
        std::vector<CommandList> lists;  // batch: copies, so the application may Reset its lists after the call
        VkSemaphore fence = VK_NULL_HANDLE;  // signal
        std::uint64_t value = 0;
        DebugMarker::Kind marker_kind = DebugMarker::Kind::set;  // marker
        std::string label;
        // Alive until the operation has run: the fences it waits on and signals, the pipeline states and root
        // signatures of the lists (the application may release them right after the call).
        std::vector<std::shared_ptr<void>> pins;
        std::vector<std::shared_ptr<void>> wait_pins;  // the fences of `waits`
    };

    bool waits_pending(const std::vector<VkSemaphore>& semaphores, const std::vector<std::uint64_t>& values) const {
        for (std::size_t i = 0; i < semaphores.size(); ++i)
            if (context_.holds().pending(semaphores[i], values[i])) return true;
        return false;
    }

    // The next batch or Signal has to be held: something is held ahead of it (queue order), or a Wait it carries
    // has no submitted Signal yet. Called with state_mutex_ held.
    bool must_hold() const { return !held_.empty() || waits_pending(waits_, wait_values_); }

    // The waits are consumed: submitted as `ticket` (0: dropped on device loss). What the application may release
    // (the fences they named) is kept until that submission has completed on the GPU, as a D3D12 queue does:
    // destroying a semaphore a pending submission uses is a validation error. Retired pins are destroyed by the
    // caller after state_mutex_ is released (drain_pins): the last release of a fence waits for a running release,
    // which must not be waiting for this lock.
    void clear_waits(std::uint64_t ticket = 0) {
        waits_.clear();
        wait_values_.clear();
        retire_pins(wait_pins_, ticket);
    }

    void retire_pins(std::vector<std::shared_ptr<void>>& pins, std::uint64_t ticket) {
        for (auto& pin : pins) {
            if (ticket) pin_queue_.push_back({ticket, std::move(pin)});
            else retired_pins_.push_back(std::move(pin));
        }
        pins.clear();
    }

    // Moves the pins whose submission has completed (and the ones retired outright) to `out`. state_mutex_ held.
    void drain_pins(std::vector<std::shared_ptr<void>>& out) {
        out.swap(retired_pins_);
        if (pin_queue_.empty()) return;
        const std::uint64_t done = completed();
        while (!pin_queue_.empty() && pin_queue_.front().ticket <= done) {
            out.push_back(std::move(pin_queue_.front().pin));
            pin_queue_.pop_front();
        }
    }

    // Appends `op`, which takes the pending waits with it (a marker has none). Called with state_mutex_ held.
    void hold(Deferred&& op) {
        op.seq = context_.holds().next_seq();
        if (op.kind != Deferred::Kind::marker) {
            op.waits = std::move(waits_);
            op.wait_values = std::move(wait_values_);
            op.wait_pins = std::move(wait_pins_);
            waits_.clear();
            wait_values_.clear();
            wait_pins_.clear();
        }
        held_.push_back(std::move(op));
        context_.holds().held_changed(1);
    }

    // Runs one held operation as the call would have, with its own waits and none of the ones queued since.
    // A submission that fails leaves its waits for the next one, as when the call is not held (a transient
    // vkQueueSubmit failure is retried by the application's next call), ahead of the waits asked for since.
    void run(Deferred& op) {
        auto later_waits = std::move(waits_);
        auto later_values = std::move(wait_values_);
        auto later_pins = std::move(wait_pins_);
        waits_ = std::move(op.waits);
        wait_values_ = std::move(op.wait_values);
        wait_pins_ = std::move(op.wait_pins);  // the fences stay alive until the submission has consumed their waits
        switch (op.kind) {
        case Deferred::Kind::batch: {
            std::vector<const CommandList*> lists;
            for (const CommandList& list : op.lists) lists.push_back(&list);
            // The pipeline states and root signatures of the lists are in op.pins (see execute()): they live until
            // the batch has been recorded and then, with the fences, until its submission has completed (below).
            const ReplayResult result = execute_now(lists);
            if (result != ReplayResult::success) {
                int none = static_cast<int>(ReplayResult::success);
                held_failure_.compare_exchange_strong(none, static_cast<int>(result));
                std::fprintf(stderr, "[rt] d3d12: a held ExecuteCommandLists failed when it was released (result %d); "
                                     "nothing was submitted for it, and a Signal behind it still signals\n",
                             static_cast<int>(result));
            }
            break;
        }
        case Deferred::Kind::signal:
            if (!submit(VK_NULL_HANDLE, op.fence, op.value))
                std::fprintf(stderr, "[rt] d3d12: a held Signal failed when it was released\n");
            break;
        case Deferred::Kind::marker:
            marker_now(op.marker_kind, op.label.c_str());
            break;
        }
        retire_pins(op.pins, submitted_);  // the fence it signals, the pipeline states: until its submission is done
        waits_.insert(waits_.end(), later_waits.begin(), later_waits.end());
        wait_values_.insert(wait_values_.end(), later_values.begin(), later_values.end());
        for (auto& pin : later_pins) wait_pins_.push_back(std::move(pin));
    }

    // A held batch may run when every Wait it carries has a submitted Signal and every deferred pipeline it names
    // has finished (docs/peer_port_streaming.md). The pipeline half is absent for an ordinary held batch (token 0).
    bool ready(const Deferred& op) const {
        if (waits_pending(op.waits, op.wait_values)) return false;
        return !op.hold_token || context_.pipeline_hold().ready(op.hold_token);
    }

    // The previous behaviour for one held submission, forced by the hold budget: wait for (or build) every pipeline
    // it names on this thread, then run it. Only called with state_mutex_ held and only when the budget is exceeded or
    // expired, so the fallback the slice keeps is exactly the old synchronous wait - moved off the submission's own
    // call to the thread that finds the hold at its bound (the submitting thread at the bound, the releasing thread at
    // expiry), never to a worker of the title.
    void force_pipeline_wait_locked(std::uint64_t token, Deferred& op) {
        std::vector<const CommandList*> lists;
        lists.reserve(op.lists.size());
        for (CommandList& list : op.lists) lists.push_back(&list);
        context_.pipeline_hold().release(token);
        timeline::line("pipeline hold forced n=%u", static_cast<unsigned>(lists.size()));
        if (!lists.empty()) context_.prepare_pipelines(lists);
        run(op);
    }

    // At the hold bound: complete the oldest held submissions that wait only for pipelines by the old synchronous
    // wait, in order, until the budget has room. A batch with fence waits cannot be forced (that would reorder it past
    // the work ahead of it), and neither can a batch behind a held Signal/marker: the queue keeps its order.
    void force_oldest_pipeline_holds_locked() {
        std::vector<std::shared_ptr<void>> retired;
        while (!held_.empty() && context_.pipeline_hold().at_bound()) {
            Deferred& front = held_.front();
            if (!front.hold_token || !front.waits.empty()) break;
            const std::uint64_t token = front.hold_token;
            if (context_.pipeline_hold().ready(token)) break;  // the next pump releases it
            Deferred op = std::move(front);
            held_.pop_front();
            context_.holds().held_changed(-1);
            context_.pipeline_hold().note_forced();
            force_pipeline_wait_locked(token, op);
        }
        drain_pins(retired);
    }

    // Holder: called by the thread whose Signal made a held operation ready (never with a queue lock held).
    bool release_ready() override {
        std::vector<std::shared_ptr<void>> retired;
        bool released = false;
        {
            std::lock_guard state(state_mutex_);
            const auto now = std::chrono::steady_clock::now();
            while (!held_.empty()) {
                Deferred& front = held_.front();
                const bool lost = context_.is_device_lost();
                if (!lost && !ready(front)) {
                    // The budget: a submission held only for pipelines longer than it may be gets the old synchronous
                    // wait here (docs/peer_port_streaming.md). A batch with fence waits keeps waiting for its fence.
                    if (front.hold_token && front.waits.empty() &&
                        context_.pipeline_hold().expired(front.hold_token, now)) {
                        const std::uint64_t token = front.hold_token;
                        Deferred op = std::move(front);
                        held_.pop_front();
                        context_.holds().held_changed(-1);
                        context_.pipeline_hold().note_timeout();
                        context_.pipeline_hold().note_forced();
                        force_pipeline_wait_locked(token, op);
                        released = true;
                        continue;
                    }
                    break;
                }
                Deferred op = std::move(held_.front());
                held_.pop_front();
                context_.holds().held_changed(-1);
                if (op.hold_token) {
                    if (lost) context_.pipeline_hold().drop(op.hold_token);
                    else context_.pipeline_hold().release(op.hold_token);
                }
                if (!lost) run(op);
                released = true;
            }
            drain_pins(retired);
        }
        return released;
    }

    // Holder: the fence is released, so what waits on it can never run.
    void discard_waiting_on(VkSemaphore semaphore) override {
        std::vector<Deferred> dropped;
        {
            std::lock_guard state(state_mutex_);
            for (auto it = held_.begin(); it != held_.end();) {
                if (std::find(it->waits.begin(), it->waits.end(), semaphore) == it->waits.end() &&
                    !(it->kind == Deferred::Kind::signal && it->fence == semaphore)) {
                    ++it;
                    continue;
                }
                std::fprintf(stderr, "[rt] d3d12: a fence was released while a queue still held work that names it; "
                                     "that work is dropped\n");
                dropped.push_back(std::move(*it));
                it = held_.erase(it);
                context_.holds().held_changed(-1);
            }
            for (const Deferred& op : dropped)
                if (op.hold_token) context_.pipeline_hold().drop(op.hold_token);
        }
    }

    // Holder: the sequence number of the oldest held operation.
    std::uint64_t oldest_held_seq() override {
        std::lock_guard state(state_mutex_);
        return held_.empty() ? 0 : held_.front().seq;
    }

    // A queue destroyed with held work. Reported, not waited for: nothing was submitted for it, so the VkQueue is
    // idle and the destructor goes on. The fences of held Signals will never reach their values, so what is waiting
    // for them on the CPU (SetEventOnCompletion, as a released fence completes them) is woken, and the batches
    // that wait for them on other queues stay held (and are reported when those queues are destroyed).
    void report_held_at_teardown() {
        std::deque<Deferred> dropped;
        {
            std::lock_guard state(state_mutex_);
            if (held_.empty()) return;
            std::uint64_t value = 0;
            for (const Deferred& op : held_)
                for (std::size_t i = 0; i < op.waits.size() && !value; ++i)
                    if (context_.holds().pending(op.waits[i], op.wait_values[i])) value = op.wait_values[i];
            std::size_t signals = 0;
            for (const Deferred& op : held_) signals += op.kind == Deferred::Kind::signal;
            std::fprintf(stderr, "[rt] d3d12: queue destroyed with %zu held operation(s) never submitted: waits for a "
                                 "fence value (%llu) that no Signal reached; the fence was never signaled%s\n",
                         held_.size(), static_cast<unsigned long long>(value),
                         signals ? "; CPU waits on the fences of its held Signals are completed" : "");
            for (const Deferred& op : held_)
                if (op.kind == Deferred::Kind::signal) context_.cancel_fence_waits(op.fence);
            for (const Deferred& op : held_)
                if (op.hold_token) context_.pipeline_hold().drop(op.hold_token);
            context_.holds().held_changed(-static_cast<std::int64_t>(held_.size()));
            dropped.swap(held_);
        }
        // The pins (fences, pipeline states) are released here, outside the lock.
    }

    struct CommandSlot {
        VkCommandBuffer buffer;
        std::uint64_t ticket;
        std::vector<std::shared_ptr<const OcclusionSegmentPool>> segments;
        std::vector<VkEvent> events;  // order_timestamp_copy's, reset and reused once the slot completes
        std::unique_ptr<PredicatedBufferCopier::Batch> predicated_copy;
        std::unique_ptr<IndirectDispatchCounter::Batch> counted_dispatch;
        std::unique_ptr<PredicateReducer::Batch> predicate; // retained until this slot completes
        std::vector<KhrInstanceStage> khr_instances;  // KHR ARRAY_OF_POINTERS staging, same lifetime
        std::vector<KhrAsQueryPool> khr_as_queries;   // KHR postbuild COMPACTED_SIZE query pools, same lifetime
    };

    struct PendingBindings {
        std::uint64_t ticket = 0;
        // Vulkan needs the resource until the bind completes and the previous allocation until it is unmapped.
        std::vector<std::shared_ptr<const TileHeapAllocation>> old_heaps;
        std::shared_ptr<SparseResource> resource;
    };

    static bool valid_tiles(const SparseResource& resource, TileRegion region) {
        return region.count && region.first < resource.tile_count && region.count <= resource.tile_count - region.first;
    }

    bool initialize_sparse_image_layout(ResourceId id) {
        std::lock_guard lock(context_.tracker_mutex_);
        if (context_.tracker_.layout(id, 0) != VK_IMAGE_LAYOUT_UNDEFINED) return true;
        auto* slot = acquire();
        if (!slot) return false;
        const VkCommandBuffer command_buffer = slot->buffer;
        auto& vk = context_.vk;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vk.vkBeginCommandBuffer(command_buffer, &begin) != VK_SUCCESS) {
            slot->ticket = 0;
            return false;
        }
        // Transition while the image is still unbound. Doing this after binding could discard another image's
        // contents in an aliased page. Subsequent uses keep the image in GENERAL.
        constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        detail::image_barrier(context_.replay_, command_buffer, context_.resources_.at(id), range,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        if (vk.vkEndCommandBuffer(command_buffer) != VK_SUCCESS || !submit(command_buffer, VK_NULL_HANDLE, 0)) {
            slot->ticket = 0;
            return false;
        }
        context_.tracker_.set(id, range, VK_IMAGE_LAYOUT_GENERAL);
        return true;
    }

    bool bind_tiles(ResourceId id, const std::shared_ptr<SparseResource>& resource,
                    std::map<std::uint64_t, TileMapping>& changes) {
        auto& vk = context_.vk;
        if (!vk.vkQueueBindSparse || context_.is_device_lost()) return false;
        PendingBindings pending{0, {}, resource};
        std::vector<VkSparseMemoryBind> binds;
        std::vector<VkSparseImageMemoryBind> image_binds;
        for (const auto& [index, page] : changes) {
            const auto old = resource->mapped.find(index);
            if (old != resource->mapped.end()) {
                if (old->second == page) continue;
                pending.old_heaps.push_back(old->second.heap);
            } else if (!page.heap) {
                continue;
            }
            if (page.heap && !(resource->memory_types & (1u << page.heap->memory_type))) return false;
            const VkDeviceMemory memory = page.heap ? page.heap->memory : VK_NULL_HANDLE;
            if (resource->image) {
                constexpr auto shape = element_tile_shape(false, 4);
                const auto extent = resource->image_extent;
                const auto columns = 1 + (extent.width - 1) / shape.width;
                const auto x = std::uint32_t(index % columns) * shape.width;
                const auto y = std::uint32_t(index / columns) * shape.height;
                image_binds.push_back({
                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}, {std::int32_t(x), std::int32_t(y), 0},
                    {std::min(shape.width, extent.width - x), std::min(shape.height, extent.height - y), 1},
                    memory, page.offset, 0});
                continue;
            }
            const VkDeviceSize resource_offset = index * tile_size_in_bytes;
            if (!binds.empty()) {
                auto& previous = binds.back();
                if (previous.resourceOffset + previous.size == resource_offset && previous.memory == memory &&
                    (!memory || previous.memoryOffset + previous.size == page.offset)) {
                    previous.size += tile_size_in_bytes;
                    continue;
                }
            }
            binds.push_back({resource_offset, tile_size_in_bytes, memory, page.offset, 0});
        }
        if (binds.empty() && image_binds.empty()) return true;
        if (!image_binds.empty() && !initialize_sparse_image_layout(id)) return false;
        const auto ticket = submitted_ + 1;
        pending.ticket = ticket;
        collect_bindings(completed());
        auto semaphores = waits_;
        auto values = wait_values_;
        // Sparse binds are not ordered against work on the same Vulkan queue by submission order alone.
        if (submitted_) {
            semaphores.push_back(progress_);
            values.push_back(submitted_);
        }
        VkTimelineSemaphoreSubmitInfo tickets{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        tickets.waitSemaphoreValueCount = std::uint32_t(values.size());
        tickets.pWaitSemaphoreValues = values.data();
        tickets.signalSemaphoreValueCount = 1;
        tickets.pSignalSemaphoreValues = &ticket;
        const VkSparseBufferMemoryBindInfo buffer_bind{resource->buffer, std::uint32_t(binds.size()), binds.data()};
        const VkSparseImageMemoryBindInfo image_bind{
            resource->image, std::uint32_t(image_binds.size()), image_binds.data()};
        VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, &tickets};
        info.waitSemaphoreCount = std::uint32_t(semaphores.size());
        info.pWaitSemaphores = semaphores.data();
        if (!binds.empty()) {
            info.bufferBindCount = 1;
            info.pBufferBinds = &buffer_bind;
        }
        if (!image_binds.empty()) {
            info.imageBindCount = 1;
            info.pImageBinds = &image_bind;
        }
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &progress_;
        pending_bindings_.push_back(std::move(pending));
        {
            std::lock_guard lock(context_.submit_mutex());
            const auto result = vk.vkQueueBindSparse(queue_, 1, &info, VK_NULL_HANDLE);
            if (result != VK_SUCCESS) {
                if (result == VK_ERROR_DEVICE_LOST) context_.mark_device_lost();
                pending_bindings_.pop_back();
                return false;
            }
            context_.record_submit_locked(progress_, ticket);
        }
        submitted_ = last_sparse_ = ticket;
        clear_waits(ticket);
        for (auto it = changes.begin(); it != changes.end();) {
            resource->mapped.erase(it->first);
            if (it->second.heap) ++it;
            else it = changes.erase(it);
        }
        resource->mapped.merge(changes);
        return true;
    }

    std::uint64_t completed() const {
        if (context_.is_device_lost()) return 0;
        std::uint64_t done = 0;
        const VkResult result = context_.vk.vkGetSemaphoreCounterValue(context_.device(), progress_, &done);
        if (result == VK_ERROR_DEVICE_LOST) context_.mark_device_lost();
        // A failed timeline query cannot be used as a resource-retirement or command-buffer reuse ticket.
        return result == VK_SUCCESS ? done : 0;
    }

    void collect_bindings(std::uint64_t done) {
        while (!pending_bindings_.empty() && pending_bindings_.front().ticket <= done)
            pending_bindings_.pop_front();
        if (last_sparse_ <= done) last_sparse_ = 0;
    }

    // Releases what every finished command buffer holds; the first of them, or null when none has finished.
    CommandSlot* recycle_finished() {
        const auto done = completed();
        if (context_.is_device_lost()) return nullptr;
        collect_bindings(done);
        CommandSlot* reuse = nullptr;
        for (auto& slot : buffers_) {
            if (slot.ticket > done) continue;
            slot.segments.clear();
            recycle_khr_stages(slot.khr_instances);
            recycle_khr_as_queries(slot.khr_as_queries);
            for (VkEvent event : slot.events)
                if (context_.vk.vkResetEvent(context_.device(), event) == VK_SUCCESS) free_events_.push_back(event);
                else context_.vk.vkDestroyEvent(context_.device(), event, nullptr);
            slot.events.clear();
            if (!reuse) reuse = &slot;
        }
        return reuse;
    }

    // A queue at the cap with nothing finished waits here for its oldest submission. execute_now() calls it before it
    // takes resource_mutex_ or tracker_mutex_: the submission waited for may itself wait on another queue's work, and
    // that queue needs those locks to submit it. One execute runs at a time on a queue, so buffers_ is this caller's.
    // A timeout only means the GPU is slow: wait again rather than allocate past the cap. False when the device is lost
    // (now or while waiting), so the caller submits nothing.
    bool wait_for_room() {
        for (;;) {
            if (context_.is_device_lost()) return false;
            if (buffers_.size() < flight_bound_) return true;
            const auto done = completed();
            std::size_t outstanding = 0;
            for (const auto& slot : buffers_) {
                if (slot.ticket <= done) return true;
                ++outstanding;
            }
            const bool trace = submission_trace_enabled();
            const auto wait_started = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const VkResult waited = wait_oldest_submitted();
            if (trace) {
                const auto wait_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                   std::chrono::steady_clock::now() - wait_started)
                                                                   .count());
                ++room_wait_count_;
                room_wait_total_us_ += wait_us;
                room_wait_max_us_ = std::max(room_wait_max_us_, wait_us);
                static std::atomic<std::uint64_t> reports{0};
                const std::uint64_t report = reports.fetch_add(1, std::memory_order_relaxed);
                if (report < 128)
                    std::fprintf(stderr,
                                 "[rt] submit-flight: room-wait lane=%s tid=%llu outstanding=%zu pool=%zu bound=%u "
                                 "result=%d wait_us=%llu\n",
                                 queue_kind_name(kind_), static_cast<unsigned long long>(submission_thread_id()),
                                 outstanding, buffers_.size(), flight_bound_, static_cast<int>(waited),
                                 static_cast<unsigned long long>(wait_us));
            }
            switch (waited) {
            case VK_SUCCESS:
                return true;
            case VK_TIMEOUT:
                continue;
            case VK_ERROR_DEVICE_LOST:
                context_.mark_device_lost();
                return false;
            default:
                // With the opt-in flight bound, an error or a pool containing only unfinished/unsubmitted slots must not
                // allocate past the selected cap. Keep the legacy allocate-on-host-error fallback exactly when the
                // switch is off (bound 64) so ordinary behavior is unchanged.
                return flight_bound_ == kMaxQueuedCommandBuffers;
            }
        }
    }

    // Waits up to kQueuedWaitNs for the oldest submitted command buffer. VK_INCOMPLETE when none is submitted yet.
    VkResult wait_oldest_submitted() {
        std::uint64_t oldest = UINT64_MAX;
        for (const auto& slot : buffers_)
            if (slot.ticket < oldest) oldest = slot.ticket;
        if (oldest == UINT64_MAX) return VK_INCOMPLETE;  // all still being recorded
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1;
        info.pSemaphores = &progress_;
        info.pValues = &oldest;
        return context_.vk.vkWaitSemaphores(context_.device(), &info, kQueuedWaitNs);
    }

    CommandSlot* acquire() {
        if (context_.is_device_lost()) return nullptr;
        context_.drain_deleted_resources(context_.completed_retire_ticket());
        CommandSlot* reuse = recycle_finished();
        if (reuse) {
            if (context_.vk.vkResetCommandBuffer(reuse->buffer, 0) != VK_SUCCESS) return nullptr;
            reuse->predicate.reset();
            reuse->predicated_copy.reset();
            reuse->counted_dispatch.reset();
            reuse->ticket = UINT64_MAX;  // in use until submitted
            return reuse;
        }
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = pool_;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        if (context_.vk.vkAllocateCommandBuffers(context_.device(), &info, &command_buffer) != VK_SUCCESS)
            return nullptr;
        buffers_.push_back({command_buffer, UINT64_MAX});
        return &buffers_.back();
    }

    // An unsignaled event for order_timestamp_copy, or null. MoltenVK decides when the command buffer is submitted
    // whether a set or wait does anything, from the event's state then, so each use gets an event of its own.
    VkEvent take_event() {
        if (!free_events_.empty()) {
            const VkEvent event = free_events_.back();
            free_events_.pop_back();
            return event;
        }
        VkEventCreateInfo info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VkEvent event = VK_NULL_HANDLE;
        if (context_.vk.vkCreateEvent(context_.device(), &info, nullptr, &event) != VK_SUCCESS) return VK_NULL_HANDLE;
        return event;
    }

    bool submit(VkCommandBuffer command_buffer, VkSemaphore fence, std::uint64_t value) {
        if (context_.is_device_lost()) return false;
        const std::uint64_t ticket = submitted_ + 1;
        if (last_sparse_) {
            collect_bindings(completed());
            if (context_.is_device_lost()) return false;
        }
        std::span<const VkSemaphore> semaphores = waits_;
        std::span<const std::uint64_t> values = wait_values_;
        std::vector<VkSemaphore> sparse_waits;
        std::vector<std::uint64_t> sparse_values;
        if (last_sparse_) {
            sparse_waits = waits_;
            sparse_values = wait_values_;
            sparse_waits.push_back(progress_);
            sparse_values.push_back(last_sparse_);
            semaphores = sparse_waits;
            values = sparse_values;
        }
        const std::vector<VkPipelineStageFlags> stages(semaphores.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        const bool trace = submission_trace_enabled();
        const auto submit_lock_started = trace ? std::chrono::steady_clock::now() :
                                                   std::chrono::steady_clock::time_point{};
        std::uint64_t submit_lock_wait_us = 0;
        std::uint64_t driver_submit_us = 0;
        VkResult res = VK_SUCCESS;
        {
            std::unique_lock lock(context_.submit_mutex());
            if (trace)
                submit_lock_wait_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                    std::chrono::steady_clock::now() - submit_lock_started)
                                                                    .count());
            VkSemaphore signals[2]{progress_, VK_NULL_HANDLE};
            std::uint64_t signal_values[2]{ticket, 0};
            std::uint32_t signal_count = 1;
            // Decided under the submit lock, so the order of the checks is the order of the submissions. A Signal
            // that is not above what was already submitted for this fence (a held Signal(F, 2) released after an
            // unheld Signal(F, 3)) leaves the Vulkan signal out: the timeline cannot go down, and waits for 2 pass.
            // The value counts as submitted only after the submit succeeded (below).
            const bool signals_fence = fence && context_.holds().would_signal(fence, value);
            if (signals_fence) {
                signals[signal_count] = fence;
                signal_values[signal_count] = value;
                ++signal_count;
            }
            VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timeline.waitSemaphoreValueCount = std::uint32_t(values.size());
            timeline.pWaitSemaphoreValues = values.data();
            timeline.signalSemaphoreValueCount = signal_count;
            timeline.pSignalSemaphoreValues = signal_values;
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
            submit.waitSemaphoreCount = std::uint32_t(semaphores.size());
            submit.pWaitSemaphores = semaphores.data();
            submit.pWaitDstStageMask = stages.data();
            submit.commandBufferCount = command_buffer ? 1 : 0;
            submit.pCommandBuffers = &command_buffer;
            submit.signalSemaphoreCount = signal_count;
            submit.pSignalSemaphores = signals;
            const auto driver_started = trace ? std::chrono::steady_clock::now() :
                                                std::chrono::steady_clock::time_point{};
            res = context_.vk.vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
            if (trace)
                driver_submit_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                  std::chrono::steady_clock::now() - driver_started)
                                                                  .count());
            if (res == VK_SUCCESS) {
                context_.record_submit_locked(progress_, ticket);
                if (signals_fence) context_.holds().signaled(fence, value);
            }
        }
        if (trace) {
            static std::atomic<std::uint64_t> calls{0};
            static std::atomic<std::uint64_t> reports{0};
            const std::uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
            if (call <= 16 || submit_lock_wait_us >= 1000 || driver_submit_us >= 1000) {
                const std::uint64_t report = reports.fetch_add(1, std::memory_order_relaxed);
                if (report < 128)
                    std::fprintf(stderr,
                                 "[rt] submit-flight: vkQueueSubmit lane=%s tid=%llu call=%llu ticket=%llu command=%d "
                                 "waits=%zu lock_us=%llu driver_us=%llu result=%d\n",
                                 queue_kind_name(kind_), static_cast<unsigned long long>(submission_thread_id()),
                                 static_cast<unsigned long long>(call), static_cast<unsigned long long>(ticket),
                                 command_buffer ? 1 : 0, semaphores.size(),
                                 static_cast<unsigned long long>(submit_lock_wait_us),
                                 static_cast<unsigned long long>(driver_submit_us), static_cast<int>(res));
            }
        }
        if (res == VK_ERROR_DEVICE_LOST) {
            context_.mark_device_lost();
            clear_waits();
            return false;
        }
        if (res != VK_SUCCESS) {
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 3)
                std::fprintf(stderr, "[rt] replay: vkQueueSubmit failed with VkResult %d\n", int(res));
        }
        const bool ok = (res == VK_SUCCESS);
        if (ok) {
            submitted_ = ticket;
            clear_waits(ticket);
            for (auto& slot : buffers_)
                if (slot.buffer == command_buffer) slot.ticket = ticket;
        }
        return ok;
    }

    VulkanContext& context_;
    VkQueue queue_;
    QueueKind kind_ = QueueKind::direct;
    std::uint32_t family_;
    std::uint32_t flight_bound_ = kMaxQueuedCommandBuffers;
    std::uint64_t room_wait_count_ = 0;
    std::uint64_t room_wait_total_us_ = 0;
    std::uint64_t room_wait_max_us_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkSemaphore progress_ = VK_NULL_HANDLE;  // our own timeline: which command buffers are done
    std::uint64_t submitted_ = 0;
    std::vector<CommandSlot> buffers_;
    std::vector<VkEvent> free_events_;  // unsignaled, for order_timestamp_copy
    std::unique_ptr<OcclusionResolve> occlusion_resolve_;
    std::uint64_t storage_init_seen_ = 0;  // the storage_init_version_ this queue has waited for
    std::uint64_t bindless_upload_seen_ = 0;  // the bindless_upload_version_ this queue has waited for
    std::uint64_t last_sparse_ = 0;
    std::deque<PendingBindings> pending_bindings_;
    std::vector<VkSemaphore> waits_;
    std::vector<std::uint64_t> wait_values_;
    // execute's working storage. Per queue like buffers_: one execute at a time on a queue, as acquire() and
    // submit() already assume; other queues prepare concurrently with their own.
    std::vector<PreparedList> prepared_;
    std::uint32_t open_labels_ = 0;  // queue labels begun and not ended
    bool ok_ = false;
    // Wait-before-Signal. state_mutex_ is outermost (before resource_mutex_ -> tracker_mutex_ -> submit_mutex_) and
    // guards the members above that execute, signal, wait and marker touch.
    std::mutex state_mutex_;
    std::deque<Deferred> held_;
    std::vector<std::shared_ptr<void>> wait_pins_;     // keep the fences of waits_ alive
    std::vector<std::shared_ptr<void>> retired_pins_;  // released by the caller once state_mutex_ is dropped
    struct TicketedPin {
        std::uint64_t ticket;
        std::shared_ptr<void> pin;
    };
    std::deque<TicketedPin> pin_queue_;  // released once progress_ reaches the ticket
    std::atomic<int> held_failure_{0};                 // a ReplayResult: the first held batch that failed
};

} // namespace palmetto
