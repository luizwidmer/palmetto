#pragma once

// The bindless descriptor set of translated shaders: set 0 of the contract in docs/design.md and
// docs/d3d12_vulkan.md section 5, as tooling/rdna2spirv.py declares it.
//
//   binding  0  sampled 2D images          4096        5  sampled 1D images          256
//            1  samplers                   1024        6  sampled 3D images          256
//            2  the descriptor hash table     1        7  3D r32ui storage images    256
//            3  sampled 2D-array images    1024        8 + 3 * dimension + class     256 each:
//            4  2D r32ui storage images    1024           storage images by component class
//   dimension: 0 = 2D, 1 = 2D array, 2 = 3D, 3 = 1D (the translator's STORE_DIMS); class: 0 = float, 1 = uint, 2 = sint.
//   bindings 20/21: sampled multisample 2D images / 2D arrays, 256 each (runtime SRVs).
//   bindings 22/23 and 24/25 are their UINT and SINT equivalents on devices supporting those classes.
// These are the default counts (BindlessFeatures::counts). With the current default policy, size_bindings() sizes the
// sampled arrays of every dimension, the sampler array and the storage arrays from device limits instead; the explicit
// legacy override keeps the pre-slice counts for 2D-array, 3D and 1D. The shaders declare runtime arrays, so any count
// works for them.
//
// A shader turns a T# or S# into an index through the hash table (descriptor_table.h) and uses that index in
// whichever binding the instruction calls for: the sampled array of the image's dimension, or one of its storage
// arrays. So an image has one index, the same in every binding of its dimension, and each dimension has its own
// index space. The storage arrays are shorter (storage_capacity()), so an image with storage views gets an index
// they hold; one without takes a higher index while there is one. Index 0 of every array is a fallback, because a
// miss gives 0: a 1x1 image of zeros, a nearest clamping sampler, and 1x1 storage images. The caller owns the views
// and samplers it adds and keeps them alive while they are in the set; this class owns only the fallbacks and the
// table's buffer.
//
// Device features: runtimeDescriptorArray for the shaders. With BindlessFeatures::update_after_bind
// (descriptorBindingSampledImageUpdateAfterBind, ...StorageImage..., ...StorageBuffer...) flush() may run while
// command buffers that use the set are pending, as long as no descriptor they use changes and
// flush_replaces_table() is false; without it the caller flushes only when none is. When it is true the flush
// does not touch what pending work reads: it makes a new generation (table buffers and descriptor set, the
// descriptors copied from a CPU shadow) and retires the old one, which release_retired() frees once the owner
// says the submissions that could have used it are done (D3D-314). With partially_bound (descriptorBindingPartiallyBound) only index 0 is written at
// creation; otherwise every element of every array starts as the fallback.
//
// The table of binding 2 lives in DEVICE_LOCAL memory: every divergent T#/S# lookup reads it, and a HOST_VISIBLE
// table measured 19-46x slower there at 100k entries on an RTX 2060 SUPER (vk_descriptor_bench, D3D-56, #639).
// flush() takes the command buffer that will use the set: it uploads only the slots that changed, from a mapped
// staging buffer, and barriers the copy before the draws. The table at creation is empty until that first flush.
//
// Current degradation policy (enabled by default; PALMETTO_BINDLESS_DEGRADATION=0 restores the pre-slice path): a
// flush never refuses a submission for want of table memory, because one allocation failure would otherwise throw
// away a whole command list. Instead:
//   * a released generation is kept as one spare, and a replacement whose table fits the spare's buffers reuses them
//     and its descriptor set instead of allocating (spares_reused());
//   * when a new generation still cannot be made, the last uploaded table and its descriptor set are kept, the
//     pending descriptor and table changes stay queued for the next flush, and every key added since that upload
//     reads the fallback index 0 (degraded_flushes(), last_flush_degraded()). The submission goes on, and a later
//     flush with memory available makes the generation and publishes the queued changes;
//   * a T#/S# key the shader's eight probes can never resolve (a full-probe hash collision, or the table at its
//     capacity limit) is refused by name: insert_key() returns false and that key reads the fallback index
//     (keys_refused()). This is a shape that cannot be expressed, not a resource shortage.
// The counts it is measured against, and the boot A/B, are in docs/bindless_degradation.md.
//
// Not thread-safe: the owner serialises calls.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "descriptor_table.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace palmetto {

// The first four dimensions are the translator's STORE_DIMS indices. A cube is a 2D array of six layers.
enum class ImageDimension : std::uint32_t { dim_2d, dim_2d_array, dim_3d, dim_1d, dim_2d_ms, dim_2d_ms_array };

// Storage views of one image by the component type a shader reads or writes it with (the translator picks by the
// T# format at run time). r32ui is for the atomic instructions, whose image type is R32ui (2D and 3D only).
// A null view leaves the fallback in that binding.
struct StorageViews {
    VkImageView as_float = VK_NULL_HANDLE;
    VkImageView as_uint = VK_NULL_HANDLE;
    VkImageView as_sint = VK_NULL_HANDLE;
    VkImageView r32ui = VK_NULL_HANDLE;
};

namespace bindless {
inline constexpr std::uint32_t binding_count = 26;
constexpr std::array<std::uint32_t, binding_count> default_counts();

enum class DegradationOverride : std::uint8_t { keep_current, enable, disable, invalid };

// The override is deliberately exact: absent/empty keeps today's behaviour, "0" restores the pre-slice sizing and
// failure path, "1" explicitly selects today's behaviour, and any other spelling is reported by the runtime and also
// keeps today's behaviour.
constexpr DegradationOverride parse_degradation_override(const char* value) {
    if (!value || value[0] == '\0') return DegradationOverride::keep_current;
    if (value[0] == '0' && value[1] == '\0') return DegradationOverride::disable;
    if (value[0] == '1' && value[1] == '\0') return DegradationOverride::enable;
    return DegradationOverride::invalid;
}

constexpr bool degradation_enabled(DegradationOverride value) { return value != DegradationOverride::disable; }
} // namespace bindless

struct BindlessFeatures {
    bool update_after_bind = false;
    bool partially_bound = false;
    // Fallback sample counts for RGBA8_UNORM, R32_UINT, R32_SINT. A single sample omits that class's MS bindings.
    std::array<VkSampleCountFlagBits, 3> ms_samples{
        VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_1_BIT};
    // Descriptors in each binding, by binding number: bindless::bindings by default, or what size_bindings() chose.
    std::array<std::uint32_t, bindless::binding_count> counts = bindless::default_counts();
    // Current sizing, spare reuse, memory-pressure degradation and named table-key refusals. The runtime keeps this
    // enabled by default; PALMETTO_BINDLESS_DEGRADATION=0 selects the pre-slice sizing/failure behaviour. Last to
    // preserve the existing aggregate-initializer order for update_after_bind / partially_bound / ms_samples / counts.
    bool degradation = true;
};

