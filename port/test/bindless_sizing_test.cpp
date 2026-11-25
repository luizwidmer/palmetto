// bindless::size_bindings with synthetic device limits, and the set's degradation rules with a fake Vulkan device: no
// GPU. The counts of set 0 come from the device's descriptor limits (vulkan_bindless.h): today's sizes where a device
// is at them, larger 2D, 2D-array, 3D, 1D, sampler and storage arrays where it has room, and less where it is below
// them. The fake device makes the allocations of a new table generation fail after a scripted number of calls, so the
// two degradation paths are measured without a driver: a released generation is reused as a spare (no allocation), and
// a replacement that still cannot be made keeps the last uploaded table and lets the submission go on.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "../src/vulkan_bindless.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace palmetto;

namespace {

struct Device {
    VkPhysicalDeviceLimits limits{};
    VkPhysicalDeviceVulkan11Properties v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
};

// The same descriptor limit for every type, update-after-bind or not; the others are unlimited.
Device uniform(std::uint32_t plain, std::uint32_t update_after_bind) {
    Device d;
    auto& l = d.limits;
    l.maxPerStageDescriptorSampledImages = l.maxDescriptorSetSampledImages = plain;
    l.maxPerStageDescriptorSamplers = l.maxDescriptorSetSamplers = plain;
    l.maxPerStageDescriptorStorageImages = l.maxDescriptorSetStorageImages = plain;
    l.maxPerStageResources = 0xffffffffu;
    l.maxColorAttachments = 8;
    l.maxSamplerAllocationCount = 4000;  // NVIDIA's; the minimum Vulkan allows
    auto& u = d.v12;
    u.maxPerStageDescriptorUpdateAfterBindSampledImages = update_after_bind;
    u.maxDescriptorSetUpdateAfterBindSampledImages = update_after_bind;
    u.maxPerStageDescriptorUpdateAfterBindSamplers = u.maxDescriptorSetUpdateAfterBindSamplers = update_after_bind;
    u.maxPerStageDescriptorUpdateAfterBindStorageImages = update_after_bind;
    u.maxDescriptorSetUpdateAfterBindStorageImages = update_after_bind;
    u.maxPerStageUpdateAfterBindResources = 0xffffffffu;
    u.maxUpdateAfterBindDescriptorsInAllPools = 0xffffffffu;
    d.v11.maxPerSetDescriptors = 0xffffffffu;
    return d;
}

BindlessFeatures with_ms(bool update_after_bind) {
    BindlessFeatures f{};
    f.update_after_bind = update_after_bind;
    f.ms_samples.fill(VK_SAMPLE_COUNT_4_BIT);
    return f;
}

struct Totals { std::uint32_t sampled = 0, samplers = 0, storage = 0, all = 0; };
Totals totals(const BindlessFeatures& f) {
    Totals t;
    for (std::uint32_t b = 0; b < bindless::binding_count; ++b) {
        if (!bindless::binding_enabled(b, f)) continue;
        const auto type = bindless::bindings[b].type;
        if (type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) t.sampled += f.counts[b];
        if (type == VK_DESCRIPTOR_TYPE_SAMPLER) t.samplers += f.counts[b];
        if (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) t.storage += f.counts[b];
        t.all += f.counts[b];
    }
    return t;
}

std::uint32_t count(const BindlessFeatures& f, ImageDimension dimension) {
    return f.counts[bindless::sampled_binding(dimension)];
}
std::uint32_t storage(const BindlessFeatures& f, ImageDimension dimension) {
    return f.counts[bindless::storage_binding(dimension, 0)];
}

// Every storage array of a dimension, the r32ui one included, holds the same indices.
bool storage_consistent(const BindlessFeatures& f) {
    for (std::uint32_t d = 0; d < 4; ++d) {
        const auto dimension = ImageDimension(d);
        const auto held = storage(f, dimension);
        if (held > count(f, dimension)) return false;
        for (std::uint32_t c = 0; c < 3; ++c)
            if (f.counts[bindless::storage_binding(dimension, c)] != held) return false;
        if (bindless::r32ui_binding(dimension) != bindless::no_binding &&
            f.counts[bindless::r32ui_binding(dimension)] != held)
            return false;
    }
    return true;
}

}  // namespace

