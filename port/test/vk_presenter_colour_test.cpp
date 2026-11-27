// GPU regression for the PresentX colour path (port/src/vulkan_present.h and present_compose.comp): synthetic SDR
// colour bars must survive both the default plain blit and SDR composition, and the switch/test encoder must produce
// ST.2084 capture values.
// Uses no title, package or game data. The CPU reference and WSI family policy are covered by
// vk_presenter_capture_colour_test; this test checks the shader and real swapchain readback.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <windows.h>

#include "vk_presenter_capture_colour.h"
#include "vk_test_device.h"
#include "../src/present_colour.h"
#include "../src/vulkan_present.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

constexpr std::uint32_t source_width = 16;
constexpr std::uint32_t source_height = 9;
constexpr std::uint32_t client_width = 128;
constexpr std::uint32_t client_height = 96;
constexpr palmetto::ResourceId source_id = 0xC011;
constexpr std::array<std::array<std::uint8_t, 3>, 4> pattern{{
    {{204, 51, 102}}, {{51, 204, 102}}, {{51, 102, 204}}, {{255, 255, 255}},
}};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    return DefWindowProcW(window, message, w, l);
}

void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

class TestWindow {
public:
    ~TestWindow() {
        if (window_) DestroyWindow(window_);
        if (registered_) UnregisterClassW(class_name_, instance_);
    }

    bool create() {
        instance_ = GetModuleHandleW(nullptr);
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = instance_;
        cls.lpszClassName = class_name_;
        if (!RegisterClassW(&cls)) return false;
        registered_ = true;
        RECT frame{0, 0, LONG(client_width), LONG(client_height)};
        if (!AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE)) return false;
        window_ = CreateWindowExW(0, class_name_, L"Palmetto presenter colour test", WS_OVERLAPPEDWINDOW,
                                  80, 80, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr,
                                  instance_, nullptr);
        if (!window_) return false;
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        UpdateWindow(window_);
        pump_messages();
        return true;
    }

    HWND get() const { return window_; }

private:
    static constexpr wchar_t class_name_[] = L"PalmettoPresenterColourTest";
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    bool registered_ = false;
};

bool is_unorm8(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM;
}

bool is_hdr10_10bit(VkFormat format) {
    return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}

std::array<int, 3> expected_sdr(const std::array<std::uint8_t, 3>& rgb) {
    return {rgb[0], rgb[1], rgb[2]};
}

std::array<int, 3> expected_pq(const std::array<std::uint8_t, 3>& rgb, float maximum) {
    using namespace palmetto::present_colour;
    const Rgb encoded{float(rgb[0]) / 255.0f, float(rgb[1]) / 255.0f, float(rgb[2]) / 255.0f};
    const Rgb linear = sdr_decode(encoded, SdrEotf::gamma22);  // the measured plane word 0 is G22/P709
    const Rgb working = to_working(linear, 1.0f, false, true, 203.0f, 1000.0f);
    const Rgb pq = nits_to_pq(working);
    return {int(std::lround(pq.r * maximum)), int(std::lround(pq.g * maximum)), int(std::lround(pq.b * maximum))};
}

bool pixel_rgb8(const std::vector<std::uint8_t>& pixels, VkExtent2D extent, VkFormat format,
                std::uint32_t x, std::uint32_t y, std::array<int, 3>& rgb) {
    if (!is_unorm8(format) || x >= extent.width || y >= extent.height ||
        pixels.size() != std::size_t(extent.width) * extent.height * 4)
        return false;
    const std::size_t at = (std::size_t(y) * extent.width + x) * 4;
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM;
    rgb = {pixels[at + (bgra ? 2 : 0)], pixels[at + 1], pixels[at + (bgra ? 0 : 2)]};
    return true;
}

bool pixel_rgb10(const std::vector<std::uint8_t>& pixels, VkExtent2D extent, VkFormat format,
                 std::uint32_t x, std::uint32_t y, std::array<int, 3>& rgb) {
    if (!is_hdr10_10bit(format) || x >= extent.width || y >= extent.height ||
        pixels.size() != std::size_t(extent.width) * extent.height * 4)
        return false;
    const std::size_t at = (std::size_t(y) * extent.width + x) * 4;
    std::uint32_t packed = 0;
    std::memcpy(&packed, pixels.data() + at, sizeof(packed));
    const int low = int(packed & 0x3ffu), green = int((packed >> 10) & 0x3ffu), high = int((packed >> 20) & 0x3ffu);
    if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) rgb = {low, green, high};
    else rgb = {high, green, low};
    return true;
}