namespace bindless {

struct Binding {
    VkDescriptorType type;
    std::uint32_t count;
};

inline constexpr std::uint32_t table_binding = 2;
inline constexpr std::uint32_t sampler_binding = 1;
inline constexpr std::uint32_t first_ms_binding = 20;
inline constexpr std::uint32_t no_binding = 0xffffffffu;

// Indexed by binding number.
inline constexpr std::array<Binding, 26> bindings = {{
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},   {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
}};

static_assert(bindings.size() == binding_count);

constexpr std::array<std::uint32_t, binding_count> default_counts() {
    std::array<std::uint32_t, binding_count> counts{};
    for (std::uint32_t b = 0; b < binding_count; ++b) counts[b] = bindings[b].count;
    return counts;
}

constexpr bool binding_enabled(std::uint32_t binding, const BindlessFeatures& features) {
    return binding < first_ms_binding || features.ms_samples[(binding - first_ms_binding) / 2] != VK_SAMPLE_COUNT_1_BIT;
}

// Every binding is visible to every shader stage. Preserve the single-sample layout on devices whose
// per-stage or per-set limits have no room for additional pairs of MS arrays.
constexpr void limit_ms_bindings(BindlessFeatures& features, std::uint32_t sampled_limit,
                                 std::uint32_t resource_limit) {
    const auto& counts = features.counts;
    std::uint32_t sampled = 0, total = 0;
    for (std::uint32_t b = 0; b < first_ms_binding; ++b) {
        if (bindings[b].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) sampled += counts[b];
        if (bindings[b].type != VK_DESCRIPTOR_TYPE_SAMPLER) total += counts[b];
    }
    for (std::uint32_t c = 0; c < features.ms_samples.size(); ++c) {
        if (features.ms_samples[c] == VK_SAMPLE_COUNT_1_BIT) continue;
        const auto b = first_ms_binding + 2 * c;
        const auto pair = counts[b] + counts[b + 1];
        if (sampled + pair > sampled_limit || total + pair > resource_limit) {
            features.ms_samples[c] = VK_SAMPLE_COUNT_1_BIT;
        } else {
            sampled += pair;
            total += pair;
        }
    }
}

constexpr bool multisampled(ImageDimension dimension) {
    return dimension == ImageDimension::dim_2d_ms || dimension == ImageDimension::dim_2d_ms_array;
}

constexpr std::uint32_t sampled_binding(ImageDimension dimension, std::uint32_t component_class = 0) {
    constexpr std::uint32_t by_dimension[] = {0, 3, 6, 5, first_ms_binding, first_ms_binding + 1};
    return by_dimension[std::uint32_t(dimension)] + (multisampled(dimension) ? component_class * 2 : 0);
}

// component_class: 0 = float, 1 = uint, 2 = sint.
constexpr std::uint32_t storage_binding(ImageDimension dimension, std::uint32_t component_class) {
    return multisampled(dimension) ? no_binding : 8 + 3 * std::uint32_t(dimension) + component_class;
}

constexpr std::uint32_t r32ui_binding(ImageDimension dimension) {
    return dimension == ImageDimension::dim_2d ? 4 : dimension == ImageDimension::dim_3d ? 7 : no_binding;
}

// Set 0's descriptor set layout at features' counts: what create_layout creates and what size_bindings'
// layout_supported asks vkGetDescriptorSetLayoutSupport about. It points into itself, so it is not copied.
struct SetLayout {
    std::array<VkDescriptorSetLayoutBinding, binding_count> entries{};
    std::array<VkDescriptorBindingFlags, binding_count> flags{};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

