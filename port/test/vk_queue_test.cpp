// D3D12 copy and compute queues on their own Vulkan queues, with cross-queue fences. No shaders or game data.
//
// The device gets a queue per D3D12 type (dedicated families when the GPU has them). A copy queue uploads a buffer
// and a texture, the way asset streaming does, signals a fence, and the direct queue waits on that fence on the GPU
// before reading both back. The compute queue does the same handoff the other way. A copy queue must refuse a
// render-target clear. Set PALMETTO_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation; any validation error
// fails the test.
#include "vk_test_device.h"
#include "../src/vulkan_backend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using namespace palmetto;
using Kind = VulkanQueue::QueueKind;

// Counts the command buffers a queue allocates, for the backlog check in main().
PFN_vkAllocateCommandBuffers real_allocate_command_buffers = nullptr;
std::uint32_t allocated_command_buffers = 0;

VKAPI_ATTR VkResult VKAPI_CALL counting_allocate_command_buffers(VkDevice device, const VkCommandBufferAllocateInfo* info,
                                                                 VkCommandBuffer* buffers) {
    allocated_command_buffers += info->commandBufferCount;
    return real_allocate_command_buffers(device, info, buffers);
}

// Makes the queue's first kStalls waits for room time out at once, as on a GPU that stopped making progress.
// While hold_progress is set, timeline counter queries report 0 so a fast GPU cannot retire command buffers
// before the loop reaches kMaxQueuedCommandBuffers; once kStalls timeouts have been injected, real progress
// resumes and the wait completes normally.
constexpr std::uint32_t kStalls = 3;
PFN_vkGetSemaphoreCounterValue real_get_semaphore_counter_value = nullptr;
PFN_vkWaitSemaphores real_wait_semaphores = nullptr;
std::atomic<std::uint32_t> injected_stalls{0};
std::atomic<bool> hold_progress{false};

VKAPI_ATTR VkResult VKAPI_CALL holding_semaphore_counter(VkDevice device, VkSemaphore semaphore,
                                                         std::uint64_t* value) {
    if (hold_progress.load()) {
        *value = 0;
        return VK_SUCCESS;
    }
    return real_get_semaphore_counter_value(device, semaphore, value);
}

VKAPI_ATTR VkResult VKAPI_CALL stalling_wait_semaphores(VkDevice device, const VkSemaphoreWaitInfo* info,
                                                        std::uint64_t timeout) {
    if (timeout == VulkanQueue::kQueuedWaitNs) {
        if (injected_stalls.load() < kStalls) {
            ++injected_stalls;
            return VK_TIMEOUT;
        }
        hold_progress.store(false);
    }
    return real_wait_semaphores(device, info, timeout);
}

const char* flags_text(VkQueueFlags f) {
    static char buf[32];
    std::snprintf(buf, sizeof buf, "%s%s%s", f & VK_QUEUE_GRAPHICS_BIT ? "G" : "", f & VK_QUEUE_COMPUTE_BIT ? "C" : "",
                  f & VK_QUEUE_TRANSFER_BIT ? "T" : "");
    return buf;
}

