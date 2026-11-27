// VulkanPresenter synchronization regressions (port/src/vulkan_present.h):
//   1. A failed vkQueueSubmit rolls back LayoutTracker changes (an UNDEFINED back buffer stays UNDEFINED until a
//      present actually submits its zero-fill).
//   2. With enable_capture(), every frame's vkCmdCopyImageToBuffer into the shared capture buffer is preceded by a
//      TRANSFER_WRITE -> TRANSFER_WRITE barrier to prevent WRITE_AFTER_WRITE hazards across the two in-flight frames.
//      A failed capture fence wait also leaves the caller's outputs untouched.
//   3. Concurrent present() + VulkanQueue::execute() + add_resource()/remove_resource() follows VulkanContext's
//      resource_mutex_ -> tracker_mutex_ -> submit_mutex_ lock hierarchy without data races or deadlocks.
//   4. A swapchain-destroy WSI layer which calls vkDeviceWaitIdle cannot overlap vkQueueSubmit on another thread.
//   5. The present fit/scale rule (src/present_fit.h) and the synthetic non-black control (docs/peer_port_first_ui.md
//      section 15): a 16:9 source is stretched over the whole window by default and fitted with black bars when the rule
//      is fit, and the presenter's own known pattern reads back its exact known colours through the same present path
//      and capture, in both rules.
// Skips with exit code 77 when no Vulkan 1.3 device with Win32 presentation is available. No game data.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>

#include "vk_presenter_capture_colour.h"
#include "vk_test_device.h"
#include "../src/vulkan_present.h"
#include "../src/present_fit.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    return DefWindowProcW(window, message, w, l);
}

void pump() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