    explicit SetLayout(const BindlessFeatures& features) {
        std::uint32_t count = 0;
        for (std::uint32_t b = 0; b < binding_count; ++b) {
            if (!binding_enabled(b, features)) continue;
            entries[count] = {b, bindings[b].type, features.counts[b], VK_SHADER_STAGE_ALL, nullptr};
            flags[count] = (features.update_after_bind ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0) |
                           (features.partially_bound ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0);
            ++count;
        }
        flags_info.bindingCount = count;
        flags_info.pBindingFlags = flags.data();
        info.pNext = &flags_info;
        info.flags = features.update_after_bind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
        info.bindingCount = count;
        info.pBindings = entries.data();
    }
    SetLayout(const SetLayout&) = delete;
    SetLayout& operator=(const SetLayout&) = delete;
};

// What size_bindings() grows to where the device's limits allow. At 32-64 bytes a descriptor the whole set is then
// 3-6 MiB (93793 descriptors on an RTX 5070).
inline constexpr std::uint32_t max_2d_images = 65536;
// The streamed content names texture arrays, volumes and 1D textures as well as 2D textures, and each dimension has
// its own index space: a 2D-array T# cannot use a 2D index. These are the growth targets for the other three sampled
// arrays, the sizes a device with room for them is given (the default counts stay 1024/256/256 when it has not).
inline constexpr std::uint32_t max_2d_array_images = 16384;
inline constexpr std::uint32_t max_3d_images = 4096;
inline constexpr std::uint32_t max_1d_images = 4096;
inline constexpr std::uint32_t max_storage_indices = 4096;  // in each storage array, at most its dimension's images
// A full D3D12 shader-visible sampler heap (2048) and room for literal and static S#s, within what the device can
// create (maxSamplerAllocationCount, at least 4000) less the samplers that are not in the set.
inline constexpr std::uint32_t max_samplers = 4096;
inline constexpr std::uint32_t reserved_samplers = 64;
// Growth leaves this many descriptors of each limit to the other sets of a pipeline layout.
inline constexpr std::uint32_t growth_headroom = 64;

// Sizes the sampled arrays of all four dimensions, the sampler and the storage arrays from the device's limits: the
// update-after-bind ones with features.update_after_bind, the plain ones otherwise. Every binding is visible to every
// stage, so the per-stage limits apply to the whole set; fragment outputs count as stage resources (D3D12 allows
// eight render targets) and standalone samplers do not, though they count in an update-after-bind pool.
//   1. The default counts, or as much of them as the device holds: samplers, then storage, then 2D images.
//   2. The MS pairs that fit beside them (limit_ms_bindings, which may turn classes off).
//   3. Growth towards the maximums above, in the same order, within maxPerSetDescriptors. When features.degradation is
//      enabled, the 2D-array, 3D and 1D sampled arrays then grow towards theirs, each step keeping its descriptor-type
//      limits and growth_headroom. That set limit only says what is sure to work (vkGetDescriptorSetLayoutSupport may
//      allow more), so it never shrinks step 1. With degradation disabled those three arrays keep their pre-slice
//      default counts.
// A storage array holds the smaller of the storage count and its dimension's images: add_image never gives a
// storage view an index past the sampled array. The other bindings keep their default counts.
// layout_supported, when given, answers vkGetDescriptorSetLayoutSupport for set 0 at the counts in its argument:
// step 3 then also grows past maxPerSetDescriptors where the device says the layout works (Intel's Xe-LP reports
// 6159 and supports the full sizes, D3D-219). It is asked only once the sum is above maxPerSetDescriptors.
// Sets features.counts; returns false when a count is below its default because the device's limits are.
inline bool size_bindings(BindlessFeatures& features, const VkPhysicalDeviceLimits& limits,
                          const VkPhysicalDeviceVulkan11Properties& v11,
                          const VkPhysicalDeviceVulkan12Properties& v12,
                          const std::function<bool(const BindlessFeatures&)>& layout_supported = {}) {
    const bool uab = features.update_after_bind;
    const std::uint32_t sampled_limit =
        uab ? std::min(v12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                       v12.maxDescriptorSetUpdateAfterBindSampledImages)
            : std::min(limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSampledImages);
    const std::uint32_t sampler_limit =
        uab ? std::min(v12.maxPerStageDescriptorUpdateAfterBindSamplers, v12.maxDescriptorSetUpdateAfterBindSamplers)
            : std::min(limits.maxPerStageDescriptorSamplers, limits.maxDescriptorSetSamplers);
    const std::uint32_t storage_limit =
        uab ? std::min(v12.maxPerStageDescriptorUpdateAfterBindStorageImages,
                       v12.maxDescriptorSetUpdateAfterBindStorageImages)
            : std::min(limits.maxPerStageDescriptorStorageImages, limits.maxDescriptorSetStorageImages);
    std::uint32_t resource_limit = uab ? v12.maxPerStageUpdateAfterBindResources : limits.maxPerStageResources;
    resource_limit -= std::min(resource_limit, std::min(limits.maxColorAttachments, 8u));
    const std::uint32_t pool_limit = uab ? v12.maxUpdateAfterBindDescriptorsInAllPools : 0xffffffffu;

    auto& counts = features.counts;
    counts = default_counts();
    std::uint32_t images = 1, samplers = 1, storage = 1;
    // Until the end the 2D storage arrays are not capped by the 2D images, which grow after them: capping can only
    // take descriptors away.
    const auto apply = [&](bool final) {
        counts[sampled_binding(ImageDimension::dim_2d)] = images;
        counts[sampler_binding] = samplers;
        for (std::uint32_t d = 0; d < 4; ++d) {
            const auto dimension = ImageDimension(d);
            const bool capped = final || dimension != ImageDimension::dim_2d;
            const std::uint32_t held = capped ? std::min(storage, counts[sampled_binding(dimension)]) : storage;
            for (std::uint32_t c = 0; c < 3; ++c) counts[storage_binding(dimension, c)] = held;
            if (r32ui_binding(dimension) != no_binding) counts[r32ui_binding(dimension)] = held;
        }
    };
    const auto fits = [&](std::uint32_t headroom, std::uint64_t set_limit) {
        apply(false);
        std::uint64_t sampled = 0, sampler = 0, stored = 0, buffers = 0;
        for (std::uint32_t b = 0; b < binding_count; ++b) {
            if (!binding_enabled(b, features)) continue;
            const auto type = bindings[b].type;
            (type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE   ? sampled
             : type == VK_DESCRIPTOR_TYPE_SAMPLER       ? sampler
             : type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? stored
                                                        : buffers) += counts[b];
        }
        const auto within = [&](std::uint64_t n, std::uint32_t limit) { return n + headroom <= limit; };
        const std::uint64_t all = sampled + sampler + stored + buffers;
        return within(sampled, sampled_limit) && within(sampler, sampler_limit) && within(stored, storage_limit) &&
               within(sampled + stored + buffers, resource_limit) && all <= pool_limit &&
               (all <= set_limit || (layout_supported && layout_supported(features)));
    };
    // Raises n to the largest value up to `high` that fits; leaves it where it is when none does.
    const auto grow = [&](std::uint32_t& n, std::uint32_t high, std::uint32_t headroom, std::uint64_t set_limit) {
        std::uint32_t low = n;
        while (low < high) {
            n = low + (high - low + 1) / 2;
            if (fits(headroom, set_limit)) low = n;
            else high = n - 1;
        }
        n = low;
    };

    constexpr std::uint64_t no_limit = ~0ull;
    const auto ms_samples = features.ms_samples;
    features.ms_samples.fill(VK_SAMPLE_COUNT_1_BIT);
    grow(samplers, bindings[sampler_binding].count, 0, no_limit);
    grow(storage, bindings[storage_binding(ImageDimension::dim_2d, 0)].count, 0, no_limit);
    grow(images, bindings[sampled_binding(ImageDimension::dim_2d)].count, 0, no_limit);
    apply(false);
    const bool full = images == bindings[0].count && samplers == bindings[sampler_binding].count &&
                      storage == bindings[storage_binding(ImageDimension::dim_2d, 0)].count;
    features.ms_samples = ms_samples;
    limit_ms_bindings(features, sampled_limit, std::min(resource_limit, pool_limit - std::min(pool_limit, samplers)));

    std::uint64_t set_limit = 0;
    for (std::uint32_t b = 0; b < binding_count; ++b)
        if (binding_enabled(b, features)) set_limit += counts[b];
    set_limit = std::max(set_limit, std::uint64_t(v11.maxPerSetDescriptors));
    const std::uint32_t creatable =
        limits.maxSamplerAllocationCount - std::min(limits.maxSamplerAllocationCount, reserved_samplers);
    grow(samplers, std::max(samplers, std::min(max_samplers, creatable)), growth_headroom, set_limit);
    grow(images, std::max(images, max_2d_images), growth_headroom, set_limit);
    grow(storage, std::max(storage, max_storage_indices), growth_headroom, set_limit);
    // The other three sampled arrays are separate index spaces. Their growth is part of the degradation policy;
    // when it is disabled, preserve the pre-slice descriptor counts exactly.
    if (features.degradation) {
        grow(counts[sampled_binding(ImageDimension::dim_2d_array)],
             std::max(counts[sampled_binding(ImageDimension::dim_2d_array)], max_2d_array_images), growth_headroom,
             set_limit);
        grow(counts[sampled_binding(ImageDimension::dim_3d)],
             std::max(counts[sampled_binding(ImageDimension::dim_3d)], max_3d_images), growth_headroom, set_limit);
        grow(counts[sampled_binding(ImageDimension::dim_1d)],
             std::max(counts[sampled_binding(ImageDimension::dim_1d)], max_1d_images), growth_headroom, set_limit);
    }
    apply(true);
    return full;
}

} // namespace bindless

class BindlessSet {
public:
    BindlessSet(PFN_vkGetDeviceProcAddr get, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
                BindlessFeatures features = {})
        : device_(device), memory_(memory), features_(features) {
        ok_ = vk_.load(get, device) && create_layout() && create_fallbacks() && create_table_buffers() && fill();
    }

    ~BindlessSet() {
        release_retired();
        release_spares();
        destroy(table_buffers_);
        if (fallback_sampler_) vk_.vkDestroySampler(device_, fallback_sampler_, nullptr);
        for (const VkImageView view : fallback_views_) vk_.vkDestroyImageView(device_, view, nullptr);
        for (const auto& image : fallback_images_) {
            if (image.image) vk_.vkDestroyImage(device_, image.image, nullptr);
            if (image.memory) vk_.vkFreeMemory(device_, image.memory, nullptr);
        }
        if (pool_) vk_.vkDestroyDescriptorPool(device_, pool_, nullptr);  // frees the set
        if (layout_) vk_.vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    }

    BindlessSet(const BindlessSet&) = delete;
    BindlessSet& operator=(const BindlessSet&) = delete;