int check_picker() {
    // Synthetic family layouts: the picker's choices don't depend on the GPU running the test.
    auto fam = [](VkQueueFlags flags, std::uint32_t granularity = 1) {
        VkQueueFamilyProperties p{};
        p.queueFlags = flags;
        p.queueCount = 1;
        p.minImageTransferGranularity = {granularity, granularity, granularity};
        return p;
    };
    constexpr VkQueueFlags G = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    constexpr VkQueueFlags C = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    constexpr VkQueueFlags T = VK_QUEUE_TRANSFER_BIT;
    {
        const std::array f{fam(G), fam(T), fam(C)};  // the common NVIDIA layout
        const auto q = pick_queue_families(f, 0);
        EXPECT(q.direct == 0 && q.compute == 2 && q.copy == 1);
    }
    {
        const std::array f{fam(G)};  // one family for everything (integrated GPUs, llvmpipe)
        const auto q = pick_queue_families(f, 0);
        EXPECT(q.direct == 0 && q.compute == 0 && q.copy == 0);
    }
    {
        const std::array f{fam(G), fam(C)};  // no DMA family: copies go to async compute
        const auto q = pick_queue_families(f, 0);
        EXPECT(q.compute == 1 && q.copy == 1);
    }
    {
        const std::array f{fam(G), fam(T, 8), fam(C)};  // a coarse DMA family is skipped
        const auto q = pick_queue_families(f, 0);
        EXPECT(q.copy == 2);
    }
    // D3D12 tile mapping is legal on all queue kinds. Dedicated async queues that cannot
    // bind sparse pages must use the graphics family when sparse buffers are offered.
    {
        constexpr VkQueueFlags S = VK_QUEUE_SPARSE_BINDING_BIT;
        const std::array f{fam(G | S), fam(T), fam(C)};
        const auto preferred = pick_queue_families(f, 0);
        EXPECT(preferred.compute == 2 && preferred.copy == 1);
        const auto corrected = sparse_capable_queue_families(f, preferred, true);
        EXPECT(corrected.direct == 0 && corrected.compute == 0 && corrected.copy == 0);
        const auto no_sparse = sparse_capable_queue_families(f, preferred, false);
        EXPECT(no_sparse.compute == 2 && no_sparse.copy == 1);
    }
    {
        constexpr VkQueueFlags S = VK_QUEUE_SPARSE_BINDING_BIT;
        const std::array f{fam(G | S), fam(T | S), fam(C | S)};
        const auto preferred = pick_queue_families(f, 0);
        const auto corrected = sparse_capable_queue_families(f, preferred, true);
        EXPECT(corrected.compute == 2 && corrected.copy == 1);  // preserve dedicated queues
    }
    {
        constexpr VkQueueFlags S = VK_QUEUE_SPARSE_BINDING_BIT;
        const std::array f{fam(G | S), fam(T), fam(C | S)};
        const auto preferred = pick_queue_families(f, 0);
        const auto corrected = sparse_capable_queue_families(f, preferred, true);
        EXPECT(corrected.compute == 2 && corrected.copy == 0);  // mixed capabilities
    }
    std::printf("queue family picker: 7 layouts ok\n");
    return 0;
}

// Which VkQueue each new D3D12 queue leases (CreateCommandQueue): an unused one while there is one, its own family's
// first, then the one with the fewest D3D12 queues. `own` and `graphics` count the D3D12 queues on each VkQueue.
int check_queue_slots() {
    const auto lease = [](std::vector<std::uint32_t>& own, std::vector<std::uint32_t>& graphics) {
        const auto slot = pick_queue_slot({own, graphics});
        if (slot.pool != SIZE_MAX) ++(slot.pool == 0 ? own : graphics)[slot.index];
        return slot;
    };
    {   // Enough queues (an RTX 5080: 8 compute, 16 graphics): every COMPUTE queue gets a VkQueue of its own.
        std::vector<std::uint32_t> compute(8, 0), graphics(16, 0);
        for (std::size_t i = 0; i < 8; ++i) {
            const auto slot = lease(compute, graphics);
            EXPECT(slot.pool == 0 && slot.index == i);
        }
        const auto ninth = lease(compute, graphics);
        EXPECT(ninth.pool == 1 && ninth.index == 0);  // then an unused graphics queue, not a shared compute one
    }
    {   // One VkQueue per family (PALMETTO_VK_MAX_QUEUES_PER_FAMILY=1): DIRECT, COMPUTE, then they share.
        std::vector<std::uint32_t> none, compute(1, 0), graphics(1, 0);
        EXPECT(lease(none, graphics).pool == 1);
        EXPECT(lease(compute, graphics).pool == 0);
        const auto second_compute = lease(compute, graphics);
        EXPECT(second_compute.pool == 0 && compute[0] == 2);  // a tie goes to the type's own family
        const auto third_compute = lease(compute, graphics);
        EXPECT(third_compute.pool == 1 && graphics[0] == 2);  // then the queue with fewer D3D12 queues
        --compute[0];
        --compute[0];  // two COMPUTE queues released
        EXPECT(lease(compute, graphics).pool == 0);
    }
    {   // A queue released by its last D3D12 queue is unused again and preferred to sharing.
        std::vector<std::uint32_t> none, graphics{3, 0, 2};
        const auto slot = lease(none, graphics);
        EXPECT(slot.pool == 1 && slot.index == 1);
    }
    {   // No VkQueue at all.
        std::vector<std::uint32_t> none;
        EXPECT(pick_queue_slot({none, none}).pool == SIZE_MAX);
    }
    std::printf("queue slots: distinct while free, shared by fewest users after\n");
    return 0;
}