// A fake Vulkan device for the degradation rules: every entry point the set loads answers success with a made-up
// handle, and the two allocation entry points can be told to fail the calls that come after a scripted point, which is
// how a device out of table memory behaves. Nothing is executed and no driver is opened.
namespace fake {

struct State {
    unsigned allocate_calls = 0, allocate_fail_remaining = 0;
    unsigned pool_calls = 0, pool_fail_remaining = 0;
    unsigned updates = 0, copies = 0, update_buffers = 0, barriers = 0;
    std::uintptr_t next_handle = 0x1000;
};
State g;
alignas(64) std::uint8_t staging[1 << 16];

std::uintptr_t handle() { return g.next_handle += 0x100; }

VKAPI_ATTR VkResult VKAPI_CALL create_descriptor_set_layout(VkDevice, const VkDescriptorSetLayoutCreateInfo*,
                                                           const VkAllocationCallbacks*, VkDescriptorSetLayout* out) {
    *out = reinterpret_cast<VkDescriptorSetLayout>(handle());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_descriptor_set_layout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL create_descriptor_pool(VkDevice, const VkDescriptorPoolCreateInfo*,
                                                      const VkAllocationCallbacks*, VkDescriptorPool* out) {
    ++g.pool_calls;
    if (g.pool_fail_remaining) {
        --g.pool_fail_remaining;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *out = reinterpret_cast<VkDescriptorPool>(handle());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_descriptor_pool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL allocate_descriptor_sets(VkDevice, const VkDescriptorSetAllocateInfo*,
                                                        VkDescriptorSet* out) {
    *out = reinterpret_cast<VkDescriptorSet>(handle());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL update_descriptor_sets(VkDevice, std::uint32_t, const VkWriteDescriptorSet*, std::uint32_t,
                                                  const VkCopyDescriptorSet*) {
    ++g.updates;
}
VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*,
                                             VkBuffer* out) {
    *out = reinterpret_cast<VkBuffer>(handle());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL get_buffer_memory_requirements(VkDevice, VkBuffer, VkMemoryRequirements* out) {
    *out = {1 << 16, 64, 0x3};
}
VKAPI_ATTR VkResult VKAPI_CALL allocate_memory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*,
                                               VkDeviceMemory* out) {
    ++g.allocate_calls;
    if (g.allocate_fail_remaining) {
        --g.allocate_fail_remaining;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *out = reinterpret_cast<VkDeviceMemory>(handle());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL bind_buffer_memory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL map_memory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags,
                                          void** out) {
    *out = staging;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create_image(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*,
                                            VkImage* out) {
    *out = reinterpret_cast<VkImage>(handle());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice, VkImage, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL get_image_memory_requirements(VkDevice, VkImage, VkMemoryRequirements* out) {
    *out = {4096, 64, 0x3};
}
VKAPI_ATTR VkResult VKAPI_CALL bind_image_memory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL create_image_view(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*,
                                                 VkImageView* out) {
    *out = reinterpret_cast<VkImageView>(handle());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_image_view(VkDevice, VkImageView, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL create_sampler(VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*,
                                              VkSampler* out) {
    *out = reinterpret_cast<VkSampler>(handle());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_sampler(VkDevice, VkSampler, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL cmd_pipeline_barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                                                VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*,
                                                std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t,
                                                const VkImageMemoryBarrier*) {
    ++g.barriers;
}
VKAPI_ATTR void VKAPI_CALL cmd_copy_buffer(VkCommandBuffer, VkBuffer, VkBuffer, std::uint32_t, const VkBufferCopy*) {
    ++g.copies;
}
VKAPI_ATTR void VKAPI_CALL cmd_update_buffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {
    ++g.update_buffers;
}
VKAPI_ATTR void VKAPI_CALL cmd_clear_color_image(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*,
                                                 std::uint32_t, const VkImageSubresourceRange*) {}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice, const char* name) {
#define FAKE_MAP(vk_name, stub) if (std::strcmp(name, #vk_name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&stub)
    FAKE_MAP(vkCreateDescriptorSetLayout, create_descriptor_set_layout);
    FAKE_MAP(vkDestroyDescriptorSetLayout, destroy_descriptor_set_layout);
    FAKE_MAP(vkCreateDescriptorPool, create_descriptor_pool);
    FAKE_MAP(vkDestroyDescriptorPool, destroy_descriptor_pool);
    FAKE_MAP(vkAllocateDescriptorSets, allocate_descriptor_sets);
    FAKE_MAP(vkUpdateDescriptorSets, update_descriptor_sets);
    FAKE_MAP(vkCreateBuffer, create_buffer);
    FAKE_MAP(vkDestroyBuffer, destroy_buffer);
    FAKE_MAP(vkGetBufferMemoryRequirements, get_buffer_memory_requirements);
    FAKE_MAP(vkAllocateMemory, allocate_memory);
    FAKE_MAP(vkFreeMemory, free_memory);
    FAKE_MAP(vkBindBufferMemory, bind_buffer_memory);
    FAKE_MAP(vkMapMemory, map_memory);
    FAKE_MAP(vkCreateImage, create_image);
    FAKE_MAP(vkDestroyImage, destroy_image);
    FAKE_MAP(vkGetImageMemoryRequirements, get_image_memory_requirements);
    FAKE_MAP(vkBindImageMemory, bind_image_memory);
    FAKE_MAP(vkCreateImageView, create_image_view);
    FAKE_MAP(vkDestroyImageView, destroy_image_view);
    FAKE_MAP(vkCreateSampler, create_sampler);
    FAKE_MAP(vkDestroySampler, destroy_sampler);
    FAKE_MAP(vkCmdPipelineBarrier, cmd_pipeline_barrier);
    FAKE_MAP(vkCmdCopyBuffer, cmd_copy_buffer);
    FAKE_MAP(vkCmdUpdateBuffer, cmd_update_buffer);
    FAKE_MAP(vkCmdClearColorImage, cmd_clear_color_image);
#undef FAKE_MAP
    return nullptr;
}

VkPhysicalDeviceMemoryProperties device_memory() {
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryTypeCount = 2;
    memory.memoryTypes[0] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0};
    memory.memoryTypes[1] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    return memory;
}

// A key with a chosen first dword: distinct keys for the table.
DescriptorKey key(std::uint32_t n) {
    return {n, 0x9e3779b9u * n, n ^ 0x55555555u, n >> 3, 7, n * n, 0x80000000u, ~n};
}

// Nine keys whose complete FNV hash is the same: the eighth fills all eight probe slots, so the ninth can never be
// found by the shader (descriptor_table_test builds the same shape).
std::array<DescriptorKey, 9> colliding_keys() {
    std::array<DescriptorKey, 9> keys{};
    for (std::uint32_t i = 0; i < keys.size(); ++i) {
        keys[i][0] = i + 1;
        std::uint32_t prefix = kFnvOffsetBasis;
        for (int word = 0; word < 7; ++word) prefix = (prefix ^ keys[i][word]) * kFnvPrime;
        keys[i][7] = prefix;
    }
    return keys;
}

const VkCommandBuffer command_buffer = reinterpret_cast<VkCommandBuffer>(std::uintptr_t(0x2000));

}  // namespace fake

namespace {

// A set over the fake device, with the update-after-bind and partially-bound flags the RTX-class device offers:
// partially bound keeps the creation writes to one descriptor per array, which is what the real device does.
struct ScopedSet {
    BindlessSet set;
    ScopedSet() : set(reinterpret_cast<PFN_vkGetDeviceProcAddr>(&fake::device_proc),
                      reinterpret_cast<VkDevice>(std::uintptr_t(0x1)), fake::device_memory(), features()) {}
    static BindlessFeatures features() {
        BindlessFeatures f;
        f.update_after_bind = true;
        f.partially_bound = true;
        return f;
    }
};

// A new generation that cannot be made is a degraded flush: it returns true (the submission is recorded), keeps the
// last uploaded table and its set, records nothing into the command buffer, and leaves the queued changes for the
// next flush; that next flush with memory available makes the generation and publishes them.
int test_degraded_flush_keeps_submission() {
    fake::g = {};
    ScopedSet scoped;
    BindlessSet& set = scoped.set;
    EXPECT(set.ok());
    // The table grows: 200 keys move it from its first capacity to a larger buffer, so a replacement is required.
    for (std::uint32_t i = 0; i < 200; ++i) EXPECT(set.insert_key(fake::key(i + 1), i + 2));
    EXPECT(set.flush_replaces_table());
    const VkDescriptorSet before = set.set();
    const unsigned updates = fake::g.updates, copies = fake::g.copies, update_buffers = fake::g.update_buffers;
    fake::g.allocate_fail_remaining = 1000;
    fake::g.pool_fail_remaining = 1000;
    EXPECT(set.flush(fake::command_buffer, 1));
    EXPECT(set.last_flush_degraded());  // the submission goes on
    EXPECT(set.degraded_flushes() == 1);
    EXPECT(set.generations_made() == 0);
    EXPECT(set.set() == before);  // the set running work may read is untouched
    EXPECT(!set.last_flush_replaced());
    EXPECT(set.retired_count() == 0 && set.spare_count() == 0);
    // Nothing was recorded for the GPU and no descriptor was written: the flush is a no-op on the device.
    EXPECT(fake::g.updates == updates && fake::g.copies == copies && fake::g.update_buffers == update_buffers);
    // The changes stay queued: the next flush still owes the replacement.
    EXPECT(set.flush_replaces_table());
    EXPECT(set.table().lookup(fake::key(7)) == 8);
    // With memory available again the same flush makes the generation and publishes every queued change.
    fake::g.allocate_fail_remaining = fake::g.pool_fail_remaining = 0;
    EXPECT(set.flush(fake::command_buffer, 2));
    EXPECT(!set.last_flush_degraded());
    EXPECT(set.degraded_flushes() == 1);
    EXPECT(set.generations_made() == 1);
    EXPECT(set.set() != before && set.retired_count() == 1);
    EXPECT(fake::g.copies + fake::g.update_buffers > copies + update_buffers);
    for (std::uint32_t i = 0; i < 200; ++i) EXPECT(set.table().lookup(fake::key(i + 1)) == i + 2);
    return 0;
}

// A released generation is kept as a spare, and a replacement whose table fits its buffers reuses it: no descriptor
// pool and no memory allocation at all, even with every allocation failing.
int test_spare_avoids_allocation() {
    fake::g = {};
    ScopedSet scoped;
    BindlessSet& set = scoped.set;
    EXPECT(set.ok());
    for (std::uint32_t i = 0; i < 200; ++i) EXPECT(set.insert_key(fake::key(i + 1), i + 2));
    // Grow the table, then replace a live key's index: the second replacement retires a generation whose buffers are
    // as large as the current table, which is the spare a later replacement reuses.
    EXPECT(set.flush(fake::command_buffer, 1) && set.generations_made() == 1);
    EXPECT(set.insert_key(fake::key(3), 5) && set.flush_replaces_table());
    EXPECT(set.flush(fake::command_buffer, 2) && set.generations_made() == 2);
    set.release_retired(UINT64_MAX);
    EXPECT(set.retired_count() == 0 && set.spare_count() == 1);
    // A live key's index changes again: the table does not grow, so the spare's buffers are the right size.
    const std::uint64_t made = set.generations_made();
    EXPECT(set.insert_key(fake::key(3), 0xffff));
    EXPECT(set.flush_replaces_table());
    const unsigned allocate_calls = fake::g.allocate_calls, pool_calls = fake::g.pool_calls;
    fake::g.allocate_fail_remaining = 1000;
    fake::g.pool_fail_remaining = 1000;
    EXPECT(set.flush(fake::command_buffer, 3));
    EXPECT(!set.last_flush_degraded() && set.last_flush_replaced());
    EXPECT(set.spares_reused() == 1 && set.spare_count() == 0);
    EXPECT(fake::g.allocate_calls == allocate_calls && fake::g.pool_calls == pool_calls);
    EXPECT(set.generations_made() == made + 1);
    EXPECT(set.table().lookup(fake::key(3)) == 0xffff);

    // The spare is inherited by the next release, but a table that grew needs more buffer than it has: with memory
    // gone the flush degrades and the spare stays for a later replacement that fits it.
    set.release_retired(UINT64_MAX);
    EXPECT(set.spare_count() == 1);
    for (std::uint32_t i = 0; i < 300; ++i) EXPECT(set.insert_key(fake::key(1000 + i), i + 1));
    const std::uint64_t degraded = set.degraded_flushes();
    fake::g.allocate_fail_remaining = 1000;
    fake::g.pool_fail_remaining = 1000;
    EXPECT(set.flush(fake::command_buffer, 4));
    EXPECT(set.last_flush_degraded() && set.degraded_flushes() == degraded + 1);
    EXPECT(set.spare_count() == 1 && set.spares_reused() == 1);
    // And a later flush grows the table.
    fake::g.allocate_fail_remaining = fake::g.pool_fail_remaining = 0;
    EXPECT(set.flush(fake::command_buffer, 5));
    EXPECT(!set.last_flush_degraded() && set.generations_made() == made + 2);
    EXPECT(set.table().lookup(fake::key(1200)) == 201);
    return 0;
}

// A key the shader's eight probes can never resolve is refused by name: the table is unchanged, the caller is told,
// and the image keeps the index it was given (its shader reads the fallback), instead of an exception leaving the
// shim during shader-view creation.
int test_unresolvable_key_is_refused() {
    fake::g = {};
    ScopedSet scoped;
    BindlessSet& set = scoped.set;
    EXPECT(set.ok());
    const auto keys = fake::colliding_keys();
    for (std::uint32_t i = 0; i < 8; ++i) EXPECT(set.insert_key(keys[i], i + 1));
    EXPECT(set.table().size() == 8);
    // Refused, counted, no exception, and nothing changed.
    EXPECT(!set.insert_key(keys[8], 9));
    EXPECT(set.keys_refused() == 1);
    EXPECT(set.table().size() == 8 && set.table().lookup(keys[8]) == 0);
    for (std::uint32_t i = 0; i < 8; ++i) EXPECT(set.table().lookup(keys[i]) == i + 1);
    // A flush still uploads the eight resolvable keys.
    EXPECT(set.flush(fake::command_buffer, 1));
    EXPECT(!set.last_flush_degraded());
    for (std::uint32_t i = 0; i < 8; ++i) EXPECT(set.table().lookup(keys[i]) == i + 1);
    return 0;
}

}  // namespace

int main() {
    using D = ImageDimension;
    constexpr auto defaults = bindless::default_counts();
    static_assert(defaults[0] == 4096 && defaults[bindless::sampler_binding] == 1024 && defaults[8] == 256);

    // An RTX 5070 (driver 621.24): 1048576 of each descriptor type, update-after-bind or not, 4000 samplers.
    for (const bool update_after_bind : {true, false}) {
        const Device d = uniform(1048576, 1048576);
        BindlessFeatures f = with_ms(update_after_bind);
        EXPECT(bindless::size_bindings(f, d.limits, d.v11, d.v12));
        EXPECT(count(f, D::dim_2d) == bindless::max_2d_images);
        EXPECT(f.counts[bindless::sampler_binding] == 4000 - bindless::reserved_samplers);
        EXPECT(f.counts[bindless::sampler_binding] > 2048);  // a full sampler heap and the fallback
        EXPECT(storage(f, D::dim_2d) == bindless::max_storage_indices);
        // The other three sampled arrays grow to their own targets: streamed content names arrays, volumes and 1D
        // textures, and each dimension is a separate index space.
        EXPECT(count(f, D::dim_2d_array) == bindless::max_2d_array_images);
        EXPECT(count(f, D::dim_3d) == bindless::max_3d_images);
        EXPECT(count(f, D::dim_1d) == bindless::max_1d_images);
        EXPECT(storage(f, D::dim_2d_array) == bindless::max_storage_indices &&
               storage(f, D::dim_3d) == bindless::max_storage_indices &&
               storage(f, D::dim_1d) == bindless::max_storage_indices);
        EXPECT(f.ms_samples[0] == VK_SAMPLE_COUNT_4_BIT && f.ms_samples[2] == VK_SAMPLE_COUNT_4_BIT);
        EXPECT(storage_consistent(f));
        if (update_after_bind)
            std::printf("RTX 5070-like: %u 2D images, %u 2D-array, %u 3D, %u 1D, %u samplers, %u storage indices "
                        "in 2D, %u descriptors\n",
                        count(f, D::dim_2d), count(f, D::dim_2d_array), count(f, D::dim_3d), count(f, D::dim_1d),
                        f.counts[bindless::sampler_binding], storage(f, D::dim_2d), totals(f).all);
    }

    // At today's sizes exactly (5632 sampled images, 1024 samplers, 3584 storage images): today's set, no MS pairs.
    {
        Device d = uniform(1048576, 1048576);
        d.v12.maxPerStageDescriptorUpdateAfterBindSampledImages = 5632;
        d.v12.maxPerStageDescriptorUpdateAfterBindSamplers = 1024;
        d.v12.maxPerStageDescriptorUpdateAfterBindStorageImages = 3584;
        BindlessFeatures f = with_ms(true);
        EXPECT(bindless::size_bindings(f, d.limits, d.v11, d.v12));
        EXPECT(count(f, D::dim_2d) == 4096 && f.counts[bindless::sampler_binding] == 1024 &&
               storage(f, D::dim_2d) == 256);
        for (std::uint32_t b = 0; b < bindless::first_ms_binding; ++b)
            if (bindless::bindings[b].type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) EXPECT(f.counts[b] == defaults[b]);
        EXPECT(f.ms_samples == (std::array{VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_1_BIT}));
        EXPECT(storage_consistent(f));
    }

    // The growth targets are a target, not a promise: a device whose sampled limit leaves no room for them keeps the
    // dimension defaults (they are never taken below 1024/256/256), and storage_consistent() holds there too.
    {
        const Device d = uniform(7168, 7168);
        BindlessFeatures f = with_ms(true);
        EXPECT(bindless::size_bindings(f, d.limits, d.v11, d.v12));
        EXPECT(count(f, D::dim_2d) == defaults[bindless::sampled_binding(D::dim_2d)]);
        EXPECT(count(f, D::dim_2d_array) == 1024 && count(f, D::dim_3d) == 256 && count(f, D::dim_1d) == 256);
        EXPECT(totals(f).sampled <= 7168 && storage_consistent(f));
    }

    // Room for part of the growth: 2D images grow first (the streamed population is 2D) and whatever is left goes
    // to the other dimensions; every count stays within its limit and the storage arrays stay consistent.
    {
        const Device d = uniform(60000, 60000);
        BindlessFeatures f = with_ms(true);
        EXPECT(bindless::size_bindings(f, d.limits, d.v11, d.v12));
        EXPECT(count(f, D::dim_2d) > 4096 && count(f, D::dim_2d) <= bindless::max_2d_images);
        EXPECT(count(f, D::dim_2d_array) >= 1024 && count(f, D::dim_3d) >= 256 && count(f, D::dim_1d) >= 256);
        EXPECT(count(f, D::dim_2d_array) <= bindless::max_2d_array_images);
        EXPECT(totals(f).sampled + bindless::growth_headroom <= 60000);
        EXPECT(storage_consistent(f));
    }

    // A small device below today's sizes: smaller arrays that fit, and false.
    {
        Device d = uniform(1048576, 1048576);
        d.limits.maxPerStageDescriptorSampledImages = 3000;
        d.limits.maxPerStageDescriptorSamplers = 512;
        d.limits.maxPerStageDescriptorStorageImages = 2000;
        d.limits.maxPerStageResources = 4500;
        BindlessFeatures f = with_ms(false);
        EXPECT(!bindless::size_bindings(f, d.limits, d.v11, d.v12));
        const Totals t = totals(f);
        EXPECT(t.sampled <= 3000 && t.samplers <= 512 && t.storage <= 2000 && t.sampled + t.storage + 1 + 8 <= 4500);
        EXPECT(count(f, D::dim_2d) >= 1 && count(f, D::dim_2d) < 4096 && f.counts[bindless::sampler_binding] == 512);
        EXPECT(f.ms_samples[0] == VK_SAMPLE_COUNT_1_BIT);
        EXPECT(storage_consistent(f));
    }

    // Update-after-bind and plain limits differ: each layout is sized by its own.
    {
        const Device d = uniform(16384, 1048576);
        BindlessFeatures uab = with_ms(true), plain = with_ms(false);
        EXPECT(bindless::size_bindings(uab, d.limits, d.v11, d.v12));
        EXPECT(bindless::size_bindings(plain, d.limits, d.v11, d.v12));
        EXPECT(count(uab, D::dim_2d) == bindless::max_2d_images);
        const Totals t = totals(plain);
        EXPECT(count(plain, D::dim_2d) > 4096 && count(plain, D::dim_2d) < 16384);
        EXPECT(t.sampled + bindless::growth_headroom <= 16384 && t.storage + bindless::growth_headroom <= 16384);
        EXPECT(plain.ms_samples[0] == VK_SAMPLE_COUNT_4_BIT && storage_consistent(plain));
        EXPECT(plain.counts[bindless::sampler_binding] == 4000 - bindless::reserved_samplers);
    }

    // maxPerSetDescriptors stops growth but never takes a count below today's.
    {
        Device d = uniform(1048576, 1048576);
        d.v11.maxPerSetDescriptors = 1024;
        BindlessFeatures f = with_ms(true);
        EXPECT(bindless::size_bindings(f, d.limits, d.v11, d.v12));
        EXPECT(count(f, D::dim_2d) == 4096 && f.counts[bindless::sampler_binding] == 1024 &&
               storage(f, D::dim_2d) == 256);
        d.v11.maxPerSetDescriptors = 30000;
        f = with_ms(true);
        EXPECT(bindless::size_bindings(f, d.limits, d.v11, d.v12));
        EXPECT(totals(f).all <= 30000 && count(f, D::dim_2d) > 4096);
    }

    // D3D-219: past maxPerSetDescriptors, growth follows layout_supported (vkGetDescriptorSetLayoutSupport), which
    // is asked only above that limit. Intel's Xe-LP reports 6159 and supports the full sizes.
    {
        Device d = uniform(1048576, 1048576);
        d.v11.maxPerSetDescriptors = 6159;
        d.limits.maxSamplerAllocationCount = 4000;
        unsigned asked = 0, asked_within = 0;
        const auto all_supported = [&](const BindlessFeatures& candidate) {
            ++asked;
            asked_within += totals(candidate).all <= 6159;
            return true;
        };
        BindlessFeatures f = with_ms(true);
        EXPECT(bindless::size_bindings(f, d.limits, d.v11, d.v12, all_supported));
        EXPECT(asked > 0 && asked_within == 0);
        EXPECT(count(f, D::dim_2d) == bindless::max_2d_images &&
               f.counts[bindless::sampler_binding] == 4000 - bindless::reserved_samplers);
        EXPECT(storage(f, D::dim_2d) == bindless::max_storage_indices && storage_consistent(f));

        // A driver that supports less: growth stops where it says no.
        const auto up_to_20000 = [&](const BindlessFeatures& candidate) { return totals(candidate).all <= 20000; };
        f = with_ms(true);
        EXPECT(bindless::size_bindings(f, d.limits, d.v11, d.v12, up_to_20000));
        EXPECT(totals(f).all <= 20000 && totals(f).all > 6159 && count(f, D::dim_2d) > 4096);

        // Without the callback, maxPerSetDescriptors still caps growth as before.
        f = with_ms(true);
        EXPECT(bindless::size_bindings(f, d.limits, d.v11, d.v12));
        EXPECT(totals(f).all <= 6159 || (count(f, D::dim_2d) == 4096 && f.counts[bindless::sampler_binding] == 1024));
    }

    // MoltenVK 1.4.3 on an M3 Max: update-after-bind limits of 1000000 sampled images and 500000 samplers, but
    // maxPerSetDescriptors 1212 and a vkGetDescriptorSetLayoutSupport that accepts only fewer descriptors than that
    // (MVKDevice::getDescriptorSetLayoutSupport), though the device creates and samples far larger sets. So the
    // set keeps the default counts: 1024 samplers, short of a full D3D12 sampler heap (d3d12_descriptor_test).
    {
        Device d = uniform(1048576, 1000000);
        d.v12.maxPerStageDescriptorUpdateAfterBindSamplers = d.v12.maxDescriptorSetUpdateAfterBindSamplers = 500000;
        d.limits.maxSamplerAllocationCount = 500000;
        d.v11.maxPerSetDescriptors = 1212;
        const auto below_1212 = [](const BindlessFeatures& candidate) { return totals(candidate).all < 1212; };
        BindlessFeatures f = with_ms(true);
        EXPECT(bindless::size_bindings(f, d.limits, d.v11, d.v12, below_1212));
        EXPECT(count(f, D::dim_2d) == 4096 && f.counts[bindless::sampler_binding] == 1024 &&
               storage(f, D::dim_2d) == 256);
    }

    // The default features are today's counts, which the device-less users (vk_image_fuzz) still get.
    EXPECT(BindlessFeatures{}.counts == defaults);
    // The degradation rules, with the fake device.
    EXPECT(test_degraded_flush_keeps_submission() == 0);
    EXPECT(test_spare_avoids_allocation() == 0);
    EXPECT(test_unresolvable_key_is_refused() == 0);
    std::printf("bindless sizing: synthetic limits gave the expected counts; the fake device's failed generations "
                "degraded and its spare replaced the allocation\n");
    return 0;
}