    bool ok() const { return ok_; }
    bool supports_ms_class(std::uint32_t component_class) const {
        return component_class < classes && features_.ms_samples[component_class] != VK_SAMPLE_COUNT_1_BIT;
    }
    // How many images of a dimension the set holds, the fallback included, and how many of the lowest of those
    // indices its storage arrays hold (0 for the MS dimensions, which have none). The same for samplers.
    std::uint32_t image_capacity(ImageDimension dimension) const {
        return features_.counts[bindless::sampled_binding(dimension)];
    }
    std::uint32_t storage_capacity(ImageDimension dimension) const {
        return bindless::multisampled(dimension) ? 0 : features_.counts[bindless::storage_binding(dimension, 0)];
    }
    std::uint32_t sampler_capacity() const { return features_.counts[bindless::sampler_binding]; }
    // How many more images of a dimension, or samplers, add_image and add_sampler can take before they return 0.
    std::uint32_t free_images(ImageDimension dimension) const {
        const auto& used = image_used_[std::uint32_t(dimension)];
        return std::uint32_t(std::count(used.begin(), used.end(), std::uint8_t(0)));
    }
    std::uint32_t free_samplers() const {
        return std::uint32_t(std::count(sampler_used_.begin(), sampler_used_.end(), std::uint8_t(0)));
    }
    // Set 0 of every pipeline layout made for translated shaders.
    VkDescriptorSetLayout layout() const { return layout_; }
    // Bound at set 0. flush() updates it in place until flush_replaces_table() is true: then the flush makes a new
    // set (and table buffers), so read set() again after it, when recording the commands that use it. A submission
    // that bound the previous set keeps it, intact, until release_retired() frees it.
    VkDescriptorSet set() const { return set_; }

    // What the degradation rules above cost and how often they ran, for the boot report and the tests: flushes that
    // could not make a new generation and kept the last uploaded table instead (the submission went on, keys added
    // since that upload read the fallback), replacements that reused a released generation instead of allocating
    // (spares_reused), and keys refused because the shader's eight probes can never resolve them (keys_refused).
    std::uint64_t degraded_flushes() const { return degraded_flushes_; }
    std::uint64_t spares_reused() const { return spares_reused_; }
    std::uint64_t keys_refused() const { return keys_refused_; }
    bool last_flush_degraded() const { return last_flush_degraded_; }
    // Released generations kept for reuse (0 or 1; release_retired keeps the largest, release_spares frees it).
    std::size_t spare_count() const { return spares_.size(); }