constexpr std::uint32_t kSize = 64 * 1024;
constexpr std::uint32_t kTex = 64;  // 64x64 RGBA8: row pitch 256, already aligned

std::uint8_t pattern(std::uint32_t i, std::uint8_t seed) { return std::uint8_t(i * 31 + seed); }

}  // namespace

int main() {
    if (check_picker() || check_queue_slots()) return 1;

    vktest::Device d;
    d.all_queue_types = true;
    if (!d.create()) {
        std::printf("SKIP: no usable Vulkan device\n");
        return 77;
    }
    std::printf("families: direct %u (%s)", d.family, flags_text(d.families[d.family].queueFlags));
    std::printf(", compute %u (%s)", d.compute_family, flags_text(d.families[d.compute_family].queueFlags));
    std::printf(", copy %u (%s)\n", d.copy_family, flags_text(d.families[d.copy_family].queueFlags));

    VulkanContext context(d.device_proc, d.device, true);
    EXPECT(context.loaded());
    VulkanQueue direct(context, d.queue, d.family, Kind::direct);
    VulkanQueue compute(context, d.compute_queue, d.compute_family, Kind::compute);
    VulkanQueue copy(context, d.copy_queue, d.copy_family, Kind::copy);
    EXPECT(direct.ok() && compute.ok() && copy.ok());

    // Buffers: an upload source, a GPU-side destination and a readback, all host-visible for the test.
    void *upload_map = nullptr, *mid_map = nullptr, *readback_map = nullptr, *tex_readback_map = nullptr;
    void* stale_src_map = nullptr;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer upload = d.buffer(kSize, usage, &upload_map);
    VkBuffer mid = d.buffer(kSize, usage, &mid_map);
    VkBuffer readback = d.buffer(kSize, usage, &readback_map);
    VkBuffer tex_readback = d.buffer(kTex * kTex * 4, usage, &tex_readback_map);
    VkBuffer stale_src = d.buffer(kSize, usage, &stale_src_map);
    VkImage texture = d.image(VK_FORMAT_R8G8B8A8_UNORM, kTex, kTex, 1, VK_IMAGE_USAGE_SAMPLED_BIT);
    EXPECT(upload && mid && readback && tex_readback && stale_src && texture);
    auto* up = static_cast<std::uint8_t*>(upload_map);
    for (std::uint32_t i = 0; i < kSize; ++i) up[i] = pattern(i, 7);

    const ResourceId upload_id = 1, mid_id = 2, readback_id = 3, tex_id = 4, tex_readback_id = 5, stale_src_id = 7;
    auto add_buffer = [&](ResourceId id, VkBuffer b, VkDeviceSize size) {
        ResourceBinding r;
        r.buffer = b;
        r.size = size;
        context.add_resource(id, r);
    };
    add_buffer(upload_id, upload, kSize);
    add_buffer(mid_id, mid, kSize);
    add_buffer(readback_id, readback, kSize);
    add_buffer(tex_readback_id, tex_readback, kTex * kTex * 4);
    add_buffer(stale_src_id, stale_src, kSize);
    ResourceBinding tex;
    tex.image = texture;
    tex.format = VK_FORMAT_R8G8B8A8_UNORM;
    tex.width = kTex;
    tex.height = kTex;
    // As the device registers a texture: tracked, still undefined, in D3D12's common state. Its first use is the
    // copy-queue upload, which can't zero-fill it first.
    context.add_resource(tex_id, tex, VK_IMAGE_LAYOUT_UNDEFINED);

    VulkanFence fence(context, 0);
    EXPECT(fence.semaphore());

    // 1. Copy queue: buffer upload and a texture upload from the same staging buffer (footprint copy), then signal.
    CommandList on_copy;
    EXPECT(on_copy.copy_buffer({mid_id, 0, upload_id, 0, kSize}));
    CopyTexture upload_tex{};
    upload_tex.dst = {tex_id, false, 0, 0, 0, 0, 0, 0};
    upload_tex.src = {upload_id, true, 0, 0, kTex, kTex, 1, kTex * 4};
    EXPECT(on_copy.copy_texture(upload_tex));
    EXPECT(on_copy.close());
    const CommandList* copy_lists[] = {&on_copy};
    EXPECT(copy.execute(copy_lists) == ReplayResult::success);
    EXPECT(copy.signal(fence.semaphore(), 1));

    // 2. Direct queue: wait for the copy queue on the GPU, then read both back.
    direct.wait(fence.semaphore(), 1);
    CommandList on_direct;
    EXPECT(on_direct.copy_buffer({readback_id, 0, mid_id, 0, kSize}));
    CopyTexture read_tex{};
    read_tex.dst = {tex_readback_id, true, 0, 0, kTex, kTex, 1, kTex * 4};
    read_tex.src = {tex_id, false, 0, 0, 0, 0, 0, 0};
    EXPECT(on_direct.copy_texture(read_tex));
    EXPECT(on_direct.close());
    const CommandList* direct_lists[] = {&on_direct};
    EXPECT(direct.execute(direct_lists) == ReplayResult::success);
    EXPECT(direct.signal(fence.semaphore(), 2));
    EXPECT(fence.wait(2, 5'000'000'000ull));

    std::uint32_t bad = 0;
    auto* rb = static_cast<std::uint8_t*>(readback_map);
    for (std::uint32_t i = 0; i < kSize; ++i) bad += rb[i] != pattern(i, 7);
    auto* tr = static_cast<std::uint8_t*>(tex_readback_map);
    for (std::uint32_t i = 0; i < kTex * kTex * 4; ++i) bad += tr[i] != pattern(i, 7);
    std::printf("copy -> direct: buffer and texture upload read back, %u bad bytes\n", bad);
    EXPECT(bad == 0);

    // 3. Compute queue the other way: wait for the direct queue, copy, signal; the host waits on the fence.
    for (std::uint32_t i = 0; i < kSize; ++i) up[i] = pattern(i, 99);
    CommandList on_direct2;
    EXPECT(on_direct2.copy_buffer({mid_id, 0, upload_id, 0, kSize}));
    EXPECT(on_direct2.close());
    const CommandList* direct_lists2[] = {&on_direct2};
    EXPECT(direct.execute(direct_lists2) == ReplayResult::success);
    EXPECT(direct.signal(fence.semaphore(), 3));
    compute.wait(fence.semaphore(), 3);
    CommandList on_compute;
    EXPECT(on_compute.copy_buffer({readback_id, 0, mid_id, 0, kSize}));
    EXPECT(on_compute.close());
    const CommandList* compute_lists[] = {&on_compute};
    EXPECT(compute.execute(compute_lists) == ReplayResult::success);
    EXPECT(compute.signal(fence.semaphore(), 4));
    EXPECT(fence.wait(4, 5'000'000'000ull));
    bad = 0;
    for (std::uint32_t i = 0; i < kSize; ++i) bad += rb[i] != pattern(i, 99);
    std::printf("direct -> compute: %u bad bytes\n", bad);
    EXPECT(bad == 0);

    // 3b. A smaller ExecuteCommandLists must keep the prepared storage but must not replay stale entries.
    // Eight lists, each copying a non-overlapping chunk from a source that stays at 0xaa, grow prepared_ to eight.
    // A one-list submission then copies only its own chunk from the upload source, now 0xbb. With resize() down and
    // range-for over all entries, the seven stale copies run after that one and put 0xaa back into mid.
    constexpr std::uint32_t kChunk = kSize / 8;
    auto* old = static_cast<std::uint8_t*>(stale_src_map);
    std::memset(old, 0xaa, kSize);
    CommandList stale[8];
    const CommandList* many[8];
    for (std::uint32_t i = 0; i < 8; ++i) {
        EXPECT(stale[i].copy_buffer({mid_id, i * kChunk, stale_src_id, i * kChunk, kChunk}));
        EXPECT(stale[i].close());
        many[i] = &stale[i];
    }
    EXPECT(direct.execute(many) == ReplayResult::success);
    EXPECT(direct.signal(fence.semaphore(), 10));
    EXPECT(fence.wait(10, 5'000'000'000ull));
    auto* md = static_cast<std::uint8_t*>(mid_map);
    std::memset(md, 0, kSize);
    for (std::uint32_t i = 0; i < kSize; ++i) up[i] = 0xbb;
    CommandList final_copy;
    EXPECT(final_copy.copy_buffer({mid_id, 7 * kChunk, upload_id, 7 * kChunk, kChunk}));
    EXPECT(final_copy.close());
    const CommandList* one[1] = {&final_copy};
    EXPECT(direct.execute(one) == ReplayResult::success);
    EXPECT(direct.signal(fence.semaphore(), 11));
    EXPECT(fence.wait(11, 5'000'000'000ull));
    EXPECT(direct.prepared_entries() == 8);
    bad = 0;
    for (std::uint32_t i = 0; i < kSize; ++i)
        bad += md[i] != (i >= 7 * kChunk ? 0xbb : 0x00);
    std::printf("prepared storage: %zu entries kept, %u stale bytes\n", direct.prepared_entries(), bad);
    EXPECT(bad == 0);

    // 4. A copy queue refuses a render-target clear and submits nothing.
    VkImage rt_image = d.image(VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ResourceBinding rt;
    rt.image = rt_image;
    rt.format = VK_FORMAT_R8G8B8A8_UNORM;
    rt.width = rt.height = 8;
    context.add_resource(6, rt, VK_IMAGE_LAYOUT_UNDEFINED);
    CommandList clear;
    TargetView view{};
    view.resource = 6;
    view.layer_count = 1;
    EXPECT(clear.clear_render_target(view, ClearColor{}));
    EXPECT(clear.close());
    const CommandList* clear_lists[] = {&clear};
    EXPECT(copy.execute(clear_lists) == ReplayResult::unsupported);
    EXPECT(compute.execute(clear_lists) == ReplayResult::unsupported);
    std::printf("copy and compute queues refuse a render-target clear\n");

    // 5. A caller that submits faster than the GPU finishes must not grow the queue's command buffers without bound:
    // at the cap it waits for the oldest one. Big host-visible copies keep the GPU behind the submitting loop. The first
    // waits time out (injected), as on a stalled GPU: the cap must hold through a timeout too.
    {
        constexpr VkDeviceSize kBig = 16u << 20;
        constexpr std::uint32_t kSubmits = 4 * VulkanQueue::kMaxQueuedCommandBuffers;
        void *src_map = nullptr, *dst_map = nullptr;
        VkBuffer big_src = d.buffer(kBig, usage, &src_map);
        VkBuffer big_dst = d.buffer(kBig, usage, &dst_map);
        EXPECT(big_src && big_dst);
        add_buffer(7, big_src, kBig);
        add_buffer(8, big_dst, kBig);
        std::memset(src_map, 0x5A, kBig);
        CommandList big_copy;
        EXPECT(big_copy.copy_buffer({8, 0, 7, 0, kBig}));
        EXPECT(big_copy.close());
        const CommandList* big_lists[] = {&big_copy};

        real_allocate_command_buffers = context.vk.vkAllocateCommandBuffers;
        real_get_semaphore_counter_value = context.vk.vkGetSemaphoreCounterValue;
        real_wait_semaphores = context.vk.vkWaitSemaphores;
        context.vk.vkAllocateCommandBuffers = counting_allocate_command_buffers;
        context.vk.vkGetSemaphoreCounterValue = holding_semaphore_counter;
        context.vk.vkWaitSemaphores = stalling_wait_semaphores;
        allocated_command_buffers = 0;
        injected_stalls.store(0);
        hold_progress.store(true);
        for (std::uint32_t i = 0; i < kSubmits; ++i) EXPECT(direct.execute(big_lists) == ReplayResult::success);
        hold_progress.store(false);
        context.vk.vkAllocateCommandBuffers = real_allocate_command_buffers;
        context.vk.vkGetSemaphoreCounterValue = real_get_semaphore_counter_value;
        context.vk.vkWaitSemaphores = real_wait_semaphores;
        EXPECT(direct.signal(fence.semaphore(), 5));
        EXPECT(fence.wait(5, 20'000'000'000ull));
        std::printf("backlog: %u submits, %u command buffers allocated (cap %zu), %u waits timed out\n", kSubmits,
                    allocated_command_buffers, VulkanQueue::kMaxQueuedCommandBuffers, injected_stalls.load());
        EXPECT(injected_stalls.load() == kStalls);  // the queue did reach the cap and wait
        EXPECT(allocated_command_buffers <= VulkanQueue::kMaxQueuedCommandBuffers);
        EXPECT(static_cast<std::uint8_t*>(dst_map)[kBig - 1] == 0x5A);
    }

    // 6. The non-blocking pipeline hold (docs/peer_port_streaming.md). The deferred-compile hooks are synthetic: a
    // batch is "held" when the test's map says one of its lists names a pipeline id the test has not marked finished.
    // What is measured is the queue's own contract, on a real device: a submission that names an unfinished pipeline
    // returns without a synchronous wait, held batches replay in submission order, the bound falls back to the old
    // wait on the submitting thread, a hold past its budget is forced, and a queue destroyed with held work drops it
    // and releases its token.
    {
        EXPECT(!context.pipeline_hold_enabled());  // switch off: every submission above ran the default path
        std::unordered_set<std::uintptr_t> unfinished;
        std::unordered_map<const CommandList*, std::vector<std::uintptr_t>> named;
        std::uint32_t prepare_calls = 0, wake_calls = 0;
        auto collect = [&](std::span<const CommandList* const> lists, std::vector<std::uintptr_t>& out) {
            bool pending = false;
            for (const CommandList* list : lists) {
                const auto it = named.find(list);
                if (it == named.end()) continue;
                for (const std::uintptr_t id : it->second)
                    if (unfinished.count(id)) {
                        out.push_back(id);
                        pending = true;
                    }
            }
            return pending;
        };
        auto finished = [&](std::span<const std::uintptr_t> ids) {
            for (const std::uintptr_t id : ids)
                if (unfinished.count(id)) return false;
            return true;
        };
        context.set_prepare_pipelines([&](std::span<const CommandList* const>) { ++prepare_calls; });
        auto install = [&](std::size_t bound, std::uint64_t budget_us) {
            PipelineHoldSettings settings;
            settings.enabled = true;
            settings.bound = bound;
            settings.budget_us = budget_us;
            context.set_pipeline_hold(collect, finished, [&] { ++wake_calls; }, settings);
        };
        install(8, 5'000'000);
        EXPECT(context.pipeline_hold_enabled());

        // Two different sources into the same destination range: the last release wins, so the destination says
        // whether the batches ran in submission order.
        void *src_a_map = nullptr, *src_b_map = nullptr;
        const VkBuffer src_a = d.buffer(kSize, usage, &src_a_map);
        const VkBuffer src_b = d.buffer(kSize, usage, &src_b_map);
        EXPECT(src_a && src_b);
        add_buffer(20, src_a, kSize);
        add_buffer(21, src_b, kSize);
        std::memset(src_a_map, 0x11, kSize);
        std::memset(src_b_map, 0x22, kSize);
        auto* hold_dst = static_cast<std::uint8_t*>(mid_map);
        std::memset(hold_dst, 0, kSize);
        CommandList held_a, held_b;
        EXPECT(held_a.copy_buffer({mid_id, 0, 20, 0, kSize}));
        EXPECT(held_a.close());
        EXPECT(held_b.copy_buffer({mid_id, 0, 21, 0, kSize}));
        EXPECT(held_b.close());
        const CommandList* held_a_lists[] = {&held_a};
        const CommandList* held_b_lists[] = {&held_b};
        unfinished = {0x100, 0x101};
        named[&held_a] = {0x100};
        named[&held_b] = {0x101};
        EXPECT(direct.execute(held_a_lists) == ReplayResult::success);
        EXPECT(context.holds().any_held());  // held, not submitted
        EXPECT(prepare_calls == 0);          // and this (the submitting) thread never entered the synchronous wait
        EXPECT(direct.execute(held_b_lists) == ReplayResult::success);  // held behind a, in order
        EXPECT(prepare_calls == 0);
        EXPECT(wake_calls >= 2);  // each hold asked the layer's flusher to look
        // b's pipeline finishes first: b is ready but must not overtake a. The test also has to tell the hold policy
        // the id finished, which is what a compile worker's completion does in the real device.
        unfinished.erase(0x101);
        context.pipeline_hold().finished(0x101);
        context.holds().pump_ready();
        EXPECT(context.holds().any_held());
        EXPECT(prepare_calls == 0);
        bool ran_early = false;
        for (std::uint32_t i = 0; i < kSize; ++i) ran_early |= hold_dst[i] != 0;
        EXPECT(!ran_early);
        // a finishes: both replay in order, and the last one (b) wins the destination.
        unfinished.erase(0x100);
        context.pipeline_hold().finished(0x100);
        context.holds().pump_ready();
        EXPECT(!context.holds().any_held());
        EXPECT(prepare_calls >= 2);  // the hook runs when the batches replay, on the releasing thread
        EXPECT(direct.idle());       // the replayed batches reach the GPU; the test reads their result
        EXPECT(hold_dst[0] == 0x22 && hold_dst[kSize - 1] == 0x22);

        // The bound: at one held submission, a second forces the oldest to complete by the old synchronous wait here
        // (the fallback the slice keeps), then is held itself.
        install(1, 5'000'000);
        EXPECT(!context.holds().any_held());
        unfinished = {0x200, 0x201};
        CommandList bound_a, bound_b;
        EXPECT(bound_a.copy_buffer({mid_id, 0, 20, 0, kSize}));
        EXPECT(bound_a.close());
        EXPECT(bound_b.copy_buffer({mid_id, 0, 21, 0, kSize}));
        EXPECT(bound_b.close());
        named[&bound_a] = {0x200};
        named[&bound_b] = {0x201};
        const CommandList* bound_a_lists[] = {&bound_a};
        const CommandList* bound_b_lists[] = {&bound_b};
        std::memset(hold_dst, 0, kSize);
        EXPECT(direct.execute(bound_a_lists) == ReplayResult::success);
        EXPECT(context.pipeline_hold().held() == 1);
        prepare_calls = 0;
        EXPECT(direct.execute(bound_b_lists) == ReplayResult::success);
        EXPECT(prepare_calls >= 1);  // the bound forced the old wait
        EXPECT(context.pipeline_hold().forced() == 1);
        EXPECT(direct.idle());
        EXPECT(hold_dst[0] == 0x11);  // a ran by the old path, in order, before b
        EXPECT(context.pipeline_hold().held() == 1);  // b is still held
        unfinished.erase(0x201);
        context.pipeline_hold().finished(0x201);
        context.holds().pump_ready();
        EXPECT(!context.holds().any_held());
        EXPECT(direct.idle());
        EXPECT(hold_dst[0] == 0x22);

        // The budget: a hold older than its budget is completed by the old wait on the releasing thread.
        install(8, 1000);  // 1 ms
        unfinished = {0x300};
        CommandList late;
        EXPECT(late.copy_buffer({mid_id, 0, 21, 0, kSize}));
        EXPECT(late.close());
        named[&late] = {0x300};
        const CommandList* late_lists[] = {&late};
        std::memset(hold_dst, 0, kSize);
        EXPECT(direct.execute(late_lists) == ReplayResult::success);
        EXPECT(context.holds().any_held());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        prepare_calls = 0;
        context.holds().pump_ready();
        EXPECT(!context.holds().any_held());
        EXPECT(prepare_calls >= 1);
        EXPECT(context.pipeline_hold().timeouts() == 1);
        EXPECT(context.pipeline_hold().forced() == 2);
        EXPECT(direct.idle());
        EXPECT(hold_dst[0] == 0x22);
        unfinished.clear();

        // A queue destroyed with held work drops it and releases its token (the existing held-work teardown).
        install(8, 5'000'000);
        unfinished = {0x400};
        CommandList dropped;
        EXPECT(dropped.copy_buffer({mid_id, 0, 20, 0, 4}));
        EXPECT(dropped.close());
        named[&dropped] = {0x400};
        {
            VulkanQueue short_lived(context, d.queue, d.family, Kind::direct);
            EXPECT(short_lived.ok());
            const CommandList* dropped_lists[] = {&dropped};
            EXPECT(short_lived.execute(dropped_lists) == ReplayResult::success);
            EXPECT(context.pipeline_hold().held() == 1);
        }
        EXPECT(context.pipeline_hold().held() == 0);
        EXPECT(context.pipeline_hold().dropped() >= 1);
        unfinished.clear();

        // Switch off again: the hooks are gone and the same lists run the default path (no hold, no wait).
        context.set_pipeline_hold(nullptr, nullptr, nullptr, PipelineHoldSettings{});
        EXPECT(!context.pipeline_hold_enabled());
        unfinished = {0x500};
        named[&held_a] = {0x500};
        prepare_calls = 0;
        EXPECT(direct.execute(held_a_lists) == ReplayResult::success);
        EXPECT(!context.holds().any_held());
        EXPECT(direct.idle());
        context.set_prepare_pipelines(nullptr);
        std::printf("pipeline hold: ordering, bound fallback, budget force and queue-teardown drop ok\n");
    }

    EXPECT(direct.idle() && compute.idle() && copy.idle());
    if (d.validation) {
        std::printf("validation errors: %u\n", d.validation_errors());
        EXPECT(d.validation_errors() == 0);
    }
    std::printf("vk_queue_test: OK\n");
    return 0;
}