template<typename ReadPixel>
bool check_bands(const std::vector<std::uint8_t>& pixels, VkExtent2D extent, VkFormat format,
                 const std::array<std::array<int, 3>, 4>& expected, int tolerance,
                 ReadPixel&& read_pixel, const char* label) {
    for (std::uint32_t band = 0; band < pattern.size(); ++band) {
        const std::uint32_t x = ((2 * band + 1) * extent.width) / 8;
        const std::uint32_t y = extent.height / 2;
        std::array<int, 3> actual{};
        if (!read_pixel(pixels, extent, format, x, y, actual)) return false;
        for (std::size_t channel = 0; channel < actual.size(); ++channel) {
            if (std::abs(actual[channel] - expected[band][channel]) > tolerance) {
                std::fprintf(stderr,
                             "%s band %u channel %zu at %u,%u: got %d, expected %d (tolerance %d; format %d)\n",
                             label, band, channel, x, y, actual[channel], expected[band][channel], tolerance, int(format));
                return false;
            }
        }
    }
    return true;
}

bool upload_pattern(vktest::Device& device, VkImage image, VkBuffer staging) {
    const auto copy = device.fn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    const auto barrier = device.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    if (!copy || !barrier) return false;
    // The caller wrote the known bars to the host-coherent staging buffer; transition and copy the image before sampling.
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {source_width, source_height, 1};
    return device.run([&](VkCommandBuffer commands) {
        VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = image;
        to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                0, nullptr, 0, nullptr, 1, &to_transfer);
        copy(commands, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier to_shader = to_transfer;
        to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                0, nullptr, 0, nullptr, 1, &to_shader);
        return true;
    });
}

}  // namespace