    // The fallback images are created with undefined contents: record this once, in a command buffer that executes
    // before anything uses the set. It moves them to GENERAL, the layout their descriptors name, and zeroes them.
    bool fallbacks_ready() const { return fallbacks_ready_; }
    void record_fallback_init(VkCommandBuffer command_buffer) {
        fallbacks_ready_ = true;
        std::vector<VkImageMemoryBarrier> barriers;
        for (const auto& image : fallback_images_) {
            if (!image.image) continue;  // unsupported multisample component class
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barriers.push_back(barrier);
        }
        vk_.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, std::uint32_t(barriers.size()), barriers.data());
        const VkClearColorValue zero{};
        for (const auto& barrier : barriers)
            vk_.vkCmdClearColorImage(command_buffer, barrier.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                                     &barrier.subresourceRange);
        VkMemoryBarrier done{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        done.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        done.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vk_.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                                 &done, 0, nullptr, 0, nullptr);
    }

    // Gives an image an index in its dimension's bindings: 0 when the dimension is full. `sampled` goes to the
    // sampled array in `sampled_layout` (the layout the image is in when shaders read it); the storage views go to
    // the storage arrays in GENERAL. For MS views, sampled_class selects the float, uint or sint sampled array.
    // The caller maps the T# to the index with insert_key(). Takes effect at flush().
    std::uint32_t add_image(ImageDimension dimension, VkImageView sampled, VkImageLayout sampled_layout,
                            const StorageViews& storage = {}, std::uint32_t sampled_class = 0) {
        const bool stores = storage.as_float || storage.as_uint || storage.as_sint || storage.r32ui;
        if (bindless::multisampled(dimension) && (stores || !supports_ms_class(sampled_class))) return 0;
        auto& used = image_used_[std::uint32_t(dimension)];
        const std::uint32_t low_end = std::min(std::uint32_t(used.size()), storage_capacity(dimension));
        std::uint32_t index = stores ? 0 : allocate(used, low_end, std::uint32_t(used.size()));
        if (!index) index = allocate(used, 1, low_end);
        if (index) write_image(dimension, index, sampled, sampled_layout, storage, sampled_class);
        return index;
    }

    // New views or a new layout for an index add_image returned (a texture's state changed, say).
    void update_image(ImageDimension dimension, std::uint32_t index, VkImageView sampled, VkImageLayout sampled_layout,
                      const StorageViews& storage = {}, std::uint32_t sampled_class = 0) {
        const auto& used = image_used_[std::uint32_t(dimension)];
        if (index && index < used.size() && used[index])
            write_image(dimension, index, sampled, sampled_layout, storage, sampled_class);
    }

    // Returns the index to the fallback and frees it. The caller removes the image's keys with erase_key() first.
    void remove_image(ImageDimension dimension, std::uint32_t index) {
        auto& used = image_used_[std::uint32_t(dimension)];
        if (!index || index >= used.size() || !used[index]) return;
        write_image(dimension, index, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, {}, 0);
        used[index] = 0;
    }

    // 0 when the sampler array is full.
    std::uint32_t add_sampler(VkSampler sampler) {
        const std::uint32_t index = allocate(sampler_used_, 1, std::uint32_t(sampler_used_.size()));
        if (index) write_sampler(index, sampler);
        return index;
    }

    void remove_sampler(std::uint32_t index) {
        if (!index || index >= sampler_used_.size() || !sampler_used_[index]) return;
        write_sampler(index, fallback_sampler_);
        sampler_used_[index] = 0;
    }

    // T# and S# keys to the indices above: the table of binding 2 (descriptor_table.h). Changes reach the shaders
    // at flush(). False when the key cannot be resolved at all (see the class comment): the table is unchanged, the
    // image keeps the index it was given, its T#/S# then reads the fallback, and this is counted and logged.
    bool insert_key(const DescriptorKey& key, std::uint32_t index) {
        DescriptorTable::InsertResult result;
        try {
            result = table_.insert_or_update(key.data(), index);
        } catch (const std::length_error& error) {
            if (!features_.degradation) throw;  // pre-slice mode preserves the old exception path
            ++keys_refused_;
            if (keys_refused_ == 1 || keys_refused_ % 4096 == 0)
                std::fprintf(stderr, "[rt] d3d12: bindless: a T#/S# key the shader's eight probes can never resolve is "
                                     "refused (%s); its shader reads the fallback index (%llu so far)\n",
                             error.what(), static_cast<unsigned long long>(keys_refused_));
            return false;
        }
        table_dirty_ |= result != DescriptorTable::InsertResult::unchanged;
        existing_entries_changed_ |= result == DescriptorTable::InsertResult::replaced ||
                                     result == DescriptorTable::InsertResult::added_with_relocation;
        return true;
    }
    // Erasing empties only the key's own slot; no other entry moves (DescriptorTable::erase), so it needs no idle.
    // The caller erases a key once no submitted work uses it (its texture is gone), like D3D12 requires.
    bool erase_key(const DescriptorKey& key) {
        if (!table_.erase(key)) return false;
        table_dirty_ = true;
        return true;
    }
    const DescriptorTable& table() const { return table_; }

    // True when the next flush() would change entries a running shader may be probing (a live key's index replaced
    // or a live key moved by a relocating insert), or replace the table's buffer. flush() then does not write in
    // place: it uploads the whole table to new buffers, names them in a new descriptor set, and retires the old
    // generation, so running work keeps reading an unchanged table and nobody waits for it. Without it the flush only
    // adds entries or empties the slots of erased keys, and the copy of an entry is not atomic: a shader probing a
    // different key could in principle see a torn entry that matches it, which is accepted as vanishingly unlikely
    // because the added or erased key is not live (no running work uses it).
    bool flush_replaces_table() const {
        return table_dirty_ && (existing_entries_changed_ || force_full_upload_ ||
                                table_.size_bytes() > table_buffers_.table.size);
    }

    // True when the table's buffer was allocated in DEVICE_LOCAL memory (the fallback for a device without it is
    // HOST_VISIBLE, which still works, just slower).
    bool table_in_device_local_memory() const { return table_device_local_; }

    // True when the command buffer that carried the last table upload reached the queue; false keeps the upload
    // obligation, and the next flush() rebuilds the whole table (the changed-slot list was consumed when the copy
    // was recorded). A failed vkQueueSubmit is the caller's signal, not ours.
    void table_upload_result(bool submitted) {
        if (!upload_in_flight_) return;
        upload_in_flight_ = false;
        if (submitted) return;
        table_dirty_ = true;
        force_full_upload_ = true;
    }

    // Writes the pending descriptor changes and uploads the table, in `command_buffer`, before the commands that use
    // the set are recorded after it. When flush_replaces_table() was true this starts a new generation (table
    // buffers, descriptor set, binding 2 and every descriptor written so far) and puts the previous one on the retire
    // list, stamped `retire_ticket`: the owner's number for "every submission made so far", which release_retired()
    // compares with what has completed. true whenever the submission may go on: a new generation that cannot be made
    // keeps the last uploaded table (see the class comment) and only makes this flush a degraded one. False is
    // reserved for the caller giving no command buffer at all, where nothing can be recorded.
    bool flush(VkCommandBuffer command_buffer, std::uint64_t retire_ticket = 0) {
        bool replaced = false;
        bool written = true;
        if (table_dirty_ || !table_uploaded_) written = upload_table(command_buffer, retire_ticket, replaced);
        last_flush_replaced_ = replaced;
        last_flush_degraded_ = false;
        if (!written) {
            if (!features_.degradation || !command_buffer) return false;
            // The new generation could not be made. The current set is the one running work reads, so its descriptors
            // are not written in place either: the last uploaded table stays, which still resolves every key it
            // holds, and the pending writes and the table's changes stay queued for the next flush. A key added
            // since that upload is a miss and reads the fallback index 0, never a wrong image.
            if (!command_buffer) return false;  // nothing could be recorded: the caller retries
            ++degraded_flushes_;
            last_flush_degraded_ = true;
            log_bounded(degraded_flushes_, "a table generation could not be made (no table memory); the last "
                                           "uploaded table is kept, keys added since it read the fallback");
            return true;
        }
        for (const Pending& pending : pending_) {
            if (pending.binding >= shadow_.size() || pending.index >= shadow_[pending.binding].size()) continue;
            shadow_[pending.binding][pending.index] = pending.info;
            shadow_written_[pending.binding][pending.index] = 1;
        }
        if (replaced) {
            write_shadow();  // includes the pending writes just folded in
        } else if (!pending_.empty()) {
            std::vector<VkWriteDescriptorSet> writes(pending_.size());
            for (std::size_t i = 0; i < pending_.size(); ++i) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = set_;
                writes[i].dstBinding = pending_[i].binding;
                writes[i].dstArrayElement = pending_[i].index;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = bindless::bindings[pending_[i].binding].type;
                writes[i].pImageInfo = &pending_[i].info;
            }
            vk_.vkUpdateDescriptorSets(device_, std::uint32_t(writes.size()), writes.data(), 0, nullptr);
        }
        pending_.clear();
        pending_slots_.clear();
        return written;
    }

    // Frees the generations (table buffers and descriptor set) that flush() retired with a ticket up to `completed`.
    // The default frees all of them: when nothing submitted before the last flush() still runs. The largest of them is
    // kept as a spare instead (release_spares() frees it in the destructor): the next replacement whose table fits
    // its buffers reuses them and the descriptor set, which is the allocation the streamed churn would otherwise
    // repeat, and the one a device out of memory cannot serve.
    void release_retired(std::uint64_t completed = UINT64_MAX) {
        std::size_t kept = 0;
        for (auto& generation : retired_) {
            if (generation.ticket <= completed) {
                if (features_.degradation && generation.set &&
                    (spares_.empty() || generation.buffers.table.size > spares_.front().buffers.table.size)) {
                    release_spares();  // one spare, the largest released table
                    spares_.push_back(generation);
                } else {
                    destroy(generation.buffers);
                    if (generation.pool) vk_.vkDestroyDescriptorPool(device_, generation.pool, nullptr);
                }
            } else {
                retired_[kept++] = generation;
            }
        }
        retired_.resize(kept);
    }
    bool has_retired() const { return !retired_.empty(); }
    // True when the last flush() started a new generation: its table upload is in that flush's command buffer, so
    // work on other queues that binds the new set must run after it (the owner orders them).
    bool last_flush_replaced() const { return last_flush_replaced_; }
    // The ticket of the oldest retired generation (0 when there is none).
    std::uint64_t oldest_retired_ticket() const { return retired_.empty() ? 0 : retired_.front().ticket; }
    // How many generations flush() has made after the first, and how many wait for release_retired().
    std::uint64_t generations_made() const { return generations_made_; }
    std::size_t retired_count() const { return retired_.size(); }

private:
    struct Functions {
#define PALMETTO_BINDLESS_FUNCTIONS(X)                                                                          \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)                   \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateBuffer)         \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkAllocateMemory)              \
    X(vkFreeMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)          \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler)     \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdUpdateBuffer) X(vkCmdClearColorImage)
#define PALMETTO_BINDLESS_MEMBER(name) PFN_##name name = nullptr;
        PALMETTO_BINDLESS_FUNCTIONS(PALMETTO_BINDLESS_MEMBER)