namespace driver {
PFN_vkGetInstanceProcAddr real_get_instance = nullptr;
PFN_vkGetDeviceProcAddr real_get_device = nullptr;
PFN_vkQueueSubmit real_submit = nullptr;
PFN_vkQueuePresentKHR real_present = nullptr;
PFN_vkWaitForFences real_wait = nullptr;
PFN_vkCmdPipelineBarrier real_barrier = nullptr;
PFN_vkCmdCopyImageToBuffer real_copy_i2b = nullptr;
PFN_vkDestroySwapchainKHR real_destroy_swapchain = nullptr;
PFN_vkDeviceWaitIdle real_device_wait_idle = nullptr;

std::atomic<VkResult> fail_next_submit{VK_SUCCESS};
std::atomic<VkResult> fail_next_wait{VK_SUCCESS};
std::function<void()> on_next_present;
std::atomic<bool> observe_capture{false};
std::atomic<bool> saw_capture_waw{false};
std::atomic<std::uint32_t> capture_copies{0};
std::atomic<std::uint32_t> capture_waw_barriers{0};
std::atomic<bool> check_swapchain_destroy{false};
std::atomic<bool> destroy_entered{false};
std::atomic<bool> submit_entered{false};
std::atomic<bool> overlap_detected{false};
std::atomic<bool> unexpected_submit_error{false};
std::mutex destroy_mutex;
std::condition_variable destroy_cv;
bool allow_destroy_finish = false;

// Synthetic WSI layer: a destroy hook initiates vkDeviceWaitIdle, as a capture layer may do internally.
// Holding the hook behind a condition variable makes the concurrency window deterministic rather than dependent
// on the installed capture software or validation logging overhead. Only the test hook waits on this condition.
VKAPI_ATTR void VKAPI_CALL destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                                              const VkAllocationCallbacks* alloc) {
    if (check_swapchain_destroy.load(std::memory_order_relaxed)) {
        destroy_entered.store(true, std::memory_order_release);
        destroy_cv.notify_all();
        {
            std::unique_lock lock(destroy_mutex);
            destroy_cv.wait(lock, [] { return allow_destroy_finish; });
        }
        overlap_detected.store(submit_entered.load(std::memory_order_acquire), std::memory_order_release);
        if (real_device_wait_idle(device) != VK_SUCCESS) unexpected_submit_error.store(true);
    }
    real_destroy_swapchain(device, swapchain, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL queue_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits,
                                            VkFence fence) {
    submit_entered.store(true, std::memory_order_release);
    const VkResult armed = fail_next_submit.exchange(VK_SUCCESS);
    if (armed != VK_SUCCESS) return armed;
    return real_submit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL queue_present(VkQueue queue, const VkPresentInfoKHR* info) {
    if (on_next_present) {
        auto cb = std::move(on_next_present);
        on_next_present = {};
        cb();
    }
    return real_present(queue, info);
}

VKAPI_ATTR VkResult VKAPI_CALL wait_for_fences(VkDevice device, std::uint32_t count, const VkFence* fences,
                                               VkBool32 wait_all, std::uint64_t timeout) {
    const VkResult armed = fail_next_wait.exchange(VK_SUCCESS);
    if (armed != VK_SUCCESS) return armed;
    return real_wait(device, count, fences, wait_all, timeout);
}

VKAPI_ATTR void VKAPI_CALL cmd_pipeline_barrier(VkCommandBuffer cb, VkPipelineStageFlags src_stage,
                                                VkPipelineStageFlags dst_stage, VkDependencyFlags flags,
                                                std::uint32_t mem_count, const VkMemoryBarrier* mem,
                                                std::uint32_t buf_count, const VkBufferMemoryBarrier* buf,
                                                std::uint32_t img_count, const VkImageMemoryBarrier* img) {
    if (observe_capture.load(std::memory_order_relaxed) && (src_stage & VK_PIPELINE_STAGE_TRANSFER_BIT) &&
        (dst_stage & VK_PIPELINE_STAGE_TRANSFER_BIT)) {
        for (std::uint32_t i = 0; i < buf_count; ++i) {
            if ((buf[i].srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) &&
                (buf[i].dstAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT))
                saw_capture_waw.store(true, std::memory_order_relaxed);
        }
        for (std::uint32_t i = 0; i < mem_count; ++i) {
            if ((mem[i].srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) &&
                (mem[i].dstAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT))
                saw_capture_waw.store(true, std::memory_order_relaxed);
        }
    }
    real_barrier(cb, src_stage, dst_stage, flags, mem_count, mem, buf_count, buf, img_count, img);
}

VKAPI_ATTR void VKAPI_CALL cmd_copy_image_to_buffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout,
                                                    VkBuffer dst, std::uint32_t count,
                                                    const VkBufferImageCopy* regions) {
    if (observe_capture.load(std::memory_order_relaxed)) {
        capture_copies.fetch_add(1, std::memory_order_relaxed);
        if (saw_capture_waw.exchange(false, std::memory_order_relaxed))
            capture_waw_barriers.fetch_add(1, std::memory_order_relaxed);
    }
    real_copy_i2b(cb, src, layout, dst, count, regions);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device(VkDevice device, const char* name) {
    const std::string_view n = name;
    if (n == "vkDestroySwapchainKHR") return reinterpret_cast<PFN_vkVoidFunction>(destroy_swapchain);
    if (n == "vkQueueSubmit") return reinterpret_cast<PFN_vkVoidFunction>(queue_submit);
    if (n == "vkQueuePresentKHR") return reinterpret_cast<PFN_vkVoidFunction>(queue_present);
    if (n == "vkWaitForFences") return reinterpret_cast<PFN_vkVoidFunction>(wait_for_fences);
    if (n == "vkCmdPipelineBarrier") return reinterpret_cast<PFN_vkVoidFunction>(cmd_pipeline_barrier);
    if (n == "vkCmdCopyImageToBuffer") return reinterpret_cast<PFN_vkVoidFunction>(cmd_copy_image_to_buffer);
    return real_get_device(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_instance(VkInstance instance, const char* name) {
    if (std::string_view(name) == "vkGetDeviceProcAddr") return reinterpret_cast<PFN_vkVoidFunction>(get_device);
    return real_get_instance(instance, name);
}

void install(PFN_vkGetInstanceProcAddr instance_proc, VkInstance instance, VkDevice device) {
    real_get_instance = instance_proc;
    real_get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(instance_proc(instance, "vkGetDeviceProcAddr"));
    real_destroy_swapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(real_get_device(device, "vkDestroySwapchainKHR"));
    real_device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(real_get_device(device, "vkDeviceWaitIdle"));
    real_submit = reinterpret_cast<PFN_vkQueueSubmit>(real_get_device(device, "vkQueueSubmit"));
    real_present = reinterpret_cast<PFN_vkQueuePresentKHR>(real_get_device(device, "vkQueuePresentKHR"));
    real_wait = reinterpret_cast<PFN_vkWaitForFences>(real_get_device(device, "vkWaitForFences"));
    real_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(real_get_device(device, "vkCmdPipelineBarrier"));
    real_copy_i2b = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(real_get_device(device, "vkCmdCopyImageToBuffer"));
}
}  // namespace driver

}  // namespace

int main() {
    using namespace palmetto;

    vktest::Device d;
    if (!d.create(true)) {
        std::fprintf(stderr, "vk_presenter_sync_test: no Vulkan device with presentation, skipped\n");
        return 77;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PalmettoPresenterSyncTest";
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    EXPECT(RegisterClassW(&wc));
    constexpr std::uint32_t width = 128, height = 96;
    RECT fr{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&fr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"Palmetto presenter sync test", WS_OVERLAPPEDWINDOW, 80, 80,
                                  fr.right - fr.left, fr.bottom - fr.top, nullptr, nullptr, wc.hInstance, nullptr);
    EXPECT(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    pump();

    driver::install(d.instance_proc, d.instance, d.device);
    {
        VulkanContext context(driver::get_device, d.device, true);
        EXPECT(context.loaded());
        VulkanQueue queue(context, d.queue, d.family);
        VulkanQueue worker_queue(context, d.queue, d.family);
        EXPECT(queue.ok() && worker_queue.ok());

        const VkImage back_img = d.image(VK_FORMAT_R8G8B8A8_UNORM, width, height, 1,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        const VkImage side_img = d.image(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        EXPECT(back_img != VK_NULL_HANDLE && side_img != VK_NULL_HANDLE);

        ResourceBinding back_rb{};
        back_rb.image = back_img;
        back_rb.format = VK_FORMAT_R8G8B8A8_UNORM;
        back_rb.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        back_rb.width = width;
        back_rb.height = height;
        back_rb.mip_levels = 1;
        back_rb.array_layers = 1;
        back_rb.depth = 1;
        back_rb.samples = 1;
        back_rb.color_attachment = true;
        context.add_resource(1, back_rb, VK_IMAGE_LAYOUT_UNDEFINED);

        ResourceBinding side_rb = back_rb;
        side_rb.image = side_img;
        side_rb.width = 32;
        side_rb.height = 32;

        const auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
            d.instance_proc(d.instance, "vkCreateWin32SurfaceKHR"));
        EXPECT(create_surface);
        VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface_info.hinstance = GetModuleHandleW(nullptr);
        surface_info.hwnd = window;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        EXPECT(create_surface(d.instance, &surface_info, nullptr, &surface) == VK_SUCCESS);

        VulkanPresenter presenter(context, driver::get_instance, d.instance, d.physical, d.queue, d.family, surface);
        EXPECT(presenter.ok());
        EXPECT(presenter.enable_capture());

        // 1. Failed vkQueueSubmit must NOT commit the UNDEFINED -> GENERAL transition in LayoutTracker (O(1) rollback
        //    even with many live tracked textures), and tracker_mutex_ must be released before vkQueuePresentKHR.
        for (ResourceId extra = 1000; extra < 1512; ++extra)
            context.add_resource(extra, side_rb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EXPECT(context.layout(1, 0) == VK_IMAGE_LAYOUT_UNDEFINED);
        driver::fail_next_submit.store(VK_ERROR_OUT_OF_HOST_MEMORY);
        EXPECT(presenter.present(1, true) == VK_ERROR_OUT_OF_HOST_MEMORY);
        EXPECT(context.layout(1, 0) == VK_IMAGE_LAYOUT_UNDEFINED);
        EXPECT(context.layout(1000, 0) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Next present succeeds, runs the deferred zero-fill, commits GENERAL, and unlocks tracker_mutex_ before
        // vkQueuePresentKHR so other threads' emit() / layout() calls are not blocked by presentation.
        std::mutex sync_mutex;
        std::condition_variable sync_cv;
        bool reader_started = false;
        bool read_done = false;
        VkImageLayout seen_during_present = VK_IMAGE_LAYOUT_UNDEFINED;
        bool unblocked_during_present = false;
        std::thread reader;
        driver::on_next_present = [&] {
            reader = std::thread([&] {
                {
                    std::lock_guard lk(sync_mutex);
                    reader_started = true;
                }
                sync_cv.notify_all();
                const VkImageLayout layout = context.layout(1, 0);
                {
                    std::lock_guard lk(sync_mutex);
                    seen_during_present = layout;
                    read_done = true;
                }
                sync_cv.notify_all();
            });
            std::unique_lock lk(sync_mutex);
            const bool started = sync_cv.wait_for(lk, std::chrono::seconds(2), [&] { return reader_started; });
            unblocked_during_present =
                started && sync_cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return read_done; });
        };
        const VkResult present_rc = presenter.present(1, true);
        if (reader.joinable()) reader.join();
        EXPECT(present_rc == VK_SUCCESS);
        EXPECT(unblocked_during_present);
        EXPECT(seen_during_present == VK_IMAGE_LAYOUT_GENERAL);
        EXPECT(context.layout(1, 0) == VK_IMAGE_LAYOUT_GENERAL);
        for (ResourceId extra = 1000; extra < 1512; ++extra) context.remove_resource(extra);
        std::vector<std::uint8_t> pixels{0x7f};
        VkExtent2D ext{1, 2};
        VkFormat fmt = VK_FORMAT_R8_UNORM;
        driver::fail_next_wait = VK_ERROR_DEVICE_LOST;
        EXPECT(!presenter.read_back(pixels, &ext, &fmt));
        EXPECT(pixels.size() == 1 && pixels[0] == 0x7f);
        EXPECT(ext.width == 1 && ext.height == 2 && fmt == VK_FORMAT_R8_UNORM);
        EXPECT(presenter.read_back(pixels, &ext, &fmt));
        EXPECT(pixels.size() == std::size_t(ext.width) * ext.height * 4);
        const std::size_t mid = (std::size_t(ext.height / 2) * ext.width + ext.width / 2) * 4;
        EXPECT(pixels[mid + 0] == 0 && pixels[mid + 1] == 0 && pixels[mid + 2] == 0);
        std::printf("presenter sync: failed-submit LayoutTracker rollback ok\n");

        // 2. Capture-buffer WAW barrier before vkCmdCopyImageToBuffer on both present() and present_blank().
        {
            CommandList clear_list;
            EXPECT(clear_list.transition({1, 0, d3d12_state::common, d3d12_state::render_target}));
            EXPECT(clear_list.clear_render_target({1, 0, 0, 1}, {0.2f, 0.6f, 0.9f, 1.0f}));
            EXPECT(clear_list.transition({1, 0, d3d12_state::render_target, d3d12_state::common}));
            EXPECT(clear_list.close());
            const CommandList* ptr = &clear_list;
            EXPECT(queue.execute({&ptr, 1}) == ReplayResult::success);
        }
        driver::saw_capture_waw.store(false);
        driver::capture_copies.store(0);
        driver::capture_waw_barriers.store(0);
        driver::observe_capture.store(true);
        EXPECT(presenter.present(1, true) == VK_SUCCESS);
        EXPECT(presenter.present(1, true) == VK_SUCCESS);
        EXPECT(presenter.present_blank(true) == VK_SUCCESS);
        EXPECT(presenter.present(1, true) == VK_SUCCESS);
        driver::observe_capture.store(false);
        EXPECT(driver::capture_copies.load() == 4);
        EXPECT(driver::capture_waw_barriers.load() == 4);

        EXPECT(presenter.read_back(pixels, &ext, &fmt));
        const bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
        EXPECT(presenter_capture_matches_clear(pixels, ext.width, ext.height, bgra));
        std::printf("presenter sync: capture-buffer WAW barriers (4/4) and full-frame read_back ok\n");

        // 2b. A Present behind held work shows what that work draws (queue_holds.h). The render waits for a gate with
        //     no submitted Signal, so the queue holds it; the presents are queued behind it instead of blitting the
        //     back buffers as they were. Three presents queue up and keep their order: a blit, a blank one (reads
        //     nothing) and a composed two-plane origin pair (present_planes), whose frame is the last one shown, so a reordering
        //     would end on the black frame.
        {
            VulkanFence gate(context, 0);
            EXPECT(gate.semaphore() != VK_NULL_HANDLE);
            const VkImage plane_img = d.image(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1,
                                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            const VkImage second_plane_img = d.image(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1,
                                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            EXPECT(plane_img != VK_NULL_HANDLE && second_plane_img != VK_NULL_HANDLE);
            ResourceBinding plane_rb = side_rb;
            plane_rb.image = plane_img;
            plane_rb.sampled = true;
            context.add_resource(2, plane_rb, VK_IMAGE_LAYOUT_UNDEFINED);
            ResourceBinding second_plane_rb = side_rb;
            second_plane_rb.image = second_plane_img;
            second_plane_rb.sampled = true;
            context.add_resource(4, second_plane_rb, VK_IMAGE_LAYOUT_UNDEFINED);
            CommandList red;
            EXPECT(red.transition({1, 0, d3d12_state::common, d3d12_state::render_target}));
            EXPECT(red.clear_render_target({1, 0, 0, 1}, {0.9f, 0.1f, 0.2f, 1.0f}));
            EXPECT(red.transition({1, 0, d3d12_state::render_target, d3d12_state::common}));
            EXPECT(red.transition({2, 0, d3d12_state::common, d3d12_state::render_target}));
            EXPECT(red.clear_render_target({2, 0, 0, 1}, {0.2f, 0.8f, 0.4f, 1.0f}));
            EXPECT(red.transition({2, 0, d3d12_state::render_target, d3d12_state::pixel_shader_resource}));
            EXPECT(red.transition({4, 0, d3d12_state::common, d3d12_state::render_target}));
            EXPECT(red.clear_render_target({4, 0, 0, 1}, {0.1f, 0.4f, 0.9f, 1.0f}));
            EXPECT(red.transition({4, 0, d3d12_state::render_target, d3d12_state::pixel_shader_resource}));
            EXPECT(red.close());
            const CommandList* ptr = &red;
            queue.wait(gate.semaphore(), 1);
            EXPECT(queue.execute({&ptr, 1}) == ReplayResult::success);   // held
            const auto capture_matches = [&](const std::array<int, 3>& rgb, int tolerance) {
                std::vector<std::uint8_t> shot;
                VkExtent2D e{};
                VkFormat f{};
                if (!presenter.read_back(shot, &e, &f)) return false;
                const VkExtent2D current = presenter.extent();
                if (e.width != current.width || e.height != current.height) return false;
                const bool bgr = (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB);
                return presenter_capture_matches_rgb(shot, e.width, e.height, bgr, rgb, tolerance);
            };
            const std::uint64_t shown = presenter.presented();
            PresentPlanes planes;
            planes.count = 2;
            planes.plane[0].source = 2;
            planes.plane[1].source = 4;
            EXPECT(presenter.present(1, true) == VK_SUCCESS);            // queued: returns at once
            EXPECT(presenter.present_blank(true) == VK_SUCCESS);         // queued behind the one before it
            EXPECT(presenter.present_planes(planes, true) == VK_SUCCESS);
            EXPECT(presenter.presented() == shown && !presenter.last_present_tore());
            EXPECT(capture_matches({51, 153, 230}, 2));
            EXPECT(gate.signal(1));   // releases the render, then the presents, in order, on this thread
            EXPECT(queue.idle());
            EXPECT(presenter.presented() == shown + 3 && presenter.composed_presents() >= 1);
            // The distinct second plane (0.1, 0.4, 0.9) is opaque and covers plane 0; this composition is last, not black.
            EXPECT(capture_matches({26, 102, 230}, 12));
            // The blit alone, with nothing held, is the plain path again and shows the red the render drew.
            EXPECT(presenter.present(1, true) == VK_SUCCESS);
            EXPECT(capture_matches({230, 26, 51}, 2));
            std::printf("presenter sync: multi-plane Present queued behind held work ran in order\n");
            context.remove_resource(2);
            context.remove_resource(4);
        }

        // 2c. A queue destroyed with held work lets the Presents queued behind it run (the work is dropped).
        {
            VulkanFence never(context, 0);
            EXPECT(never.semaphore() != VK_NULL_HANDLE);
            CommandList noop;
            EXPECT(noop.transition({1, 0, d3d12_state::common, d3d12_state::render_target}));
            EXPECT(noop.clear_render_target({1, 0, 0, 1}, {0.0f, 0.0f, 0.0f, 1.0f}));  // never replayed
            EXPECT(noop.transition({1, 0, d3d12_state::render_target, d3d12_state::common}));
            EXPECT(noop.close());
            const CommandList* ptr = &noop;
            const std::uint64_t shown = presenter.presented();
            {
                VulkanQueue doomed(context, d.queue, d.family);
                EXPECT(doomed.ok());
                doomed.wait(never.semaphore(), 1);
                EXPECT(doomed.execute({&ptr, 1}) == ReplayResult::success);   // held for good
                EXPECT(presenter.present(1, true) == VK_SUCCESS);             // queued behind it
                EXPECT(presenter.presented() == shown);
            }
            EXPECT(presenter.presented() == shown + 1);
            std::printf("presenter sync: Present queued behind a destroyed queue's held work ran\n");
        }

        // 2d. The fit/scale rule and the synthetic non-black control (docs/peer_port_first_ui.md section 15). The source
        //     here is a 16:9 texture cleared to one colour, so under the tree's default rule the whole 128x96 window is
        //     that colour; under the fit rule the picture sits in the centred aspect rectangle and the rest of the window
        //     is black. The synthetic control then presents the presenter's own known four-band pattern in place of the
        //     title's plane through this same present path and capture, and its known colours are read back.
        {
            EXPECT(queue.idle());  // the section-2c holds are drained: a present below runs on this thread, not queued
            constexpr std::uint32_t source_width = 32, source_height = 18;  // 16:9
            const VkImage target_img = d.image(VK_FORMAT_R8G8B8A8_UNORM, source_width, source_height, 1,
                                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            EXPECT(target_img != VK_NULL_HANDLE);
            ResourceBinding target_rb = back_rb;
            target_rb.image = target_img;
            target_rb.width = source_width;
            target_rb.height = source_height;
            context.add_resource(3, target_rb, VK_IMAGE_LAYOUT_UNDEFINED);
            {
                CommandList paint;
                EXPECT(paint.transition({3, 0, d3d12_state::common, d3d12_state::render_target}));
                EXPECT(paint.clear_render_target({3, 0, 0, 1}, {0.2f, 0.6f, 0.9f, 1.0f}));  // the capture helper's clear
                EXPECT(paint.transition({3, 0, d3d12_state::render_target, d3d12_state::common}));
                EXPECT(paint.close());
                const CommandList* ptr = &paint;
                EXPECT(queue.execute({&ptr, 1}) == ReplayResult::success);
            }
            const auto shot = [&](std::vector<std::uint8_t>& pixels, VkExtent2D& extent, bool& bgra) {
                VkFormat format = VK_FORMAT_UNDEFINED;
                if (!presenter.read_back(pixels, &extent, &format)) return false;
                bgra = (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB);
                return true;
            };
            const auto pixel = [](const std::vector<std::uint8_t>& pixels, const VkExtent2D& extent, bool bgra, int x,
                                  int y) {
                const std::size_t at = (std::size_t(y) * extent.width + std::size_t(x)) * 4;
                return std::array<int, 3>{pixels[at + (bgra ? 2 : 0)], pixels[at + 1], pixels[at + (bgra ? 0 : 2)]};
            };
            const auto same = [](const std::array<int, 3>& a, const std::array<int, 3>& b, int tolerance) {
                for (int i = 0; i < 3; ++i)
                    if (std::abs(a[i] - b[i]) > tolerance) return false;
                return true;
            };
            const std::array<int, 3> picture_colour{51, 153, 230};
            const std::array<int, 3> black{0, 0, 0};
            std::vector<std::uint8_t> pixels;
            VkExtent2D extent{};
            bool bgra = false;

            // fill (the tree's measured rule): the whole window is the picture, no bars.
            presenter.set_fit_rule(present_fit::Rule::fill);
            EXPECT(presenter.present(3, true) == VK_SUCCESS);
            EXPECT(shot(pixels, extent, bgra));
            const auto fill_rect = present_fit::scale_rect(source_width, source_height, extent.width, extent.height,
                                                           present_fit::Rule::fill);
            EXPECT(fill_rect.left == 0 && fill_rect.top == 0 && fill_rect.right == std::int32_t(extent.width) &&
                   fill_rect.bottom == std::int32_t(extent.height));
            EXPECT(presenter_capture_matches_rgb(pixels, extent.width, extent.height, bgra, picture_colour, 2));

            // fit: the 16:9 picture is width-limited in the 128x96 window, so the bars are above and below it.
            presenter.set_fit_rule(present_fit::Rule::fit);
            EXPECT(presenter.present(3, true) == VK_SUCCESS);
            EXPECT(shot(pixels, extent, bgra));
            const auto fit_rect = present_fit::scale_rect(source_width, source_height, extent.width, extent.height,
                                                          present_fit::Rule::fit);
            EXPECT(fit_rect.left == 0 && fit_rect.right == std::int32_t(extent.width));
            EXPECT(fit_rect.top > 0 && fit_rect.bottom < std::int32_t(extent.height));  // bars exist
            const int middle_x = int(extent.width / 2);
            EXPECT(same(pixel(pixels, extent, bgra, middle_x, fit_rect.top + 2), picture_colour, 2));
            EXPECT(same(pixel(pixels, extent, bgra, middle_x, (fit_rect.bottom - fit_rect.top) / 2 + fit_rect.top),
                        picture_colour, 2));
            EXPECT(same(pixel(pixels, extent, bgra, middle_x, fit_rect.top / 2), black, 2));
            EXPECT(same(pixel(pixels, extent, bgra, middle_x, (fit_rect.bottom + std::int32_t(extent.height)) / 2), black,
                        2));
            std::printf("presenter sync: fill stretches and fit letterboxes the same 16:9 source ok\n");

            // The synthetic control: the presenter's own 16:9 four-band pattern, first stretched over the window.
            const std::uint32_t layout_before = std::uint32_t(context.layout(1, 0));
            EXPECT(!presenter.synthetic_ready());
            presenter.set_fit_rule(present_fit::Rule::fill);
            presenter.set_synthetic_presents(1);
            EXPECT(presenter.present(1, true) == VK_SUCCESS);
            EXPECT(presenter.synthetic_ready());
            EXPECT(presenter.synthetic_presents_left() == 0);
            EXPECT(std::uint32_t(context.layout(1, 0)) == layout_before);  // the title's plane is not read or re-laid-out
            EXPECT(shot(pixels, extent, bgra));
            for (std::uint32_t band = 0; band < 4; ++band) {
                const int x = int((band * extent.width) / 4 + extent.width / 8);
                const std::array<int, 3> expected{VulkanPresenter::synthetic_bands[band][0],
                                                  VulkanPresenter::synthetic_bands[band][1],
                                                  VulkanPresenter::synthetic_bands[band][2]};
                EXPECT(same(pixel(pixels, extent, bgra, x, int(extent.height / 2)), expected, 2));
            }
            std::printf("presenter sync: synthetic known pattern presented through present() and captured ok\n");

            // The same pattern under the fit rule: the bands inside the aspect rectangle, black bars outside it.
            presenter.set_fit_rule(present_fit::Rule::fit);
            presenter.set_synthetic_presents(1);
            EXPECT(presenter.present(1, true) == VK_SUCCESS);
            EXPECT(shot(pixels, extent, bgra));
            const auto synthetic_rect =
                present_fit::scale_rect(int(VulkanPresenter::synthetic_width),
                                        int(VulkanPresenter::synthetic_height), extent.width, extent.height,
                                        present_fit::Rule::fit);
            EXPECT(present_fit::has_bars(synthetic_rect, std::int32_t(extent.width), std::int32_t(extent.height)));
            for (std::uint32_t band = 0; band < 4; ++band) {
                const int x = synthetic_rect.left +
                              int((band * (synthetic_rect.right - synthetic_rect.left)) / 4 +
                                  (synthetic_rect.right - synthetic_rect.left) / 8);
                const std::array<int, 3> expected{VulkanPresenter::synthetic_bands[band][0],
                                                  VulkanPresenter::synthetic_bands[band][1],
                                                  VulkanPresenter::synthetic_bands[band][2]};
                EXPECT(same(pixel(pixels, extent, bgra, x, (synthetic_rect.top + synthetic_rect.bottom) / 2), expected,
                            2));
            }
            EXPECT(same(pixel(pixels, extent, bgra, int(extent.width / 2), synthetic_rect.top / 2), black, 2));
            presenter.set_fit_rule(present_fit::Rule::fill);
            presenter.set_synthetic_presents(0);
            context.remove_resource(3);
            std::printf("presenter sync: synthetic pattern fitted with black bars through the same capture ok\n");
        }

        // 2e. Readiness-only kind-7 seam: a synthetic RGB9E5 sampled image with the measured source extent is accepted
        // by the existing PresentX compose pass and stretched over its output. This proves only Vulkan format sampling,
        // whole-source rect plumbing and the generic compositor; it does not claim that the title's FFX output reaches
        // this presenter, nor does the SDR-range synthetic texel establish the title's transfer function.
        {
            using namespace upscale_kind7;
            constexpr VkFormat source_format = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
            EXPECT(format_from_vk(source_format) == Format::r9g9b9e5_ufloat);
            EXPECT(format_from_vk(VK_FORMAT_R16G16B16A16_SFLOAT) == Format::r16g16b16a16_float);
            EXPECT(format_from_vk(VK_FORMAT_R8G8B8A8_UNORM) == Format::other);

            VkFormatProperties properties{};
            const auto get_format_properties =
                d.instance_fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
            EXPECT(get_format_properties);
            get_format_properties(d.physical, source_format, &properties);
            const bool transfer_destination =
                (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;
            if (!sampled_source_format(source_format, properties.optimalTilingFeatures) || !transfer_destination) {
                std::printf("kind-7 compose seam: E5 sampled/transfer-dst feature unavailable, GPU source test skipped\n");
            } else {
                Inputs input{};
                input.switch_on = parse_switch("measured");  // a synthetic switch-on plan; no process environment change
                input.provenance = Provenance::series_s_kind7_output;
                input.source_resolved = true;
                input.source_format = format_from_vk(source_format);
                input.source_width = source_width;
                input.source_height = source_height;
                input.target_format = format_from_vk(VK_FORMAT_R16G16B16A16_SFLOAT);
                input.target_width = target_width;
                input.target_height = target_height;
                const Plan candidate = plan_present_candidate(input, source_format, properties.optimalTilingFeatures,
                                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                              VK_FORMAT_R16G16B16A16_SFLOAT);
                EXPECT(candidate.candidate_mapping && !candidate.compose &&
                       candidate.refusal == Refusal::consumer_not_observed);
                EXPECT(candidate.source_rect.right == float(source_width) &&
                       candidate.source_rect.bottom == float(source_height) &&
                       candidate.target_rect.right == float(target_width) &&
                       candidate.target_rect.bottom == float(target_height));

                const VkImage source = d.image(source_format, source_width, source_height, 1, VK_IMAGE_USAGE_SAMPLED_BIT);
                EXPECT(source != VK_NULL_HANDLE);
                EXPECT(d.run([&](VkCommandBuffer command) {
                    d.initial_layout(command, source, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    VkClearColorValue colour{};
                    colour.float32[0] = 0.5f;
                    colour.float32[1] = 0.25f;
                    colour.float32[2] = 0.125f;
                    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    d.fn<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(
                        command, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
                    VkImageMemoryBarrier readable{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    readable.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    readable.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    readable.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    readable.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    readable.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    readable.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    readable.image = source;
                    readable.subresourceRange = range;
                    d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
                        command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                        0, nullptr, 1, &readable);
                    return true;
                }));
                ResourceBinding source_binding{};
                source_binding.image = source;
                source_binding.format = source_format;
                source_binding.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
                source_binding.width = source_width;
                source_binding.height = source_height;
                source_binding.mip_levels = 1;
                source_binding.array_layers = 1;
                source_binding.depth = 1;
                source_binding.samples = 1;
                source_binding.sampled = true;
                context.add_resource(4, source_binding, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

                PresentPlanes planes{};
                planes.count = 1;
                planes.plane[0].source = 4;
                planes.plane[0].has_source_rect = true;
                // Independent generic PresentX source rect; do not use the refused candidate plan to authorize a draw.
                planes.plane[0].source_rect = {0, 0, static_cast<std::int32_t>(source_width),
                                               static_cast<std::int32_t>(source_height)};
                planes.plane[0].point_filter = true;  // synthetic control; the title's kind-7 filter is unmeasured
                EXPECT(presenter.present_planes(planes, true) == VK_SUCCESS);
                std::vector<std::uint8_t> pixels;
                VkExtent2D extent{};
                VkFormat capture_format = VK_FORMAT_UNDEFINED;
                EXPECT(presenter.read_back(pixels, &extent, &capture_format));
                const bool bgra = capture_format == VK_FORMAT_B8G8R8A8_UNORM || capture_format == VK_FORMAT_B8G8R8A8_SRGB;
                EXPECT(presenter_capture_matches_rgb(pixels, extent.width, extent.height, bgra, {128, 64, 32}, 2));
                context.remove_resource(4);
                std::printf("kind-7 compose seam: synthetic E5 sampled plane -> PresentComposer capture matched; title link remains unobserved\n");
            }
        }

        // 3. Concurrent present() + VulkanQueue::execute() + add/remove_resource() hammering LayoutTracker.
        std::atomic<bool> stop{false};
        std::atomic<bool> worker_failed{false};
        // The worker is paced: VulkanQueue::execute() has no back-pressure (it allocates a new command buffer whenever
        // none has finished), so an unthrottled loop queues thousands of submissions on the shared VkQueue and
        // present()'s frame fence is signalled only after all of them drain, while the worker keeps contending for
        // submit_mutex_ and tracker_mutex_ at full CPU. A slow GPU (an iGPU, or several processes sharing one) then
        // leaves present() stuck for tens of seconds. Draining every few rounds keeps the interleaving, not the backlog.
        constexpr std::uint64_t drain_every = 8;
        std::thread worker([&] {
            std::uint64_t round = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const ResourceId base = 100 + (round % 8) * 32;
                for (ResourceId i = 0; i < 32; ++i)
                    context.add_resource(base + i, side_rb, VK_IMAGE_LAYOUT_UNDEFINED);
                CommandList list;
                if (!list.clear_render_target({base, 0, 0, 1}, {0.1f, 0.2f, 0.3f, 1.0f}) || !list.close()) {
                    worker_failed.store(true);
                    break;
                }
                const CommandList* ptr = &list;
                if (worker_queue.execute({&ptr, 1}) != ReplayResult::success) {
                    worker_failed.store(true);
                    break;
                }
                (void)context.layout(base, 0);
                for (ResourceId i = 0; i < 32; ++i) context.remove_resource(base + i);
                if (++round % drain_every == 0 && !worker_queue.idle()) {
                    worker_failed.store(true);
                    break;
                }
            }
        });

        VkResult present_err = VK_SUCCESS;
        for (int i = 0; i < 40; ++i) {
            pump();
            present_err = presenter.present(1, false);
            if (present_err != VK_SUCCESS) break;
        }
        stop.store(true, std::memory_order_relaxed);
        worker.join();
        EXPECT(present_err == VK_SUCCESS);
        EXPECT(!worker_failed.load());
        EXPECT(queue.idle() && worker_queue.idle());
        std::printf("presenter sync: concurrent present + queue.execute + tracker mutations ok\n");

        // 4. Emulate a WSI capture layer that performs device-idle inside vkDestroySwapchainKHR.
        // The presenter first enters the destroy hook; a second thread then attempts a real queue submission.
        // Once the hook is released, that submission must not have entered the Vulkan driver while the hook ran.
        driver::destroy_entered.store(false);
        driver::submit_entered.store(false);
        driver::overlap_detected.store(false);
        driver::unexpected_submit_error.store(false);
        {
            std::lock_guard lock(driver::destroy_mutex);
            driver::allow_destroy_finish = false;
        }
        driver::check_swapchain_destroy.store(true);
        VkResult recreated = VK_ERROR_UNKNOWN;
        std::thread destroy_thread([&] { recreated = presenter.present(1, true); });
        bool entered = false;
        {
            std::unique_lock lock(driver::destroy_mutex);
            entered = driver::destroy_cv.wait_for(lock, std::chrono::seconds(5),
                                                  [] { return driver::destroy_entered.load(); });
        }
        std::atomic<bool> submit_started{false};
        ReplayResult submit_result = ReplayResult::unsupported;
        std::thread submit_thread;
        if (entered) {
            submit_thread = std::thread([&] {
                submit_started.store(true);
                CommandList list;
                if (!list.clear_render_target({1, 0, 0, 1}, {0.3f, 0.2f, 0.1f, 1.0f}) || !list.close()) return;
                const CommandList* ptr = &list;
                submit_result = worker_queue.execute({&ptr, 1});
            });
            for (int attempt = 0; attempt < 100 && !submit_started.load(); ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // Give the competing thread time to reach the submit point if no mutex is protecting destruction.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        {
            std::lock_guard lock(driver::destroy_mutex);
            driver::allow_destroy_finish = true;
        }
        driver::destroy_cv.notify_all();
        destroy_thread.join();
        if (submit_thread.joinable()) submit_thread.join();
        driver::check_swapchain_destroy.store(false);
        EXPECT(entered);
        EXPECT(recreated == VK_SUCCESS);
        EXPECT(submit_started.load());
        EXPECT(submit_result == ReplayResult::success);
        EXPECT(!driver::overlap_detected.load());
        EXPECT(!driver::unexpected_submit_error.load());
        EXPECT(queue.idle() && worker_queue.idle());
        std::printf("presenter sync: synthetic WSI device-idle during swapchain destruction serialized ok\n");
    }

    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    EXPECT(d.validation_errors() == 0);
    std::printf("vk_presenter_sync_test: ok\n");
    return 0;
}