int main() {
    using namespace palmetto;

    vktest::Device device;
    if (!device.create(true)) {
        std::printf("SKIP: no Vulkan device with Win32 presentation\n");
        return 77;
    }

    TestWindow window;
    EXPECT(window.create());
    VulkanContext context(device.device_proc, device.device, true);
    EXPECT(context.loaded());

    const VkImage image = device.image(VK_FORMAT_R8G8B8A8_UNORM, source_width, source_height, 1,
                                       VK_IMAGE_USAGE_SAMPLED_BIT);
    void* mapped = nullptr;
    const VkBuffer staging = device.buffer(source_width * source_height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &mapped);
    EXPECT(image != VK_NULL_HANDLE && staging != VK_NULL_HANDLE && mapped);
    std::array<std::uint8_t, source_width * source_height * 4> bytes{};
    for (std::uint32_t y = 0; y < source_height; ++y) {
        for (std::uint32_t x = 0; x < source_width; ++x) {
            const std::size_t at = (std::size_t(y) * source_width + x) * 4;
            const auto& rgb = pattern[x / (source_width / 4)];
            bytes[at + 0] = rgb[0];
            bytes[at + 1] = rgb[1];
            bytes[at + 2] = rgb[2];
            bytes[at + 3] = 255;
        }
    }
    std::memcpy(mapped, bytes.data(), bytes.size());
    if (!upload_pattern(device, image, staging)) {
        std::printf("SKIP: test device could not stage a sampled RGBA8 pattern\n");
        return 77;
    }

    ResourceBinding source{};
    source.image = image;
    source.format = VK_FORMAT_R8G8B8A8_UNORM;
    source.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    source.width = source_width;
    source.height = source_height;
    source.depth = 1;
    source.mip_levels = 1;
    source.array_layers = 1;
    source.samples = 1;
    source.sampled = true;
    context.add_resource(source_id, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const auto create_surface = device.instance_fn<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR");
    EXPECT(create_surface != nullptr);
    VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hinstance = GetModuleHandleW(nullptr);
    surface_info.hwnd = window.get();
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    EXPECT(create_surface(device.instance, &surface_info, nullptr, &surface) == VK_SUCCESS);

    {
        VulkanPresenter presenter(context, device.instance_proc, device.instance, device.physical, device.queue,
                                  device.family, surface, device.swapchain_maintenance1);
        EXPECT(presenter.ok());
        if (!presenter.enable_capture()) {
            std::printf("SKIP: surface cannot capture an 8-bit presenter frame\n");
            return 77;
        }
        if (!is_unorm8(presenter.format())) {
            std::printf("SKIP: default compose test needs an 8-bit UNORM surface\n");
            return 77;
        }

        PresentPlanes planes{};
        planes.count = 1;
        planes.sdr_white_nits = 203.0f;
        planes.sdr_eotf = PresentPlanes::SdrEotf::gamma22;
        planes.hdr_peak_nits = 1000.0f;
        planes.plane[0].source = source_id;
        planes.plane[0].has_source_rect = true;
        planes.plane[0].source_rect = {0, 0, int(source_width), int(source_height)};
        planes.plane[0].point_filter = true;

        std::vector<std::uint8_t> pixels;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::array<std::array<int, 3>, 4> sdr_expected{};
        for (std::size_t i = 0; i < pattern.size(); ++i) sdr_expected[i] = expected_sdr(pattern[i]);

        // The measured whole-plane path is the plain UNORM blit: sample band centres away from their edges and verify
        // that its resize and channel swizzle preserve the known encoded source values exactly.
        VkResult result = presenter.present(source_id, true);
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            std::printf("SKIP: device cannot blit the RGBA8 test source to the surface\n");
            return 77;
        }
        EXPECT(result == VK_SUCCESS);
        EXPECT(presenter.read_back(pixels, &extent, &format));
        EXPECT(is_unorm8(format));
        EXPECT(check_bands(pixels, extent, format, sdr_expected, 1, pixel_rgb8, "plain UNORM8 blit value copy"));
        EXPECT(!presenter.hdr_active());
        std::printf("present colour GPU: measured G22/P709 bars retain their values through the default plain UNORM blit\n");

        result = presenter.present_planes(planes, true);
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            std::printf("SKIP: device cannot run the present composition pipeline\n");
            return 77;
        }
        EXPECT(result == VK_SUCCESS);
        EXPECT(presenter.read_back(pixels, &extent, &format));
        EXPECT(is_unorm8(format));
        EXPECT(check_bands(pixels, extent, format, sdr_expected, 2, pixel_rgb8, "SDR gamma-2.2 round trip"));
        EXPECT(!presenter.hdr_active());
        std::printf("present colour GPU: measured G22/P709 bars round-trip through the default UNORM composition\n");

        // The test-only encoder seam holds the same 8-bit UNORM swapchain while asking the shader to emit PQ values.
        // This keeps the GPU capture check available on machines whose WSI does not expose an HDR10 surface.
        presenter.force_hdr_encoding(true);
        result = presenter.present_planes(planes, true);
        EXPECT(result == VK_SUCCESS);
        EXPECT(presenter.read_back(pixels, &extent, &format));
        EXPECT(is_unorm8(format) && !presenter.hdr_active());
        std::array<std::array<int, 3>, 4> pq8_expected{};
        for (std::size_t i = 0; i < pattern.size(); ++i) pq8_expected[i] = expected_pq(pattern[i], 255.0f);
        EXPECT(check_bands(pixels, extent, format, pq8_expected, 3, pixel_rgb8, "PQ encoder into UNORM8"));
        presenter.force_hdr_encoding(false);
        std::printf("present colour GPU: shader PQ/Rec.2020 encoding matches CPU reference in UNORM8 capture\n");

        // The production diagnostic requests an actual HDR10 WSI format. A capable surface captures 10-bit PQ; an
        // incapable one has a named refusal and must leave the composed SDR pixels unchanged.
        presenter.set_hdr10_output_diagnostic(true);
        result = presenter.present_planes(planes, true);
        EXPECT(result == VK_SUCCESS);
        EXPECT(presenter.read_back(pixels, &extent, &format));
        if (presenter.hdr_active()) {
            EXPECT(is_hdr10_10bit(format));
            std::array<std::array<int, 3>, 4> pq10_expected{};
            for (std::size_t i = 0; i < pattern.size(); ++i) pq10_expected[i] = expected_pq(pattern[i], 1023.0f);
            EXPECT(check_bands(pixels, extent, format, pq10_expected, 5, pixel_rgb10, "HDR10 PQ output"));
            std::printf("present colour GPU: PALMETTO_PRESENT_HDR10 output captured as ST.2084 10-bit\n");
        } else {
            EXPECT(is_unorm8(format));
            EXPECT(check_bands(pixels, extent, format, sdr_expected, 2, pixel_rgb8, "HDR10 surface refusal SDR fallback"));
            std::printf("present colour GPU: HDR10 surface unavailable; opt-in path refused by name and SDR fallback captured\n");
        }
        presenter.set_hdr10_output_diagnostic(false);
    }

    context.remove_resource(source_id);
    EXPECT(device.validation_errors() == 0);
    std::printf("vk_presenter_colour_test: ok\n");
    return 0;
}