#undef PALMETTO_BINDLESS_MEMBER

        bool load(PFN_vkGetDeviceProcAddr get, VkDevice device) {
            bool all = true;
#define PALMETTO_BINDLESS_LOAD(name)                                      \
    name = reinterpret_cast<PFN_##name>(get(device, #name));             \
    all = all && name != nullptr;
            PALMETTO_BINDLESS_FUNCTIONS(PALMETTO_BINDLESS_LOAD)
#undef PALMETTO_BINDLESS_LOAD
#undef PALMETTO_BINDLESS_FUNCTIONS
            return all;
        }
    };

    struct FallbackImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct TableBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        std::size_t size = 0;
        bool device_local = false;  // the memory type this buffer was given (staging is always host-visible)
    };

    // The table and the mapped staging copy the CPU writes before vkCmdCopyBuffer moves the changes.
    struct TableBuffers {
        TableBuffer table;    // DEVICE_LOCAL when the device has it; what binding 2 names
        TableBuffer staging;  // HOST_VISIBLE | HOST_COHERENT
    };

    struct Pending {
        std::uint32_t binding;
        std::uint32_t index;
        VkDescriptorImageInfo info;
    };

    static constexpr std::uint32_t dimensions = 6, classes = 3;

    // A bounded named log for the degradation counters: the first event and every 4096th, so a streamed load that
    // degrades often does not flood stderr.
    static void log_bounded(std::uint64_t count, const char* what) {
        if (count == 1 || count % 4096 == 0)
            std::fprintf(stderr, "[rt] d3d12: bindless: %s (%llu so far)\n", what,
                         static_cast<unsigned long long>(count));
    }

    // The first free index in [begin, end), marked used; 0 when there is none.
    static std::uint32_t allocate(std::vector<std::uint8_t>& used, std::uint32_t begin, std::uint32_t end) {
        for (std::uint32_t i = begin; i < end; ++i) {
            if (used[i]) continue;
            used[i] = 1;
            return i;
        }
        return 0;
    }

    std::uint32_t memory_type(std::uint32_t allowed, VkMemoryPropertyFlags wanted) const {
        for (std::uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
            if ((allowed & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        return 0xffffffffu;
    }

    bool create_layout() {
        const bindless::SetLayout layout(features_);
        const std::uint32_t count = layout.info.bindingCount;
        pool_size_count_ = count;
        for (std::uint32_t i = 0; i < count; ++i)
            pool_sizes_[i] = {layout.entries[i].descriptorType, layout.entries[i].descriptorCount};
        if (vk_.vkCreateDescriptorSetLayout(device_, &layout.info, nullptr, &layout_) != VK_SUCCESS) return false;
        if (!make_set(pool_, set_)) return false;
        for (std::uint32_t b = 0; b < bindless::bindings.size(); ++b) {
            if (b == bindless::table_binding || !bindless::binding_enabled(b, features_)) continue;
            shadow_[b].assign(features_.counts[b], VkDescriptorImageInfo{});
            shadow_written_[b].assign(features_.counts[b], 0);
        }

        for (std::uint32_t d = 0; d < dimensions; ++d) {
            image_used_[d].assign(image_capacity(ImageDimension(d)), 0);
            image_used_[d][0] = 1;
        }
        sampler_used_.assign(sampler_capacity(), 0);
        sampler_used_[0] = 1;
        return true;
    }

    // A one-set pool and its set at the layout's sizes: the first generation's, and each replacement's.
    bool make_set(VkDescriptorPool& pool, VkDescriptorSet& set) const {
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.flags = features_.update_after_bind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = pool_size_count_;
        pool_info.pPoolSizes = pool_sizes_.data();
        if (vk_.vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool) != VK_SUCCESS) {
            pool = VK_NULL_HANDLE;
            return false;
        }
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_info.descriptorPool = pool;
        set_info.descriptorSetCount = 1;
        set_info.pSetLayouts = &layout_;
        if (vk_.vkAllocateDescriptorSets(device_, &set_info, &set) == VK_SUCCESS) return true;
        vk_.vkDestroyDescriptorPool(device_, pool, nullptr);
        pool = VK_NULL_HANDLE;
        set = VK_NULL_HANDLE;
        return false;
    }

    // Writes every descriptor written so far (the shadow) into the current set, one update per run of written
    // elements: the set was just made, and unwritten elements stay unwritten, as partially_bound allows.
    void write_shadow() const {
        std::vector<VkWriteDescriptorSet> writes;
        for (std::uint32_t b = 0; b < bindless::bindings.size(); ++b) {
            const auto& written = shadow_written_[b];
            for (std::size_t i = 0; i < written.size();) {
                if (!written[i]) {
                    ++i;
                    continue;
                }
                std::size_t end = i;
                while (end < written.size() && written[end]) ++end;
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = set_;
                write.dstBinding = b;
                write.dstArrayElement = std::uint32_t(i);
                write.descriptorCount = std::uint32_t(end - i);
                write.descriptorType = bindless::bindings[b].type;
                write.pImageInfo = &shadow_[b][i];
                writes.push_back(write);
                i = end;
            }
        }
        if (!writes.empty())
            vk_.vkUpdateDescriptorSets(device_, std::uint32_t(writes.size()), writes.data(), 0, nullptr);
    }

    // One 1x1 image for each image type and component class, and the views of them every binding needs.
    bool create_fallbacks() {
        // Each MS class shares one sampled-only fallback between its 2D and 2D-array views.
        constexpr VkImageType types[] = {VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D, VK_IMAGE_TYPE_1D, VK_IMAGE_TYPE_2D};
        constexpr std::uint32_t ms_type = 3;
        constexpr VkFormat formats[classes] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
        // dimension -> (index into types, view type)
        constexpr std::uint32_t type_of[dimensions] = {0, 0, 1, 2, ms_type, ms_type};
        constexpr VkImageViewType view_types[dimensions] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                                            VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_1D,
                                                            VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY};
        for (std::uint32_t t = 0; t < std::size(types); ++t) {
            for (std::uint32_t c = 0; c < classes; ++c) {
                if (t == ms_type && !supports_ms_class(c)) continue;
                VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                info.imageType = types[t];
                info.format = formats[c];
                info.extent = {1, 1, 1};
                info.mipLevels = 1;
                info.arrayLayers = 1;
                info.samples = t == ms_type ? features_.ms_samples[c] : VK_SAMPLE_COUNT_1_BIT;
                info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                if (t != ms_type) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
                if (t == ms_type || c == 0) info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
                FallbackImage& image = fallback_images_[t * classes + c];
                if (vk_.vkCreateImage(device_, &info, nullptr, &image.image) != VK_SUCCESS) return false;
                VkMemoryRequirements requirements{};
                vk_.vkGetImageMemoryRequirements(device_, image.image, &requirements);
                VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocation.allocationSize = requirements.size;
                allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, 0);
                if (allocation.memoryTypeIndex == 0xffffffffu ||
                    vk_.vkAllocateMemory(device_, &allocation, nullptr, &image.memory) != VK_SUCCESS ||
                    vk_.vkBindImageMemory(device_, image.image, image.memory, 0) != VK_SUCCESS)
                    return false;
            }
        }
        for (std::uint32_t d = 0; d < dimensions; ++d) {
            const ImageDimension dimension{d};
            for (std::uint32_t c = 0; c < classes; ++c) {
                VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                info.image = fallback_images_[type_of[d] * classes + c].image;
                if (!info.image) continue;
                info.viewType = view_types[d];
                info.format = formats[c];
                info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkImageView view = VK_NULL_HANDLE;
                if (vk_.vkCreateImageView(device_, &info, nullptr, &view) != VK_SUCCESS) return false;
                fallback_views_.push_back(view);
                if (c == 0 || bindless::multisampled(dimension))
                    fallback_by_binding_[bindless::sampled_binding(dimension, c)] = view;
                const auto storage_at = bindless::storage_binding(dimension, c);
                if (storage_at != bindless::no_binding) fallback_by_binding_[storage_at] = view;
                const auto r32ui_at = bindless::r32ui_binding(dimension);
                if (c == 1 && r32ui_at != bindless::no_binding) fallback_by_binding_[r32ui_at] = view;
            }
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        return vk_.vkCreateSampler(device_, &sampler, nullptr, &fallback_sampler_) == VK_SUCCESS;
    }

    bool create(TableBuffer& out, std::size_t bytes, bool device_local, bool* got_device_local = nullptr) const {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     (device_local ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        TableBuffer made;
        made.size = bytes;
        if (vk_.vkCreateBuffer(device_, &info, nullptr, &made.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vk_.vkGetBufferMemoryRequirements(device_, made.buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex =
            device_local ? memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                         : memory_type(requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (device_local && allocation.memoryTypeIndex == 0xffffffffu)
            allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (got_device_local)
            *got_device_local = allocation.memoryTypeIndex != 0xffffffffu &&
                                (memory_.memoryTypes[allocation.memoryTypeIndex].propertyFlags &
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        made.device_local = got_device_local ? *got_device_local
                                            : device_local && allocation.memoryTypeIndex != 0xffffffffu;
        bool ready = allocation.memoryTypeIndex != 0xffffffffu &&
                     vk_.vkAllocateMemory(device_, &allocation, nullptr, &made.memory) == VK_SUCCESS &&
                     vk_.vkBindBufferMemory(device_, made.buffer, made.memory, 0) == VK_SUCCESS;
        if (ready && !device_local)
            ready = vk_.vkMapMemory(device_, made.memory, 0, VK_WHOLE_SIZE, 0, &made.mapped) == VK_SUCCESS;
        if (!ready) {
            destroy(made);
            return false;
        }
        out = made;
        return true;
    }

    void destroy(TableBuffer& buffer) const {
        if (buffer.buffer) vk_.vkDestroyBuffer(device_, buffer.buffer, nullptr);
        if (buffer.memory) vk_.vkFreeMemory(device_, buffer.memory, nullptr);  // unmaps it
        buffer = {};
    }

    void destroy(TableBuffers& buffers) const {
        destroy(buffers.staging);
        destroy(buffers.table);
    }

    // Frees the spare generation kept by release_retired(); the destructor calls it, nothing else does.
    void release_spares() {
        for (auto& spare : spares_) {
            destroy(spare.buffers);
            if (spare.pool) vk_.vkDestroyDescriptorPool(device_, spare.pool, nullptr);
        }
        spares_.clear();
    }

    bool create_table_buffers() {
        const std::size_t bytes = table_.size_bytes();
        TableBuffers made;
        bool device_local = false;
        if (!create(made.table, bytes, true, &device_local) || !create(made.staging, bytes, false)) {
            destroy(made);
            return false;
        }
        table_buffers_ = made;
        table_device_local_ = device_local;
        table_uploaded_ = false;  // the first flush() uploads it, in the command buffer that will use it
        table_dirty_ = existing_entries_changed_ = false;
        return true;
    }

    // Uploads what changed since the last call: the whole table when it grew (the mask moved) or was never uploaded,
    // the changed slots otherwise. The copy and its barrier go into `command_buffer`, before the draws.
    bool upload_table(VkCommandBuffer command_buffer, std::uint64_t retire_ticket, bool& replaced) {
        if (!command_buffer) return false;
        const std::size_t bytes = table_.size_bytes();
        // A table that grew, or whose live entries change, goes to a new generation; the old one may still be read.
        const bool replace = bytes > table_buffers_.table.size ||
                             (table_uploaded_ && (existing_entries_changed_ || force_full_upload_));
        bool full = force_full_upload_ || !table_uploaded_ || replace;
        if (replace) {
            TableBuffers larger;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (features_.degradation && !spares_.empty() && spares_.back().buffers.table.size >= bytes &&
                spares_.back().buffers.staging.size >= bytes) {
                // A released generation whose buffers hold this table: its buffers, descriptor set and pool are
                // reused, so this replacement allocates nothing. Extra buffer bytes are unused (binding 2 names the
                // whole buffer and the shader's mask reads only the table's slots). The set it was handed can hold
                // only descriptors the shadow also holds, and write_shadow() rewrites every element ever written.
                Generation spare = std::move(spares_.back());
                spares_.pop_back();
                larger = spare.buffers;
                pool = spare.pool;
                set = spare.set;
                ++spares_reused_;
            } else if (!create(larger.table, bytes, true, nullptr) || !create(larger.staging, bytes, false) ||
                       !make_set(pool, set)) {
                destroy(larger);
                return false;
            }
            table_device_local_ = larger.table.device_local;
            retired_.push_back({table_buffers_, pool_, set_, retire_ticket});
            table_buffers_ = larger;
            pool_ = pool;
            set_ = set;
            write_table_binding(table_buffers_.table.buffer);
            replaced = true;
            ++generations_made_;
        }
        DescriptorTable::Changes changes = table_.take_changes();
        full = full || changes.all;
        bool recorded = false;
        // The staging buffer is written only for buffers that were just created (first upload or growth): a later
        // flush then either updates slots with vkCmdUpdateBuffer or rebuilds from a fresh pair, so no copy still in
        // flight can be reading bytes the CPU rewrites.
        const bool fresh = replace || !table_uploaded_;
        const auto upload_all = [&]() {
            if (fresh) {
                table_.write_to_buffer(table_buffers_.staging.mapped);
                copy_table_range(command_buffer, 0, bytes);
            } else {
                const std::vector<std::uint32_t> words = table_.build_gpu_buffer();
                update_table_range(command_buffer, 0, words.data(), bytes);
            }
            recorded = true;
        };
        if (full) {
            upload_all();
        } else if (!changes.slots.empty()) {
            std::vector<std::uint32_t> slots = std::move(changes.slots);
            std::sort(slots.begin(), slots.end());
            slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
            std::vector<std::uint32_t> words;
            bool staged = true;
            for (std::size_t first = 0; staged && first < slots.size();) {
                std::size_t last = first;
                while (last + 1 < slots.size() && slots[last + 1] == slots[last] + 1) ++last;
                const std::uint32_t count = std::uint32_t(last - first + 1);
                words.resize(std::size_t(count) * 10);
                staged = table_.read_slots(slots[first], count, words.data());
                if (staged) {
                    update_table_range(command_buffer, entry_offset(slots[first]), words.data(),
                                       VkDeviceSize(words.size() * sizeof(std::uint32_t)));
                    recorded = true;
                }
                first = last + 1;
            }
            if (!staged) upload_all();  // an inconsistent slot list: the whole table is still correct
        }
        if (!recorded) return true;  // nothing was copied: keep the obligation for the next flush
        record_table_barrier(command_buffer);
        upload_in_flight_ = true;
        table_dirty_ = existing_entries_changed_ = false;
        table_uploaded_ = true;
        force_full_upload_ = false;
        return true;
    }

    // Copies the data into the command buffer while recording (64 KiB per call, the Vulkan limit), so no staging
    // buffer is read at execution: a later flush can rewrite its own staging without racing a pending copy.
    void update_table_range(VkCommandBuffer command_buffer, VkDeviceSize offset, const void* data,
                            VkDeviceSize size) const {
        constexpr VkDeviceSize max_chunk = 65536;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (VkDeviceSize done = 0; done < size; done += max_chunk) {
            const VkDeviceSize chunk = std::min(max_chunk, size - done);
            vk_.vkCmdUpdateBuffer(command_buffer, table_buffers_.table.buffer, offset + done, chunk, bytes + done);
        }
    }

    void copy_table_range(VkCommandBuffer command_buffer, VkDeviceSize offset, VkDeviceSize size) const {
        const VkBufferCopy region{offset, offset, size};
        vk_.vkCmdCopyBuffer(command_buffer, table_buffers_.staging.buffer, table_buffers_.table.buffer, 1, &region);
    }

    void record_table_barrier(VkCommandBuffer command_buffer) const {
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = table_buffers_.table.buffer;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
        vk_.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 nullptr, 1, &barrier, 0, nullptr);
    }

    // The entry's first dword after the mask word; each entry is 10 dwords (40 bytes).
    static constexpr VkDeviceSize entry_bytes = 10 * sizeof(std::uint32_t);
    static VkDeviceSize entry_offset(std::uint32_t slot) {
        return sizeof(std::uint32_t) + VkDeviceSize(slot) * entry_bytes;
    }

    void write_table_binding(VkBuffer buffer) const {
        const VkDescriptorBufferInfo info{buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set_;
        write.dstBinding = bindless::table_binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        vk_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    VkDescriptorImageInfo fallback_info(std::uint32_t binding) const {
        if (binding == bindless::sampler_binding) return {fallback_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        return {VK_NULL_HANDLE, fallback_by_binding_[binding], VK_IMAGE_LAYOUT_GENERAL};
    }

    // The table at binding 2 and the fallback at index 0 of every array, or in every element without partially_bound.
    bool fill() {
        write_table_binding(table_buffers_.table.buffer);
        std::vector<std::vector<VkDescriptorImageInfo>> infos;
        std::vector<VkWriteDescriptorSet> writes;
        infos.reserve(bindless::bindings.size());
        for (std::uint32_t b = 0; b < bindless::bindings.size(); ++b) {
            if (b == bindless::table_binding || !bindless::binding_enabled(b, features_)) continue;
            const std::uint32_t count = features_.partially_bound ? 1 : features_.counts[b];
            infos.emplace_back(count, fallback_info(b));
            std::fill_n(shadow_[b].begin(), count, infos.back()[0]);
            std::fill_n(shadow_written_[b].begin(), count, std::uint8_t(1));
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set_;
            write.dstBinding = b;
            write.descriptorCount = count;
            write.descriptorType = bindless::bindings[b].type;
            write.pImageInfo = infos.back().data();
            writes.push_back(write);
        }
        vk_.vkUpdateDescriptorSets(device_, std::uint32_t(writes.size()), writes.data(), 0, nullptr);
        return true;
    }

    // A slot has one pending write: a later one takes its place. So a view or sampler that is removed before the
    // next flush(), and may be destroyed by then, is never written.
    void queue(std::uint32_t binding, std::uint32_t index, const VkDescriptorImageInfo& info) {
        const std::uint64_t slot = (std::uint64_t(binding) << 32) | index;
        const auto [at, added] = pending_slots_.try_emplace(slot, pending_.size());
        if (added)
            pending_.push_back({binding, index, info});
        else
            pending_[at->second].info = info;
    }

    void write_sampler(std::uint32_t index, VkSampler sampler) {
        queue(bindless::sampler_binding, index, {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    }

    // Queues every binding of the dimension at `index`: the given views, or the fallback where one is null.
    void write_image(ImageDimension dimension, std::uint32_t index, VkImageView sampled, VkImageLayout sampled_layout,
                     const StorageViews& storage, std::uint32_t sampled_class) {
        if (bindless::multisampled(dimension)) {
            for (std::uint32_t c = 0; c < classes; ++c) {
                if (!supports_ms_class(c)) continue;
                const auto binding = bindless::sampled_binding(dimension, c);
                queue(binding, index, c == sampled_class && sampled
                                          ? VkDescriptorImageInfo{VK_NULL_HANDLE, sampled, sampled_layout}
                                          : fallback_info(binding));
            }
            return;
        }
        const std::uint32_t sampled_at = bindless::sampled_binding(dimension);
        queue(sampled_at, index,
              sampled ? VkDescriptorImageInfo{VK_NULL_HANDLE, sampled, sampled_layout} : fallback_info(sampled_at));
        const auto store = [&](std::uint32_t binding, VkImageView view) {
            if (binding == bindless::no_binding || index >= features_.counts[binding]) return;
            queue(binding, index,
                  view ? VkDescriptorImageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL} : fallback_info(binding));
        };
        store(bindless::storage_binding(dimension, 0), storage.as_float);
        store(bindless::storage_binding(dimension, 1), storage.as_uint);
        store(bindless::storage_binding(dimension, 2), storage.as_sint);
        store(bindless::r32ui_binding(dimension), storage.r32ui);
    }

    VkDevice device_;
    VkPhysicalDeviceMemoryProperties memory_;
    BindlessFeatures features_;
    Functions vk_;
    bool ok_ = false;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    std::array<FallbackImage, 4 * classes> fallback_images_{};  // [2D, 3D, 1D, 2D MS][class]
    std::vector<VkImageView> fallback_views_;
    std::array<VkImageView, bindless::bindings.size()> fallback_by_binding_{};
    VkSampler fallback_sampler_ = VK_NULL_HANDLE;
    bool fallbacks_ready_ = false;
    std::array<std::vector<std::uint8_t>, dimensions> image_used_;
    std::vector<std::uint8_t> sampler_used_;
    std::vector<Pending> pending_;
    std::unordered_map<std::uint64_t, std::size_t> pending_slots_;  // binding and index -> place in pending_
    DescriptorTable table_{64};
    TableBuffers table_buffers_;
    bool table_uploaded_ = false;  // the table's current contents are in table_buffers_.table
    bool table_device_local_ = false;
    bool table_dirty_ = false;    // the table differs from the buffer
    bool existing_entries_changed_ = false;  // ... and not only by entries added
    bool upload_in_flight_ = false;   // a copy is recorded, not yet known to have reached the queue
    bool force_full_upload_ = false;  // the last submit failed: rebuild the whole table on the next flush
    struct Generation {  // what a replaced table leaves for release_retired()
        TableBuffers buffers;
        VkDescriptorPool pool = VK_NULL_HANDLE;  // owns the descriptor set
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::uint64_t ticket = 0;
    };
    std::vector<Generation> retired_;
    std::vector<Generation> spares_;  // released generations kept for reuse (release_retired, release_spares)
    std::uint64_t generations_made_ = 0;
    std::uint64_t degraded_flushes_ = 0;  // flushes that could not make a generation and kept the last table
    std::uint64_t spares_reused_ = 0;     // replacements that reused a spare's buffers and descriptor set
    std::uint64_t keys_refused_ = 0;      // keys the shader's eight probes can never resolve
    bool last_flush_replaced_ = false;
    bool last_flush_degraded_ = false;
    std::array<VkDescriptorPoolSize, bindless::bindings.size()> pool_sizes_{};
    std::uint32_t pool_size_count_ = 0;
    // Every descriptor written to the set (the table's binding aside), to fill the next generation's set.
    std::array<std::vector<VkDescriptorImageInfo>, bindless::bindings.size()> shadow_;
    std::array<std::vector<std::uint8_t>, bindless::bindings.size()> shadow_written_;
};

} // namespace palmetto
