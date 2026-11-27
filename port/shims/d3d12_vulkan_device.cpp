// A Vulkan-backed ID3D12Device (PC ABI) for the Xbox-ABI device proxy (d3d12_xs_device.cpp) to forward to.
// Implemented: queues, allocators, command lists, fences, descriptor heaps, render-target and depth-stencil views,
// committed buffers and textures (1D, 1D arrays, 2D, multisampled 2D, 3D) and their 1/2/3 variants, allocation
// info, CPU texture copies (WriteToSubresource/ReadFromSubresource), query heaps, a few feature queries. Everything else is a
// generated stub that reports itself once. See docs/d3d12_vulkan.md section 4.1/4.2 for what each answer means and
// what is still open.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "d3d12_com.h"
#include "d3d12_feature_compat.h"

#include "d3d12_stubs.h"  // generated
#include "d3d12_com_object.h"
#include "d3d12_trace.h"
#include "d3d12_xs_device.h"
#include "dxgi_com.h"
#include "../src/arena_size.h"
#include "../src/gfx10_descriptors.h"
#include "../src/depth24_fragment.h"
#include "../src/uav_clear.h"
#include "../src/vulkan_bindless.h"
#include "../src/vulkan_memory.h"
#include "../src/image_memory_pool.h"  // docs/streamed_image_backing.md: paged memory for images
#include "../src/vulkan_mesh_limits.h"
#include "../src/resource_tiling.h"
#include "../src/cached_pso.h"
#include "../src/pipeline_cache.h"
#include "../src/conservative_raster.h"
#include "../src/resource_detile.h"
#include "../src/plane_refresh.h"  // docs/peer_port_movie.md: the per-submission refresh of a title-written texture
#include "../src/shader_library.h"
#include "../src/graphics_pso.h"
#include "../src/spirv_f16_halves.h"
#include "../src/spirv_stores.h"
#include "../src/vulkan_present.h"
#include "xbox_present.h"  // docs/present_wiring.md: the console PresentX registration hook
#include "../src/vulkan_device_filter.h"
#include "../src/vulkan13_features.h"
#include "../src/pipeline_budget.h"  // docs/memory_growth.md: the live-pipeline cap and its LRU
#include "../src/deferred_compile.h"  // docs/deferred_compile.md: off-thread compute-pipeline compilation
#include "../src/console_texture.h"  // the console's depth-stencil + unordered-access translation (docs/3d_textures.md)

#include <chrono>
#include <vulkan/vulkan_win32.h>
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <cwchar>
#include <vector>

namespace rt::d3d12 {
namespace {

// The format-family helpers and the console depth-stencil + unordered-access translation are shared with the GPU-free
// tests (port/src/console_texture.h); they stay unqualified at their call sites here.
using palmetto::console_depth_uav_translated;
using palmetto::depth_stencil_family;
using palmetto::format_family;

// Every descriptor slot is this many bytes; an RTV or DSV slot holds a palmetto::TargetView. The console's own
// increments are open question 4 in docs/d3d12_vulkan.md; the application reads them from the device, as it must.
constexpr UINT kDescriptorSize = descriptor_size;
// The first 64 KiB of the GPU address arena are never handed out: VA 0 stays invalid, and the page is kept zero
// for unbound vertex and index buffers to read (D3D12 reads zeros there).
constexpr VkDeviceSize kArenaReserved = 64 * 1024;
static_assert(sizeof(palmetto::TargetView) <= kDescriptorSize);

// The shader model and root signature version this layer can honour. Nothing in the runtime compiles DXIL: the
// game's shaders arrive as SPIR-V from the offline translator, and the only DXIL path (the BVH builder) is lowered
// to standard DXIL, whose baseline is 6.0. Answering the driver's own maximum would tell the game it can hand this
// layer shaders it has no front end for, which section 4.1 of docs/d3d12_vulkan.md calls worse than a false
// "unsupported" at boot. Raise these when a front end for the higher version lands.
constexpr D3D_SHADER_MODEL kHighestShaderModel = D3D_SHADER_MODEL_6_0;
constexpr D3D_ROOT_SIGNATURE_VERSION kHighestRootSignatureVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

std::atomic<int> g_global_realtime_privilege_override{-1};

void log_once(const char* what, int value) {
    static std::mutex mutex;
    static std::set<std::pair<std::string, int>> seen;
    std::lock_guard lock(mutex);
    if (seen.insert({what, value}).second) std::fprintf(stderr, "[rt] d3d12: %s %d\n", what, value);
}

// Two test settings that make the standalone device look like a GPU with fewer Vulkan queues, so the path where D3D12
// queues share a VkQueue runs on any GPU; both together give the Intel UHD 770 on Windows and lavapipe (one family,
// one queue). PALMETTO_VK_MAX_QUEUES_PER_FAMILY=N (1 to 16): at most N VkQueues per queue family (unset, or not a
// number from 1 to 16: 16). PALMETTO_VK_GRAPHICS_FAMILY_ONLY=1: COMPUTE and COPY queues use the graphics family, as on
// a GPU without async compute or transfer families. The game's device ignores both (QueueTestSettings::ignore).

// The setting's value, or false when it is unset; set but ignored, it says so.
bool queue_test_setting(const char* name, char* text, size_t size, rt::d3d12::QueueTestSettings settings) {
    size_t length = 0;
    if (getenv_s(&length, text, size, name) != 0 || length <= 1) return false;
    if (settings == rt::d3d12::QueueTestSettings::apply) return true;
    std::fprintf(stderr, "[rt] d3d12: %s is a test setting; the game's device ignores it (all its Vulkan queues)\n",
                 name);
    return false;
}

bool graphics_family_only(rt::d3d12::QueueTestSettings settings) {
    char text[8] = {};
    const bool on = queue_test_setting("PALMETTO_VK_GRAPHICS_FAMILY_ONLY", text, sizeof(text), settings) &&
                    text[0] != '0';
    if (on) std::fprintf(stderr, "[rt] d3d12: PALMETTO_VK_GRAPHICS_FAMILY_ONLY: every D3D12 queue in the graphics family\n");
    return on;
}

std::uint32_t max_queues_per_family(rt::d3d12::QueueTestSettings settings) {
    constexpr std::uint32_t most = 16;
    char text[16] = {};
    if (!queue_test_setting("PALMETTO_VK_MAX_QUEUES_PER_FAMILY", text, sizeof(text), settings)) return most;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end || value < 1 || value > most) {
        std::fprintf(stderr, "[rt] d3d12: PALMETTO_VK_MAX_QUEUES_PER_FAMILY=%s is not a number from 1 to %u: ignored\n",
                     text, most);
        return most;
    }
    std::fprintf(stderr, "[rt] d3d12: PALMETTO_VK_MAX_QUEUES_PER_FAMILY: at most %lu Vulkan queue(s) per family\n",
                 value);
    return std::uint32_t(value);
}

// The pipelines build stage writes its cache beside palmetto.exe, independently of the working directory.
std::filesystem::path pipeline_cache_path() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path() / L"pipeline_cache.bin";
}

// A corpus cache may be several GiB. Keep the read-only view alive through vkCreatePipelineCache
// and deny writers while the view is in use.
class PipelineCacheSeed {
public:
    explicit PipelineCacheSeed(const std::filesystem::path& path) {
        if (path.empty()) return;
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER length{};
        constexpr auto header_size = static_cast<LONGLONG>(sizeof(VkPipelineCacheHeaderVersionOne));
        if (!GetFileSizeEx(file_, &length) || length.QuadPart < header_size)
            return;
        const HANDLE mapping = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) return;
        data_ = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(mapping);
        if (data_) size_ = static_cast<std::size_t>(length.QuadPart);
    }

    ~PipelineCacheSeed() {
        if (data_) UnmapViewOfFile(data_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }

    PipelineCacheSeed(const PipelineCacheSeed&) = delete;
    PipelineCacheSeed& operator=(const PipelineCacheSeed&) = delete;

    const void* data() const { return data_; }
    std::size_t size() const { return size_; }

    bool compatible(const palmetto::cached_pso::Identity& identity) const {
        static_assert(sizeof(VkPipelineCacheHeaderVersionOne) == 32);
        VkPipelineCacheHeaderVersionOne header{};
        // The header is little-endian, like the host. The driver interprets the opaque payload.
        std::memcpy(&header, data_, sizeof(header));
        return header.headerSize == sizeof(header) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
               header.vendorID == identity.vendor && header.deviceID == identity.device &&
               std::memcmp(header.pipelineCacheUUID, identity.cache_uuid, VK_UUID_SIZE) == 0;
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    const void* data_ = nullptr;
    std::size_t size_ = 0;
};

VkPipelineCache create_seeded_pipeline_cache(PFN_vkCreatePipelineCache create_cache, VkDevice device,
                                             const palmetto::cached_pso::Identity& identity) {
    const PipelineCacheSeed seed(pipeline_cache_path());
    if (!seed.data()) return VK_NULL_HANDLE;
    if (!seed.compatible(identity)) {
        std::fprintf(stderr, "[rt] d3d12: pipeline_cache.bin does not match this Vulkan device\n");
        return VK_NULL_HANDLE;
    }
    const VkPipelineCacheCreateInfo create{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                           nullptr, 0, seed.size(), seed.data()};
    VkPipelineCache cache = VK_NULL_HANDLE;
    if (create_cache(device, &create, nullptr, &cache) != VK_SUCCESS) {
        std::fprintf(stderr, "[rt] d3d12: Vulkan declined the pipeline cache seed\n");
        return VK_NULL_HANDLE;
    }
    std::fprintf(stderr, "[rt] d3d12: loaded %zu bytes from pipeline_cache.bin\n", seed.size());
    return cache;
}

// ---- opt-in pipeline profile (PALMETTO_PIPELINE_PROFILE=1) ------------------------------------------------
//
// Where a boot's pipeline time goes: the layer's own work (shader module and pipeline-layout creation) is timed
// separately from the driver's compile (vkCreate*Pipelines); the driver's pipeline-creation feedback says whether a
// pipeline came out of the VkPipelineCache; and four wall-clock windows separate the first burst from late repeats.
// Off by default: with the switch unset no clock is read and no line is printed, so the default path is unchanged.
// docs/pipeline_cache.md records the measured profile.

bool pipeline_profile_enabled() {
    static const bool enabled = [] {
        char text[4] = {};
        size_t length = 0;
        return getenv_s(&length, text, "PALMETTO_PIPELINE_PROFILE") == 0 && length > 1 && text[0] != '0';
    }();
    return enabled;
}

using PipelineClock = std::chrono::steady_clock;

std::uint64_t pipeline_now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(PipelineClock::now().time_since_epoch()).count());
}

std::uint64_t pipeline_elapsed_us(PipelineClock::time_point started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(PipelineClock::now() - started).count());
}

struct PipelineProfile {
    // Per kind of pipeline: how many creation calls the title made, how many reached vkCreate*Pipelines, the wall time
    // of those calls, the driver's own share, the layer's module/layout work, and the four windows by creation time.
    struct Counters {
        std::atomic<std::uint64_t> calls{0};
        std::atomic<std::uint64_t> created{0};
        std::atomic<std::uint64_t> us{0};
        std::atomic<std::uint64_t> driver_calls{0};
        std::atomic<std::uint64_t> driver_us{0};
        std::atomic<std::uint64_t> module_calls{0};
        std::atomic<std::uint64_t> module_us{0};
        std::atomic<std::uint64_t> layout_calls{0};
        std::atomic<std::uint64_t> layout_us{0};
        std::atomic<std::uint64_t> window_calls[4] = {};
        std::atomic<std::uint64_t> window_us[4] = {};
    };
    struct SlowEntry {
        std::uint64_t us = 0;
        bool graphics = false;
        std::string label;
    };
    std::mutex mutex;  // the slowest list and the report line
    Counters graphics;
    Counters compute;
    // VkPipelineCreationFeedback over every vkCreate*Pipelines call, both kinds: the number the persistent cache is
    // there to move (0 hits on a cold run, most of the boot's pipelines on a warm one).
    std::atomic<std::uint64_t> cache_hits{0};
    std::atomic<std::uint64_t> cache_misses{0};
    std::atomic<std::uint64_t> cache_unknown{0};
    // Calls answered by the per-process recipe cache (PALMETTO_PIPELINE_RECIPE_CACHE), i.e. calls that never reached
    // the driver: the number that switch is there to move.
    std::atomic<std::uint64_t> graphics_recipe_hits{0};
    std::atomic<std::uint64_t> compute_recipe_hits{0};
    std::array<SlowEntry, 5> slowest{};
    std::atomic<std::uint64_t> slow_floor_us{0};  // the smallest kept time once the list is full, else 0
    std::atomic<std::int64_t> started_us{0};      // the first creation's start; the windows are relative to it
    std::atomic<std::int64_t> last_report_us{0};
};

PipelineProfile& pipeline_profile() {
    static auto* profile = new PipelineProfile();  // leaked: the process exits with it
    return *profile;
}

int pipeline_window(std::uint64_t now_us) {
    const std::int64_t started = pipeline_profile().started_us.load(std::memory_order_relaxed);
    const std::uint64_t elapsed = now_us > static_cast<std::uint64_t>(started) ? now_us - started : 0;
    if (elapsed < 5'000'000) return 0;    // 0-5 s
    if (elapsed < 30'000'000) return 1;   // 5-30 s
    if (elapsed < 120'000'000) return 2;  // 30-120 s
    return 3;                             // later (the late/repeated creations)
}

void pipeline_profile_report();  // prints the cumulative line and the slowest entries

void pipeline_profile_maybe_report(std::uint64_t now_us) {
    auto& profile = pipeline_profile();
    const std::uint64_t calls =
        profile.graphics.calls.load(std::memory_order_relaxed) + profile.compute.calls.load(std::memory_order_relaxed);
    // A line at the first few calls, at every power of two up to 4096, then every 8192, and at least every 10 s: a boot
    // that is killed at its timeout still leaves the last cumulative line behind.
    const bool landmark = calls == 1 || calls == 16 || calls == 256 || calls == 1024 || calls == 2048 ||
                          calls == 4096 || (calls >= 8192 && calls % 8192 == 0);
    const std::int64_t last = profile.last_report_us.load(std::memory_order_relaxed);
    if (!landmark && now_us < static_cast<std::uint64_t>(last) + 10'000'000) return;
    pipeline_profile_report();
}

void ensure_pipeline_maintenance();  // defined after the cache save path, which it also drives

void pipeline_profile_begin(bool graphics) {
    ensure_pipeline_maintenance();
    auto& counters = graphics ? pipeline_profile().graphics : pipeline_profile().compute;
    counters.calls.fetch_add(1, std::memory_order_relaxed);
}

void pipeline_profile_driver(bool graphics, std::uint64_t us, int cache_state) {
    auto& profile = pipeline_profile();
    auto& counters = graphics ? profile.graphics : profile.compute;
    counters.driver_calls.fetch_add(1, std::memory_order_relaxed);
    counters.driver_us.fetch_add(us, std::memory_order_relaxed);
    // cache_state: 0 = the driver created it, 1 = it came from the application cache, -1 = no feedback.
    if (cache_state == 1)
        profile.cache_hits.fetch_add(1, std::memory_order_relaxed);
    else if (cache_state == 0)
        profile.cache_misses.fetch_add(1, std::memory_order_relaxed);
    else
        profile.cache_unknown.fetch_add(1, std::memory_order_relaxed);
}

void pipeline_profile_module(bool graphics, std::uint64_t us) {
    auto& counters = graphics ? pipeline_profile().graphics : pipeline_profile().compute;
    counters.module_calls.fetch_add(1, std::memory_order_relaxed);
    counters.module_us.fetch_add(us, std::memory_order_relaxed);
}

void pipeline_profile_layout(bool graphics, std::uint64_t us) {
    auto& counters = graphics ? pipeline_profile().graphics : pipeline_profile().compute;
    counters.layout_calls.fetch_add(1, std::memory_order_relaxed);
    counters.layout_us.fetch_add(us, std::memory_order_relaxed);
}

void pipeline_profile_recipe_hit(bool graphics) {
    auto& profile = pipeline_profile();
    (graphics ? profile.graphics_recipe_hits : profile.compute_recipe_hits)
        .fetch_add(1, std::memory_order_relaxed);
}

void pipeline_profile_created(bool graphics, PipelineClock::time_point started, const std::string& label) {
    auto& profile = pipeline_profile();
    auto& counters = graphics ? profile.graphics : profile.compute;
    const std::uint64_t now_us = pipeline_now_us();
    const std::uint64_t us = pipeline_elapsed_us(started);
    std::int64_t started_us = 0;
    profile.started_us.compare_exchange_strong(started_us, static_cast<std::int64_t>(now_us - us),
                                               std::memory_order_relaxed);
    counters.created.fetch_add(1, std::memory_order_relaxed);
    counters.us.fetch_add(us, std::memory_order_relaxed);
    const int window = pipeline_window(now_us);
    counters.window_calls[window].fetch_add(1, std::memory_order_relaxed);
    counters.window_us[window].fetch_add(us, std::memory_order_relaxed);
    if (!label.empty() && us > profile.slow_floor_us.load(std::memory_order_relaxed)) {
        std::lock_guard lock(profile.mutex);
        auto& list = profile.slowest;
        std::size_t slot = 0;
        while (slot < list.size() && list[slot].us != 0) ++slot;
        if (slot == list.size()) {
            slot = 0;
            for (std::size_t i = 1; i < list.size(); ++i)
                if (list[i].us < list[slot].us) slot = i;
            if (list[slot].us >= us) return;  // not slow enough after all (another thread got there first)
        }
        list[slot] = PipelineProfile::SlowEntry{us, graphics, label};
        std::uint64_t floor = ~std::uint64_t{0};
        bool full = true;
        for (const auto& entry : list) {
            if (entry.us == 0) full = false;
            floor = (std::min)(floor, entry.us);
        }
        profile.slow_floor_us.store(full ? floor : 0, std::memory_order_relaxed);
    }
    pipeline_profile_maybe_report(now_us);
}

std::string pipeline_profile_label(const char* first, const char* second, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                   bool mesh) {
    char label[256] = {};
    std::snprintf(label, sizeof(label), "%s=%.48s %s=%.48s ms=%d rt=%04x,%04x,%04x,%04x ds=%04x samples=%u",
                  mesh ? "ms" : "vs", first ? first : "-", "ps", second ? second : "-", mesh ? 1 : 0,
                  unsigned(desc.RTVFormats[0]), unsigned(desc.RTVFormats[1]), unsigned(desc.RTVFormats[2]),
                  unsigned(desc.RTVFormats[3]), unsigned(desc.DSVFormat), unsigned(desc.SampleDesc.Count));
    return label;
}

std::string pipeline_profile_label_compute(const char* name, std::size_t bindings) {
    char label[128] = {};
    std::snprintf(label, sizeof(label), "cs=%.80s bindings=%zu", name ? name : "-", bindings);
    return label;
}

void pipeline_profile_report() {
    auto& profile = pipeline_profile();
    std::lock_guard lock(profile.mutex);
    const std::uint64_t now_us = pipeline_now_us();
    profile.last_report_us.store(static_cast<std::int64_t>(now_us), std::memory_order_relaxed);
    const auto ms = [](std::uint64_t us) { return static_cast<double>(us) / 1000.0; };
    const auto& g = profile.graphics;
    const auto& c = profile.compute;
    const std::uint64_t started = static_cast<std::uint64_t>(profile.started_us.load(std::memory_order_relaxed));
    const double since_first_s = started && now_us > started ? static_cast<double>(now_us - started) / 1e6 : 0.0;
    std::fprintf(stderr,
                 "[rt] pipeline: profile at %.1f s: graphics %llu call(s), %llu created, %.1f ms creation; driver %llu "
                 "compile(s) %.1f ms; modules %llu %.1f ms; layouts %llu %.1f ms\n",
                 since_first_s, static_cast<unsigned long long>(g.calls.load()),
                 static_cast<unsigned long long>(g.created.load()), ms(g.us.load()),
                 static_cast<unsigned long long>(g.driver_calls.load()), ms(g.driver_us.load()),
                 static_cast<unsigned long long>(g.module_calls.load()), ms(g.module_us.load()),
                 static_cast<unsigned long long>(g.layout_calls.load()), ms(g.layout_us.load()));
    std::fprintf(stderr,
                 "[rt] pipeline: profile: compute %llu call(s), %llu created, %.1f ms creation; driver %llu compile(s) "
                 "%.1f ms; modules %llu %.1f ms; layouts %llu %.1f ms\n",
                 static_cast<unsigned long long>(c.calls.load()), static_cast<unsigned long long>(c.created.load()),
                 ms(c.us.load()), static_cast<unsigned long long>(c.driver_calls.load()), ms(c.driver_us.load()),
                 static_cast<unsigned long long>(c.module_calls.load()), ms(c.module_us.load()),
                 static_cast<unsigned long long>(c.layout_calls.load()), ms(c.layout_us.load()));
    std::fprintf(stderr,
                 "[rt] pipeline: profile windows (created/us): graphics 0-5 s %llu/%.1f ms, 5-30 s %llu/%.1f ms, "
                 "30-120 s %llu/%.1f ms, 120+ s %llu/%.1f ms; compute 0-5 s %llu/%.1f ms, 5-30 s %llu/%.1f ms, "
                 "30-120 s %llu/%.1f ms, 120+ s %llu/%.1f ms\n",
                 static_cast<unsigned long long>(g.window_calls[0].load()), ms(g.window_us[0].load()),
                 static_cast<unsigned long long>(g.window_calls[1].load()), ms(g.window_us[1].load()),
                 static_cast<unsigned long long>(g.window_calls[2].load()), ms(g.window_us[2].load()),
                 static_cast<unsigned long long>(g.window_calls[3].load()), ms(g.window_us[3].load()),
                 static_cast<unsigned long long>(c.window_calls[0].load()), ms(c.window_us[0].load()),
                 static_cast<unsigned long long>(c.window_calls[1].load()), ms(c.window_us[1].load()),
                 static_cast<unsigned long long>(c.window_calls[2].load()), ms(c.window_us[2].load()),
                 static_cast<unsigned long long>(c.window_calls[3].load()), ms(c.window_us[3].load()));
    std::fprintf(stderr,
                 "[rt] pipeline: profile driver cache feedback: %llu hit(s), %llu miss(es), %llu without feedback\n",
                 static_cast<unsigned long long>(profile.cache_hits.load()),
                 static_cast<unsigned long long>(profile.cache_misses.load()),
                 static_cast<unsigned long long>(profile.cache_unknown.load()));
    std::fprintf(stderr, "[rt] pipeline: profile recipe-cache hits: graphics %llu, compute %llu\n",
                 static_cast<unsigned long long>(profile.graphics_recipe_hits.load()),
                 static_cast<unsigned long long>(profile.compute_recipe_hits.load()));
    for (std::size_t i = 0; i < profile.slowest.size(); ++i) {
        const auto& entry = profile.slowest[i];
        if (entry.us == 0) continue;
        std::fprintf(stderr, "[rt] pipeline: profile slowest #%zu %s %.1f ms: %s\n", i + 1,
                     entry.graphics ? "graphics" : "compute", ms(entry.us), entry.label.c_str());
    }
}

// ---- persistent pipeline cache (PALMETTO_PIPELINE_CACHE=<file>) ------------------------------------------
//
// The device's VkPipelineCache is created with a previous run's vkGetPipelineCacheData blob, and the current blob is
// written back while the title compiles pipelines and again when the device goes away, so a warm boot does not make the
// driver compile the same SPIR-V to the same GPU ISA again. Opt-in and off by default; the switch names the file, and
// every failure (missing, corrupt, another device, over the size bound, a read-only directory) falls back to the path
// the layer had before (the build stage's seed, or an empty cache) without failing the boot. docs/pipeline_cache.md.
//
// Multiple devices: a process may create more than one device; each gets its own VkPipelineCache, so each file is
// owned by one device (the first to create its cache) and the last writer of a file would otherwise make the other
// device's cache incompatible. A second device on the same path still loads the file when its header matches (the
// VkPipelineCacheCreateInfo compatibility rule below) but does not save it; the registry logs that once.

struct PipelineCacheFileConfig {
    std::filesystem::path path;    // empty: off (today's behaviour)
    std::size_t max_bytes = 0;
    bool enabled = false;
};

const PipelineCacheFileConfig& pipeline_cache_file_config() {
    static const PipelineCacheFileConfig config = [] {
        PipelineCacheFileConfig out;
        char text[4096] = {};
        size_t length = 0;
        if (getenv_s(&length, text, sizeof(text), "PALMETTO_PIPELINE_CACHE") != 0) {
            std::fprintf(stderr,
                         "[rt] pipeline: PALMETTO_PIPELINE_CACHE is longer than %zu characters: the persistent cache "
                         "is off\n",
                         sizeof(text) - 1);
            return out;
        }
        if (length <= 1) return out;
        out.path = std::filesystem::path(std::u8string(text, text + length - 1));
        out.enabled = !out.path.empty();
        // The file is bounded so a driver that grows its cache cannot fill the disk: vkGetPipelineCacheData is asked
        // for the size first and nothing over the bound is read or written. 512 MiB holds the boot's pipelines on
        // the measured driver; PALMETTO_PIPELINE_CACHE_MAX_MB raises or lowers it (1 to 16384).
        constexpr unsigned long kDefaultMaxMb = 512;
        unsigned long max_mb = kDefaultMaxMb;
        char size_text[32] = {};
        size_t size_length = 0;
        if (getenv_s(&size_length, size_text, sizeof(size_text), "PALMETTO_PIPELINE_CACHE_MAX_MB") == 0 &&
            size_length > 1) {
            char* end = nullptr;
            const unsigned long value = std::strtoul(size_text, &end, 10);
            if (end == size_text || *end || value < 1 || value > 16384)
                std::fprintf(stderr,
                             "[rt] pipeline: PALMETTO_PIPELINE_CACHE_MAX_MB=%s is not a number of MiB from 1 to 16384: "
                             "using %lu\n",
                             size_text, kDefaultMaxMb);
            else
                max_mb = value;
        }
        out.max_bytes = static_cast<std::size_t>(max_mb) << 20;
        std::fprintf(stderr, "[rt] pipeline: persistent pipeline cache at %ls, bound %lu MiB\n", out.path.wstring().c_str(),
                     max_mb);
        return out;
    }();
    return config;
}

palmetto::pipeline_cache::Identity pipeline_cache_identity(const palmetto::cached_pso::Identity& pso) {
    palmetto::pipeline_cache::Identity identity;
    identity.vendor = pso.vendor;
    identity.device = pso.device;
    std::memcpy(identity.uuid, pso.cache_uuid, sizeof(identity.uuid));
    return identity;
}

// The header fields the persistent file is read and written by must be the Vulkan header's (src/pipeline_cache.h has
// the same 32 bytes without the Vulkan includes the pure test does not need).
static_assert(sizeof(VkPipelineCacheHeaderVersionOne) == palmetto::pipeline_cache::header_bytes);
static_assert(offsetof(VkPipelineCacheHeaderVersionOne, headerSize) == 0);
static_assert(offsetof(VkPipelineCacheHeaderVersionOne, headerVersion) == 4);
static_assert(offsetof(VkPipelineCacheHeaderVersionOne, vendorID) == 8);
static_assert(offsetof(VkPipelineCacheHeaderVersionOne, deviceID) == 12);
static_assert(offsetof(VkPipelineCacheHeaderVersionOne, pipelineCacheUUID) == 16);
static_assert(VK_PIPELINE_CACHE_HEADER_VERSION_ONE == palmetto::pipeline_cache::header_version_one);

// A live VkPipelineCache whose blob is written to `path`. The owner is shared with the process-wide list the save
// path walks; `mutex` serialises snapshot/replace against destruction (host access to a VkPipelineCache is not
// externally synchronised for creation: the cache is created without EXTERNALLY_SYNCHRONIZED_BIT, so the driver
// serialises concurrent creation; the mutex covers this layer's own vkGetPipelineCacheData and destroy).
struct PipelineCacheOwner {
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::filesystem::path path;
    std::size_t max_bytes = 0;
    PFN_vkGetPipelineCacheData get_data = nullptr;
    bool live = true;  // guarded by mutex
    std::uint64_t writes = 0;
    std::uint64_t write_failures = 0;
    std::size_t saved_bytes = 0;
    PipelineClock::time_point last_save{};
};

struct PipelineCacheRegistry {
    std::mutex mutex;
    std::vector<std::shared_ptr<PipelineCacheOwner>> owners;
    std::atomic<bool> enabled{false};  // maybe_save's fast path: no owner, no clock read
    std::atomic<std::int64_t> last_check_us{0};
};

PipelineCacheRegistry& pipeline_cache_registry() {
    static auto* registry = new PipelineCacheRegistry();  // leaked: atexit and the device both use it
    return *registry;
}

// A snapshot is skipped while the cache has not grown by a MiB, and a check happens at most every 4 s; a full save
// every 12 s. A boot killed at its timeout keeps the last save, which is the point of the periodic path.
constexpr std::uint64_t kPipelineCacheCheckIntervalUs = 4'000'000;
constexpr std::uint64_t kPipelineCacheSaveIntervalUs = 12'000'000;
constexpr std::size_t kPipelineCacheMinGrowthBytes = 1u << 20;

std::vector<char> snapshot_pipeline_cache(PipelineCacheOwner& owner, const char** why) {
    // vkGetPipelineCacheData writes a prefix and returns VK_INCOMPLETE when a worker grows the cache between the size
    // query and the read; retry with headroom, as the pipeline build stage does (test/pipeline_cache_snapshot.h).
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::size_t size = 0;
        if (owner.get_data(owner.device, owner.cache, &size, nullptr) != VK_SUCCESS) {
            *why = "the driver refused a size query";
            return {};
        }
        if (size < palmetto::pipeline_cache::header_bytes) {
            *why = "smaller than a cache header";
            return {};
        }
        if (size > owner.max_bytes) {
            *why = "over the size bound";
            return {};
        }
        std::size_t capacity = size + (size >> (4 - attempt));
        capacity = (std::min)(capacity, owner.max_bytes);
        std::vector<char> data(capacity);
        std::size_t written = capacity;
        const VkResult result = owner.get_data(owner.device, owner.cache, &written, data.data());
        if (result == VK_INCOMPLETE) continue;
        if (result != VK_SUCCESS) {
            *why = "the driver refused the read";
            return {};
        }
        if (written < palmetto::pipeline_cache::header_bytes || written > capacity) {
            *why = "an invalid byte count";
            return {};
        }
        data.resize(written);
        return data;
    }
    *why = "it grew while being read on four attempts";
    return {};
}

void save_pipeline_cache_owner(const std::shared_ptr<PipelineCacheOwner>& owner, bool final_save) {
    std::lock_guard lock(owner->mutex);
    if (!owner->live || !owner->cache || !owner->get_data || owner->path.empty()) return;
    const auto now = PipelineClock::now();
    if (!final_save && now - owner->last_save < std::chrono::microseconds(kPipelineCacheSaveIntervalUs)) return;
    owner->last_save = now;
    const char* why = "";
    const std::vector<char> data = snapshot_pipeline_cache(*owner, &why);
    if (data.empty()) {
        if (final_save || owner->write_failures < 3) {
            ++owner->write_failures;
            std::fprintf(stderr, "[rt] pipeline: could not read the pipeline cache to save it: %s\n", why);
        }
        return;
    }
    if (!final_save && data.size() < owner->saved_bytes + kPipelineCacheMinGrowthBytes) return;
    const auto write_started = PipelineClock::now();
    const std::error_code error =
        palmetto::pipeline_cache::write_file(owner->path, data.data(), data.size(), owner->max_bytes);
    const double ms = static_cast<double>(pipeline_elapsed_us(write_started)) / 1000.0;
    if (error) {
        if (final_save || owner->write_failures < 3)
            std::fprintf(stderr, "[rt] pipeline: could not save the pipeline cache to %ls: %s\n",
                         owner->path.wstring().c_str(), error.message().c_str());
        ++owner->write_failures;
        return;
    }
    owner->saved_bytes = data.size();
    ++owner->writes;
    std::fprintf(stderr, "[rt] pipeline: saved %.1f MiB of pipeline cache to %ls (write %llu, %.1f ms)\n",
                 static_cast<double>(data.size()) / (1024.0 * 1024.0), owner->path.wstring().c_str(),
                 static_cast<unsigned long long>(owner->writes), ms);
}

void save_pipeline_caches(bool final_save) {
    auto& registry = pipeline_cache_registry();
    if (!registry.enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(registry.mutex);
    for (const auto& owner : registry.owners) save_pipeline_cache_owner(owner, final_save);
}

// Called after every driver pipeline creation (and from the save path): cheap when the switch is off, and the
// 4 s/12 s bounds keep the write cost off the creation path.
void maybe_save_pipeline_caches() {
    auto& registry = pipeline_cache_registry();
    if (!registry.enabled.load(std::memory_order_relaxed)) return;
    const std::int64_t now = static_cast<std::int64_t>(pipeline_now_us());
    std::int64_t last = registry.last_check_us.load(std::memory_order_relaxed);
    if (now - last < static_cast<std::int64_t>(kPipelineCacheCheckIntervalUs)) return;
    if (!registry.last_check_us.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    save_pipeline_caches(false);
}

// One background tick for the two switches: the profile prints its last cumulative line after the creation burst has
// stopped (a boot killed at its timeout has no exit hook for a final summary), and the cache saves the pipelines the
// burst created after its last mid-burst save. Off unless one of the switches is on: the thread is started from the
// first profile sample or cache registration only. The thread is detached; every object it touches outlives the
// process (both registries are leaked on purpose), and the device unregisters its cache before destroying it.
void ensure_pipeline_maintenance() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                if (pipeline_profile_enabled()) pipeline_profile_maybe_report(pipeline_now_us());
                maybe_save_pipeline_caches();
            }
        }).detach();
    });
}

bool register_pipeline_cache(const std::shared_ptr<PipelineCacheOwner>& owner) {
    ensure_pipeline_maintenance();
    auto& registry = pipeline_cache_registry();
    std::lock_guard lock(registry.mutex);
    // The switch names one process-wide path, so the first device owns the file. A second device (same path, maybe
    // another GPU identity) still has its own in-memory cache, but saving it would clobber the owner's blob: it is not
    // registered, logs once and keeps working for the boot.
    for (const auto& existing : registry.owners) {
        if (existing->path != owner->path) continue;
        std::fprintf(stderr,
                     "[rt] pipeline: %ls is already owned by another device in this process; this device's pipeline "
                     "cache is not saved\n",
                     owner->path.wstring().c_str());
        return false;
    }
    registry.owners.push_back(owner);
    if (!registry.enabled.exchange(true)) {
        static bool atexit_registered = false;
        if (!atexit_registered) {
            atexit_registered = true;
            std::atexit([] { save_pipeline_caches(true); });
        }
    }
    return true;
}

void unregister_pipeline_cache(const std::shared_ptr<PipelineCacheOwner>& owner) {
    save_pipeline_cache_owner(owner, true);  // the final write, while the cache is still usable
    {
        std::lock_guard lock(owner->mutex);
        owner->live = false;
    }
    auto& registry = pipeline_cache_registry();
    std::lock_guard lock(registry.mutex);
    std::erase(registry.owners, owner);
    if (registry.owners.empty()) registry.enabled.store(false, std::memory_order_relaxed);
}

// Creates the VkPipelineCache with the previous run's blob when the file is usable; on every other status the caller
// falls back to the build stage's seed (create_seeded_pipeline_cache) and then to an empty cache.
VkPipelineCache create_persistent_pipeline_cache(PFN_vkCreatePipelineCache create_cache, VkDevice device,
                                                 const palmetto::cached_pso::Identity& pso_identity,
                                                 const PipelineCacheFileConfig& config) {
    const palmetto::pipeline_cache::Loaded loaded =
        palmetto::pipeline_cache::load_file(config.path, config.max_bytes, pipeline_cache_identity(pso_identity));
    switch (loaded.status) {
        case palmetto::pipeline_cache::Status::ok:
            std::fprintf(stderr, "[rt] pipeline: loaded %.1f MiB of pipeline cache from %ls\n",
                         static_cast<double>(loaded.data.size()) / (1024.0 * 1024.0), config.path.wstring().c_str());
            break;
        case palmetto::pipeline_cache::Status::missing:
            std::fprintf(stderr,
                         "[rt] pipeline: no pipeline cache at %ls yet; this run seeds and saves the first one\n",
                         config.path.wstring().c_str());
            return VK_NULL_HANDLE;  // nothing to seed from, but the caller keeps it as the file to save to
        default:
            std::fprintf(stderr, "[rt] pipeline: ignoring %ls: %s\n", config.path.wstring().c_str(),
                         palmetto::pipeline_cache::status_name(loaded.status));
            return VK_NULL_HANDLE;
    }
    const VkPipelineCacheCreateInfo create{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, loaded.data.size(),
                                           loaded.data.data()};
    VkPipelineCache cache = VK_NULL_HANDLE;
    if (create_cache(device, &create, nullptr, &cache) != VK_SUCCESS) {
        std::fprintf(stderr, "[rt] pipeline: Vulkan declined the saved pipeline cache; starting from the seed\n");
        return VK_NULL_HANDLE;
    }
    return cache;
}

// A shader whose subgroup size cannot be had runs at the device's: inexact if it crosses the halves of a wave64 (or,
// for a wave32 shader on a 64-wide default, reads lanes that are not its own). Logged once per stage with the first
// shader's name, then again at 10, 100, 1000... of that stage so a whole corpus stays readable.
void report_subgroup_unavailable(const palmetto::ShaderEntry& entry, const palmetto::SubgroupRequest& request,
                                 const palmetto::SubgroupSizeCaps& caps) {
    static std::mutex mutex;
    static std::map<std::string, std::pair<std::set<std::string>, unsigned>> seen;  // stage -> names, next report
    std::lock_guard lock(mutex);
    auto& [names, next_report] = seen[entry.stage];
    if (!names.insert(entry.name).second) return;
    if (names.size() == 1) next_report = 10;
    else if (names.size() != next_report) return;
    else next_report *= 10;
    const bool emulable = entry.stage == "cs" || entry.stage == "ms";
    const std::string so_far =
        names.size() > 1 ? " (shaders of this stage so far: " + std::to_string(names.size()) + ")" : "";
    std::fprintf(stderr,
                 "[rt] d3d12: %s (%s) is a %u-wide shader but this device cannot require that subgroup size for the "
                 "stage (device sizes %u..%u, default %u, subgroupSizeControl %s); it runs at the device's size and "
                 "is inexact where lanes of the other half matter%s\n",
                 entry.name.c_str(), entry.stage.c_str(), request.wanted, caps.min_size, caps.max_size,
                 caps.default_size, caps.subgroup_size_control ? "on" : "off",
                 so_far.c_str());
    if (emulable && request.wanted == 64)
        std::fprintf(stderr, "[rt] d3d12: translate wave64 %s shaders with --wave64-emulate for this GPU "
                             "(build_port.py --wave64-emulate auto does)\n", entry.stage.c_str());
    else if (request.wanted == 64)
        std::fprintf(stderr, "[rt] d3d12: no emulation exists for wave64 %s shaders yet (docs/d3d12_vulkan.md, "
                             "subgroup size)\n", entry.stage.c_str());
}

struct Format {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspects = 0;
    bool typeless = false;  // created with MUTABLE_FORMAT so typed views can reinterpret it
    bool d24_as_float = false;  // D24_UNORM_S8_UINT stored as D32_SFLOAT_S8_UINT (no D24 on the GPU)
    bool d24 = false;           // a D24_UNORM_S8_UINT family format, native or stored as D32
    // A UAV texture whose format has no storage support, stored as the mutable unsigned image `format` names (R32_UINT
    // for R9G9B9E5_SHAREDEXP, R16_UINT for B5G5R5A1_UNORM): the format its SRVs, RTVs and T#s use. Its storage view is
    // `format`, in the uint class; shaders write the packed bits. UNDEFINED for every other texture.
    VkFormat uint_backed = VK_FORMAT_UNDEFINED;
    // A 96-bit R32G32B32 format, held in the R32G32B32A32 image `format` names (ResourceBinding::rgb96).
    bool rgb96 = false;
};

// 96-bit textures are always RGBA32 images, even where the GPU has an RGB32 one (most have none with optimal
// tiling): D3D12 places subresources at multiples of 512 bytes, Vulkan wants a copy's buffer offset to be a
// multiple of the 12-byte texel (VUID-vkCmdCopyBufferToImage-dstImage-07975), so copies need the repack either
// way (docs section 4.2).
constexpr Format rgb96_format(VkFormat wide, bool typeless = false) {
    Format format{wide, VK_IMAGE_ASPECT_COLOR_BIT, typeless};
    format.rgb96 = true;
    return format;
}

// DXGI -> Vulkan for the formats render targets, depth buffers and common textures use. R32 and R16 typeless become
// depth formats when the resource allows depth-stencil; R32G8X24 and R24G8 always do, as Vulkan has no colour format
// with their layout.
Format vulkan_format(DXGI_FORMAT format, bool depth) {
    constexpr VkImageAspectFlags color = VK_IMAGE_ASPECT_COLOR_BIT, d = VK_IMAGE_ASPECT_DEPTH_BIT,
                                 ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return {VK_FORMAT_R32G32B32A32_SFLOAT, color, true};
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return {VK_FORMAT_R32G32B32A32_SFLOAT, color};
    case DXGI_FORMAT_R32G32B32A32_UINT: return {VK_FORMAT_R32G32B32A32_UINT, color};
    case DXGI_FORMAT_R32G32B32A32_SINT: return {VK_FORMAT_R32G32B32A32_SINT, color};
    case DXGI_FORMAT_R32G32B32_TYPELESS: return rgb96_format(VK_FORMAT_R32G32B32A32_SFLOAT, true);
    case DXGI_FORMAT_R32G32B32_FLOAT: return rgb96_format(VK_FORMAT_R32G32B32A32_SFLOAT);
    case DXGI_FORMAT_R32G32B32_UINT: return rgb96_format(VK_FORMAT_R32G32B32A32_UINT);
    case DXGI_FORMAT_R32G32B32_SINT: return rgb96_format(VK_FORMAT_R32G32B32A32_SINT);
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return {VK_FORMAT_R16G16B16A16_SFLOAT, color, true};
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return {VK_FORMAT_R16G16B16A16_SFLOAT, color};
    case DXGI_FORMAT_R16G16B16A16_UNORM: return {VK_FORMAT_R16G16B16A16_UNORM, color};
    case DXGI_FORMAT_R16G16B16A16_SNORM: return {VK_FORMAT_R16G16B16A16_SNORM, color};
    case DXGI_FORMAT_R16G16B16A16_UINT: return {VK_FORMAT_R16G16B16A16_UINT, color};
    case DXGI_FORMAT_R16G16B16A16_SINT: return {VK_FORMAT_R16G16B16A16_SINT, color};
    case DXGI_FORMAT_R32G32_TYPELESS: return {VK_FORMAT_R32G32_SFLOAT, color, true};
    case DXGI_FORMAT_R32G32_FLOAT: return {VK_FORMAT_R32G32_SFLOAT, color};
    case DXGI_FORMAT_R32G32_UINT: return {VK_FORMAT_R32G32_UINT, color};
    case DXGI_FORMAT_R32G32_SINT: return {VK_FORMAT_R32G32_SINT, color};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, color, true};
    case DXGI_FORMAT_R10G10B10A2_UNORM: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, color};
    case DXGI_FORMAT_R10G10B10A2_UINT: return {VK_FORMAT_A2B10G10R10_UINT_PACK32, color};
    // XR_BIAS is the same packed 10:10:10:2 bit layout as R10G10B10A2_UNORM. Vulkan has no XR_BIAS format:
    // the application biases and scales the channels in its shader, so the resource itself is an ordinary UNORM.
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, color};
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, color};
    case DXGI_FORMAT_R11G11B10_FLOAT: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, color};
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return {VK_FORMAT_R8G8B8A8_UNORM, color, true};
    case DXGI_FORMAT_R8G8B8A8_UNORM: return {VK_FORMAT_R8G8B8A8_UNORM, color};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, color};
    case DXGI_FORMAT_R8G8B8A8_SNORM: return {VK_FORMAT_R8G8B8A8_SNORM, color};
    case DXGI_FORMAT_R8G8B8A8_UINT: return {VK_FORMAT_R8G8B8A8_UINT, color};
    case DXGI_FORMAT_R8G8B8A8_SINT: return {VK_FORMAT_R8G8B8A8_SINT, color};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return {VK_FORMAT_B8G8R8A8_UNORM, color, true};
    case DXGI_FORMAT_B8G8R8A8_UNORM: return {VK_FORMAT_B8G8R8A8_UNORM, color};
    // DXGI names channels from the low bits up, Vulkan from the high bits down: B in bits 0-3, A in 12-15 in both.
    // Vulkan 1.3 requires sampling it (formerly VK_EXT_4444_formats).
    case DXGI_FORMAT_B4G4R4A4_UNORM: return {VK_FORMAT_A4R4G4B4_UNORM_PACK16, color};
    // A in bits 0-3 and R in 12-15 (cleared and read back on native D3D12, AMD and WARP): Vulkan's R4G4B4A4_PACK16.
    case DXGI_FORMAT_A4B4G4R4_UNORM: return {VK_FORMAT_R4G4B4A4_UNORM_PACK16, color};
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return {VK_FORMAT_B8G8R8A8_SRGB, color};
    // The X byte has no channel of its own: the BGRA8 images hold it, and shader views read alpha as 1.
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return {VK_FORMAT_B8G8R8A8_UNORM, color, true};
    case DXGI_FORMAT_B8G8R8X8_UNORM: return {VK_FORMAT_B8G8R8A8_UNORM, color};
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return {VK_FORMAT_B8G8R8A8_SRGB, color};
    case DXGI_FORMAT_R16G16_TYPELESS: return {VK_FORMAT_R16G16_SFLOAT, color, true};
    case DXGI_FORMAT_R16G16_FLOAT: return {VK_FORMAT_R16G16_SFLOAT, color};
    case DXGI_FORMAT_R16G16_UNORM: return {VK_FORMAT_R16G16_UNORM, color};
    case DXGI_FORMAT_R16G16_SNORM: return {VK_FORMAT_R16G16_SNORM, color};
    case DXGI_FORMAT_R16G16_UINT: return {VK_FORMAT_R16G16_UINT, color};
    case DXGI_FORMAT_R16G16_SINT: return {VK_FORMAT_R16G16_SINT, color};
    case DXGI_FORMAT_R8G8_TYPELESS: return {VK_FORMAT_R8G8_UNORM, color, true};
    case DXGI_FORMAT_R8G8_UNORM: return {VK_FORMAT_R8G8_UNORM, color};
    case DXGI_FORMAT_R8G8_SNORM: return {VK_FORMAT_R8G8_SNORM, color};
    case DXGI_FORMAT_R16_UNORM: return {VK_FORMAT_R16_UNORM, color};
    case DXGI_FORMAT_R16_SNORM: return {VK_FORMAT_R16_SNORM, color};
    case DXGI_FORMAT_R16_UINT: return {VK_FORMAT_R16_UINT, color};
    case DXGI_FORMAT_R16_SINT: return {VK_FORMAT_R16_SINT, color};
    // DXGI names packed channels from the high bits down, Vulkan's PACK formats the same way but spelled the other
    // way round: B5G6R5 (blue in bits 0-4) is R5G6B5_PACK16 and B5G5R5A1 (alpha in bit 15) is A1R5G5B5_PACK16.
    case DXGI_FORMAT_B5G6R5_UNORM: return {VK_FORMAT_R5G6B5_UNORM_PACK16, color};
    case DXGI_FORMAT_B5G5R5A1_UNORM: return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, color};
    case DXGI_FORMAT_R8G8_UINT: return {VK_FORMAT_R8G8_UINT, color};
    case DXGI_FORMAT_R8G8_SINT: return {VK_FORMAT_R8G8_SINT, color};
    case DXGI_FORMAT_R16_FLOAT: return {VK_FORMAT_R16_SFLOAT, color};
    case DXGI_FORMAT_R8_UNORM: return {VK_FORMAT_R8_UNORM, color};
    case DXGI_FORMAT_R8_TYPELESS: return {VK_FORMAT_R8_UNORM, color, true};
    case DXGI_FORMAT_R8_SNORM: return {VK_FORMAT_R8_SNORM, color};
    case DXGI_FORMAT_R8_UINT: return {VK_FORMAT_R8_UINT, color};
    case DXGI_FORMAT_R8_SINT: return {VK_FORMAT_R8_SINT, color};
    case DXGI_FORMAT_R32_FLOAT: return {VK_FORMAT_R32_SFLOAT, color};
    case DXGI_FORMAT_R32_UINT: return {VK_FORMAT_R32_UINT, color};
    case DXGI_FORMAT_R32_SINT: return {VK_FORMAT_R32_SINT, color};
    case DXGI_FORMAT_BC1_TYPELESS: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC1_UNORM: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC1_UNORM_SRGB: return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, color};
    case DXGI_FORMAT_BC2_TYPELESS: return {VK_FORMAT_BC2_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC2_UNORM: return {VK_FORMAT_BC2_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC2_UNORM_SRGB: return {VK_FORMAT_BC2_SRGB_BLOCK, color};
    case DXGI_FORMAT_BC3_TYPELESS: return {VK_FORMAT_BC3_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC3_UNORM: return {VK_FORMAT_BC3_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC3_UNORM_SRGB: return {VK_FORMAT_BC3_SRGB_BLOCK, color};
    case DXGI_FORMAT_BC4_TYPELESS: return {VK_FORMAT_BC4_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC4_UNORM: return {VK_FORMAT_BC4_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC4_SNORM: return {VK_FORMAT_BC4_SNORM_BLOCK, color};
    case DXGI_FORMAT_BC5_TYPELESS: return {VK_FORMAT_BC5_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC5_UNORM: return {VK_FORMAT_BC5_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC5_SNORM: return {VK_FORMAT_BC5_SNORM_BLOCK, color};
    case DXGI_FORMAT_BC6H_TYPELESS: return {VK_FORMAT_BC6H_UFLOAT_BLOCK, color, true};
    case DXGI_FORMAT_BC6H_UF16: return {VK_FORMAT_BC6H_UFLOAT_BLOCK, color};
    case DXGI_FORMAT_BC6H_SF16: return {VK_FORMAT_BC6H_SFLOAT_BLOCK, color};
    case DXGI_FORMAT_BC7_TYPELESS: return {VK_FORMAT_BC7_UNORM_BLOCK, color, true};
    case DXGI_FORMAT_BC7_UNORM: return {VK_FORMAT_BC7_UNORM_BLOCK, color};
    case DXGI_FORMAT_BC7_UNORM_SRGB: return {VK_FORMAT_BC7_SRGB_BLOCK, color};
    case DXGI_FORMAT_D32_FLOAT: return {VK_FORMAT_D32_SFLOAT, d};
    case DXGI_FORMAT_D16_UNORM: return {VK_FORMAT_D16_UNORM, d};
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return {VK_FORMAT_D32_SFLOAT_S8_UINT, ds};
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return {VK_FORMAT_D24_UNORM_S8_UINT, ds};
    case DXGI_FORMAT_R32_TYPELESS:
        return depth ? Format{VK_FORMAT_D32_SFLOAT, d, true} : Format{VK_FORMAT_R32_SFLOAT, color, true};
    case DXGI_FORMAT_R16_TYPELESS:
        return depth ? Format{VK_FORMAT_D16_UNORM, d, true} : Format{VK_FORMAT_R16_UNORM, color, true};
    case DXGI_FORMAT_R32G8X24_TYPELESS: return {VK_FORMAT_D32_SFLOAT_S8_UINT, ds, true};
    case DXGI_FORMAT_R24G8_TYPELESS: return {VK_FORMAT_D24_UNORM_S8_UINT, ds, true};
    default: return {};
    }
}

Format texture_storage_format(DXGI_FORMAT format, bool depth) {
    return vulkan_format(format == DXGI_FORMAT_A8_UNORM ? DXGI_FORMAT_R8_UNORM : format, depth);
}

// The three-channel format a view of a 96-bit texture names in its T# (GFX10_FORMAT_32_32_32_*): the format
// whose channels the view's swizzle is taken from, so alpha reads as one whatever the image's fourth channel holds.
VkFormat rgb96_named_format(VkFormat image_view_format) {
    switch (image_view_format) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case VK_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32_UINT;
    case VK_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32_SINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}
// And back: the format of the view of the RGBA32 image that shows such a T#.
VkFormat rgb96_view_format(VkFormat image_format, std::uint32_t gfx10_format) {
    for (const VkFormat wide : {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT})
        if (palmetto::gfx10::format_from_vulkan(rgb96_named_format(wide)) == gfx10_format)
            return rgb96_named_format(image_format) != VK_FORMAT_UNDEFINED ? wide : VK_FORMAT_UNDEFINED;
    return VK_FORMAT_UNDEFINED;
}

// Bytes per texel or block as D3D12 lays the format out in a footprint.
palmetto::BlockFormat footprint_block(const Format& format, VkImageAspectFlags aspect) {
    palmetto::BlockFormat block = palmetto::block_format(format.format, aspect);
    if (format.rgb96 && block.bytes) block.bytes = palmetto::rgb96_bytes;
    return block;
}

// 96-bit textures are sampled and copied, nothing else yet: the flags creation refuses, and
// GetResourceAllocationInfo with it. The PC runtime (WARP) refuses unordered access and depth too; it does make
// 96-bit render targets (d3d12_rgb96_test prints both devices' answers).
bool rgb96_refused(const D3D12_RESOURCE_DESC& desc) {
    return vulkan_format(desc.Format, false).rgb96 &&
           (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                          D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
}

// B8G8R8X8: the BGRA8 Vulkan formats with the alpha byte unused. Its typeless family is a family of its own, so
// only a castable-format list lets a view cross between it and B8G8R8A8, although both land on the same VkFormat.
bool alpha_unused(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8X8_TYPELESS || format == DXGI_FORMAT_B8G8R8X8_UNORM ||
           format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

// The 8-bit four-channel UNORM and SRGB formats in either channel order, B8G8R8X8 included. A texture in one of them
// takes an RTV in another when its castable-format list names it: WARP and AMD's D3D12 driver store the clear or the
// draw in the view's channel order and encoding.
bool rgba8_any_order(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return true;
    default: return false;
    }
}

// The full mip chain's length for a texture this size.
UINT16 full_mips(UINT64 width, UINT height, UINT depth) {
    UINT16 mips = 0;
    for (UINT64 size = std::max<UINT64>({width, height, depth}); size; size >>= 1) ++mips;
    return mips;
}

// XR_BIAS is a display format: the PC runtime accepts it only as a single-mip, single-sample 2D texture, and
// refuses 3D, extra mips and multisampling in creation, allocation and footprints alike.
bool xr_bias_compatible(const D3D12_RESOURCE_DESC& desc) {
    if (desc.Format != DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) return true;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) return false;
    const UINT mips = desc.MipLevels ? desc.MipLevels : full_mips(desc.Width, desc.Height, 1);
    return mips == 1;
}

// The Xbox tiled layout word (slot 76, docs/d3d12_vulkan.md section 4.2) is outside D3D12_TEXTURE_LAYOUT's
// enumerators, so compare as an integer.
constexpr UINT kXboxTiledLayout = 0x101;

bool is_tiled_layout(D3D12_TEXTURE_LAYOUT layout) {
    return layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE || static_cast<UINT>(layout) == kXboxTiledLayout;
}

// A volume RTV covers W slices of one mip. UINT_MAX extends through the last slice of that mip.
palmetto::TargetView volume_rtv(palmetto::ResourceId id, const D3D12_RESOURCE_DESC& resource,
                               const D3D12_RENDER_TARGET_VIEW_DESC* view) {
    if (!(resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || vulkan_format(resource.Format, false).typeless)
        return {};
    if (view && (view->ViewDimension != D3D12_RTV_DIMENSION_TEXTURE3D ||
                 (view->Format != DXGI_FORMAT_UNKNOWN && view->Format != resource.Format))) return {};
    const D3D12_TEX3D_RTV slices = view ? view->Texture3D : D3D12_TEX3D_RTV{0, 0, UINT_MAX};
    if (slices.MipSlice >= resource.MipLevels) return {};
    const UINT depth = std::max(1u, UINT(resource.DepthOrArraySize) >> slices.MipSlice);
    if (slices.FirstWSlice >= depth) return {};
    const UINT remaining = depth - slices.FirstWSlice;
    const UINT count = slices.WSize == UINT_MAX ? remaining : slices.WSize;
    if (!count || count > remaining) return {};
    return {id, slices.MipSlice, slices.FirstWSlice, count};
}

palmetto::TargetView line_target_view(palmetto::ResourceId id, const D3D12_RESOURCE_DESC& resource,
                                     UINT mip, UINT first_layer, UINT layer_count) {
    if (resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D || mip >= resource.MipLevels ||
        first_layer >= resource.DepthOrArraySize)
        return {};
    const UINT remaining = UINT(resource.DepthOrArraySize) - first_layer;
    const UINT count = layer_count == UINT_MAX ? remaining : layer_count;
    if (!count || count > remaining) return {};
    return {id, mip, first_layer, count};
}

// ArraySize of a 2D-array RTV or DSV: UINT_MAX is every slice from the first on, as on the host. Any other count
// is kept, and UINT_MAX with a first slice past the array too, so an out-of-range view is refused as before.
UINT view_slices(const D3D12_RESOURCE_DESC& resource, UINT first_layer, UINT layer_count) {
    const bool remaining = layer_count == UINT_MAX && first_layer < resource.DepthOrArraySize;
    return remaining ? resource.DepthOrArraySize - first_layer : layer_count;
}

// The VkImage a texture description becomes, with its mip count already resolved (MipLevels != 0). Shared by
// texture creation and GetResourceAllocationInfo, so the sizes reported are the sizes later allocated.
// Keeps the optional VkImageFormatListCreateInfo alongside VkImageCreateInfo so info.pNext stays valid.
// attachment_usable: the GPU can render to `format`. A UAV texture then also gets COLOR_ATTACHMENT usage, because
// ClearUnorderedAccessView* with rectangles clears through an attachment (vkCmdClearAttachments).
struct TextureCreateInfo {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    std::array<VkFormat, 2> view_formats{};

    TextureCreateInfo(const D3D12_RESOURCE_DESC& desc, const Format& format, bool attachment_usable) {
        const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const bool one_d = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        info.flags = format.typeless ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        if (volume && (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
            info.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        info.imageType = volume ? VK_IMAGE_TYPE_3D : one_d ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
        info.format = format.format;
        info.extent = {std::uint32_t(desc.Width), one_d ? 1u : desc.Height,
                       volume ? UINT(desc.DepthOrArraySize) : 1u};
        info.mipLevels = desc.MipLevels;
        info.arrayLayers = volume ? 1 : desc.DepthOrArraySize;
        info.samples = VkSampleCountFlagBits(std::max(1u, desc.SampleDesc.Count));
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        // No B8G8R8X8 view is ever a UAV, so its typeless family needs no storage usage (not every GPU has it for BGRA8).
        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) && !alpha_unused(desc.Format)) {
            info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            if (!volume && info.samples == VK_SAMPLE_COUNT_1_BIT && attachment_usable)
                info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (format.uint_backed) {
            // The unsigned format owns the storage usage; views in the logical format restrict their usage (SAMPLED_BIT
            // for SRVs, COLOR_ATTACHMENT_BIT for RTVs).
            info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            view_formats = {format.format, format.uint_backed};
            format_list.viewFormatCount = 2;
            format_list.pViewFormats = view_formats.data();
            info.pNext = &format_list;
        }
    }

    TextureCreateInfo(const TextureCreateInfo&) = delete;
    TextureCreateInfo& operator=(const TextureCreateInfo&) = delete;
};

// The folder build_port recorded in shader_path.txt at `config`, or empty without one. Throws when the path cannot be
// converted: MSVC's STL reports a Windows path that is not UTF-8 as std::system_error, libstdc++ as filesystem_error.
std::filesystem::path shader_directory_from_file(const std::filesystem::path& config) {
    std::ifstream file(config, std::ios::binary);
    if (!file) return {};
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (text.empty() || text.find('\0') != std::string::npos) return {};

    // build_port records an absolute Unix path on Linux. Resolve it inside the running Wine prefix.
    if (text.front() == '/') {
        using WineDosPath = wchar_t* (CDECL*)(const char*);
        const auto to_dos = std::bit_cast<WineDosPath>(
            ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"));
        if (!to_dos) return {};
        const auto release = [](wchar_t* path) { ::HeapFree(::GetProcessHeap(), 0, path); };
        const std::unique_ptr<wchar_t, decltype(release)> path(to_dos(text.c_str()), release);
        if (path) return std::filesystem::path(path.get());
        return {};
    }
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// ---- per-process pipeline recipe cache (PALMETTO_PIPELINE_RECIPE_CACHE=1) --------------------------------
//
// A boot creates 51,786 graphics pipeline states and the driver says only 4,571 of those recipes are distinct (the
// console's derived-state path re-asks for the same description thousands of times, docs/pipeline_cache.md). The
// device's VkPipelineCache makes the repeat compiles cheap, but the layer still resolves the shaders, patches their
// SPIR-V, creates two VkShaderModules and a VkPipelineLayout and builds the VkPipeline for every one of the calls.
// This cache keeps the first result of a description and lets an identical description adopt it: the same
// VkPipeline, VkPipelineLayout and shader modules, reference-counted by the PipelineState objects that use them, with
// the replay-visible fields (formats, push-constant stages, code base copy, diagnostics) copied from the shared entry.
//
// The key is the device's content key for the description, `PipelineState::cache_key_` built by pipeline_key(): a
// SHA-256 over the description's state (blend/depth/raster/input layout/formats/sample state), the shader bytecode
// contents (the DXBC container checksum, or a hash of the bytes) and the root signature's content - no pointer or
// address is part of it, except that a *foreign* root signature (not one this layer created) is hashed by address, so
// such a PSO is never cached (PipelineState::cache_key_deterministic_).
//
// Off by default: with the switch unset no lookup happens and every PSO owns its Vulkan objects as before.
bool pipeline_recipe_cache_enabled() {
    static const bool enabled = [] {
        char text[4] = {};
        size_t length = 0;
        return getenv_s(&length, text, "PALMETTO_PIPELINE_RECIPE_CACHE") == 0 && length > 1 && text[0] != '0';
    }();
    return enabled;
}

// What a graphics pipeline is made from, so variants can be built at draw time. Namespace-scope (it was
// PipelineState::Recipe) so a shared entry can hold a copy of it.
struct GraphicsRecipe {
    bool mesh = false;  // no vertex input or input assembly state, and no dynamic topology or strides
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    // One per entry of create_graphics's stage table (VS, GS, PS), static_assert there; what `stages` chain.
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required_sizes[3]{};
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputBindingDivisorDescriptionEXT> divisors;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    VkPipelineRasterizationStateCreateInfo raster{};
    bool conservative = false;
    VkPipelineMultisampleStateCreateInfo multisample{};
    VkSampleMask sample_mask = ~0u;
    VkPipelineDepthStencilStateCreateInfo depth{};
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    VkPipelineColorBlendStateCreateInfo blend{};
};

// One created graphics pipeline and everything an identical description reuses: the pipeline, its layout, the shader
// modules (a lazy unbound-attachment/PS-control variant built at draw time needs them), the recipe for those variants,
// and the fields create_graphics computes while resolving the shaders. Reference-counted by the PipelineState objects
// that adopt it; the last release destroys the Vulkan objects exactly once: pipeline first, then modules, layout last.
struct SharedGraphicsPipeline {
    const palmetto::VulkanDeviceFunctions* vk = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkShaderModule> modules;
    GraphicsRecipe recipe;
    std::vector<palmetto::SubgroupRequest> subgroup_requests;
    VkShaderStageFlags push_stages = 0;
    VkDescriptorSet bindless_set = VK_NULL_HANDLE;
    std::vector<VkFormat> color_formats;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags depth_aspects = 0;
    std::uint32_t vertex_slots = 0;
    std::uint32_t zero_step_slots = 0;
    std::uint32_t strip_cut = 0;
    std::uint32_t table_windows = 0;
    std::uint32_t samples = 1;
    bool mesh = false;
    bool has_pixel_stage = false;
    std::uint32_t ps_images = 0;
    std::uint32_t vs_loads = 0;
    // The root signature's console user-SGPR placement (docs/vertex_buffer_va.md section 18): the same parameters at
    // their console slots, which the replay converts a graphics draw's recorded block into.
    bool console_ok = false;
    std::uint32_t console_parameter_count = 0;
    palmetto::GraphicsPipeline::ConsoleParameter console_parameter[16];
    palmetto::VertexPathInfo vertex_path;  // docs/vertex_path.md, the diagnostic the PSO reports without re-resolving
    std::vector<std::uint8_t> code_bytes;   // the program bytes the per-PSO code-base arena copy needs (empty: none)

    SharedGraphicsPipeline() = default;
    SharedGraphicsPipeline(const SharedGraphicsPipeline&) = delete;
    SharedGraphicsPipeline& operator=(const SharedGraphicsPipeline&) = delete;
    ~SharedGraphicsPipeline() {
        if (!vk || !device) return;
        if (pipeline) vk->vkDestroyPipeline(device, pipeline, nullptr);
        for (VkShaderModule module : modules) vk->vkDestroyShaderModule(device, module, nullptr);
        if (layout) vk->vkDestroyPipelineLayout(device, layout, nullptr);
    }
};

// The compute version: the pipeline, its layout and its module, plus the program bytes the per-PSO code-base copy
// needs.
struct SharedComputePipeline {
    const palmetto::VulkanDeviceFunctions* vk = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set = VK_NULL_HANDLE;
    std::vector<std::uint8_t> program_bytes;

    SharedComputePipeline() = default;
    SharedComputePipeline(const SharedComputePipeline&) = delete;
    SharedComputePipeline& operator=(const SharedComputePipeline&) = delete;
    ~SharedComputePipeline() {
        if (!vk || !device) return;
        if (pipeline) vk->vkDestroyPipeline(device, pipeline, nullptr);
        if (module) vk->vkDestroyShaderModule(device, module, nullptr);
        if (layout) vk->vkDestroyPipelineLayout(device, layout, nullptr);
    }
};

// PALMETTO_IMAGE_MEM_TRACE=1: the image-memory census (DeviceState::ImageMemoryCensus below). Off by default: with the
// switch unset the recording functions return on one cached boolean, so the default path is unchanged.
inline bool image_mem_trace_enabled() {
    static const bool enabled = [] {
        char text[4] = {};
        size_t length = 0;
        return getenv_s(&length, text, "PALMETTO_IMAGE_MEM_TRACE") == 0 && length > 1 && text[0] != '0';
    }();
    return enabled;
}

// PALMETTO_IMAGE_MEM_POOL=1: small textures share blocks of device memory (image_memory_pool.h); unset, every image
// keeps its own VkDeviceMemory, exactly as before.
inline bool image_memory_pool_enabled() {
    static const bool enabled = [] {
        char text[4] = {};
        size_t length = 0;
        return getenv_s(&length, text, "PALMETTO_IMAGE_MEM_POOL") == 0 && length > 1 && text[0] != '0';
    }();
    return enabled;
}

// PALMETTO_IMAGE_DEFERRED=1: a title-placed texture is created without memory and gets it at its first host write or
// before the first submission that could use it. Unset, the texture is backed at creation, as before.
inline bool deferred_image_backing_enabled() {
    static const bool enabled = [] {
        char text[4] = {};
        size_t length = 0;
        return getenv_s(&length, text, "PALMETTO_IMAGE_DEFERRED") == 0 && length > 1 && text[0] != '0';
    }();
    return enabled;
}

// The console-type switch the Series-S upscale-target translation is gated on (docs/decomp_renderer.md section 12).
// Read once, and only the Series-S answer arms it: the kind-7 FidelityFX branch and the upscale shape it creates are
// what the switch makes the title take. The loader owns the switch (port/src/pe_loader.cpp); this device reads the same
// variable so the translation can be off unless the console arm is on. Every other spelling, and an unset switch,
// leaves the default path and the R9G9B9E5 render target keeps its existing named refusal.
inline bool console_series_s_upscale_targets() {
    static const bool enabled = [] {
        char text[32] = {};
        (void)GetEnvironmentVariableA("PALMETTO_CONSOLE_TYPE", text, sizeof(text));
        return palmetto::console_type_is_series_s(text);
    }();
    return enabled;
}

// What a device's children need from it, kept apart from the COM class so children can hold a plain pointer.
class PipelineState;  // defined below: the live-pipeline budget evicts through it
class DescriptorHeap;  // defined below; DeviceState keeps the console-addressed shader-visible ones to refresh
class Resource;        // defined below; DeviceState keeps the title-placed textures to import (docs/texture_import.md)

struct DeviceState {
    VulkanDeviceInfo info;
    std::vector<std::uint32_t> families;

    template<typename CreateInfo>
    void share(CreateInfo& create) const {
        if (families.size() < 2) return;
        create.sharingMode = VK_SHARING_MODE_CONCURRENT;
        create.queueFamilyIndexCount = std::uint32_t(families.size());
        create.pQueueFamilyIndices = families.data();
    }
    std::unique_ptr<palmetto::VulkanContext> context;
    VkPhysicalDeviceMemoryProperties memory{};
    // D3D-96: upload-heap buffers prefer DEVICE_LOCAL | HOST_VISIBLE memory, so the GPU reads them from VRAM. Only
    // with Resizable BAR (palmetto::resizable_bar); PALMETTO_UPLOAD_SYSMEM=1 keeps them in system memory to compare.
    bool upload_in_vram = false;
    // D3D-299: the first texture or default-heap buffer that lands outside device-local memory is logged, once.
    mutable std::once_flag spill_logged;

    // Diagnostic, off by default (PALMETTO_IMAGE_MEM_TRACE=1): what the load stage's memory allocations look like, so
    // the decision to pool image memory is made on our own tree's numbers (docs/streamed_image_backing.md). Every image
    // `create_texture` makes is recorded with its description, its chosen memory type's flags and its size; every other
    // allocation the layer makes through `allocate` is counted. Lines are bounded (the first 256 images, and a summary
    // line every 128), so a boot that makes thousands cannot flood the log.
    struct ImageMemoryCensus {
        std::mutex mutex;
        std::uint64_t images = 0;
        std::uint64_t image_bytes = 0;
        std::uint64_t device_local = 0;
        std::uint64_t device_local_bytes = 0;
        std::uint64_t host_visible = 0;  // a CPU-visible texture (host-transfer or a mapped heap)
        std::uint64_t host_visible_bytes = 0;
        std::uint64_t host_only = 0;  // neither flag: the driver's own choice, counted apart
        std::uint64_t largest_image = 0;
        std::uint64_t render_target = 0;
        std::uint64_t depth_stencil = 0;
        std::uint64_t cpu_visible_images = 0;
        std::uint64_t pooled_images = 0;  // the ones whose memory is a range of a block
        std::uint64_t pooled_bytes = 0;
        std::uint64_t line_count = 0;
        std::uint64_t summary_count = 0;
        // Every successful DeviceState::allocate (images, buffers outside the arena, heaps, staging, the arena itself).
        std::uint64_t allocations = 0;
        std::uint64_t allocation_bytes = 0;
        std::chrono::steady_clock::time_point first_image{};
        bool started = false;
    };
    mutable ImageMemoryCensus image_census;

    // The census's recorders (the second is called from the const allocate()).
    void record_image_allocation(const D3D12_RESOURCE_DESC& desc, VkDeviceSize bytes, VkMemoryPropertyFlags flags,
                                 bool cpu_visible, bool pooled = false) {
        if (!image_mem_trace_enabled()) return;
        std::lock_guard lock(image_census.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (!image_census.started) {
            image_census.started = true;
            image_census.first_image = now;
        }
        const double seconds = std::chrono::duration<double>(now - image_census.first_image).count();
        ++image_census.images;
        image_census.image_bytes += bytes;
        if (pooled) {
            ++image_census.pooled_images;
            image_census.pooled_bytes += bytes;
        }
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            ++image_census.device_local;
            image_census.device_local_bytes += bytes;
        } else if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            ++image_census.host_visible;
            image_census.host_visible_bytes += bytes;
        } else {
            ++image_census.host_only;
        }
        if (bytes > image_census.largest_image) image_census.largest_image = bytes;
        if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ++image_census.render_target;
        if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ++image_census.depth_stencil;
        if (cpu_visible) ++image_census.cpu_visible_images;
        if (image_census.line_count < 256) {
            ++image_census.line_count;
            std::fprintf(stderr,
                         "[rt] image-mem: #%llu at %.1f s %llux%ux%u format %d flags 0x%x mips %u layers %u, %llu B, "
                         "memory type flags 0x%x%s%s%s%s\n",
                         static_cast<unsigned long long>(image_census.images), seconds,
                         static_cast<unsigned long long>(desc.Width), desc.Height, desc.DepthOrArraySize,
                         int(desc.Format), unsigned(desc.Flags), desc.MipLevels, desc.DepthOrArraySize,
                         static_cast<unsigned long long>(bytes), unsigned(flags),
                         (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " device-local" : "",
                         (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " host-visible" : "",
                         cpu_visible ? " cpu-visible" : "", pooled ? " pooled" : "");
        }
        if ((image_census.images <= 1 || image_census.images % 8 == 0) && image_census.summary_count < 512) {
            ++image_census.summary_count;
            std::fprintf(stderr,
                         "[rt] image-mem: %llu images, %.1f MiB (device-local %llu, %.1f MiB; host-visible %llu, "
                         "%.1f MiB; other %llu; pooled %llu, %.1f MiB; deferred waiting %llu), largest %.1f MiB, "
                         "%llu render targets, %llu depth targets, %llu CPU-visible; %llu allocations of all kinds, "
                         "%.1f MiB; %llu pool blocks, %.1f MiB, %llu ranges live, at %.1f s\n",
                         static_cast<unsigned long long>(image_census.images),
                         double(image_census.image_bytes) / (1024.0 * 1024.0),
                         static_cast<unsigned long long>(image_census.device_local),
                         double(image_census.device_local_bytes) / (1024.0 * 1024.0),
                         static_cast<unsigned long long>(image_census.host_visible),
                         double(image_census.host_visible_bytes) / (1024.0 * 1024.0),
                         static_cast<unsigned long long>(image_census.host_only),
                         static_cast<unsigned long long>(image_census.pooled_images),
                         double(image_census.pooled_bytes) / (1024.0 * 1024.0),
                         static_cast<unsigned long long>(deferred_waiting.load(std::memory_order_relaxed)),
                         double(image_census.largest_image) / (1024.0 * 1024.0),
                         static_cast<unsigned long long>(image_census.render_target),
                         static_cast<unsigned long long>(image_census.depth_stencil),
                         static_cast<unsigned long long>(image_census.cpu_visible_images),
                         static_cast<unsigned long long>(image_census.allocations),
                         double(image_census.allocation_bytes) / (1024.0 * 1024.0),
                         static_cast<unsigned long long>(pool_blocks_created.load(std::memory_order_relaxed)),
                         double(pool_bytes_created.load(std::memory_order_relaxed)) / (1024.0 * 1024.0),
                         static_cast<unsigned long long>(pool_ranges_live.load(std::memory_order_relaxed)), seconds);
        }
    }
    void record_allocation(VkDeviceSize bytes) const {
        if (!image_mem_trace_enabled()) return;
        std::lock_guard lock(image_census.mutex);
        ++image_census.allocations;
        image_census.allocation_bytes += bytes;
    }

    // D3D-299, shared by the dedicated and the pooled image path: the first allocation that preferred device-local
    // memory, needed no host access and got a type without it is logged once, so a run on a full GPU shows the spill.
    void note_spill_once(VkDeviceSize bytes, std::uint32_t type) const {
        std::call_once(spill_logged, [&] {
            const std::uint32_t heap = memory.memoryTypes[type].heapIndex;
            std::fprintf(stderr,
                         "[rt] d3d12: a %llu-byte allocation did not fit in device-local memory and went to memory "
                         "type %u (heap %u, %llu MiB); later ones are not logged\n",
                         static_cast<unsigned long long>(bytes), type, heap,
                         static_cast<unsigned long long>(memory.memoryHeaps[heap].size >> 20));
        });
    }

    // ---- the live-pipeline budget (pipeline_budget.h, docs/memory_growth.md) -----------------------------------
//
// A boot creates 51,786 console graphics pipeline states and the layer built a VkPipeline for every one of them at
// creation; the driver keeps per-pipeline host memory (measured: ~40 GB of process private bytes in 120 s, in
// nvoglv64.dll/nvgpucomp64.dll allocations under PipelineState::build). The frame loop binds a handful, so every graphics
// PSO is created with its recipe, modules and layout but no VkPipeline and is built at its first bind (the opt-in recipe
// cache keeps its eager build, so that slice's semantics are unchanged); a bind past the cap evicts the least recently
// bound live pipeline, which a later bind rebuilds from the retained recipe, modules and layout. The cap and the lazy
// build are one switch each, for the A/B: PALMETTO_PIPELINE_LIVE_CAP=0 rebuilds the old path exactly
// (PALMETTO_PIPELINE_LAZY_BUILD is the eager-build arm). Both defaults are the measured-correct ones: cap 16, lazy on.
//
// Lock order (docs/memory_growth.md, the default-boot freeze): a lazily built pipeline makes this path an exclusive
// writer of VulkanContext::resource_mutex_ from inside the frame loop (set_pipeline_handle() below). The replay holds that
// same lock shared for a whole execute(), so nothing the replay calls may take it again: a nested shared acquire blocks
// behind the queued writer while the writer waits for the replay's outer shared lock. The replay-side accessors are
// therefore lock-free by contract - heap_base() is the one this path made reachable.
inline std::size_t pipeline_live_cap() {
    static const std::size_t cap = [] {
        char value[16] = {};
        size_t length = 0;
        // Unset: the measured-correct default (16 live, docs/memory_growth.md). Set: the value; 0 leaves the budget off
        // and restores the pre-slice eager path exactly, for the A/B.
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_LIVE_CAP") != 0 || length <= 1)
            return palmetto::PipelineBudget::kDefaultCap;
        return static_cast<std::size_t>(std::strtoull(value, nullptr, 10));
    }();
    return cap;
}

inline bool pipeline_lazy_build_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        size_t length = 0;
        // Unset: on (build at the first bind, under the live cap, docs/memory_growth.md). Set with a leading 0: every
        // graphics pipeline is built eagerly at creation instead, the A/B's old path.
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_LAZY_BUILD") != 0 || length <= 1) return true;
        return value[0] != '0';
    }();
    return enabled;
}

// ---- off-thread compute-pipeline compilation (docs/deferred_compile.md) -----------------------------------
//
// PALMETTO_PIPELINE_DEFER_COMPILE=1: CreateComputePipelineState resolves the recipe, creates the shader module and
// the pipeline layout and registers the replay entry, but does not call vkCreateComputePipelines; a compile worker of
// this device makes that driver call off the title's threads, and the submission that first names the pipeline waits
// for it on the submitting (render) thread. Off by default; unset is the eager path exactly. The bound and the worker
// count are separate switches for the A/B; a creation past the bound falls back to the synchronous path.
inline bool pipeline_defer_compile_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_DEFER_COMPILE") != 0 || length <= 1) return false;
        return value[0] != '0';
    }();
    return enabled;
}

inline unsigned pipeline_compile_worker_count() {
    static const unsigned count = [] {
        char value[8] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_COMPILE_WORKERS") != 0 || length <= 1) return 1u;
        const unsigned parsed = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
        return parsed < 1 ? 1u : (parsed > 8 ? 8u : parsed);
    }();
    return count;
}

inline std::size_t pipeline_compile_queue_bound() {
    static const std::size_t bound = [] {
        char value[16] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_COMPILE_BOUND") != 0 || length <= 1) return std::size_t{256};
        const unsigned long parsed = std::strtoul(value, nullptr, 10);
        return parsed < 1 ? std::size_t{1} : static_cast<std::size_t>(parsed);
    }();
    return bound;
}

// PALMETTO_PIPELINE_PREWARM=1 (docs/vertex_buffer_va.md section 13): the deferred compile queue serves the newest
// queued build first and the submit path knows a deferred pipeline the worker already finished, so a pipeline the
// title created just before naming it is ready before the submission instead of stalling it. The deferred switch is
// still the only way in (PALMETTO_PIPELINE_DEFER_COMPILE); this only changes the order the queue's workers take and
// the counter's meaning. Off by default: the default order and counters are the measured FIFO arm.
inline bool pipeline_prewarm_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_PREWARM") != 0 || length <= 1) return false;
        return value[0] != '0';
    }();
    return enabled;
}

// PALMETTO_PIPELINE_CALL_STATS=<seconds>: a per-thread report of who enters pipeline creation and how long each caller
// spends inside the driver's vkCreate*Pipelines call, every <seconds> (default 5, off when unset or "0"). Measurement
// only: off is the default path exactly.
inline bool pipeline_call_stats_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_CALL_STATS") != 0 || length <= 1) return false;
        return value[0] != '0';
    }();
    return enabled;
}

inline double pipeline_call_stats_interval_s() {
    static const double interval = [] {
        char value[8] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_CALL_STATS") != 0 || length <= 1) return 5.0;
        const double parsed = std::strtod(value, nullptr);
        return parsed >= 1.0 ? parsed : 5.0;
    }();
    return interval;
}

// ---- the non-blocking submission hold (docs/peer_port_streaming.md) -----------------------------------------
//
// PALMETTO_PIPELINE_HOLD=1: a submission that names a deferred compute pipeline the compile queue has not finished
// is held instead of waited for (docs/deferred_compile.md's submit-side wait is the measured stall this removes from
// the title's thread). The held batch is replayed in submission order, by the next submit/present on the submitting
// thread or by this device's hold flusher, once every pipeline it names has been built. Two budgets bound it:
// PALMETTO_PIPELINE_HOLD_BOUND submissions at once, and PALMETTO_PIPELINE_HOLD_BUDGET_MS held before the old
// synchronous wait is forced. Off by default: unset is the current path exactly (no hold, no flusher thread).
inline bool pipeline_hold_enabled() {
    static const bool enabled = [] {
        char value[16] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_HOLD") != 0 || length <= 1) return false;
        return palmetto::pipeline_hold_switch_enabled(value);
    }();
    return enabled;
}

inline std::size_t pipeline_hold_bound() {
    static const std::size_t bound = [] {
        char value[16] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_HOLD_BOUND") != 0 || length <= 1)
            return palmetto::PipelineHoldSettings{}.bound;
        return palmetto::pipeline_hold_bound(value);
    }();
    return bound;
}

inline std::uint64_t pipeline_hold_budget_ms() {
    static const std::uint64_t budget = [] {
        char value[16] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_PIPELINE_HOLD_BUDGET_MS") != 0 || length <= 1)
            return static_cast<std::uint64_t>(palmetto::PipelineHoldSettings{}.budget_us / 1000);
        return palmetto::pipeline_hold_budget_ms(value);
    }();
    return budget;
}

// ---- pooled and deferred image memory (docs/streamed_image_backing.md) ------------------------------------
    //
    // PALMETTO_IMAGE_MEM_POOL=1: small textures that are neither attachments nor CPU-visible share 64 MiB blocks of
    // device memory as ranges (image_memory_pool.h), instead of one vkAllocateMemory and one vkFreeMemory each;
    // everything else keeps its own allocation. PALMETTO_IMAGE_DEFERRED=1: a title-placed texture is created with no
    // memory and gets it at its first host write or before the first submission that could use it.
    //
    // Both switches are off by default and leave the default path byte-identical: no block is allocated, no image is
    // deferred and every lookup below is one false branch.
    std::unique_ptr<palmetto::ImageMemoryPool<VkDeviceMemory>> image_memory_pool;
    PFN_vkGetImageMemoryRequirements2 image_requirements2 = nullptr;  // dedicated-allocation check, Vulkan 1.1+
    std::uint64_t pool_block_bytes = 0;                               // the block images share; 0: no pool
    // Live pool counters for the census: a block allocated, a range handed out and a range given back. Kept in the
    // device so the summary line needs no lock of the pool's.
    std::atomic<std::uint64_t> pool_blocks_created{0}, pool_bytes_created{0}, pool_ranges_live{0};
    // Images whose backing is deferred: created without memory and waited for by the queue hook. Guarded by
    // deferred_images_mutex, which the queue hook, the import path and a resource's destruction all take.
    std::mutex deferred_images_mutex;
    std::vector<Resource*> deferred_images;
    std::atomic<std::uint64_t> deferred_bound_count{0}, deferred_bound_bytes{0}, deferred_waiting{0};
    // The live-pipeline budget (pipeline_budget.h, docs/memory_growth.md): the first `pipeline_live_cap` graphics PSOs
    // are built at creation, later ones at their first bind, and a bind past the cap evicts the least recently bound
    // live pipeline (never one bound within the budget's grace). A pipeline state keeps its recipe, modules and layout,
    // so an evicted pipeline is rebuilt on its next bind; the old object is destroyed through the deferred queue.
    std::mutex pipeline_budget_mutex;
    palmetto::PipelineBudget pipeline_budget{pipeline_live_cap(), palmetto::PipelineBudget::kDefaultGrace};
    std::unordered_map<std::uintptr_t, PipelineState*> pipeline_owners;
    std::atomic<std::uint64_t> graphics_pipelines_created{0};
    std::uint64_t pipelines_built = 0, pipelines_evicted = 0, pipelines_rebuilt = 0;
    bool defer_graphics_build();          // true: this graphics PSO is built at its first bind, not now
    bool note_pipeline_built(PipelineState* state);  // records a build and evicts past the cap; false if it was evicted
    void forget_pipeline(std::uintptr_t id);         // the PSO died: drop it from the budget
    std::string pipeline_census_text();
    void start_image_memory_pool();
    bool bind_deferred_images();  // the queue hook: false refuses the submission rather than using an unbound image
    void add_deferred_image(Resource* resource);
    void remove_deferred_image(Resource* resource);

    // docs/deferred_compile.md: off-thread compute-pipeline compilation. The queue is bounded and its workers start on
    // the first deferred creation; the map holds the task per PSO id, so the submission path can find (and wait for)
    // the pipeline a list names. The counters answer "who blocks in pipeline creation and for how long".
    palmetto::CompileQueue compile_queue{pipeline_compile_queue_bound(), pipeline_prewarm_enabled()};
    std::atomic<bool> compile_queue_started{false};
    std::mutex deferred_compute_mutex;
    std::unordered_map<std::uintptr_t, std::shared_ptr<palmetto::CompileQueue::Task>> deferred_compute_tasks;
    std::atomic<std::uint64_t> deferred_built_count{0}, deferred_failed_count{0}, deferred_fallback_count{0};
    void start_compile_workers();
    std::shared_ptr<palmetto::CompileQueue::Task> defer_compute_build(PipelineState* state, std::function<bool()> build);
    void forget_deferred_compute(std::uintptr_t id);
    void prepare_deferred_compute_pipelines(std::span<const palmetto::CommandList* const> lists);
    std::string deferred_compile_text();
    void stop_compile_workers();
    // The non-blocking submission hold (docs/peer_port_streaming.md): the queue's hooks read the deferred task map, a
    // finished build wakes the hold flusher, and the flusher releases the held submissions off the title's threads.
    bool collect_pending_pipelines(std::span<const palmetto::CommandList* const> lists,
                                   std::vector<std::uintptr_t>& out);
    bool pipelines_finished(std::span<const std::uintptr_t> ids);
    void pipeline_build_finished(std::uintptr_t id);
    void notify_pipeline_hold();
    void start_pipeline_hold_flusher();
    void stop_pipeline_hold_flusher();
    std::thread hold_flusher;
    std::mutex hold_flusher_mutex;
    std::condition_variable hold_flusher_cv;
    bool hold_flusher_started = false;
    bool hold_flusher_stop = false;
    bool hold_flusher_pending = false;
    // The per-thread call report (PALMETTO_PIPELINE_CALL_STATS): created and printed on first use and owned by the
    // device, so its report can append the compile queue's counters; stopped in ~Device before the Vulkan device goes.
    std::mutex call_stats_mutex;
    palmetto::PipelineCallStats* call_stats = nullptr;
    std::thread call_stats_thread;
    std::atomic<bool> call_stats_started{false};
    std::atomic<bool> call_stats_stop{false};
    palmetto::PipelineCallStats* pipeline_stats() {
        if (!pipeline_call_stats_enabled()) return nullptr;
        if (!call_stats_started.load(std::memory_order_acquire)) start_pipeline_call_stats();
        return call_stats;
    }
    void start_pipeline_call_stats();
    void stop_pipeline_call_stats();

    // The shader-visible descriptor heaps the title placed at a console address (device slot 14): their descriptors are
    // CPU memory the GPU cannot address, so each keeps an arena mirror that a queue refreshes before a submission
    // (docs/page_address_space.md). Guarded by console_heaps_mutex; sync_console_heaps is defined after DescriptorHeap.
    std::mutex console_heaps_mutex;
    std::vector<DescriptorHeap*> console_heaps;
    void add_console_heap(DescriptorHeap* heap) {
        std::lock_guard lock(console_heaps_mutex);
        if (std::find(console_heaps.begin(), console_heaps.end(), heap) == console_heaps.end())
            console_heaps.push_back(heap);
    }
    void remove_console_heap(DescriptorHeap* heap) {
        std::lock_guard lock(console_heaps_mutex);
        std::erase(console_heaps, heap);
    }
    void sync_console_heaps();
    // A title-placed texture's stand-in (device slot 133, PALMETTO_TEXTURE_IMPORT): the title's pages hold row-major
    // subresource bytes, so they are copied into the committed image before the first submission that can sample it
    // (docs/texture_import.md). Pending entries are drained by the queue hook below; bounded and logged.
    std::mutex placed_texture_mutex;
    std::vector<Resource*> placed_textures_pending;
    // The per-submission refresh of title-written textures (docs/peer_port_movie.md section 8; port/src/plane_refresh.h
    // is the pure half). A texture whose pages were imported once into its stand-in is tracked here, with the shim's own
    // reference kept for the process's lifetime - the list is bounded by the tracker's ranges and the shadow budget, and
    // that bounded hold is what makes the per-submission walk safe against a title releasing the resource. The walk
    // compares the title's pages against the imported shadow and re-uploads only the changed row band; with
    // PALMETTO_PLANE_REFRESH unset the list is never filled and the walk is not entered (the tree's one-shot import).
    std::mutex placed_refresh_mutex;
    std::vector<Resource*> placed_refresh;
    std::uint64_t placed_refresh_shadow_bytes = 0;  // the shadows' bytes: the bound on what the refresh may hold
    std::size_t placed_refresh_next = 0;            // round-robin start, moved by one each submission
    palmetto::PlaneRefreshCounters placed_refresh_counters;
    std::uint64_t placed_refresh_walks = 0;
    // The switches, parsed once per process: PALMETTO_PLANE_REFRESH=hash|watch and PALMETTO_PLANE_REFRESH_TRACE.
    bool title_alias_watch_enabled() {
        static const bool enabled = [] {
            char value[16] = {};
            std::size_t length = 0;
            if (getenv_s(&length, value, "PALMETTO_XMEM_ALIAS_WATCH") != 0 || length <= 1) return false;
            return rt::xmem::page_watch_alias_switch_on(value);
        }();
        if (enabled) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12: PALMETTO_XMEM_ALIAS_WATCH=1: watched texture pages include every current "
                             "xmem view; mapping changes rebuild the view set, with bounded failure using the hash "
                             "fallback\n");
        }
        return enabled;
    }
    palmetto::PlaneRefreshPlan plane_refresh_plan() {
        static const palmetto::PlaneRefreshPlan plan = [] {
            char refresh[24] = {};
            char trace[24] = {};
            std::size_t refresh_length = 0, trace_length = 0;
            const errno_t refresh_error = getenv_s(&refresh_length, refresh, "PALMETTO_PLANE_REFRESH");
            const errno_t trace_error = getenv_s(&trace_length, trace, "PALMETTO_PLANE_REFRESH_TRACE");
            const palmetto::PlaneRefreshPlan parsed = palmetto::plane_refresh_plan_from(
                refresh_error == 0 && refresh_length > 1 ? refresh : nullptr,
                trace_error == 0 && trace_length > 1 ? trace : nullptr);
            if (refresh_error == 0 && refresh_length > 1 && !parsed.enabled())
                std::fprintf(stderr,
                             "[rt] d3d12: PALMETTO_PLANE_REFRESH=%s is not hash or watch: title-written texture pages "
                             "keep the one-shot import (no per-submission refresh)\n",
                             refresh);
            if (!parsed.enabled()) return parsed;
            // The guard-page tracker has one change flag for every range it arms, and the descriptor-mirror gate
            // (PALMETTO_CARD_COPY_WATCH) polls it first in a submission and suspends every range around its own scan. A
            // texture write would therefore be consumed by that poll and missed here, and a write inside its suspended
            // window would not be recorded at all: with the mirror gate on, watch mode falls back to the hash compare
            // (which needs no shared signal), and says so.
            palmetto::PlaneRefreshPlan plan = parsed;
            char mirror_watch[24] = {};
            std::size_t mirror_length = 0;
            if (parsed.mode == palmetto::PlaneRefreshMode::watch &&
                getenv_s(&mirror_length, mirror_watch, "PALMETTO_CARD_COPY_WATCH") == 0 && mirror_length > 1 &&
                mirror_watch[0] != '0') {
                plan.mode = palmetto::PlaneRefreshMode::hash;
                std::fprintf(stderr,
                             "[rt] d3d12: PALMETTO_PLANE_REFRESH=watch with PALMETTO_CARD_COPY_WATCH set: the "
                             "descriptor-mirror gate shares the tracker's single change flag and its suspended window, "
                             "so the texture refresh uses the hash compare instead\n");
            }
            std::fprintf(stderr,
                         "[rt] d3d12: PALMETTO_PLANE_REFRESH=%s: title-written texture pages are refreshed before "
                         "the submission that samples them%s\n",
                         plan.mode == palmetto::PlaneRefreshMode::watch ? "watch" : "hash",
                         plan.arms_tracker() ? " (the guard-page tracker is the pre-filter)" : "");
            return plan;
        }();
        return plan;
    }
    // Unregisters a tracked texture: the walk dropped it (a shadow size that no longer matches, or a refused range) and
    // the reference the pending list took is released. Defined after Resource, whose shadow size it reads.
    void untrack_placed_refresh(Resource* resource);
    // The per-submission walk (defined after Resource: it calls the resource's own refresh). A no-op with the switch off.
    void refresh_placed_textures();

    // Diagnostic (docs/frame_capture.md, the colour path): the resources the title placed through its console creation path,
    // by the title VA they were placed at, so a T# descriptor a shader fetches itself (out of a published block, not a
    // table slot) can be given a layer image from the resource it names. Registered on creation, removed on release.
    std::mutex placed_resources_mutex;
    std::unordered_map<std::uint64_t, Resource*> placed_resources;
    std::atomic<std::uint64_t> texture_imports{0}, texture_import_bytes{0};
    void import_placed_textures();
    // Page content copied from the title's pages into the arena stand-ins (docs/page_content.md), for the report.
    std::atomic<std::uint64_t> pages_pulled{0}, pages_pulled_bytes{0};
    // Title pages a command named directly (docs/vertex_buffer_va.md): the VkBuffers imported over the title's own
    // pages at first use, as (buffer, memory) for this device's lifetime; the teardown destroys them with the arena.
    std::mutex imported_title_mutex;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> imported_title_buffers;
    // The copy-source pulls of docs/page_content.md, counted apart from the draw-read pulls: requests (every copy
    // source the replay asks about) and pulls (those whose arena VA a title range stands in for).
    std::atomic<std::uint64_t> copy_source_requests{0};
    std::atomic<std::uint64_t> copy_source_pulls{0}, copy_source_bytes{0};    // What D3D12_OPTIONS1 answers with: the physical device's own subgroup facts, read on first use.
    struct SubgroupFacts {
        UINT lane_min = 0;
        UINT lane_max = 0;
        bool wave_ops = false;
    };
    SubgroupFacts subgroups;
    std::once_flag subgroups_once;
    // VkPhysicalDeviceMeshShaderPropertiesEXT, read on first use by a mesh PSO; zeros when it cannot be asked.
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    bool mesh_properties_read = false;
    std::mutex mesh_properties_mutex;
    PFN_vkGetPhysicalDeviceFormatProperties format_properties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties image_format_properties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 image_format_properties2 = nullptr;
    PFN_vkGetDeviceImageMemoryRequirements image_requirements = nullptr;  // Vulkan 1.3 or VK_KHR_maintenance4
    LUID luid{};       // VkPhysicalDeviceIDProperties::deviceLUID; zero when the driver reports none
    // An integrated GPU (VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU): D3D12_FEATURE_ARCHITECTURE answers UMA, the custom
    // heap equivalents are in MEMORY_POOL_L0 and custom heaps in L1 are refused, as the host runtime does there.
    bool uma = false;
    // VkPipelineCreationFeedback is core in Vulkan 1.3 (PALMETTO_PIPELINE_PROFILE reads it; a pre-1.3 device would
    // need VK_EXT_pipeline_creation_feedback, which this layer does not enable, so it reports "without feedback").
    bool pipeline_creation_feedback = false;
    palmetto::cached_pso::Identity pso_identity;  // what GetCachedBlob records and CachedPSO is checked against
    std::once_flag pipeline_cache_once;
    VkPipelineCache pipeline_cache_handle = VK_NULL_HANDLE;
    // PALMETTO_PIPELINE_CACHE: the file this device's cache blob is saved to; null unless the switch names one.
    std::shared_ptr<PipelineCacheOwner> pipeline_cache_owner;

    VkPipelineCache pipeline_cache() {
        std::call_once(pipeline_cache_once, [this] {
            const auto& vk = context->vk;
            if (!vk.vkCreatePipelineCache || !vk.vkDestroyPipelineCache) return;
            const PipelineCacheFileConfig& file = pipeline_cache_file_config();
            if (!file.enabled) {  // today's path, byte-identical: the build stage's seed, else an empty cache
                pipeline_cache_handle = create_seeded_pipeline_cache(vk.vkCreatePipelineCache, info.device, pso_identity);
                if (pipeline_cache_handle) return;
                const VkPipelineCacheCreateInfo create{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
                if (vk.vkCreatePipelineCache(info.device, &create, nullptr, &pipeline_cache_handle) != VK_SUCCESS)
                    pipeline_cache_handle = VK_NULL_HANDLE;
                return;
            }
            // Opt-in persistence: the previous run's blob when it is this device's, else the build stage's seed, else
            // empty. Every failure lands on the same path the layer had before (nothing fails the boot).
            pipeline_cache_handle = create_persistent_pipeline_cache(vk.vkCreatePipelineCache, info.device, pso_identity, file);
            if (!pipeline_cache_handle) {
                pipeline_cache_handle = create_seeded_pipeline_cache(vk.vkCreatePipelineCache, info.device, pso_identity);
                if (!pipeline_cache_handle) {
                    const VkPipelineCacheCreateInfo create{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
                    if (vk.vkCreatePipelineCache(info.device, &create, nullptr, &pipeline_cache_handle) != VK_SUCCESS)
                        pipeline_cache_handle = VK_NULL_HANDLE;
                }
            }
            if (!pipeline_cache_handle) return;
            // The file is written by this device from now on: its blob in the cache includes everything it accepted.
            auto owner = std::make_shared<PipelineCacheOwner>();
            owner->device = info.device;
            owner->cache = pipeline_cache_handle;
            owner->path = file.path;
            owner->max_bytes = file.max_bytes;
            owner->get_data = vk.vkGetPipelineCacheData;  // a Vulkan 1.0 entry point; null only on a broken loader
            owner->last_save = PipelineClock::now();
            // Registered only when this device owns the file: a second device on the same path keeps its cache in
            // memory but does not save it (the registry logs why).
            if (owner->get_data && register_pipeline_cache(owner)) pipeline_cache_owner = owner;
        });
        // Without EXTERNALLY_SYNCHRONIZED_BIT the driver synchronizes concurrent pipeline creation using this cache.
        return pipeline_cache_handle;
    }

    // The device's final write: with a clean exit the current blob is saved here (the periodic path covers a run that
    // is killed at its timeout). Called before vkDestroyPipelineCache, while the blob is still readable.
    void close_pipeline_cache() {
        if (!pipeline_cache_owner) return;
        unregister_pipeline_cache(pipeline_cache_owner);
        pipeline_cache_owner.reset();
    }

    // ---- per-process recipe cache (PALMETTO_PIPELINE_RECIPE_CACHE=1) -----------------------------------------
    // Keyed by PipelineState::cache_key_, the content key of the description (pipeline_key). The maps hold weak
    // references: the PipelineState objects own the entries through their shared_ptr, so the last PSO to release a
    // recipe destroys its Vulkan objects, and a cache entry never outlives its users. Lookups are by value; the entry
    // is immutable once inserted, so adopting one from another thread is safe (the map itself has its own mutex).
    std::mutex recipe_cache_mutex;
    std::unordered_map<std::string, std::weak_ptr<SharedGraphicsPipeline>> graphics_recipes;
    std::unordered_map<std::string, std::weak_ptr<SharedComputePipeline>> compute_recipes;
    std::size_t recipe_inserts = 0;

    std::shared_ptr<SharedGraphicsPipeline> find_graphics_recipe(const std::string& key) {
        std::lock_guard lock(recipe_cache_mutex);
        const auto it = graphics_recipes.find(key);
        if (it == graphics_recipes.end()) return {};
        std::shared_ptr<SharedGraphicsPipeline> entry = it->second.lock();
        if (!entry) graphics_recipes.erase(it);
        return entry;
    }

    std::shared_ptr<SharedComputePipeline> find_compute_recipe(const std::string& key) {
        std::lock_guard lock(recipe_cache_mutex);
        const auto it = compute_recipes.find(key);
        if (it == compute_recipes.end()) return {};
        std::shared_ptr<SharedComputePipeline> entry = it->second.lock();
        if (!entry) compute_recipes.erase(it);
        return entry;
    }

    // Inserts or replaces the entry for `key` and returns the stored pointer. A thread that lost a race keeps its own
    // entry (both stay correct; the map points at the latest one). Dead entries are swept periodically so a test that
    // creates and destroys many states does not grow the map.
    std::shared_ptr<SharedGraphicsPipeline> add_graphics_recipe(const std::string& key,
                                                               std::shared_ptr<SharedGraphicsPipeline> entry) {
        std::lock_guard lock(recipe_cache_mutex);
        graphics_recipes[key] = entry;
        if (++recipe_inserts % 256 == 0) {
            for (auto it = graphics_recipes.begin(); it != graphics_recipes.end();)
                it = it->second.expired() ? graphics_recipes.erase(it) : std::next(it);
            for (auto it = compute_recipes.begin(); it != compute_recipes.end();)
                it = it->second.expired() ? compute_recipes.erase(it) : std::next(it);
        }
        return entry;
    }

    std::shared_ptr<SharedComputePipeline> add_compute_recipe(const std::string& key,
                                                             std::shared_ptr<SharedComputePipeline> entry) {
        std::lock_guard lock(recipe_cache_mutex);
        compute_recipes[key] = entry;
        return entry;
    }

    QueueClock clock;  // what every queue's GetClockCalibration uses
    PFN_vkGetPhysicalDeviceFormatProperties2 format_properties2 = nullptr;
    // True when VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 lacks native STORAGE_IMAGE but mutable R32_UINT+{R32_UINT,E5B9G9R9}
    // is supported, so R9G9B9E5_SHAREDEXP UAV textures are backed by R32_UINT with separate storage/sampled views.
    bool r9g9b9e5_uav_emulated = false;
    // The same for B5G5R5A1_UNORM: VK_FORMAT_A1R5G5B5_UNORM_PACK16 without STORAGE_IMAGE, backed by mutable R16_UINT.
    bool b5g5r5a1_uav_emulated = false;
    // The same for the 8-bit sRGB formats: VK_FORMAT_R8G8B8A8_SRGB / B8G8R8A8_SRGB without STORAGE_IMAGE (as on most
    // GPUs), backed by a mutable image of the UNORM format of the same order. The host runtime makes such textures.
    bool rgba8_srgb_uav_emulated = false;
    bool bgra8_srgb_uav_emulated = false;

    // Cached physical-device no-attachment sample counts for D3D12_OPTIONS19.
    UINT no_output_sample_counts = 0;
    std::uint32_t f16_half_top_location = 29;
    float max_sampler_lod_bias = 0;
    D3D12_CONSERVATIVE_RASTERIZATION_TIER conservative_raster_tier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
    bool conservative_post_depth_coverage = false;
    // VkPhysicalDeviceDepthStencilResolveProperties: the modes a depth or stencil ResolveSubresourceRegion can use.
    VkResolveModeFlags depth_resolve_modes = 0;
    VkResolveModeFlags stencil_resolve_modes = 0;

    // VK_EXT_host_image_copy; null unless the device enabled it and its copies accept GENERAL.
    PFN_vkCopyMemoryToImageEXT copy_memory_to_image = nullptr;
    PFN_vkCopyImageToMemoryEXT copy_image_to_memory = nullptr;
    PFN_vkTransitionImageLayoutEXT transition_image_layout = nullptr;
    // VK_EXT_external_memory_host; null unless info.host_pointer_alignment is set and the entry point loaded.
    PFN_vkGetMemoryHostPointerPropertiesEXT get_host_pointer_properties = nullptr;

    // WriteToSubresource/ReadFromSubresource without host image copy: one staging copy at a time, submitted to
    // the device's VkQueue and waited for. Created on first use.
    std::mutex staging_mutex;
    std::unique_ptr<palmetto::VulkanQueue> staging_queue;

    // The GPU address arena (docs/d3d12_vulkan.md section 3.3): one host-visible, device-addressable VkBuffer that
    // stands in for the console's GPU address space. A console VA is an offset in it, and heap_base (push-constant
    // dwords 32-33) is its device address. It is at most 4 GiB, so every VA fits the 32-bit descriptor-table
    // pointers some shaders use, and no larger than the device's maxBufferSize and maxMemoryAllocationSize
    // (src/arena_size.h). The first 64 KiB are never handed out, so no resource has VA 0.
    struct Arena {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint8_t* mapped = nullptr;
        VkDeviceAddress address = 0;
        VkDeviceSize size = 0;
        std::uint32_t memory_type = 0;
        std::mutex mutex;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size

        // The high-water mark (D3D-55, open question 5): the arena's size is a guess until a run measures it.
        // `peak_end` is the highest end offset ever handed out, the size the arena must have; `peak_in_use` is the
        // most bytes live at once, which differs from it by fragmentation.
        bool log = false;  // PALMETTO_ARENA_LOG=1: one line per VA handed out
        VkDeviceSize in_use = 0, peak_in_use = 0, peak_end = 0;
        VkDeviceSize use_in_use[4] = {}, use_peak[4] = {};
        std::uint64_t allocations = 0, failures = 0;
    } arena;

    // The arena staging copies of title spans a translated shader must fetch by a heap-relative address
    // (docs/vertex_buffer_va.md section 18.3): one range per title VA, re-copied on each use. Bounded to eight ranges.
    struct StagedSpan {
        std::uint64_t bytes = 0;
        std::uint64_t offset = 0;
    };
    std::mutex staged_mutex_;
    std::unordered_map<std::uint64_t, StagedSpan> staged_spans_;

    std::uint32_t sparse_buffer_memory_types = 0;
    bool sparse_image_tiles = false;

    bool tiles_share_arena() const { return (sparse_buffer_memory_types & (1u << arena.memory_type)) != 0; }

    VkBufferCreateInfo sparse_buffer_info(VkDeviceSize bytes) const {
        VkBufferCreateInfo result{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        result.size = bytes;
        result.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT |
                       VK_BUFFER_CREATE_SPARSE_ALIASED_BIT;
        result.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (info.conditional_rendering) result.usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
        // D3D12 reserved buffers are visible to DIRECT, COPY and COMPUTE queues without exposing Vulkan
        // queue-family ownership. Their tile bindings and accesses may use distinct families, so match the
        // CONCURRENT sharing policy used for ordinary buffers and the GPU address arena.
        share(result);
        return result;
    }

    // What an arena range holds, for the high-water report (D3D-55).
    enum ArenaUse { arena_buffer, arena_descriptor_heap, arena_shader, arena_heap };
    static constexpr const char* kArenaUseNames[4] = {"buffer", "descriptor heap", "shader", "heap"};

    // A zero-size range is refused: it would share its offset with the next range, and its release would collide
    // with that range's free entry. The fit test cannot wrap: free blocks lie inside the arena (below 4 GiB).
    bool arena_allocate(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize& out, ArenaUse use) {
        if (!bytes || bytes > arena.size) return false;
        std::lock_guard lock(arena.mutex);
        for (auto it = arena.free.begin(); it != arena.free.end(); ++it) {
            const VkDeviceSize start = (it->first + alignment - 1) / alignment * alignment;
            const VkDeviceSize end = it->first + it->second;
            if (start >= end || bytes > end - start) continue;
            const VkDeviceSize block = it->first;
            arena.free.erase(it);
            if (start > block) arena.free[block] = start - block;
            if (start + bytes < end) arena.free[start + bytes] = end - (start + bytes);
            out = start;
            ++arena.allocations;
            arena.in_use += bytes;
            arena.peak_in_use = std::max(arena.peak_in_use, arena.in_use);
            arena.peak_end = std::max(arena.peak_end, start + bytes);
            arena.use_in_use[use] += bytes;
            arena.use_peak[use] = std::max(arena.use_peak[use], arena.use_in_use[use]);
            if (arena.log)
                std::fprintf(stderr, "[rt] d3d12: arena %s VA 0x%llx + 0x%llx\n", kArenaUseNames[use],
                             (unsigned long long)start, (unsigned long long)bytes);
            return true;
        }
        ++arena.failures;
        if (arena.log)
            std::fprintf(stderr, "[rt] d3d12: arena %s: no room for 0x%llx bytes\n", kArenaUseNames[use],
                         (unsigned long long)bytes);
        return false;
    }

    // A console VA outside the arena (an upload or readback buffer: its device address minus heap_base).
    void log_va(const char* what, std::uint64_t va, VkDeviceSize bytes) {
        if (arena.log)
            std::fprintf(stderr, "[rt] d3d12: %s VA 0x%llx + 0x%llx (outside the arena)\n", what,
                         (unsigned long long)va, (unsigned long long)bytes);
    }

    // Printed once at device release, so a run reports the size the arena needs.
    void report_arena() {
        std::lock_guard lock(arena.mutex);
        if (!arena.size) return;
        const double mib = 1.0 / (1 << 20);
        std::fprintf(stderr,
                     "[rt] d3d12: GPU address arena: peak end %.1f of %.1f MiB (%.1f%%), peak live %.1f MiB "
                     "(buffers %.1f, descriptor heaps %.1f, shaders %.1f, heaps %.1f), %llu allocations, %llu refused\n",
                     arena.peak_end * mib, arena.size * mib, 100.0 * double(arena.peak_end) / double(arena.size),
                     arena.peak_in_use * mib, arena.use_peak[arena_buffer] * mib,
                     arena.use_peak[arena_descriptor_heap] * mib, arena.use_peak[arena_shader] * mib,
                     arena.use_peak[arena_heap] * mib,
                     (unsigned long long)arena.allocations, (unsigned long long)arena.failures);
        // The page content of docs/page_content.md: how much of the title's page memory the replay pulled into the
        // stand-ins. Counts only.
        if (pages_pulled.load(std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12: page content: %llu pull(s), %.1f MiB copied from the title's pages\n",
                         static_cast<unsigned long long>(pages_pulled.load(std::memory_order_relaxed)),
                         double(pages_pulled_bytes.load(std::memory_order_relaxed)) * mib);
        // The title-VA work of docs/page_address_space.md: the arena ranges that stand in for title VAs, and how many
        // shader-visible copies of those VAs the replay had to rewrite. Counts only.
        if (context) {
            std::fprintf(stderr,
                         "[rt] d3d12: title VAs: %zu arena range(s), %llu shader-visible VA(s) rebased, %llu "
                         "descriptor V# base(s) rewritten\n",
                         context->console_va_ranges(),
                         static_cast<unsigned long long>(context->console_va_rebased_values()),
                         static_cast<unsigned long long>(context->console_va_rebased_fields()));
            // docs/vertex_buffer_va.md: the title pages a command named directly and the device imported at first use.
            const std::size_t imported_ranges = context->imported_title_ranges();
            if (imported_ranges)
                std::fprintf(stderr,
                             "[rt] d3d12: title pages imported at first use: %zu range(s), %.1f MiB, %llu command "
                             "argument(s) resolved to them\n",
                             imported_ranges, double(context->imported_title_bytes()) / (1024.0 * 1024.0),
                             static_cast<unsigned long long>(context->imported_title_hits()));
        }
    }

    void arena_release(VkDeviceSize offset, VkDeviceSize bytes, ArenaUse use) {
        if (!bytes) return;
        std::lock_guard lock(arena.mutex);
        arena.in_use -= bytes;
        arena.use_in_use[use] -= bytes;
        auto it = arena.free.emplace(offset, bytes).first;
        if (auto next = std::next(it); next != arena.free.end() && it->first + it->second == next->first) {
            it->second += next->second;
            arena.free.erase(next);
        }
        if (it != arena.free.begin()) {
            if (auto previous = std::prev(it); previous->first + previous->second == it->first) {
                previous->second += it->second;
                arena.free.erase(it);
            }
        }
    }

    // The translator's output, loaded on first use.
    std::mutex library_mutex;
    bool library_tried = false;
    palmetto::ShaderLibrary shader_library;

    const palmetto::ShaderLibrary* library() {
        std::lock_guard lock(library_mutex);
        if (!library_tried) {
            library_tried = true;
            // Nothing may leave here into Create*PipelineState: a path the STL cannot convert throws std::system_error
            // under MSVC, which is not a filesystem_error, so every std::exception gives the message.
            try {
                std::string error;
                const std::filesystem::path dir = info.shader_dir.empty() ? configured_shader_directory(&error)
                                                                          : std::filesystem::path(info.shader_dir);
                if (!error.empty())
                    std::fprintf(stderr, "[rt] d3d12: invalid translated shader path: %s\n", error.c_str());
                else if (dir.empty())
                    std::fprintf(stderr,
                                 "[rt] d3d12: no translated shaders: set PALMETTO_SHADERS to <work>/shaders/spirv\n");
                else if (!shader_library.load(dir, &error))
                    std::fprintf(stderr, "[rt] d3d12: %s\n", error.c_str());
                else {
                    const auto path = shader_library.directory().u8string();
                    std::fprintf(stderr, "[rt] d3d12: %zu translated shaders from %s\n",
                                 shader_library.size(), reinterpret_cast<const char*>(path.c_str()));
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[rt] d3d12: invalid translated shader path: %s\n", e.what());
            }
        }
        return shader_library.loaded() ? &shader_library : nullptr;
    }

    bool depth_usable(VkFormat format) const {
        VkFormatProperties properties{};
        format_properties(info.physical, format, &properties);
        return properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }

    bool color_attachment_usable(VkFormat format) const {
        VkFormatProperties properties{};
        format_properties(info.physical, format, &properties);
        return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    }

    bool storage_usable(VkFormat format) const {
        VkFormatProperties properties{};
        format_properties(info.physical, format, &properties);
        return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    }

    bool sampled_usable(VkFormat format) const {
        VkFormatProperties properties{};
        format_properties(info.physical, format, &properties);
        return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    }

    // Whether the GPU needs and supports backing UAV textures of `logical` (no storage support) with a mutable image of
    // the unsigned format `backing` of the same size.
    bool probe_uint_backed_uav(VkFormat logical, VkFormat backing) const {
        if (storage_usable(logical)) return false;
        if (!image_format_properties2 || !storage_usable(backing) || !sampled_usable(logical)) return false;
        const VkFormat view_formats[2] = {backing, logical};
        VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        list.viewFormatCount = 2;
        list.pViewFormats = view_formats;
        VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &list};
        query.format = backing;
        query.type = VK_IMAGE_TYPE_2D;
        query.tiling = VK_IMAGE_TILING_OPTIMAL;
        query.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        query.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        return image_format_properties2(info.physical, &query, &props) == VK_SUCCESS;
    }

    // The limits apply to this exact combination of format, type, tiling, usage, create flags and pNext format list.
    HRESULT check_image_support(const VkImageCreateInfo& create) const {
        VkImageFormatProperties limits{};
        VkResult result = VK_ERROR_FORMAT_NOT_SUPPORTED;
        if (image_format_properties2) {
            VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
            query.pNext = create.pNext;
            query.format = create.format;
            query.type = create.imageType;
            query.tiling = create.tiling;
            query.usage = create.usage;
            query.flags = create.flags;
            VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            result = image_format_properties2(info.physical, &query, &props);
            limits = props.imageFormatProperties;
        } else {
            result = image_format_properties(info.physical, create.format, create.imageType, create.tiling,
                                             create.usage, create.flags, &limits);
        }
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) return E_INVALIDARG;
        if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) return E_OUTOFMEMORY;
        if (result != VK_SUCCESS) return E_FAIL;
        if (create.extent.width > limits.maxExtent.width || create.extent.height > limits.maxExtent.height ||
            create.extent.depth > limits.maxExtent.depth || create.mipLevels > limits.maxMipLevels ||
            create.arrayLayers > limits.maxArrayLayers || !(limits.sampleCounts & create.samples))
            return E_INVALIDARG;
        return S_OK;
    }

    // D3D12_OPTIONS1's wave facts, from the physical device: which subgroup operations exist and what wave sizes it
    // can run. Zeros and false when the device cannot be asked, which the query then reports as unsupported.
    // Read once: std::call_once makes a concurrent first caller wait until the facts are complete.
    const SubgroupFacts& subgroup_facts() {
        std::call_once(subgroups_once, [this] { read_subgroup_facts(); });
        return subgroups;
    }
    void read_subgroup_facts() {
        const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            info.get_instance_proc
                ? info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties2")
                : nullptr);
        if (!properties2 || !info.physical) return;
        VkPhysicalDeviceSubgroupSizeControlProperties size_control{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
                                                   &size_control};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
        properties2(info.physical, &properties);
        // Every group the wave intrinsics map to, because D3D12 promises them all behind the one bool.
        constexpr VkSubgroupFeatureFlags kD3D12WaveOps =
            VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
            VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
        subgroups.wave_ops = (subgroup.supportedOperations & kD3D12WaveOps) == kD3D12WaveOps;
        subgroups.lane_min = size_control.minSubgroupSize ? size_control.minSubgroupSize : subgroup.subgroupSize;
        subgroups.lane_max = size_control.maxSubgroupSize ? size_control.maxSubgroupSize : subgroups.lane_min;
        if (subgroups.lane_max < subgroups.lane_min) subgroups.lane_max = subgroups.lane_min;
        // D3D-121: Intel's D3D12 driver answers a fixed 16..16 on its older integrated GPUs, where Vulkan reports
        // the compiler's whole 8..32 choice. Measured on Xe-LP: UHD 770 (32.0.101.6129), Raptor Lake-S Mobile
        // (101.7092) and Tiger Lake 0x9a78 (101.7088). Discrete Arc is not measured yet (TEST-57), so it keeps the
        // Vulkan range. D3D-280: from Xe-LPG on the driver answers what Vulkan reports (Meteor Lake 0x7d55,
        // 32.0.101.6127: 8..32 on both sides), so there is no fixed answer to mirror and those lines keep the
        // Vulkan range too.
        constexpr uint32_t kIntelVendorId = 0x8086;
        constexpr UINT kIntelD3D12Lanes = 16;
        const uint32_t intel_device_id = properties.properties.deviceID;
        // The Xe-core integrated lines from Xe-LPG on. Device IDs are not generation-ordered (Tiger Lake's 0x9a78
        // sorts above Meteor Lake's), so the ranges are listed out, not compared as a cutoff.
        const bool intel_xe_lpg_or_later =
            (intel_device_id >= 0x7d40 && intel_device_id <= 0x7dc5) ||  // Meteor Lake, Arrow Lake
            (intel_device_id >= 0x64a0 && intel_device_id <= 0x64b6);    // Lunar Lake
        if (properties.properties.vendorID == kIntelVendorId &&
            properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && !intel_xe_lpg_or_later &&
            subgroups.lane_min <= kIntelD3D12Lanes && kIntelD3D12Lanes <= subgroups.lane_max)
            subgroups.lane_min = subgroups.lane_max = kIntelD3D12Lanes;
    }

    // The device's mesh-shader limits (output vertices and primitives, workgroup size), for mesh PSO creation.
    const VkPhysicalDeviceMeshShaderPropertiesEXT& mesh_limits() {
        std::lock_guard lock(mesh_properties_mutex);
        if (mesh_properties_read) return mesh_properties;
        mesh_properties_read = true;
        const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            info.get_instance_proc ? info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties2") : nullptr);
        if (!properties2 || !info.physical || !info.mesh_shader) return mesh_properties;
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &mesh_properties};
        properties2(info.physical, &properties);
        mesh_properties.pNext = nullptr;
        return mesh_properties;
    }

    // Integer sampled images need not support multisampling. Pick a supported count for the
    // zero image in the corresponding set-0 bindings; one means that class is unavailable.
    VkSampleCountFlagBits ms_fallback_samples(VkFormat format) const {
        VkImageFormatProperties props{};
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (image_format_properties(info.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                    usage, 0, &props) == VK_SUCCESS) {
            for (UINT count = D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT; count > 1; count >>= 1)
                if (props.sampleCounts & count) return VkSampleCountFlagBits(count);
        }
        return VK_SAMPLE_COUNT_1_BIT;
    }

    // The format a texture description is created with, D24S8 and R9G9B9E5 UAV fallbacks included.
    Format texture_format(const D3D12_RESOURCE_DESC& desc) const {
        if (!xr_bias_compatible(desc)) return {};
        if (desc.Format == DXGI_FORMAT_A8_UNORM) {
            // Alpha-only. A plain texture is sampled through the R8 image's alpha (see the A8 view path), and a render
            // target is the same image with the colour-attachment usage: the title sizes a 3840x2160, 1-sample A8
            // render target for its output mask, and refusing it here made the allocation query report the invalid
            // D3D12 result, which the factory turned into a null resource the title then tripped over (measured: the
            // slot-108 query for format 65, then the title's int3 asserts at game+0xABFE1B and game+0xCC2936).
            // A sampled A8 texture may also be 1D or 3D: the title sizes 128x1 1D A8 textures through device slot 25
            // (flags 0x300000, layouts 0x0 and 0x100), and refusing them made that query answer UINT64_MAX, which the
            // title asserts on at game+0x344196. An A8 render target stays 2D, as measured, and depth-stencil or
            // unordered access stays refused.
            const bool sampled_line_or_volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
                                                desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            if ((desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D && !sampled_line_or_volume) ||
                ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
                 desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) ||
                desc.SampleDesc.Count != 1 ||
                (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
                return {};
            if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) && !color_attachment_usable(VK_FORMAT_R8_UNORM)) {
                log_once("A8_UNORM render targets need R8_UNORM colour-attachment support; format", desc.Format);
                return {};
            }
            return Format{VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT};
        }
        if (desc.Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
            if ((desc.SampleDesc.Count != 1 && (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
                                               (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) ||
                (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)))
                return {};
            const bool render_target = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
            const bool unordered_access = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
            const bool native_attachments =
                color_attachment_usable(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) &&
                (!unordered_access || storage_usable(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32));
            if (render_target && !native_attachments) {
                // The console's Series-S upscale output (docs/decomp_renderer.md section 12): R9G9B9E5 with both a
                // render-target and an unordered-access flag. The console creates it; this host cannot attach or store
                // E5B9G9R9. Off the console arm it stays refused exactly as before; on the console arm the measured
                // support is the UAV-only emulation's mutable R32_UINT image plus a refusal of the attachment view.
                if (!palmetto::console_r9g9b9e5_render_target(desc)) {
                    log_once("R9G9B9E5_SHAREDEXP render targets need colour-attachment and storage support for the "
                             "format; flags",
                             int(desc.Flags));
                    return {};
                }
                if (!console_series_s_upscale_targets()) {
                    log_once("an R9G9B9E5_SHAREDEXP render target is only served on the Series-S console arm "
                             "(PALMETTO_CONSOLE_TYPE=6); flags",
                             int(desc.Flags));
                    return {};
                }
                if (!r9g9b9e5_uav_emulated) {
                    log_once("Series-S R9G9B9E5_SHAREDEXP render target: the host has neither an E5B9G9R9 attachment "
                             "nor a mutable R32_UINT backing; flags",
                             int(desc.Flags));
                    return {};
                }
                log_once("Series-S R9G9B9E5_SHAREDEXP render target: backed by a mutable R32_UINT image, its "
                         "render-target view is refused by name; flags",
                         int(desc.Flags));
            }
            if (unordered_access && r9g9b9e5_uav_emulated) {
                log_once("R9G9B9E5_SHAREDEXP UAV unavailable natively, using mutable R32_UINT; format", desc.Format);
                Format backed{VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT};
                backed.uint_backed = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
                return backed;
            }
        }
        if (rgb96_refused(desc)) {
            if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
                log_once("96-bit (R32G32B32) render targets not supported; format", desc.Format);
            return {};
        }
        // The host runtime refuses typed B8G8R8X8 with unordered access (its typeless family is accepted): the
        // BGRA8 image could take storage writes, but D3D12 has no typed UAV for these formats.
        if (alpha_unused(desc.Format) && desc.Format != DXGI_FORMAT_B8G8R8X8_TYPELESS &&
            (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
            log_once("B8G8R8X8 textures cannot allow unordered access; format", desc.Format);
            return {};
        }
        // A B5G5R5A1 UAV texture where VK_FORMAT_A1R5G5B5_UNORM_PACK16 has no storage support: a mutable R16_UINT image
        // (Format::uint_backed). Its RTVs and SRVs view it as A1R5G5B5; shaders store the packed 16 bits.
        if (desc.Format == DXGI_FORMAT_B5G5R5A1_UNORM && (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
            !storage_usable(VK_FORMAT_A1R5G5B5_UNORM_PACK16)) {
            if (!b5g5r5a1_uav_emulated || desc.SampleDesc.Count > 1 ||
                ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
                 !color_attachment_usable(VK_FORMAT_A1R5G5B5_UNORM_PACK16)))
                return {};
            log_once("B5G5R5A1_UNORM UAV unavailable natively, using mutable R16_UINT; format", desc.Format);
            Format backed{VK_FORMAT_R16_UINT, VK_IMAGE_ASPECT_COLOR_BIT};
            backed.uint_backed = VK_FORMAT_A1R5G5B5_UNORM_PACK16;
            return backed;
        }
        // An 8-bit sRGB UAV texture where the sRGB format has no storage support: the host runtime makes it (RTX 4090
        // and WARP; a retail title sizes a 2048x2048 R8G8B8A8_UNORM_SRGB render target with unordered access), but no
        // typed UAV is sRGB in D3D12, so shaders write the raw bytes. A mutable image of the UNORM format of the same
        // channel order (Format::uint_backed): its storage view is UNORM, its SRVs and RTVs are sRGB, so sampling
        // decodes and rendering encodes as on the host.
        if ((desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) &&
            (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
            const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            const VkFormat srgb = rgba ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_SRGB;
            if (!storage_usable(srgb)) {
                if (!(rgba ? rgba8_srgb_uav_emulated : bgra8_srgb_uav_emulated) || desc.SampleDesc.Count > 1 ||
                    ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) && !color_attachment_usable(srgb)))
                    return {};
                log_once("8-bit sRGB UAV unavailable natively, using a mutable UNORM image; format", desc.Format);
                Format backed{rgba ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT};
                backed.uint_backed = srgb;
                return backed;
            }
        }
        Format format = texture_storage_format(desc.Format, desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && (format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT))
            return {};
        format.d24 = format.format == VK_FORMAT_D24_UNORM_S8_UINT;  // native or not: clears are quantised
        if (format.format == VK_FORMAT_D24_UNORM_S8_UINT && !depth_usable(format.format)) {
            log_once("D24_UNORM_S8_UINT unavailable, using D32_SFLOAT_S8_UINT; format", desc.Format);
            format.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
            format.d24_as_float = true;
        }
        return format;
    }

    // The memory an image made from `create` needs, or false if the device can't make that image.
    bool memory_requirements(const VkImageCreateInfo& create, VkMemoryRequirements& out) const {
        if (FAILED(check_image_support(create))) return false;
        if (image_requirements) {
            VkDeviceImageMemoryRequirements query{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
            query.pCreateInfo = &create;
            VkMemoryRequirements2 result{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
            image_requirements(info.device, &query, &result);
            out = result.memoryRequirements;
            return true;
        }
        auto& vk = context->vk;  // without the query, a throwaway image
        VkImage image = VK_NULL_HANDLE;
        VkImageCreateInfo shared = create;
        share(shared);
        if (vk.vkCreateImage(info.device, &shared, nullptr, &image) != VK_SUCCESS) return false;
        vk.vkGetImageMemoryRequirements(info.device, image, &out);
        vk.vkDestroyImage(info.device, image, nullptr);
        return true;
    }

    // Set 0 of the translated shaders (vulkan_bindless.h, docs section 3.4) and what has been put in it. A shader
    // finds a T# or S# by its bytes (the descriptor hash table), so one entry serves every heap slot that holds
    // those bytes: a copied descriptor needs nothing more. An image's entries live until its texture is
    // destroyed, samplers until the device is.
    struct ImageEntry {
        palmetto::ImageDimension dimension;
        std::uint32_t index;
        VkImageView sampled;
        VkImageView storage;  // the same range without a component mapping, for the storage bindings; or null
        std::uint64_t address;
        // Diagnostic (docs/frame_capture.md): the layer image, its format, its top-mip extent, the D3D12 resource behind
        // it and the title VA that resource was placed at (0: an ordinary committed resource). The frame dump's
        // sampled-texture readback and its mapping chain read these; nothing on the draw path does.
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        palmetto::ResourceId resource = 0;
        std::uint64_t title_va = 0;
        std::uint64_t title_bytes = 0;  // docs/texture_source.md: the placed span the page-writer watch watches
    };
    std::unique_ptr<palmetto::BindlessSet> bindless;
    std::mutex descriptor_mutex;  // the set and the containers below
    // vkDeviceWaitIdle calls made for the bindless set (none, unless PALMETTO_BINDLESS_IDLE_WAIT restores the old wait).
    std::atomic<std::uint64_t> bindless_idle_waits{0};
    bool bindless_legacy_idle = false;
    std::atomic<bool> bindless_cap_logged{false};
    std::map<palmetto::DescriptorKey, ImageEntry> image_entries;
    std::multimap<std::uint64_t, palmetto::DescriptorKey> image_keys;  // by the texture's address
    std::vector<ImageEntry> replaced_images;  // kept until their texture goes: work in flight may still use them
    std::map<palmetto::DescriptorKey, VkSampler> sampler_entries;
    struct CustomBorderColor {
        bool is_int = false;
        VkClearColorValue value{};
        bool operator==(const CustomBorderColor& o) const {
            return is_int == o.is_int && std::memcmp(&value, &o.value, sizeof(value)) == 0;
        }
    };
    std::vector<CustomBorderColor> border_color_table;

    std::uint32_t register_border_color(bool is_int, const void* components) {
        std::lock_guard lock(descriptor_mutex);
        CustomBorderColor entry{};
        entry.is_int = is_int;
        std::memcpy(&entry.value, components, sizeof(entry.value));
        for (std::size_t i = 0; i < border_color_table.size(); ++i) {
            if (border_color_table[i] == entry) return std::uint32_t(i);
        }
        if (border_color_table.size() >= 4000) {
            log_once("custom sampler border colour table is full (4000 entries); clamping index",
                     int(border_color_table.size()));
            return 3999;
        }
        const std::uint32_t idx = std::uint32_t(border_color_table.size());
        border_color_table.push_back(entry);
        return idx;
    }
    // A T# names a texture by an address of its own: above 4 GiB, which the GPU address arena for buffers (docs
    // section 3.3) stays below, and 256 bytes apart (the T# keeps addresses in 256-byte units). Nothing
    // dereferences it.
    std::atomic<std::uint64_t> next_image_address{std::uint64_t(1) << 32};

    // 0 float, 1 unsigned integer, 2 signed integer: which of the three storage-image arrays a view belongs in.
    static int numeric_class(VkFormat format) {
        switch (format) {
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 1;
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32B32A32_SINT: return 2;
        default: return 0;
        }
    }

    void release(const ImageEntry& entry) {  // descriptor_mutex held
        const std::uint64_t slot_handle =
            (static_cast<std::uint64_t>(entry.dimension) << 32) | static_cast<std::uint64_t>(entry.index);
        context->defer_destroy(palmetto::DeferredDeletionQueue::Kind::bindless_slot, slot_handle);
        context->defer_destroy(palmetto::DeferredDeletionQueue::Kind::image_view, entry.sampled);
        if (entry.storage) context->defer_destroy(palmetto::DeferredDeletionQueue::Kind::image_view, entry.storage);
    }

    // Gives the T# `words` a view in set 0 and an entry in the hash table, unless it has them. `storage`: shaders
    // write through it too (an unordered-access view), so it also goes into the storage arrays, which are
    // shorter. False when set 0 cannot show the image; the descriptor then finds the fallback at index 0.
    bool bind_image(const palmetto::gfx10::ImageWords& words, const palmetto::gfx10::Image& image, VkImage vk_image,
                    VkFormat image_format, VkImageAspectFlags aspects, bool storage,
                    bool allow_channel_cast = false, VkFormat uint_backed = VK_FORMAT_UNDEFINED,
                    palmetto::ResourceId resource = 0, std::uint64_t title_va = 0, std::uint64_t title_bytes = 0) {
        namespace g = palmetto::gfx10;
        std::lock_guard lock(descriptor_mutex);
        if (!bindless) return false;
        const auto existing = image_entries.find(words);
        if (existing != image_entries.end() && (existing->second.storage || !storage)) return true;

        palmetto::ImageDimension dimension;
        switch (image.type) {
        case g::ImageType::image_2d: dimension = palmetto::ImageDimension::dim_2d; break;
        case g::ImageType::image_2d_array:
        case g::ImageType::cube: dimension = palmetto::ImageDimension::dim_2d_array; break;
        case g::ImageType::image_3d: dimension = palmetto::ImageDimension::dim_3d; break;
        case g::ImageType::image_1d: dimension = palmetto::ImageDimension::dim_1d; break;
        case g::ImageType::image_2d_msaa: dimension = palmetto::ImageDimension::dim_2d_ms; break;
        case g::ImageType::image_2d_msaa_array: dimension = palmetto::ImageDimension::dim_2d_ms_array; break;
        default:
            log_once("set 0 has no array for this image type:", int(image.type));
            return false;
        }
        const bool depth_or_stencil = (aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
        if (storage && depth_or_stencil) {
            log_once("storage view not supported for depth-stencil image format:", int(image_format));
            return false;
        }
        // A T# of 8_UINT on a depth-stencil image is a view of its stencil plane (image_descriptor, D3D-75). Vulkan
        // views it in the image's own format with the stencil aspect, and returns the value in R, as a T# whose
        // format is 8_UINT reads it from X.
        const bool stencil = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) && image.format == g::stencil_format;
        const VkFormat logical_format = uint_backed ? uint_backed : image_format;
        VkFormat view_format =
            stencil ? image_format : g::vulkan_view_format(logical_format, image.format, allow_channel_cast);
        VkFormat channels_format = view_format;  // whose channels the T#'s swizzle selects from
        if (view_format == VK_FORMAT_UNDEFINED) {  // a 96-bit texture's T# names RGB32; its image is RGBA32
            view_format = rgb96_view_format(image_format, image.format);
            channels_format = rgb96_named_format(view_format);
        }
        if (view_format == VK_FORMAT_UNDEFINED ||
            (depth_or_stencil && !g::is_depth_stencil_format(view_format))) {
            log_once("no Vulkan view format for the GFX10 format of a view:", int(image.format));
            return false;
        }
        const unsigned sampled_class = g::multisampled(image.type) ? numeric_class(view_format) : 0;
        if (g::multisampled(image.type) && !bindless->supports_ms_class(sampled_class)) {
            log_once("set 0 has no multisample binding for this numeric class:", int(sampled_class));
            return false;
        }
        auto& vk = context->vk;
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = vk_image;
        view_info.viewType = g::vulkan_view_type(image.type);
        view_info.format = view_format;
        VkImageViewUsageCreateInfo sampled_usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        if (uint_backed) {
            sampled_usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            view_info.pNext = &sampled_usage;
        }
        auto swizzle = image.swizzle;
        VkImageViewMinLodCreateInfoEXT clamp{VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT};
        const bool supports_lod_clamp = image.type == g::ImageType::image_1d || image.type == g::ImageType::image_2d ||
                                        image.type == g::ImageType::image_2d_array || image.type == g::ImageType::cube ||
                                        image.type == g::ImageType::image_3d;
        if (!storage && supports_lod_clamp && (aspects & VK_IMAGE_ASPECT_COLOR_BIT)) {
            // Use the clamp in the T# itself: views with identical keys must sample identically.
            clamp.minLod = g::image_min_lod(words);
            if (clamp.minLod > float(image.first_mip + image.mip_count - 1)) {
                // The accessible mip set is empty. Keep the dimensions and constant/missing channels.
                for (auto& channel : swizzle)
                    if (channel != g::select_one) channel = g::select_zero;
            } else if (clamp.minLod > float(image.first_mip)) {
                if (!info.image_view_min_lod) {
                    log_once("SRV requires VK_EXT_image_view_min_lod; first mip", int(image.first_mip));
                    return false;
                }
                clamp.pNext = view_info.pNext;
                view_info.pNext = &clamp;
            }
        }
        view_info.components = g::vulkan_swizzle(swizzle, channels_format);
        // The cast: MSVC's enums are ints, and it rejects int to VkImageAspectFlags in braces as narrowing.
        view_info.subresourceRange = {
            VkImageAspectFlags(stencil                                   ? VK_IMAGE_ASPECT_STENCIL_BIT
                               : (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                       : VK_IMAGE_ASPECT_COLOR_BIT),
            image.first_mip, image.mip_count, image.first_layer, image.layer_count};
        ImageEntry entry{dimension, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, image.address};
        entry.image = vk_image;
        entry.format = image_format;
        entry.extent = {image.width, image.height};
        entry.resource = resource;
        entry.title_va = title_va;
        entry.title_bytes = title_bytes;
        if (vk.vkCreateImageView(info.device, &view_info, nullptr, &entry.sampled) != VK_SUCCESS) return false;
        palmetto::StorageViews views{};
        if (storage) {
            VkImageViewCreateInfo storage_info = view_info;
            storage_info.components = {};
            VkImageViewUsageCreateInfo storage_usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
            const VkFormat storage_format = uint_backed ? image_format : view_format;
            if (uint_backed) {
                storage_info.format = storage_format;
                storage_usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
                storage_info.pNext = &storage_usage;
            }
            if (vk.vkCreateImageView(info.device, &storage_info, nullptr, &entry.storage) != VK_SUCCESS) {
                vk.vkDestroyImageView(info.device, entry.sampled, nullptr);
                return false;
            }
            const int numeric = numeric_class(storage_format);
            (numeric == 1 ? views.as_uint : numeric == 2 ? views.as_sint : views.as_float) = entry.storage;
            if (storage_format == VK_FORMAT_R32_UINT) views.r32ui = entry.storage;
        }
        // Every state a shader can read or write the texture in is GENERAL (ResourceBinding::sampled_in_general).
        entry.index = bindless->add_image(dimension, entry.sampled, VK_IMAGE_LAYOUT_GENERAL, views, sampled_class);
        if (!entry.index) {
            log_once("a bindless image array is full; dimension", int(dimension));
            vk.vkDestroyImageView(info.device, entry.sampled, nullptr);
            if (entry.storage) vk.vkDestroyImageView(info.device, entry.storage, nullptr);
            return false;
        }
        if (existing != image_entries.end()) {
            replaced_images.push_back(existing->second);  // the read-only entry of the same bytes
            existing->second = entry;
        } else {
            image_entries.emplace(words, entry);
            image_keys.emplace(image.address, words);
        }
        bindless->insert_key(words, entry.index);
        return true;
    }

    // The texture at `address` is gone: its views leave set 0 and its T#s the hash table.
    void forget_image(std::uint64_t address) {
        std::lock_guard lock(descriptor_mutex);
        if (!bindless) return;
        const auto [first, last] = image_keys.equal_range(address);
        for (auto it = first; it != last; ++it) {
            const auto entry = image_entries.find(it->second);
            bindless->erase_key(it->second);
            release(entry->second);
            image_entries.erase(entry);
        }
        image_keys.erase(first, last);
        std::erase_if(replaced_images, [&](const ImageEntry& entry) {
            if (entry.address != address) return false;
            release(entry);
            return true;
        });
    }

    // Diagnostic (docs/frame_capture.md): one descriptor table of the draw the frame dump armed, read from the console
    // heap mirror the way console_pull_table reads it. Copies up to `palmetto::max_sampled_keys` slot keys out (a T# is
    // 8 dwords; a V# or S# occupies the first 4 and is flagged as not an image) and resolves every T# set 0 holds a
    // layer image for, with that image's D3D12 resource and the title VA it was placed at (docs/frame_capture.md, the
    // texture-source mapping chain), so the frame dump can copy each sampled image and say whether its content is black.
    // Reads only: nothing is pulled, bound or recorded, and a miss just leaves the key unresolved.
    bool sampled_table(std::uint64_t table_va, std::uint32_t max_descriptors, palmetto::SampledTable& out) {
        if (!context || !table_va || !arena.mapped) return false;
        std::uint64_t mirror = 0, available = 0;
        if (!context->console_va_span(table_va, palmetto::ConsoleVaKind::descriptors, mirror, available)) return false;
        if (mirror >= arena.size) return false;
        const std::uint32_t slots = std::min<std::uint32_t>(
            max_descriptors, std::uint32_t(std::min<std::uint64_t>(available, arena.size - mirror) / kDescriptorSize));
        out = {};
        out.slots = slots;
        std::unique_lock<std::mutex> lock(descriptor_mutex);
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            const auto* words = reinterpret_cast<const std::uint32_t*>(arena.mapped + mirror +
                                                                       VkDeviceSize(slot) * kDescriptorSize);
            const palmetto::gfx10::ImageWords descriptor{words[0], words[1], words[2], words[3],
                                                         words[4], words[5], words[6], words[7]};
            const bool image = palmetto::gfx10::is_image(descriptor);
            // A slot can hold descriptor-shaped data that fails the T# decode (the compose pass's tables carry float
            // constants in descriptor slots): those go to the buffer/data list instead of being reported as a sampled T#.
            bool tsharp = image;
            palmetto::gfx10::Image decoded_image{};
            if (tsharp && !palmetto::gfx10::decode(descriptor, decoded_image)) tsharp = false;
            palmetto::SampledKey* key_ptr = nullptr;
            if (tsharp) {
                if (out.keys >= palmetto::max_sampled_keys) continue;
                key_ptr = &out.key[out.keys++];
            } else {
                if (out.buffer_keys >= std::size(out.buffer_key)) continue;
                key_ptr = &out.buffer_key[out.buffer_keys++];
            }
            palmetto::SampledKey& key = *key_ptr;
            std::memcpy(key.words, words, sizeof(key.words));
            key.slot = slot;
            key.image = tsharp;
            if (!tsharp) continue;
            const auto entry = image_entries.find(palmetto::make_tsharp_key(descriptor.data()));
            if (entry == image_entries.end() || !entry->second.image) continue;
            key.resolved = true;
            key.image_handle = entry->second.image;
            key.format = entry->second.format;
            key.extent = entry->second.extent;
            key.resource = entry->second.resource;
            key.title_va = entry->second.title_va;
            // Diagnostic (docs/texture_source.md): watch the pages of a sampled placed texture, so a measurement can say
            // whether anything ever writes this population. Off unless PALMETTO_TEXTURE_PAGE_WATCH is set.
            if (entry->second.title_va && entry->second.title_bytes)
                watch_placed_pages(entry->second.title_va, entry->second.title_bytes, true);
        }
        lock.unlock();
        // A V# slot's base, resolved for the colour-path printer (docs/colour_path.md): the title span whose pages back
        // the block the V# names, and whether the base is that span's arena stand-in (a rebased title VA) or the title VA
        // itself. A slot that is not a decodable V# (a sampler, float data) stays unmapped. A small block is registered
        // with the exact-content writer watch when the colour-path switch is on, so a boot can say whether the title ever
        // writes the data the pass reads. Resolved after the descriptor lock is released: the title-VA map has its own
        // lock and this path must not establish an order with it.
        for (std::uint32_t k = 0; k < out.buffer_keys; ++k) {
            palmetto::SampledKey& key = out.buffer_key[k];
            const palmetto::gfx10::BufferWords buffer_words{key.words[0], key.words[1], key.words[2], key.words[3]};
            palmetto::gfx10::Buffer buffer{};
            if (!palmetto::gfx10::decode(buffer_words, buffer) || !buffer.address) continue;
            std::uint64_t title = 0, span = 0;
            if (context->console_arena_span(buffer.address, title, span)) {
                key.buffer_title_va = title;
                key.buffer_title_bytes = span;
                key.buffer_arena = true;
            } else if (context->console_va_span(buffer.address, palmetto::ConsoleVaKind::buffer, title, span)) {
                key.buffer_title_va = title;
                key.buffer_title_bytes = span;
                key.buffer_arena = false;
            }
            const std::uint64_t declared =
                buffer.stride ? std::uint64_t(buffer.stride) * buffer.num_records : std::uint64_t(buffer.num_records);
            if (key.buffer_title_va && declared && declared <= palmetto::frame_dump::colour_watch_max_bytes &&
                palmetto::frame_dump::colour_path_enabled())
                watch_colour_pages(key.buffer_title_va, declared);
        }
        return true;
    }

    // Diagnostic (docs/texture_source.md): the page-writer watch for sampled placed textures. Up to four spans; every
    // 64 submissions each span's committed, readable, non-zero bytes are counted and a change is printed (the first 20
    // changes in total). A span that never changes over the window is reported by the absence of change lines, and the
    // first sample is always printed so the watch's start is visible. Off unless PALMETTO_TEXTURE_PAGE_WATCH is set;
    // reads only, never writes title memory.
    // The colour path (docs/colour_path.md) reuses the same machinery for at most four small spans (the block a V# slot
    // names): those are also compared byte for byte, so a write that keeps the span's non-zero byte count is seen too.
    struct PageWatch {
        std::uint64_t title_va = 0;
        std::uint64_t bytes = 0;
        std::uint64_t last_nonzero = 0;
        std::uint64_t last_scanned = 0;  // committed+readable page-head bytes at the last sample (commit transitions)
        unsigned changes = 0;
        bool started = false;  // the first sample (all-zero, nothing committed yet) is always printed
        // Exact content (docs/colour_path.md): the small colour span's bytes at the last sample, compared verbatim.
        std::uint32_t content_bytes = 0;
        std::uint8_t content[palmetto::frame_dump::colour_watch_max_bytes] = {};
    };
    PageWatch page_watches_[4];
    unsigned page_watch_count_ = 0;
    PageWatch colour_watches_[4];  // docs/colour_path.md: the exact-content watch for the pass's constants blocks
    unsigned colour_watch_count_ = 0;
    std::uint64_t page_watch_ticks_ = 0;
    bool page_watch_on_ = false;
    bool page_watch_env_read_ = false;

    bool page_watch_enabled() {
        if (!page_watch_env_read_) {
            page_watch_env_read_ = true;
            const char* value = std::getenv("PALMETTO_TEXTURE_PAGE_WATCH");
            page_watch_on_ = value && *value && *value != '0' && *value != 'n';
        }
        return page_watch_on_;
    }

    // Register a span to watch. `sampled` marks the population the frame's armed draw actually samples; when the four
    // slots are full such a span replaces the last one, so the drawn texture is never the one left out.
    void watch_placed_pages(std::uint64_t title_va, std::uint64_t bytes, bool sampled = false) {
        if (!title_va || !bytes || !page_watch_enabled()) return;
        for (unsigned i = 0; i < page_watch_count_; ++i)
            if (page_watches_[i].title_va == title_va) return;
        unsigned slot = page_watch_count_;
        if (slot >= std::size(page_watches_)) {
            if (!sampled) return;
            slot = static_cast<unsigned>(std::size(page_watches_)) - 1;
        } else {
            ++page_watch_count_;
        }
        PageWatch& watch = page_watches_[slot];
        watch = PageWatch{};
        watch.title_va = title_va;
        watch.bytes = bytes;
        std::fprintf(stderr,
                     "[rt] d3d12: page watch: watching title VA 0x%llx + 0x%llx%s (PALMETTO_TEXTURE_PAGE_WATCH=1)\n",
                     static_cast<unsigned long long>(title_va), static_cast<unsigned long long>(bytes),
                     sampled ? ", the armed draw's sampled texture" : "");
    }

    // Register a small title span for the exact-content writer watch (docs/colour_path.md): the block the constant
    // pass's descriptor-table slot 0 V# names. Bounded to four spans and colour_watch_max_bytes; reads only, never writes
    // the title's memory. The watch is active from the next submission on (the tick scans every 64th).
    void watch_colour_pages(std::uint64_t title_va, std::uint64_t bytes) {
        if (!title_va || !bytes) return;
        bytes = std::min<std::uint64_t>(bytes, palmetto::frame_dump::colour_watch_max_bytes);
        for (unsigned i = 0; i < colour_watch_count_; ++i)
            if (colour_watches_[i].title_va == title_va) return;
        if (colour_watch_count_ >= std::size(colour_watches_)) return;
        PageWatch& watch = colour_watches_[colour_watch_count_++];
        watch = PageWatch{};
        watch.title_va = title_va;
        watch.bytes = bytes;
        std::fprintf(stderr,
                     "[rt] d3d12: page watch: colour source title VA 0x%llx + 0x%llx (an exact-content writer watch; "
                     "PALMETTO_COLOUR_PATH=1)\n",
                     static_cast<unsigned long long>(title_va), static_cast<unsigned long long>(bytes));
    }

    // Up to colour_watch_max_bytes of a title span's bytes, from a committed, readable page only (the same protection
    // rule count_span_nonzero uses). Used by the exact-content watch, so a small block's content is compared verbatim.
    std::uint32_t read_span_content(std::uint64_t title_va, std::uint64_t bytes, std::uint8_t* out) const {
        if (!title_va || !bytes || !out) return 0;
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(title_va)), &info, sizeof(info)) !=
            sizeof(info))
            return 0;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        std::uint32_t chunk = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(bytes, palmetto::frame_dump::colour_watch_max_bytes));
        const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (region_end > title_va) chunk = std::min<std::uint32_t>(chunk, std::uint32_t(region_end - title_va));
        if (!chunk) return 0;
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(title_va)), chunk);
        return chunk;
    }

    // Sampled non-zero bytes of a title span: the first 4096 bytes of every 64 KiB page that is committed and readable
    // (page heads, so the scan is bounded to a sixteenth of the span), plus how many bytes were scanned. A page that is
    // not committed contributes nothing.
    std::uint64_t count_span_nonzero(std::uint64_t title_va, std::uint64_t bytes, std::uint64_t& scanned) const {
        scanned = 0;
        std::uint64_t nonzero = 0;
        const std::uint64_t end = title_va + bytes;
        for (std::uint64_t at = title_va; at < end;) {
            MEMORY_BASIC_INFORMATION info{};
            if (::VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), &info, sizeof(info)) !=
                sizeof(info))
                break;
            const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            // Walk page by page even inside one big committed region, so every 64 KiB page's head is sampled rather than
            // only the first page of the region.
            std::uint64_t next = (region_end > at ? region_end : end);
            const std::uint64_t page_end = at + 0x10000;
            if (next > page_end) next = page_end;
            if (next > end) next = end;
            // The title's pages are console memory: a committed page may be mapped write-combined or non-cached, which
            // is readable and is what the import's own commit check accepts (it only refuses NOACCESS and GUARD). The
            // watch must use the same rule, or a readable span would read as "0 scanned".
            if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const auto* data = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(at));
                // 256 bytes per page rather than 4096: console memory can be non-cached, where reads are slow, and a fill
                // that writes a page touches its head. Keeps the whole-span scan cheap enough not to slow the boot.
                const std::uint64_t head = std::min<std::uint64_t>(0x100, next - at);
                for (std::uint64_t i = 0; i < head; ++i)
                    if (data[i]) ++nonzero;
                scanned += head;
            }
            at = next > at ? next : at + 0x10000;
        }
        return nonzero;
    }

    void page_watch_tick() {
        const bool textures = page_watch_enabled() && page_watch_count_;
        if (!textures && !colour_watch_count_) return;
        ++page_watch_ticks_;
        if (page_watch_ticks_ % 64 != 1) return;  // the first submission, then every 64th
        const std::uint64_t sample = page_watch_ticks_ / 64 + 1;
        // A line for the first sample, for every change of (committed bytes, non-zero bytes) - so an all-zero span that
        // gets committed is visible too - and for every 64th sample as a running "latest" line.
        const bool periodic = sample % 64 == 0;
        for (unsigned i = 0; textures && i < page_watch_count_; ++i) {
            PageWatch& watch = page_watches_[i];
            std::uint64_t scanned = 0;
            const std::uint64_t nonzero = count_span_nonzero(watch.title_va, watch.bytes, scanned);
            const bool changed = scanned != watch.last_scanned || nonzero != watch.last_nonzero;
            const bool first = !watch.started;
            if (!changed && !periodic && !first) continue;
            watch.started = true;
            if (changed) ++watch.changes;
            static std::atomic<unsigned> printed{0};
            if (printed.fetch_add(1, std::memory_order_relaxed) < 120)
                std::fprintf(stderr,
                             "[rt] d3d12: page watch: title VA 0x%llx: sampled non-zero bytes %llu -> %llu of %llu "
                             "scanned (sample %llu, change %u%s)\n",
                             static_cast<unsigned long long>(watch.title_va),
                             static_cast<unsigned long long>(watch.last_nonzero),
                             static_cast<unsigned long long>(nonzero),
                             static_cast<unsigned long long>(scanned), static_cast<unsigned long long>(sample),
                             watch.changes, periodic && !changed ? ", periodic" : "");
            watch.last_nonzero = nonzero;
            watch.last_scanned = scanned;
        }
        // The colour path's exact-content watch (docs/colour_path.md): the block the constant pass's table slot 0 names
        // is sampled byte for byte, so a write that keeps the non-zero count is still a change. The first sample prints
        // the content, a later one prints the dwords that changed, and a span that never changes prints only its first
        // sample - a boot with no change line therefore measured "the title never writes the block".
        static std::atomic<unsigned> colour_printed{0};
        for (unsigned i = 0; i < colour_watch_count_; ++i) {
            PageWatch& watch = colour_watches_[i];
            std::uint8_t content[palmetto::frame_dump::colour_watch_max_bytes] = {};
            const std::uint32_t got = read_span_content(watch.title_va, watch.bytes, content);
            const bool first = !watch.started;
            const bool resized = got != watch.content_bytes;
            const std::uint32_t compared = std::min(got, watch.content_bytes);
            const bool changed =
                compared && palmetto::frame_dump::colour_bytes_differ(watch.content, content, compared);
            if (!first && !changed && !resized) continue;
            watch.started = true;
            if (changed) ++watch.changes;
            if (colour_printed.fetch_add(1, std::memory_order_relaxed) < 40) {
                if (changed) {
                    std::fprintf(stderr,
                                 "[rt] d3d12: page watch: colour source title VA 0x%llx CONTENT CHANGED at sample %llu "
                                 "(submission ~%llu):",
                                 static_cast<unsigned long long>(watch.title_va),
                                 static_cast<unsigned long long>(sample),
                                 static_cast<unsigned long long>(page_watch_ticks_));
                    for (std::uint32_t w = 0; w < compared / 4; ++w) {
                        std::uint32_t before = 0, after = 0;
                        std::memcpy(&before, watch.content + w * 4, sizeof(before));
                        std::memcpy(&after, content + w * 4, sizeof(after));
                        if (before != after) std::fprintf(stderr, " [%u] %08x -> %08x", w, before, after);
                    }
                    std::fprintf(stderr, "\n");
                } else {
                    std::uint64_t nonzero = 0;
                    for (std::uint32_t b = 0; b < got; ++b)
                        if (content[b]) ++nonzero;
                    std::fprintf(stderr,
                                 "[rt] d3d12: page watch: colour source title VA 0x%llx: read %u of 0x%llx B, %llu "
                                 "non-zero byte(s), %u dword(s) (sample %llu, submission ~%llu)\n",
                                 static_cast<unsigned long long>(watch.title_va), got,
                                 static_cast<unsigned long long>(watch.bytes),
                                 static_cast<unsigned long long>(nonzero), got / 4,
                                 static_cast<unsigned long long>(sample),
                                 static_cast<unsigned long long>(page_watch_ticks_));
                    // The dwords themselves, so the block the module reads can be quoted from the boot.
                    for (std::uint32_t base = 0; got >= 4 && base < got / 4; base += 8) {
                        std::fprintf(stderr, "[rt] d3d12: page watch: colour source dwords [%2u..%2u]:", base,
                                     std::min(base + 7, got / 4 - 1));
                        for (std::uint32_t w = base; w < got / 4 && w < base + 8; ++w) {
                            std::uint32_t value = 0;
                            std::memcpy(&value, content + w * 4, sizeof(value));
                            std::fprintf(stderr, " %08x", value);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            std::memcpy(watch.content, content, got);
            watch.content_bytes = got;
        }
    }

    // Diagnostic (docs/frame_capture.md): read up to `bytes` of the title's own memory at `va` (the constants a draw's
    // V# names), in place. A V# base the page-address-space work rebased holds the *arena* VA of the stand-in that
    // mirrors the title range (docs/page_address_space.md), so that case reads the mirror; otherwise the base is a title
    // VA and the bytes are read from the title's own memory. False when neither is readable; `read` is what was copied.
    // Reads only: no copy into the arena and no logging.
    bool read_title_memory(std::uint64_t va, std::uint32_t bytes, std::uint8_t* out, std::uint32_t& read) {
        read = 0;
        if (!va || !bytes || !out) return false;
        if (context && arena.mapped) {
            std::uint64_t title = 0, available = 0;
            if (context->console_arena_span(va, title, available) && va < arena.size) {
                const std::uint64_t room = std::min<std::uint64_t>(
                    bytes, std::min<std::uint64_t>(available, arena.size - va));
                if (!room) return false;
                std::memcpy(out, arena.mapped + va, std::size_t(room));
                read = std::uint32_t(room);
                return true;
            }
        }
        const auto base = static_cast<std::uintptr_t>(va);
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<const void*>(base), &info, sizeof(info)) != sizeof(info)) return false;
        if (info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE)))
            return false;
        const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        std::uint32_t chunk = bytes;
        if (region_end > base) chunk = std::min<std::uint32_t>(chunk, std::uint32_t(region_end - base));
        if (!chunk) return false;
        std::memcpy(out, reinterpret_cast<const void*>(base), chunk);
        read = chunk;
        return true;
    }

    // Diagnostic (docs/frame_capture.md, the colour path): bind layer images for the T#s a shader fetches itself out of
    // its published matrix block (PALMETTO_TEXTURE_BIND_FETCHED); defined after class Resource.
    void bind_fetched_textures(std::uint64_t table_title_va);
    // Diagnostic (docs/vertex_path.md): read bytes at a device address a translated shader builds itself (heap_base + a
    // value from its push constants), through the arena's host mapping. Reads only, and never copies into the arena.
    // This runs from the replay's prepare, i.e. inside execute()'s shared resource lock, so it must take no lock of the
    // device's or the context's: heap_base() is a lock-free read for exactly that reason (docs/memory_growth.md).
    bool read_device_memory(std::uint64_t address, std::uint32_t bytes, std::uint8_t* out, std::uint32_t& read) {
        read = 0;
        if (!address || !bytes || !out || !context || !arena.mapped) return false;
        const std::uint64_t base = context->heap_base();
        if (address < base) return false;
        const std::uint64_t offset = address - base;
        if (offset >= arena.size) return false;
        const std::uint64_t room = std::min<std::uint64_t>(bytes, arena.size - offset);
        if (!room) return false;
        std::memcpy(out, arena.mapped + offset, std::size_t(room));
        read = std::uint32_t(room);
        return true;
    }

    // Diagnostic (docs/vertex_path.md, the geometry fix): write the console driver-block V# into the arena mirror at an
    // address a translated shader fetches it from. Opt-in only; bounds-checked; writes exactly `bytes` and nothing else.
    // Like read_device_memory it runs inside execute()'s shared resource lock, so it must not acquire a lock that a bind
    // path queues on: heap_base() is lock-free for that reason (docs/memory_growth.md).
    bool write_device_memory(std::uint64_t address, std::uint32_t bytes, const std::uint8_t* data) {
        if (!address || !bytes || !data || !context || !arena.mapped) return false;
        const std::uint64_t base = context->heap_base();
        if (address < base) return false;
        const std::uint64_t offset = address - base;
        if (offset >= arena.size || bytes > arena.size - offset) return false;
        std::memcpy(arena.mapped + offset, data, std::size_t(bytes));
        return true;
    }

    // Diagnostic (docs/vertex_path.md, the geometry fix): a small scratch range in the GPU address arena for a
    // per-pipeline driver block, whose heap-relative address the geometry fix emits in the s0/s1 user-data pair.
    bool allocate_driver_block(std::uint32_t bytes, std::uint64_t& address) {
        address = 0;
        if (!bytes || !context) return false;
        VkDeviceSize offset = 0;
        if (!arena_allocate(bytes, 256, offset, arena_buffer)) return false;
        address = offset;
        return true;
    }

    // The heap-relative address a buffer_at result starts at, for the console driver-block V# (docs/vertex_buffer_va.md
    // section 18.3): only the GPU address arena itself is reachable through the shader's heap_base + value model. False
    // for any other buffer, including a title-pages import whose device address heap_base cannot name; those go through
    // stage_title_span.
    bool buffer_arena_address(VkBuffer buffer, VkDeviceSize offset, std::uint64_t& address) {
        address = 0;
        if (!buffer || buffer != arena.buffer || offset >= arena.size) return false;
        address = offset;
        return true;
    }

    // A cached arena staging copy of a span of the title's own memory (docs/vertex_buffer_va.md section 18.3): a vertex
    // stream the layer imported over the title's pages at first use has a device address the shader cannot form from
    // heap_base, so the bytes are copied into an arena range and the driver-block V# names that range. One range per
    // title VA (bounded to eight, 4 MiB each) and re-copied on every use, because the title may rewrite its vertices
    // between draws. The pages are the title's committed host memory (that is why they were importable), so a memcpy
    // from the title VA reads them.
    bool stage_title_span(std::uint64_t va, std::uint64_t bytes, std::uint64_t& address) {
        address = 0;
        const auto refuse = [](const char* why, std::uint64_t a, std::uint64_t b) {
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
                std::fprintf(stderr,
                             "[rt] d3d12: title span staging: refused %s at VA 0x%llx + 0x%llx (the draws whose vertex "
                             "stream it holds keep their recorded s0/s1 pair)\n",
                             why, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
            return false;
        };
        if (!va || !bytes) return false;
        if (!context || !arena.mapped) return refuse("no GPU address arena", va, bytes);
        constexpr std::uint64_t kMaxStageBytes = 4ull << 20;
        if (bytes > kMaxStageBytes) return refuse("the span is larger than the 4 MiB staging bound", va, bytes);
        std::lock_guard lock(staged_mutex_);
        auto found = staged_spans_.find(va);
        if (found == staged_spans_.end()) {
            if (staged_spans_.size() >= 8) return refuse("the staging cache holds its eight ranges", va, bytes);
            VkDeviceSize offset = 0;
            if (!arena_allocate(bytes, 256, offset, arena_buffer)) return refuse("no arena room", va, bytes);
            found = staged_spans_.emplace(va, StagedSpan{bytes, offset}).first;
        }
        if (found->second.bytes < bytes) return refuse("a shorter range is already cached for this VA", va, bytes);
        const auto base = static_cast<const std::uint8_t*>(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va)));
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(base, &info, sizeof(info)) != sizeof(info))
            return refuse("VirtualQuery cannot describe the pages", va, bytes);
        if (info.State != MEM_COMMIT) return refuse("the pages are not committed", va, bytes);
        if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE)) {
            char why[64] = {};
            std::snprintf(why, sizeof(why), "the pages' protection 0x%lx is not readable host memory",
                          static_cast<unsigned long>(info.Protect));
            return refuse(why, va, bytes);
        }
        std::memcpy(arena.mapped + found->second.offset, base, std::size_t(bytes));
        address = found->second.offset;
        return true;
    }

    // Gives the S# `words` a sampler in set 0 and an entry in the hash table, unless it has them.
    bool bind_sampler(const palmetto::gfx10::SamplerWords& words) {
        namespace g = palmetto::gfx10;
        std::lock_guard lock(descriptor_mutex);
        if (!bindless) return false;
        const palmetto::DescriptorKey key = palmetto::make_ssharp_key(words.data());
        if (sampler_entries.count(key)) return true;
        const g::Sampler sampler = g::decode(words);
        VkSamplerCreateInfo sampler_info = g::vulkan_sampler(sampler, max_sampler_lod_bias);
        // What vulkan_sampler leaves to its caller, and features this device may not have enabled.
        if (sampler_info.anisotropyEnable && !info.sampler_anisotropy) {
            log_once("anisotropic sampler without samplerAnisotropy; anisotropy", int(sampler.max_anisotropy));
            sampler_info.anisotropyEnable = VK_FALSE;
        }
        for (VkSamplerAddressMode* mode :
             {&sampler_info.addressModeU, &sampler_info.addressModeV, &sampler_info.addressModeW}) {
            if (*mode != VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE || info.sampler_mirror_clamp_to_edge) continue;
            log_once("mirror-once sampler without samplerMirrorClampToEdge; clamped instead, mode", int(*mode));
            *mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }
        VkSamplerReductionModeCreateInfo reduction{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
        if (sampler.filter_mode != g::FilterMode::blend) {
            if (info.sampler_filter_minmax && !sampler_info.compareEnable) {
                reduction.reductionMode = g::vulkan_reduction_mode(sampler.filter_mode);
                reduction.pNext = sampler_info.pNext;
                sampler_info.pNext = &reduction;
            } else {
                log_once("minimum/maximum filter sampler without samplerFilterMinmax; blended instead, mode",
                         int(sampler.filter_mode));
            }
        }
        VkSamplerCustomBorderColorCreateInfoEXT custom_border{
            VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
        if (sampler.border_color == g::BorderColor::table) {
            const bool is_int = sampler.border_color_index < border_color_table.size() &&
                                border_color_table[sampler.border_color_index].is_int;
            if (info.custom_border_color && info.custom_border_color_without_format &&
                sampler.border_color_index < border_color_table.size()) {
                const auto& entry = border_color_table[sampler.border_color_index];
                custom_border.customBorderColor = entry.value;
                custom_border.format = VK_FORMAT_UNDEFINED;
                sampler_info.borderColor =
                    entry.is_int ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
                custom_border.pNext = sampler_info.pNext;
                sampler_info.pNext = &custom_border;
            } else {
                sampler_info.borderColor =
                    is_int ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                log_once("custom sampler border colours without VK_EXT_custom_border_color; transparent black instead, index",
                         int(sampler.border_color_index));
            }
        }
        VkSampler vk_sampler = VK_NULL_HANDLE;
        if (!context->vk.vkCreateSampler ||
            context->vk.vkCreateSampler(info.device, &sampler_info, nullptr, &vk_sampler) != VK_SUCCESS)
            return false;
        const std::uint32_t index = bindless->add_sampler(vk_sampler);
        if (!index) {
            log_once("the bindless sampler array is full; samplers", int(sampler_entries.size()));
            context->vk.vkDestroySampler(info.device, vk_sampler, nullptr);
            return false;
        }
        sampler_entries.emplace(key, vk_sampler);
        bindless->insert_key(key, index);
        return true;
    }

    // An optimal-tiling image of this format can take VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT.
    bool host_transfer_format(VkFormat format) const {
        if (!copy_memory_to_image || !format_properties2) return false;
        VkFormatProperties3 properties3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &properties3};
        format_properties2(info.physical, format, &properties);
        return properties3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT_EXT;
    }

    // A memory type with `required`, preferring one that also has `preferred`, in a heap large enough for the
    // allocation (palmetto::memory_type_order). `chosen`, when given, receives the property flags of the type the
    // memory came from.
    VkDeviceMemory allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags* chosen = nullptr,
                            bool device_address = false, std::uint32_t* chosen_type = nullptr) const {
        VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        for (const std::uint32_t i :
             palmetto::memory_type_order(memory, requirements.memoryTypeBits, required, preferred, requirements.size)) {
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            if (device_address) allocate.pNext = &flags_info;
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = i;
            VkDeviceMemory out = VK_NULL_HANDLE;
            if (context->vk.vkAllocateMemory(info.device, &allocate, nullptr, &out) == VK_SUCCESS) {
                if (chosen) *chosen = memory.memoryTypes[i].propertyFlags;
                if (chosen_type) *chosen_type = i;
                record_allocation(requirements.size);
                if (palmetto::spilled_to_host(required, preferred, memory.memoryTypes[i].propertyFlags))
                    note_spill_once(requirements.size, i);
                return out;
            }
        }
        return VK_NULL_HANDLE;
    }
};

// The Vulkan format for a texture this layer can create, with the D24 fallback; VK_FORMAT_UNDEFINED, after a log
// line, when it cannot. Committed and placed textures share it so both refuse the same formats for the same reason.
Format texture_format(const DeviceState& device, const D3D12_RESOURCE_DESC& desc) {
    Format format = vulkan_format(desc.Format, desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    if (format.format == VK_FORMAT_D24_UNORM_S8_UINT && !device.depth_usable(format.format)) {
        log_once("D24_UNORM_S8_UINT unavailable, using D32_SFLOAT_S8_UINT; format", desc.Format);
        format.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    if (format.format == VK_FORMAT_UNDEFINED) log_once("texture format not supported yet:", desc.Format);
    return format;
}

// The castable-format list a committed or placed texture was created with, mapped to Vulkan (D3D-85's committed
// path, D3D-90's placed one). Each format must map and be the same texel size as the resource's: the host PC
// runtime refuses other sizes, and a size-mismatched VkImageFormatListCreateInfo is a Vulkan validation error
// (VUID-VkImageCreateInfo-pNext-06722). A nonzero count with a null list is no list. E_INVALIDARG on a bad format.
HRESULT castable_view_formats(const Format& format, UINT32 count, const DXGI_FORMAT* castable,
                              std::vector<VkFormat>& out) {
    if (!count || !castable) return S_OK;
    const bool depth = (format.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
    const palmetto::BlockFormat own = palmetto::block_format(format.format, format.aspects);
    for (UINT32 i = 0; i < count; ++i) {
        const Format cast = vulkan_format(castable[i], depth);
        if (cast.format == VK_FORMAT_UNDEFINED) {
            log_once("castable format not supported yet:", castable[i]);
            return E_INVALIDARG;
        }
        const palmetto::BlockFormat block = palmetto::block_format(cast.format, cast.aspects);
        if (own.bytes && block.bytes && (block.bytes != own.bytes || block.block != own.block)) {
            log_once("castable format is a different texel size than the resource:", castable[i]);
            return E_INVALIDARG;
        }
        if (std::find(out.begin(), out.end(), cast.format) == out.end()) out.push_back(cast.format);
    }
    return S_OK;
}

// Argument checks the host PC runtime makes (without the debug layer) before it creates anything, so the layer
// returns the same HRESULTs. Each rule was measured against the host runtime by d3d12_vulkan_device_test
// (compare_invalid_arguments, D3D-49); the texture size, mip, sample-count and alignment rules are in
// GetResourceAllocationInfo's allocation(), which CreateCommittedResource also applies.

// ALLOW_RENDER_TARGET: the host refuses the families below and accepts the rest, D32_FLOAT, D16_UNORM,
// R9G9B9E5_SHAREDEXP and NV12 among them. R32G32B32 is the driver's answer (refused by both AMD drivers measured,
// accepted by WARP); it is refused here, since Vulkan drivers rarely render to it.
bool render_target_family(DXGI_FORMAT format) {
    switch (format_family(format)) {
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R1_UNORM:
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC7_TYPELESS:
            return false;
        default:
            return true;
    }
}

// Why the host would refuse these heap properties, or null if it accepts them. `uma` is the device's
// D3D12_FEATURE_ARCHITECTURE answer: a UMA adapter has no MEMORY_POOL_L1, and the host refuses a custom heap in it
// (measured on a Radeon 780M, AMD 32.0.13030.13001, and on WARP: E_INVALIDARG from CreateHeap and
// CreateCommittedResource for every CPU page property).
const char* heap_properties_refusal(const D3D12_HEAP_PROPERTIES& heap, bool uma) {
    if (heap.Type < D3D12_HEAP_TYPE_DEFAULT || heap.Type > D3D12_HEAP_TYPE_GPU_UPLOAD) return "heap type";
    // CheckFeatureSupport reports OPTIONS16.GPUUploadHeapSupported FALSE, and a host runtime without GPU upload heaps
    // (Resizable BAR off) refuses the type.
    if (heap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD) return "GPU upload heap, which OPTIONS16 reports unsupported";
    const bool custom = heap.Type == D3D12_HEAP_TYPE_CUSTOM;
    const bool page_given = heap.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    const bool pool_given = heap.MemoryPoolPreference != D3D12_MEMORY_POOL_UNKNOWN;
    if (custom ? !(page_given && pool_given) : (page_given || pool_given))
        return "CPU page property or memory pool (given only, and both, on custom heaps)";
    if (heap.CreationNodeMask > 1 || heap.VisibleNodeMask > 1) return "node mask (one node)";
    if (uma && custom && heap.MemoryPoolPreference == D3D12_MEMORY_POOL_L1) return "MEMORY_POOL_L1 on a UMA adapter";
    return nullptr;
}

// Why the host would refuse this CreateHeap description, or null if these checks pass (D3D-238). A probe on an
// RX 5700 and WARP refuses an alignment other than 0, 64 KiB and 4 MiB, the heap properties above (a type of 0, a
// custom heap without a CPU page property), and a heap that denies buffers and both kinds of texture. ALLOW_DISPLAY
// (CreateCommittedResource on a default heap only, per the D3D12_HEAP_FLAGS documentation) is refused too, confirmed
// on an RTX 4070 SUPER and a UHD 770 host by @CowBoYdu76. Those hosts accept HARDWARE_PROTECTED, so it is not refused.
const char* heap_desc_refusal(const D3D12_HEAP_DESC& desc, bool uma) {
    if (!desc.SizeInBytes) return "zero size";
    if (desc.Alignment && desc.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
        desc.Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
        return "alignment (0, 64 KiB or 4 MiB)";
    if (const char* why = heap_properties_refusal(desc.Properties, uma)) return why;
    constexpr UINT deny_all = D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES |
                              D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
    if ((UINT(desc.Flags) & deny_all) == deny_all) return "heap flags denying every resource kind";
    if (UINT(desc.Flags) & D3D12_HEAP_FLAG_ALLOW_DISPLAY) return "ALLOW_DISPLAY (committed resources only)";
    return nullptr;
}

// Why the host would refuse this committed resource, or null if these checks pass. The flag and format tables were
// measured on an RX 9070 XT, an integrated Radeon and WARP, which agree on all of them. Cross-adapter resources
// (valid there only as a row-major texture on a SHARED | SHARED_CROSS_ADAPTER heap) are refused: this layer has no
// cross-adapter sharing.
// The initial state a committed, placed or reserved resource may start in, as the host runtime answers it
// (d3d12_vulkan_device_test's compare_placed_reserved and its "committed: ... state" cases, RTX 3060 / 610.88).
// A buffer takes any bits, unknown ones too, except RAYTRACING_ACCELERATION_STRUCTURE without UAV access, or at all
// on a device without acceleration structures (the host refuses it where OPTIONS5 reports no ray tracing: an
// integrated Radeon, measured by @aguedou-34 on #2462; here, a device with neither the KHR acceleration structures
// nor the XDXR builds). A texture
// takes exactly GENERIC_READ, or one write state alone (RENDER_TARGET, UNORDERED_ACCESS and DEPTH_WRITE need the
// matching resource flag), or any mix of read states (DEPTH_READ needs ALLOW_DEPTH_STENCIL; the shader-resource
// states are refused with DENY_SHADER_RESOURCE). The buffer-only bits (vertex/constant, index, indirect, stream
// output, acceleration structure) and undefined bits are refused on a texture, also inside a mix.
const char* initial_state_refusal(bool buffer, UINT flags, D3D12_RESOURCE_STATES state,
                                  bool acceleration_structures) {
    const UINT s = UINT(state);
    if (buffer) {
        if ((s & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) && !acceleration_structures)
            return "acceleration-structure state on a device without acceleration structures";
        if ((s & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) &&
            !(flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            return "acceleration-structure state on a buffer without UAV access";
        return nullptr;
    }
    if (s == D3D12_RESOURCE_STATE_GENERIC_READ) return nullptr;
    constexpr UINT writes = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                            D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST |
                            D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE |
                            D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE | D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
    constexpr UINT shader_reads =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr UINT reads = D3D12_RESOURCE_STATE_DEPTH_READ | shader_reads | D3D12_RESOURCE_STATE_COPY_SOURCE |
                           D3D12_RESOURCE_STATE_RESOLVE_SOURCE | D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE |
                           D3D12_RESOURCE_STATE_VIDEO_DECODE_READ | D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ |
                           D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
    if (s & ~(writes | reads)) return "initial state of a texture (buffer-only or undefined bits)";
    if (s & writes) {
        if (s & (s - 1)) return "initial state of a texture (a write state with another state)";
        if (s == D3D12_RESOURCE_STATE_RENDER_TARGET && !(flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
            return "RENDER_TARGET state without ALLOW_RENDER_TARGET";
        if (s == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && !(flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            return "UNORDERED_ACCESS state without ALLOW_UNORDERED_ACCESS";
        if (s == D3D12_RESOURCE_STATE_DEPTH_WRITE && !(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            return "DEPTH_WRITE state without ALLOW_DEPTH_STENCIL";
        return nullptr;
    }
    if ((s & D3D12_RESOURCE_STATE_DEPTH_READ) && !(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return "DEPTH_READ state without ALLOW_DEPTH_STENCIL";
    if ((s & shader_reads) && (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        return "shader-resource state with DENY_SHADER_RESOURCE";
    return nullptr;
}

const char* committed_resource_refusal(const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS heap_flags,
                                       const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
                                       const D3D12_CLEAR_VALUE* clear, bool acceleration_structures, bool uma) {
    if (const char* why = heap_properties_refusal(heap, uma)) return why;
    // The DENY flags of a committed resource's heap are the runtime's to choose; most other bits are refused too.
    constexpr UINT committed_heap_flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_DISPLAY |
                                          D3D12_HEAP_FLAG_HARDWARE_PROTECTED | D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT |
                                          D3D12_HEAP_FLAG_CREATE_NOT_ZEROED | D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS;
    if (UINT(heap_flags) & ~committed_heap_flags) return "heap flags of a committed resource";

    const bool buffer = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    // 0x400 is not in this SDK's d3d12.h (10.0.26100), but the host accepts it on buffers and textures; 0x200 and
    // 0x800 it refuses. RAYTRACING_ACCELERATION_STRUCTURE is accepted on a texture too.
    constexpr UINT buffer_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                  D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
                                  D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE | 0x400;
    constexpr UINT texture_flags =
        buffer_flags | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    const UINT flags = UINT(desc.Flags);
    if (flags & ~(buffer ? buffer_flags : texture_flags)) return "resource flags";
    const bool rt = flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const bool ds = flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const bool uav = flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // ALLOW_SHADER_ATOMICS is for a resource shaders can write: RTX 4070 Laptop (621.24) and WARP refuse it without
    // ALLOW_UNORDERED_ACCESS, on buffers and textures alike. It changes nothing on one adapter; GetHeapProperties keeps it.
    if ((UINT(heap_flags) & D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS) && !uav)
        return "ALLOW_SHADER_ATOMICS without ALLOW_UNORDERED_ACCESS";
    // Upload heaps start in (a part of) GENERIC_READ, readback heaps in COPY_DEST; COMMON on both; no UAVs.
    if (heap.Type == D3D12_HEAP_TYPE_UPLOAD && (uav || (UINT(state) & ~UINT(D3D12_RESOURCE_STATE_GENERIC_READ))))
        return "upload heap state or UAV";
    if (heap.Type == D3D12_HEAP_TYPE_READBACK && (uav || (UINT(state) & ~UINT(D3D12_RESOURCE_STATE_COPY_DEST))))
        return "readback heap state or UAV";

    if (const char* why = initial_state_refusal(buffer, flags, state, acceleration_structures)) return why;

    if (buffer) {
        if (!desc.Width || desc.Height != 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
            desc.Format != DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
            desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
            return "buffer description";
        if (desc.Alignment && desc.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) return "buffer alignment";
        if (clear) return "clear value on a buffer";
        if (heap_flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) return "display heap for a buffer";
        return nullptr;
    }
    if (desc.Dimension < D3D12_RESOURCE_DIMENSION_TEXTURE1D || desc.Dimension > D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        return "resource dimension";
    if ((rt && ds) || (ds && uav) || ((flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) && !ds))
        return "resource flag combination";
    if (ds && !depth_stencil_family(desc.Format)) return "depth-stencil format";
    if (rt && !render_target_family(desc.Format)) return "render-target format";
    const UINT samples = desc.SampleDesc.Count;
    if (samples > 1 && !rt && !ds) return "multisampled texture that is not a render target or depth buffer";
    // Every adapter measured reports one quality level; the two standard patterns are accepted too. They are spelled
    // with the DXGI names: D3D12_STANDARD/CENTER_MULTISAMPLE_PATTERN, the same values, are only in d3d12.h from
    // DirectX-Headers 1.613 on, and older Windows SDK 10.0.26100 headers lack them.
    if (samples > 1 && desc.SampleDesc.Quality &&
        desc.SampleDesc.Quality != DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN &&
        desc.SampleDesc.Quality != DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN)
        return "sample quality";
    // A clear value is only for render targets and depth buffers, in the resource's format family.
    if (clear && (!(rt || ds) || clear->Format == DXGI_FORMAT_UNKNOWN ||
                  format_family(clear->Format) != format_family(desc.Format)))
        return "clear value";
    return nullptr;
}

// Why the host refuses this enhanced-barrier initial layout for `desc`, or null when valid (measured on Windows D3D12).
const char* barrier_layout_refusal(const D3D12_RESOURCE_DESC& desc, D3D12_BARRIER_LAYOUT layout) {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return layout == D3D12_BARRIER_LAYOUT_UNDEFINED ? nullptr : "buffer barrier layout";
    const UINT flags = UINT(desc.Flags);
    switch (layout) {
    case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
        return (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? nullptr : "RENDER_TARGET barrier layout";
    case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS:
        return (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? nullptr : "UNORDERED_ACCESS barrier layout";
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
        return (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? nullptr : "DEPTH_STENCIL barrier layout";
    case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE:
        return (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ? "SHADER_RESOURCE barrier layout" : nullptr;
    default:
        return nullptr;
    }
}

// Console Xbox D3D12 CreateDescriptorHeap (device slot 14) passes the PC description followed by the heap address
// and size, as measured in the retail boot (issue #313 comment 7107265058): a 16-byte PC D3D12_DESCRIPTOR_HEAP_DESC,
// then address at +0x10 and size at +0x18. The title asserts GetCPUDescriptorHandleForHeapStart() == address.
// Backing this range with title memory / section 3.3 aliasing is tracked separately; this path only satisfies the
// observed ABI contract and must not be inferred from normal PC 16-byte CreateDescriptorHeap calls.
struct ConsoleDescriptorHeapDesc {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    std::uint64_t address = 0;
    std::uint64_t size = 0;
};

// The title-VA rebasing of docs/page_address_space.md (design B): a slot-133 stand-in is aliased under the VA the title
// placed the pages at, and the title's shader-visible VAs are rebased into the arena. PALMETTO_TITLE_VA=0 disables it,
// for an A/B boot in which every draw that names the title's pages stays refused, as the tree behaved before it.
inline bool title_va_rebasing() {
    static const bool on = [] {
        char value[8] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_TITLE_VA") != 0 || length <= 1) return true;
        return value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// The title's page content (docs/page_content.md): the bytes behind a title VA are copied into the arena range the GPU
// reads. PALMETTO_PAGE_PULL=0 disables the copy, for an A/B boot in which the stand-ins hold their own bytes.
inline bool title_pages_enabled() {
    static const bool on = [] {
        char value[8] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_PAGE_PULL") != 0 || length <= 1) return true;
        return value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// A title-placed texture's pages (docs/texture_import.md): the row-major bytes the title writes through the console
// texture computer are copied into the committed image before the first submission that can sample it. Opt-in, because
// the writes only exist when the XG shim answers with the linear layout (PALMETTO_XG_LAYOUT=linear); off by default so
// the default path is byte-identical to the previous stand-in behaviour.
inline bool placed_texture_import_enabled() {
    static const bool on = [] {
        char value[8] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_TEXTURE_IMPORT") != 0 || length <= 1) return false;
        return value[0] != '0' && value[0] != 'n';
    }();
    return on;
}

// 64 KiB, the console's page size and the granularity the title's own placeholders use.
constexpr std::uint64_t kConsoleHeapPage = 0x10000;

// Opt-in stand-in for a compute pipeline whose program has no translation (PALMETTO_STANDIN_PIPELINES=1). Diagnostic
// only: it lets the boot continue past a missing module so the next stop can be measured, and it says so in the log.
inline bool standin_pipelines() {
    static const bool on = [] {
        const char* value = std::getenv("PALMETTO_STANDIN_PIPELINES");
        return value && *value && *value != '0';
    }();
    return on;
}

// The VS+PS pair the graphics stand-in is built from: console device slot 57 creates a graphics pipeline from a
// deserialized record this tree cannot convert yet, so with PALMETTO_STANDIN_PIPELINES on it hands the title a real
// graphics pipeline built from this pair instead of a null PSO (create_standin_graphics_pipeline at the end of this
// file). The pair is this project's own: the empty-interface partner stubs tooling/spirv_stubs.py emits for the
// translator's pipeline-check tests (there as SPIR-V 1.0), embedded here as words so a stand-in needs no shader
// directory, no library entry and no game data - a vertex shader writing BuiltIn Position and a fragment shader
// writing Location 0. Diagnostic only: the stand-in's draws do not do the record's work.
inline constexpr std::uint32_t kStandinVsSpirv[] = {
    0x07230203u, 0x00010000u, 0x00000000u, 0x0000000du,
    0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0006000fu, 0x00000000u,
    0x0000000bu, 0x6e69616du, 0x00000000u, 0x0000000au,
    0x00040047u, 0x0000000au, 0x0000000bu, 0x00000000u,
    0x00020013u, 0x00000001u, 0x00030021u, 0x00000002u,
    0x00000001u, 0x00030016u, 0x00000003u, 0x00000020u,
    0x00040017u, 0x00000004u, 0x00000003u, 0x00000004u,
    0x00040020u, 0x00000005u, 0x00000003u, 0x00000004u,
    0x0004002bu, 0x00000003u, 0x00000006u, 0x00000000u,
    0x0004002bu, 0x00000003u, 0x00000007u, 0x3f800000u,
    0x0007002cu, 0x00000004u, 0x00000008u, 0x00000006u,
    0x00000006u, 0x00000006u, 0x00000007u, 0x0007002cu,
    0x00000004u, 0x00000009u, 0x00000006u, 0x00000006u,
    0x00000006u, 0x00000006u, 0x0004003bu, 0x00000005u,
    0x0000000au, 0x00000003u, 0x00050036u, 0x00000001u,
    0x0000000bu, 0x00000000u, 0x00000002u, 0x000200f8u,
    0x0000000cu, 0x0003003eu, 0x0000000au, 0x00000008u,
    0x000100fdu, 0x00010038u,
};
inline constexpr std::uint32_t kStandinPsSpirv[] = {
    0x07230203u, 0x00010000u, 0x00000000u, 0x0000000bu,
    0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0006000fu, 0x00000004u,
    0x00000009u, 0x6e69616du, 0x00000000u, 0x00000008u,
    0x00030010u, 0x00000009u, 0x00000007u, 0x00040047u,
    0x00000008u, 0x0000001eu, 0x00000000u, 0x00020013u,
    0x00000001u, 0x00030021u, 0x00000002u, 0x00000001u,
    0x00030016u, 0x00000003u, 0x00000020u, 0x00040017u,
    0x00000004u, 0x00000003u, 0x00000004u, 0x00040020u,
    0x00000005u, 0x00000003u, 0x00000004u, 0x0004002bu,
    0x00000003u, 0x00000006u, 0x00000000u, 0x0007002cu,
    0x00000004u, 0x00000007u, 0x00000006u, 0x00000006u,
    0x00000006u, 0x00000006u, 0x0004003bu, 0x00000005u,
    0x00000008u, 0x00000003u, 0x00050036u, 0x00000001u,
    0x00000009u, 0x00000000u, 0x00000002u, 0x000200f8u,
    0x0000000au, 0x0003003eu, 0x00000008u, 0x00000007u,
    0x000100fdu, 0x00010038u,
};

// The stand-in module for one graphics stage name; empty for a stage the stand-in does not cover.
inline const std::vector<std::uint32_t>& standin_stage_words(const char* name) {
    static const std::vector<std::uint32_t> vs(std::begin(kStandinVsSpirv), std::end(kStandinVsSpirv));
    static const std::vector<std::uint32_t> ps(std::begin(kStandinPsSpirv), std::end(kStandinPsSpirv));
    static const std::vector<std::uint32_t> none;
    if (name && std::strcmp(name, "vs") == 0) return vs;
    if (name && std::strcmp(name, "ps") == 0) return ps;
    return none;
}

// The library entry a stand-in stage reports: default-constructed, so it has no bindings, literal samplers, input
// signature, subgroup request or f16 halves. The rest of create_graphics then handles a stand-in stage exactly like a
// translated one, without a library record (which matters when the shader directory failed to load).
inline const palmetto::ShaderEntry& standin_entry() {
    static const palmetto::ShaderEntry entry;
    return entry;
}

// Map a pagefile-backed section over [address, address+size) at that exact address, one 64 KiB page at a time.
// Returns the address when every page was mapped, null otherwise.
inline void* console_heap_back(std::uint64_t address, SIZE_T size) {
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                           PMEM_EXTENDED_PARAMETER, ULONG);
    static const auto map_view3 = [] {
        const HMODULE kernelbase = ::GetModuleHandleW(L"kernelbase.dll");
        return reinterpret_cast<MapViewOfFile3Fn>(
            kernelbase ? ::GetProcAddress(kernelbase, "MapViewOfFile3") : nullptr);
    }();
    if (!map_view3 || size == 0) return nullptr;

    const SIZE_T page = static_cast<SIZE_T>(kConsoleHeapPage);
    for (SIZE_T done = 0; done < size; done += page) {
        // A page that is already committed and writable needs no mapping: the title's range can overlap memory this
        // process already owns (another console heap, or a section the loader mapped), and replacing it there would
        // fail and be reported as "not backed" even though writes would have landed.
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<void*>(static_cast<std::uintptr_t>(address + done)), &info,
                           sizeof(info)) == sizeof(info)) {
            const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (info.State == MEM_COMMIT && (info.Protect & writable) && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                continue;
        }
        const HANDLE section = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                    static_cast<DWORD>(page), nullptr);
        if (!section) return nullptr;
        void* const target = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address + done));
        const void* mapped = map_view3(section, ::GetCurrentProcess(), target, 0, page, MEM_REPLACE_PLACEHOLDER,
                                       PAGE_READWRITE, nullptr, 0);
        ::CloseHandle(section);
        if (mapped != target) {
            // The range is not a placeholder here (or is already backed); try a plain commit of that page.
            if (!::VirtualAlloc(target, page, MEM_COMMIT, PAGE_READWRITE)) return nullptr;
        }
    }
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
}

// One line describing what the address range is, so a backing failure names its cause instead of only its effect.
inline void console_heap_report(std::uint64_t address, SIZE_T size) {
    MEMORY_BASIC_INFORMATION info{};
    const SIZE_T got = ::VirtualQuery(reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), &info, sizeof(info));
    if (got != sizeof(info)) {
        std::fprintf(stderr, "[rt] d3d12: console heap at 0x%llx: VirtualQuery failed (error %lu)\n",
                     static_cast<unsigned long long>(address), GetLastError());
        return;
    }
    std::fprintf(stderr,
                 "[rt] d3d12: console heap at 0x%llx, %llu bytes: state 0x%lx, protect 0x%lx, type 0x%lx, "
                 "region 0x%llx+0x%llx\n",
                 static_cast<unsigned long long>(address), static_cast<unsigned long long>(size),
                 static_cast<unsigned long>(info.State), static_cast<unsigned long>(info.Protect),
                 static_cast<unsigned long>(info.Type),
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info.BaseAddress)),
                 static_cast<unsigned long long>(info.RegionSize));
}

class DescriptorHeap final
    : public Child<DescriptorHeap, ID3D12DescriptorHeapStubs, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable,
                   ID3D12DescriptorHeap> {
public:
    // {4a7d2c90-5e1b-4f63-8c0a-91d3b6e2f417}
    static constexpr GUID kTag{0x4a7d2c90, 0x5e1b, 0x4f63, {0x8c, 0x0a, 0x91, 0xd3, 0xb6, 0xe2, 0xf4, 0x17}};

    // A shader-visible heap lives in the GPU address arena: its CPU handles point at the arena's host mapping and
    // its GPU handle is the console VA, so the bytes the game or Create*View writes are what shaders load. Other
    // heaps (RTV, DSV, non-visible staging heaps) stay in host memory.
    // A shader-visible heap that the arena cannot hold is not created (ok() is false, E_OUTOFMEMORY), as a
    // default-heap buffer is not: in host memory its GPU handle would be 0, and every table would read VA 0.
    DescriptorHeap(DeviceState& device, const D3D12_DESCRIPTOR_HEAP_DESC& desc, std::uint64_t console_address = 0,
                   std::uint64_t console_size = 0)
        : device_(device), desc_(desc), console_address_(console_address), console_size_(console_size),
          // A console heap address is a 64 KiB-aligned title VA. Anything else is a field that happens to sit where
          // the address would be - the retail boot hands 0x20 for one heap, which is no address at all - and treating
          // it as one takes the heap out of the arena and then fails to back it.
          console_addressed_(console_address != 0 && (console_address % kConsoleHeapPage) == 0 &&
                             console_address >= kConsoleHeapPage) {
        const VkDeviceSize bytes = VkDeviceSize(desc_.NumDescriptors) * kDescriptorSize;
        if (!console_addressed_ && console_address != 0) {
            log_once("descriptor heap carries a value that is not a console address (not 64 KiB aligned); "
                     "using the arena, low word",
                     static_cast<int>(console_address & 0xffffu));
        }
        if (console_addressed_) {
            // The retail boot checks GetCPUDescriptorHandleForHeapStart() == address and then writes descriptors
            // through that handle, so the title's range has to be real memory at that address. Map exactly the
            // descriptor bytes, rounded to the page granularity xmem's placeholders use: the title reserved this
            // range itself, and a pagefile-backed section replaces the placeholder page by page.
            const SIZE_T want = static_cast<SIZE_T>(
                (std::max<VkDeviceSize>(bytes, 1) + kConsoleHeapPage - 1) & ~(kConsoleHeapPage - 1));
            void* backing = console_heap_back(console_address_, want);
            log_once(backing ? "descriptor heap uses console address, backed in place"
                             : "descriptor heap uses console address, NOT backed (writes will fault)",
                     int(desc_.Type));
            if (!backing)
                console_heap_report(console_address_, want);
            cpu_ = reinterpret_cast<std::uint8_t*>(backing ? backing
                                                          : reinterpret_cast<void*>(static_cast<std::uintptr_t>(
                                                                console_address_)));
            in_arena_ = false;
            // A shader-visible heap's descriptors are CPU memory the GPU cannot address: a shader that reads a table
            // at heap_base + table_VA only lands in an arena range. Keep an arena mirror of the descriptors here, which
            // a queue refreshes before every submission, and register the title's console VA as an alias for it
            // (docs/page_address_space.md). The GPU handle stays the console VA, so the title's own address arithmetic
            // is unchanged; every copy of that VA is rebased to the mirror at replay.
            const bool shader_visible = (desc_.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
            if (!shader_visible || !bytes || !title_va_rebasing()) return;
            if (!backing || !device.arena.mapped) {
                // Nothing the GPU could read: a draw that names this heap must stay refused, never executed.
                device.context->mark_console_va_incomplete();
                log_once("a console descriptor heap cannot be mirrored into the arena; draws that name it stay "
                         "refused; bytes",
                         int(std::min<VkDeviceSize>(bytes, INT_MAX)));
                return;
            }
            if (!device.arena_allocate(bytes, 256, mirror_offset_, DeviceState::arena_descriptor_heap)) {
                device.context->mark_console_va_incomplete();
                log_once("GPU address arena full for a console descriptor heap mirror; draws that name it stay "
                         "refused; bytes",
                         int(std::min<VkDeviceSize>(bytes, INT_MAX)));
                return;
            }
            mirrored_ = true;
            device.context->add_console_va_range(console_address_, bytes, mirror_offset_,
                                                 palmetto::ConsoleVaKind::descriptors);
            device.add_console_heap(this);
            sync_console_descriptors();
            return;
        }
        if ((desc_.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && device.arena.mapped) {
            if (!device.arena_allocate(bytes, 256, arena_offset_, DeviceState::arena_descriptor_heap)) {
                log_once("GPU address arena full (raise PALMETTO_ARENA_MB); shader-visible heap size in KiB:",
                         int(std::min<VkDeviceSize>(bytes / 1024, INT_MAX)));
                return;
            }
            in_arena_ = true;
            cpu_ = device.arena.mapped + arena_offset_;
            std::memset(cpu_, 0, std::size_t(bytes));
            // The title can write its own V#s here through the CPU handle, naming its pages by title VAs. Those bases
            // are rebased in place before each submission, like a console heap's mirror (docs/page_address_space.md).
            if (!title_va_rebasing()) return;
            scan_in_place_ = true;
            device.add_console_heap(this);
        } else {
            storage_.reset(new std::byte[std::size_t(bytes)]());
            cpu_ = reinterpret_cast<std::uint8_t*>(storage_.get());
        }
    }

    ~DescriptorHeap() override {
        if (mirrored_) {
            device_.remove_console_heap(this);  // no refresh in flight once this returns
            device_.context->remove_console_va_range(console_address_);
            device_.arena_release(mirror_offset_, VkDeviceSize(desc_.NumDescriptors) * kDescriptorSize,
                                  DeviceState::arena_descriptor_heap);
        } else if (scan_in_place_) {
            device_.remove_console_heap(this);
        }
        if (in_arena_) device_.arena_release(arena_offset_, VkDeviceSize(desc_.NumDescriptors) * kDescriptorSize,
                                            DeviceState::arena_descriptor_heap);
    }

    // Copies the title's CPU descriptors into the arena mirror, rebasing the V# bases the title wrote there to the
    // arena ranges that stand in for the title-placed buffers they name. Runs on the replay thread before a
    // submission, with the device's console-heap list locked (DeviceState::sync_console_heaps).
    void sync_console_descriptors() {
        if (!cpu_) return;
        const VkDeviceSize bytes = VkDeviceSize(desc_.NumDescriptors) * kDescriptorSize;
        if (!mirrored_) {
            // A shader-visible heap in the arena: the title may have written its own V#s straight into it through the
            // CPU handle, so the bases that name a title-placed buffer are rewritten in place. The rewrite is
            // idempotent: a rebased base is an arena VA, which no title range holds.
            if (!scan_in_place_) return;
            const std::size_t patched = device_.context->patch_console_descriptor_bases(cpu_, std::size_t(bytes));
            static std::atomic<unsigned> logged{0};
            if (patched && logged.fetch_add(1, std::memory_order_relaxed) < 4)
                std::fprintf(stderr,
                             "[rt] d3d12: shader-visible heap in the arena at VA 0x%llx: %zu title-written V# base(s) "
                             "rebased\n",
                             static_cast<unsigned long long>(arena_offset_), patched);
            return;
        }
        // Nothing the title wrote changed since the last refresh: copying the same bytes again would only cost time.
        std::uint8_t* mirror = device_.arena.mapped + mirror_offset_;
        if (mirrored_bytes_.size() != bytes) {
            // First refresh: the whole heap at once.
            mirrored_bytes_.assign(cpu_, cpu_ + bytes);
            std::memcpy(mirror, cpu_, std::size_t(bytes));
            const std::size_t patched = device_.context->patch_console_descriptor_bases(mirror, std::size_t(bytes));
            static std::atomic<unsigned> logged{0};
            if (patched && logged.fetch_add(1, std::memory_order_relaxed) < 4)
                std::fprintf(stderr,
                             "[rt] d3d12: console descriptor heap 0x%llx: %zu V# base(s) rebased in its arena mirror at "
                             "VA 0x%llx\n",
                             static_cast<unsigned long long>(console_address_), patched,
                             static_cast<unsigned long long>(mirror_offset_));
            return;
        }
        // A frame rewrites only a few descriptors, but the heap's GPU-visible mirror is large: copy and rebase the
        // slots that changed, so a submission costs one pass over the title's (CPU) bytes, not a rewrite of the whole
        // mirror. The mirror is GPU-readable memory (BAR/system), where a full rewrite is the expensive side.
        std::size_t changed = 0, patched_total = 0;
        for (std::size_t at = 0; at + kDescriptorSize <= std::size_t(bytes);
             at += kDescriptorSize) {
            if (std::memcmp(cpu_ + at, mirrored_bytes_.data() + at, kDescriptorSize) == 0) continue;
            std::memcpy(mirror + at, cpu_ + at, kDescriptorSize);
            std::memcpy(mirrored_bytes_.data() + at, cpu_ + at, kDescriptorSize);
            patched_total += device_.context->patch_console_descriptor_bases(mirror + at, kDescriptorSize);
            ++changed;
        }
        static std::atomic<unsigned> logged_changes{0};
        if (changed && logged_changes.fetch_add(1, std::memory_order_relaxed) < 4)
            std::fprintf(stderr,
                         "[rt] d3d12: console descriptor heap 0x%llx: %zu changed slot(s), %zu V# base(s) rebased\n",
                         static_cast<unsigned long long>(console_address_), changed, patched_total);
    }

    bool ok() const { return cpu_ != nullptr; }

    // A node mask of 0 reads back as the one node of a single adapter, as on the PC runtime (d3d12_node_mask_test).
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override {
        D3D12_DESCRIPTOR_HEAP_DESC desc = desc_;
        if (!desc.NodeMask) desc.NodeMask = 1;
        return desc;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override {
        return {reinterpret_cast<SIZE_T>(cpu_)};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override {
        if (!(desc_.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return {0};
        if (console_addressed_) return {static_cast<UINT64>(console_address_)};
        if (!in_arena_) log_once("shader-visible heap without a GPU address arena; flags", int(desc_.Flags));
        return {in_arena_ ? UINT64(arena_offset_) : 0};
    }

    const D3D12_DESCRIPTOR_HEAP_DESC& raw_desc() const { return desc_; }
    D3D12_GPU_VIRTUAL_ADDRESS gpu_base() const {
        if (console_addressed_) return static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(console_address_);
        return in_arena_ ? UINT64(arena_offset_) : 0;
    }

private:
    DeviceState& device_;
    D3D12_DESCRIPTOR_HEAP_DESC desc_;
    std::unique_ptr<std::byte[]> storage_;
    std::uint8_t* cpu_ = nullptr;
    VkDeviceSize arena_offset_ = 0;
    bool in_arena_ = false;
    std::uint64_t console_address_ = 0;
    std::uint64_t console_size_ = 0;
    bool console_addressed_ = false;
    // The arena mirror of a console-addressed shader-visible heap's descriptors (docs/page_address_space.md), and
    // whether it exists: without one the heap's table VAs stay unrebased, and draws that name it are refused.
    VkDeviceSize mirror_offset_ = 0;
    bool mirrored_ = false;
    bool scan_in_place_ = false;  // a shader-visible heap in the arena: title-written V# bases are rebased in place
    std::vector<std::uint8_t> mirrored_bytes_;  // the console bytes the mirror was last made from (unrebased)
};

// Called by the queue before it records a submission's lists (ReplayFunctions::sync_console_heaps): every
// shader-visible descriptor heap a title can write itself gets the title VAs it wrote rebased, so nothing a shader
// reads through a table names a VA outside the arena the shader adds heap_base to.
void DeviceState::sync_console_heaps() {
    const auto started = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(console_heaps_mutex);
        for (DescriptorHeap* heap : console_heaps) heap->sync_console_descriptors();
    }
    // Bounded diagnostic: what the refresh costs each submission, so a frame that turns slow can be told apart from a
    // GPU-side cost. Microseconds, counts only.
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 12) {
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
        std::fprintf(stderr, "[rt] d3d12: console heap refresh took %.0f us\n", us);
    }
}

class Resource final
    : public Child<Resource, ID3D12ResourceStubs, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Resource> {
public:
    // {9b3e5f12-7c48-4d0e-a6b1-2f8d4c7e9a35}
    static constexpr GUID kTag{0x9b3e5f12, 0x7c48, 0x4d0e, {0xa6, 0xb1, 0x2f, 0x8d, 0x4c, 0x7e, 0x9a, 0x35}};

    Resource(DeviceState& device, const D3D12_RESOURCE_DESC& desc, const D3D12_HEAP_PROPERTIES& heap,
             D3D12_HEAP_FLAGS heap_flags)
        : device_(device), desc_(desc), heap_(heap), heap_flags_(heap_flags) {}

    // A placed resource lives inside a heap's arena range: the heap owns the range, so only the console VA is kept
    // here and nothing goes back to the arena when this object dies.
    Resource(DeviceState& device, const D3D12_RESOURCE_DESC& desc, const D3D12_HEAP_PROPERTIES& heap,
             D3D12_HEAP_FLAGS heap_flags, VkDeviceSize placed_va)
        : device_(device), desc_(desc), heap_(heap), heap_flags_(heap_flags), placed_(true), va_(placed_va) {}

    ~Resource() override {
        // The title VA this resource's stand-in was aliased under goes away with it (docs/page_address_space.md).
        if (console_alias_) device_.context->remove_console_va_range(console_alias_);
        if (accel_) {
            device_.context->unregister_acceleration_structure(gpu_va_);
            if (buffer_va_ && buffer_va_ != gpu_va_) device_.context->unregister_acceleration_structure(buffer_va_);
            device_.context->defer_destroy(palmetto::DeferredDeletionQueue::Kind::acceleration_structure, accel_);
        }
        if (image_address_) device_.forget_image(image_address_);
        if (placed_pages_) {
            // Diagnostic (docs/frame_capture.md): the title VA is free again; drop the placed-resource registry entry.
            std::lock_guard resources_lock(device_.placed_resources_mutex);
            const auto found = device_.placed_resources.find(placed_pages_);
            if (found != device_.placed_resources.end() && found->second == this)
                device_.placed_resources.erase(found);
        }
        device_.context->remove_resource(id());
        if (host_import_) {
            // VK_EXT_external_memory_host needs the caller's pages to stay valid for the imported memory's whole
            // lifetime, and a title may free them right after releasing the resource. So the memory is not left to
            // the deferred queue (freed after the *next* submission retires): wait for what was submitted, then
            // free it now (found in review).
            device_.context->wait_retire_ticket(device_.context->submitted_retire_ticket());
            auto& vk = device_.context->vk;
            if (buffer_) vk.vkDestroyBuffer(device_.info.device, buffer_, nullptr);
            if (memory_) vk.vkFreeMemory(device_.info.device, memory_, nullptr);
            buffer_ = VK_NULL_HANDLE;
            memory_ = VK_NULL_HANDLE;
        }
        if (in_arena_) device_.arena_release(arena_offset_, desc_.Width, DeviceState::arena_buffer);
        if (image_ && !sparse_image_)
            device_.context->defer_destroy(palmetto::DeferredDeletionQueue::Kind::image, image_);
        if (buffer_) device_.context->defer_destroy(palmetto::DeferredDeletionQueue::Kind::buffer, buffer_);
        // Deferred backing: an image never bound holds no memory, and the queue hook must stop waiting for it.
        if (deferred_ && !backing_bound_) device_.remove_deferred_image(this);
        if (pooled_range_) {
            // The range goes back to its block after the submissions that used the image have retired, in the same
            // deferred batch as the image's destruction just above. The token keeps the range alive until then.
            auto range = std::move(pooled_range_);
            auto release = new std::function<void()>([range] {
                (void)range;  // dropped here: its destructor hands the bytes back to the block
            });
            device_.context->defer_destroy(palmetto::DeferredDeletionQueue::Kind::pooled_image_memory,
                                           std::uint64_t(reinterpret_cast<std::uintptr_t>(release)));
            device_.pool_ranges_live.fetch_sub(1, std::memory_order_relaxed);
        }
        if (memory_) device_.context->defer_destroy(palmetto::DeferredDeletionQueue::Kind::device_memory, memory_);
    }

    palmetto::ResourceId id() const { return resource_id(this); }
    // The title VA a placed texture was placed at (device slot 133), or 0 for an ordinary committed resource
    // (docs/frame_capture.md, the texture-source mapping chain).
    std::uint64_t placed_pages() const { return placed_pages_; }
    // Where a title-placed texture's bytes are read on this host: the console placement VA itself, or the paired CPU
    // reservation's address at the same offset when the texture was placed in a GPU-view window
    // (docs/peer_port_movie.md section 11; 0 for a resource that was not resolved through a pair).
    std::uint64_t placed_read_pages() const { return placed_source_pages_ ? placed_source_pages_ : placed_pages_; }
    // The bytes of subresource 0's tightly row-major footprint, or 0 for a resource that is not a title-placed texture
    // (docs/texture_source.md: the page-writer watch watches exactly the span the import would read).
    std::uint64_t placed_subresource_bytes() const {
        if (!image_ || !placed_pages_) return 0;
        Region r{};
        if (!region(0, nullptr, r) || !r.rows || !r.slices) return 0;
        return r.row_bytes * r.rows * r.slices;
    }
    VkImage image() const { return image_; }
    bool sparse_image() const { return sparse_image_; }
    VkBuffer buffer() const { return buffer_; }
    const D3D12_RESOURCE_DESC& desc() const { return desc_; }
    VkFormat format() const { return format_; }                  // of the image
    bool has_view_format_list() const { return !view_formats_.empty(); }
    bool allows_view_format(VkFormat format) const {
        return view_formats_.empty() || format == format_ ||
               std::find(view_formats_.begin(), view_formats_.end(), format) != view_formats_.end();
    }
    // The castable-format list in DXGI formats, which the Vulkan list can't tell apart where two families share a
    // VkFormat: B8G8R8X8 and B8G8R8A8 are both BGRA8 images.
    void keep_castable_formats(UINT32 count, const DXGI_FORMAT* formats) {
        if (count && formats) castable_formats_.assign(formats, formats + count);
    }
    bool lists_castable_format(DXGI_FORMAT format) const {
        return std::find(castable_formats_.begin(), castable_formats_.end(), format) != castable_formats_.end();
    }
    bool has_castable_formats() const { return !castable_formats_.empty(); }
    VkFormat uint_backed() const { return uint_backed_; }  // Format::uint_backed
    VkImageAspectFlags aspects() const { return aspects_; }
    bool rgb96() const { return rgb96_; }                        // a 96-bit texture in an RGBA32 image
    std::uint64_t image_address() const { return image_address_; }  // what a T# names the texture by
    VkMemoryPropertyFlags memory_flags() const { return memory_flags_; }  // of the memory type behind it
    const DeviceState& device_state() const { return device_; }

    // This buffer as an arena range, when it is one: its console VA is an arena offset and heap_base + VA is its device
    // address. That is what lets the VA the title placed a slot-133 resource at be an alias for it (docs page
    // page_address_space.md).
    bool arena_range(VkDeviceSize& offset, VkDeviceSize& bytes) const {
        if (!in_arena_ || !desc_.Width) return false;
        offset = arena_offset_;
        bytes = desc_.Width;
        return true;
    }
    // Registers the title VA `title` as an alias for this buffer's arena range, so command arguments that name the
    // pages by the VA the title chose resolve to the stand-in (VulkanContext::buffer_at) and every shader-visible copy
    // of the VA is rebased to the arena range (ReplayFunctions::rebase_console_va).
    void alias_console_va(std::uint64_t title) {
        if (!title_va_rebasing()) return;
        VkDeviceSize offset = 0, bytes = 0;
        if (!title || !arena_range(offset, bytes)) return;
        console_alias_ = title;
        device_.context->add_console_va_range(title, bytes, offset, palmetto::ConsoleVaKind::buffer);
    }

    // A title-placed texture's stand-in (device slot 133, PALMETTO_TEXTURE_IMPORT). The title writes its texture bytes
    // into `address` - in the opt-in XG linear mode the fill is a row-major CPU copy per subresource (docs/texture_layout.md)
    // - and this committed image is what the GPU samples, so before the first submission that can sample it the pages
    // are copied into the image, the texture analogue of alias_console_va (docs/texture_import.md). One bounded import
    // per resource; a description with more than one subresource imports subresource 0 only, because the title's linear
    // subresource order is not measured yet (the measured textures are 1-mip).
    void set_placed_pages(std::uint64_t address) { placed_pages_ = address; }
    // The console's answer to GetGPUVirtualAddress for a title-placed texture (device slot 133): the title's own
    // placement address, as the console's placed pages are CPU-addressable (Xbox unified memory). The PC committed
    // stand-in is not backed by those pages; this is the console translation only, chosen in
    // DeviceState::create_at_host_address and never on the PC paths (docs/3d_textures.md).
    void set_console_placed_va(std::uint64_t address) { console_placed_va_ = address; }
    bool has_console_placed_va() const { return console_placed_va_ != 0; }
    // Returns true when the import must be tried again at the next submission: the fill runs on the title's thread
    // after it creates the texture, so the first submissions after creation can still see all-zero or uncommitted
    // pages. Bounded (a fixed attempt budget), and every give-up is logged once.
    bool import_placed_pages() {
        if (!image_ || !placed_pages_ || placed_imported_ || placed_skipped_) return false;
        // The first host write is one of the two points a deferred image is backed at: the copy below needs the image
        // bound. A backing failure gives up this import rather than writing into an unbound image.
        if (deferred_ && !ensure_backing()) {
            placed_skipped_ = true;
            return false;
        }
        constexpr unsigned kMaxAttempts = 512;  // ~a second of submissions at the measured rate
        // Bounded by what the title's texture-computer path can fill in one boot (docs/texture_layout.md measured
        // 34,044 B; the cap leaves room for more of the same kind while keeping one import from dominating a
        // submission). A description larger than that is a title-placed render target or shadow, whose pages the
        // texture computer never writes; importing those would put uninitialised memory into an image the frame
        // renders into, so they are skipped and logged. Diagnostic (docs/texture_source.md): PALMETTO_TEXTURE_IMPORT_MAX
        // lifts the cap for one measurement, loudly; the default and the default path are unchanged.
        static const std::uint64_t placed_import_cap = [] {
            const char* value = std::getenv("PALMETTO_TEXTURE_IMPORT_MAX");
            return value && *value ? std::strtoull(value, nullptr, 0) : std::uint64_t(16) * 1024 * 1024;
        }();
        if (const char* override_value = std::getenv("PALMETTO_TEXTURE_IMPORT_MAX");
            override_value && *override_value) {
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 1)
                std::fprintf(stderr,
                             "[rt] d3d12: placed texture import: size cap overridden to %llu B by "
                             "PALMETTO_TEXTURE_IMPORT_MAX (default %u B)\n",
                             static_cast<unsigned long long>(placed_import_cap), 16u * 1024u * 1024u);
        }
        const std::uint32_t subresources = binding_.mip_levels * binding_.array_layers;
        if (!subresources) {
            placed_skipped_ = true;
            return false;
        }
        Region r{};
        if (!region(0, nullptr, r) || !r.rows || !r.slices) {
            placed_skipped_ = true;
            return false;
        }
        const std::uint64_t sub_bytes = r.row_bytes * r.rows * r.slices;
        if (!sub_bytes || sub_bytes > placed_import_cap || r.row_bytes > UINT32_MAX || r.row_bytes * r.rows > UINT32_MAX) {
            placed_skipped_ = true;
            // Diagnostic (docs/texture_source.md): which placed texture a decision was made for, so a sampled T# can be
            // followed to its resource and the reason its pages were not imported. Bounded to eight of each decision.
            static std::atomic<unsigned> too_large{0};
            if (too_large.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr,
                             "[rt] d3d12: placed texture import: resource 0x%llx at title VA 0x%llx is larger than "
                             "the import size cap (%llu B; PALMETTO_TEXTURE_IMPORT_MAX lifts it); left as the committed "
                             "stand-in\n",
                             static_cast<unsigned long long>(id()),
                             static_cast<unsigned long long>(placed_pages_),
                             static_cast<unsigned long long>(sub_bytes));
            return false;
        }
        if (++placed_attempts_ > kMaxAttempts) {
            placed_skipped_ = true;
            static std::atomic<unsigned> gave_up{0};
            if (gave_up.fetch_add(1, std::memory_order_relaxed) < 24)
                std::fprintf(stderr,
                             "[rt] d3d12: placed texture import: resource 0x%llx at title VA 0x%llx: pages never held "
                             "texture-computer bytes after %u attempt(s); left as the committed stand-in\n",
                             static_cast<unsigned long long>(id()),
                             static_cast<unsigned long long>(placed_pages_), kMaxAttempts);
            return false;
        }
        // docs/peer_port_movie.md section 11: a texture the title placed inside its GPU-view reservation has no pages in
        // that window on this host - the title maps them into the CPU reservation the GPU view is paired with, and the
        // shim's resolve hook names the same offset there. That address is kept for every later attempt and for the
        // per-submission refresh, so the import and the refresh read one source. With the pairing switch off the hook
        // answers "unavailable" and the source is the console placement VA, the path before this port, byte for byte.
        std::uint64_t read_address = placed_pages_;
        if (rt::xmem::gpu_view_resolve) {
            std::uint64_t cpu = 0;
            const rt::xmem::GpuViewResolve answer = rt::xmem::gpu_view_resolve(placed_pages_, sub_bytes, &cpu);
            if (answer == rt::xmem::GpuViewResolve::gpu_only) {
                // The VA is inside a GPU-view reservation with no pair: reading that window would read memory no title
                // page is mapped at. Refuse by name (bounded) rather than import what is there.
                placed_skipped_ = true;
                static std::atomic<unsigned> refused{0};
                if (refused.fetch_add(1, std::memory_order_relaxed) < 8)
                    std::fprintf(stderr,
                                 "[rt] d3d12: placed texture import: resource 0x%llx at console VA 0x%llx is in a "
                                 "GPU-view reservation with no paired CPU reservation; left as the committed stand-in "
                                 "(the window holds no title pages on this host)\n",
                                 static_cast<unsigned long long>(id()),
                                 static_cast<unsigned long long>(placed_pages_));
                return false;
            }
            if (answer == rt::xmem::GpuViewResolve::paired) {
                placed_source_pages_ = cpu;
                read_address = cpu;
                static std::atomic<unsigned> resolved{0};
                if (resolved.fetch_add(1, std::memory_order_relaxed) < 8)
                    std::fprintf(stderr,
                                 "[rt] d3d12: placed texture import: resource 0x%llx at console VA 0x%llx resolves "
                                 "through the paired CPU reservation at 0x%llx (subresource 0, %llu B)\n",
                                 static_cast<unsigned long long>(id()),
                                 static_cast<unsigned long long>(placed_pages_),
                                 static_cast<unsigned long long>(cpu),
                                 static_cast<unsigned long long>(sub_bytes));
            }
        }
        // The pages must be committed and readable: an unfilled texture's pages can still be uncommitted, and reading
        // them would fault inside the copy.
        for (std::uint64_t at = read_address; at < read_address + sub_bytes;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), &info, sizeof(info)) !=
                sizeof(info))
                return true;
            if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 || !info.RegionSize)
                return true;
            at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        }
        // Content gate: only a texture-computer fill writes these pages, and it writes non-zero bytes (the measured
        // copies are an atlas and white 1x1 defaults). An all-zero head means the fill has not run yet, so wait for a
        // later submission instead of importing zeros.
        {
            const auto* head = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(read_address));
            const std::uint64_t look = std::min<std::uint64_t>(sub_bytes, 4096);
            bool any = false;
            for (std::uint64_t i = 0; i < look && !any; ++i) any = head[i] != 0;
            if (!any) {
                if (!placed_wait_logged_) {
                    placed_wait_logged_ = true;
                    static std::atomic<unsigned> waiting{0};
                    if (waiting.fetch_add(1, std::memory_order_relaxed) < 32)
                        std::fprintf(stderr,
                                     "[rt] d3d12: placed texture import: resource 0x%llx at title VA 0x%llx: the "
                                     "pages' head is all zero, waiting for the texture-computer fill\n",
                                     static_cast<unsigned long long>(id()),
                                     static_cast<unsigned long long>(placed_pages_));
                }
                return true;
            }
        }
        // cpu_copy's pack takes a tightly row-major source: row pitch = one row of texels, slice pitch = one slice.
        const void* pages = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(read_address));
        // The per-submission refresh (docs/peer_port_movie.md section 8): with the switch on, the pages are read once
        // here into a CPU shadow - the bytes the image gets and the bytes the next submission compares the title's pages
        // against. With the switch off nothing is allocated and the import below is the tree's, byte for byte.
        const palmetto::PlaneRefreshPlan refresh = device_.plane_refresh_plan();
        const bool refreshable = refresh.enabled() && r.slices == 1 && r.row_bytes <= UINT32_MAX;
        if (refreshable) {
            placed_shadow_.resize(static_cast<std::size_t>(sub_bytes));
            std::memcpy(placed_shadow_.data(), pages, static_cast<std::size_t>(sub_bytes));
        }
        const void* source = refreshable ? static_cast<const void*>(placed_shadow_.data()) : pages;
        if (FAILED(cpu_copy(true, 0, nullptr, const_cast<void*>(source), static_cast<UINT>(r.row_bytes),
                            static_cast<UINT>(r.row_bytes * r.rows), true))) {
            placed_skipped_ = true;
            static std::atomic<unsigned> unreadable{0};
            if (unreadable.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr,
                             "[rt] d3d12: placed texture import: resource 0x%llx at title VA 0x%llx: the pages could "
                             "not be imported; left as the committed stand-in\n",
                             static_cast<unsigned long long>(id()),
                             static_cast<unsigned long long>(placed_pages_));
            return false;
        }
        placed_imported_ = true;
        if (refreshable) {
            placed_row_bytes_ = static_cast<std::uint32_t>(r.row_bytes);
            placed_rows_ = r.rows;
            // The tracker is the pre-filter (watch mode) and only for a span whose guard cost the measurement bounds
            // (port/src/plane_refresh.h): a larger texture keeps the hash compare. An arm that fails (a page not
            // committed, another owner's guard, the tracker's range slots full) is the documented fallback: hash.
            if (refresh.arms_tracker() && sub_bytes <= palmetto::plane_refresh_watch_max_bytes) {
                const bool alias_watch = device_.title_alias_watch_enabled();
                const auto arm = alias_watch ? rt::xmem::page_watch_arm_aliases : rt::xmem::page_watch_arm;
                if (arm) placed_watch_armed_ = arm(pages, static_cast<std::size_t>(sub_bytes));
                if (alias_watch && !placed_watch_armed_) {
                    static std::atomic<unsigned> alias_fallbacks{0};
                    if (alias_fallbacks.fetch_add(1, std::memory_order_relaxed) < 4)
                        std::fprintf(stderr,
                                     "[rt] d3d12: placed texture 0x%llx: xmem alias watch unavailable or over its "
                                     "range bound; this texture remains on the hash compare\n",
                                     static_cast<unsigned long long>(id()));
                }
            }
        }
        device_.texture_imports.fetch_add(1, std::memory_order_relaxed);

        device_.texture_import_bytes.fetch_add(sub_bytes, std::memory_order_relaxed);
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
            std::fprintf(stderr,
                         "[rt] d3d12: placed texture import: resource 0x%llx at title VA 0x%llx: subresource 0, %llu B "
                         "from the title's pages into a %ux%u image%s\n",
                         static_cast<unsigned long long>(id()),
                         static_cast<unsigned long long>(placed_pages_),
                         static_cast<unsigned long long>(sub_bytes), r.box.right, r.box.bottom,
                         subresources > 1 ? " (more than one subresource: the rest is not imported yet)" : "");
        return false;
    }

    // ---- the per-submission refresh of a title-written texture (docs/peer_port_movie.md section 8) -------------------
    // The import above is one submission's answer; a title that rewrites its texture (the movie's planes are the
    // measured case) would keep the first frame. These are the impure half of port/src/plane_refresh.h: the shadow's
    // size, the one comparison and the narrow re-upload.
    bool placed_imported() const { return placed_imported_; }
    std::uint64_t placed_shadow_bytes() const { return placed_shadow_.size(); }
    bool placed_watch_armed() const { return placed_watch_armed_; }
    bool placed_refresh_available() const {
        return placed_imported_ && image_ && placed_pages_ && !placed_shadow_.empty() && placed_row_bytes_ && placed_rows_;
    }
    void stop_placed_refresh() { placed_refresh_tracking_ = false; }
    bool placed_refresh_tracking() const { return placed_refresh_tracking_; }
    void set_placed_refresh_tracking() { placed_refresh_tracking_ = true; }

    enum class RefreshOutcome : std::uint8_t {
        unchanged = 0,   // the pages match the shadow: nothing to upload
        changed = 1,     // the changed row band was re-uploaded
        unreadable = 2,  // the pages are not committed/readable now: left as they were, retried next submission
        failed = 3,      // the shape no longer matches the shadow, or the upload refused: the texture stops tracking
    };
    struct RefreshResult {
        RefreshOutcome outcome = RefreshOutcome::failed;
        std::uint64_t upload_bytes = 0;
        bool narrow = false;
    };

    // One submission's refresh: compare the title's pages against the imported shadow, copy the changed bytes into the
    // shadow and re-upload only the rows they touch (the whole subresource when the change cannot be narrowed: more than
    // one slice, a block-compressed format, or every row). Reads only the title's memory and writes only the stand-in.
    // The counters are the device's, updated by the walk: two queues on one device must not race them here.
    RefreshResult refresh_placed_pages() {
        RefreshResult result;
        if (!placed_refresh_available()) return result;
        Region r{};
        if (!region(0, nullptr, r) || !r.rows || r.slices != 1) return result;
        const std::uint64_t sub_bytes = r.row_bytes * r.rows;
        if (sub_bytes != placed_shadow_.size()) return result;
        // The pages must be committed and readable. PAGE_GUARD is not a refusal here: in watch mode the tracker's guards
        // are on these pages, the read trips them, and the walk re-arms them after the compare. PAGE_NOACCESS is. The
        // address is the one the import resolved (the paired CPU reservation for a texture placed in a GPU view), so the
        // refresh compares the same bytes the import took (docs/peer_port_movie.md section 11).
        const std::uint64_t read_address = placed_read_pages();
        for (std::uint64_t at = read_address; at < read_address + sub_bytes;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), &info, sizeof(info)) !=
                sizeof(info)) {
                result.outcome = RefreshOutcome::unreadable;
                return result;
            }
            if (info.State != MEM_COMMIT || (info.Protect & PAGE_NOACCESS) != 0 || !info.RegionSize) {
                result.outcome = RefreshOutcome::unreadable;
                return result;
            }
            at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        }
        const auto* pages = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(read_address));
        std::uint64_t first = 0, last = 0;
        if (!palmetto::changed_byte_range(placed_shadow_.data(), pages, sub_bytes, first, last)) {
            result.outcome = RefreshOutcome::unchanged;
            return result;
        }
        std::memcpy(placed_shadow_.data() + first, pages + first, static_cast<std::size_t>(last - first));
        std::uint32_t top = 0, bottom = 0;
        // Block-compressed rows are copy units of four texels: a box's top has to be a multiple of the block, which a
        // byte-derived row is not, so those formats re-upload the subresource whole.
        const bool narrow =
            r.block.block == 1 &&
            palmetto::changed_rows(r.row_bytes, r.rows, first, last, top, bottom) && bottom > top && bottom - top < r.rows;
        HRESULT hr = E_FAIL;
        if (narrow) {
            const D3D12_BOX box{0, static_cast<UINT>(top), 0, r.box.right, static_cast<UINT>(bottom), 1};
            const std::uint64_t rows = bottom - top;
            hr = cpu_copy(true, 0, &box, placed_shadow_.data() + std::uint64_t(top) * r.row_bytes,
                          static_cast<UINT>(r.row_bytes), static_cast<UINT>(r.row_bytes * rows), true);
            if (SUCCEEDED(hr)) {
                result.upload_bytes = r.row_bytes * rows;
                result.narrow = true;
            }
        } else {
            hr = cpu_copy(true, 0, nullptr, placed_shadow_.data(), static_cast<UINT>(r.row_bytes),
                          static_cast<UINT>(r.row_bytes * r.rows), true);
            if (SUCCEEDED(hr)) result.upload_bytes = sub_bytes;
        }
        if (FAILED(hr)) return result;
        result.outcome = RefreshOutcome::changed;
        return result;
    }

    // Default-heap buffers are ranges of the GPU address arena: console VA = offset, zeroed through the arena's host
    // mapping. Upload and readback buffers keep their own host memory; with buffer device addresses their console
    // VA is their device address minus heap_base, so a shader's heap_base + VA still lands on them (section 3.3).
    HRESULT create_buffer(D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON) {
        auto& vk = device_.context->vk;
        // A buffer has at least one byte, and its size must survive alignment to 64 KiB without wrapping.
        if (!desc_.Width || desc_.Width > ~UINT64(0) - D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) return E_INVALIDARG;
        const bool host = cpu_visible();
        palmetto::ResourceBinding binding{};  // a buffer: no image
        binding.size = desc_.Width;
        if (!host && device_.arena.mapped) {
            if (!device_.arena_allocate(desc_.Width, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, arena_offset_,
                                        DeviceState::arena_buffer)) {
                log_once("GPU address arena full (raise PALMETTO_ARENA_MB); buffer size in KiB:",
                         int(desc_.Width / 1024));
                return E_OUTOFMEMORY;
            }
            in_arena_ = true;
            std::memset(device_.arena.mapped + arena_offset_, 0, std::size_t(desc_.Width));
            va_ = arena_offset_;
            binding.va = va_;
            binding.buffer = device_.arena.buffer;
            binding.buffer_offset = arena_offset_;
            // No host pointer: this is DEFAULT-heap arena memory the GPU may write, so its mapped bytes can be
            // stale. host_at() must not treat it as a CPU-written instance pointer list.
            // A default-heap AS resource is one of these ranges: a GENERIC acceleration structure covers it where
            // it sits. It is named by its real device address (heap base + offset), as instance descriptors and
            // BuildRaytracingAccelerationStructure hand that address to the driver, not a console VA.
            if (device_.info.ray_tracing &&
                initial_state == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE &&
                vk.vkCreateAccelerationStructureKHR) {
                VkAccelerationStructureCreateInfoKHR asci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
                asci.buffer = device_.arena.buffer;
                asci.offset = arena_offset_;
                asci.size = desc_.Width;
                asci.type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR;
                if (vk.vkCreateAccelerationStructureKHR(device_.info.device, &asci, nullptr, &accel_) == VK_SUCCESS) {
                    gpu_va_ = device_.arena.address + va_;
                    device_.context->register_acceleration_structure(gpu_va_, accel_);
                }
            }
            device_.context->add_resource(id(), binding);
            return S_OK;
        }
        const bool addressable = device_.arena.buffer != VK_NULL_HANDLE;
        const bool need_bda = device_.info.buffer_device_address || device_.info.ray_tracing;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = desc_.Width;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if (device_.info.conditional_rendering) info.usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
        if (addressable || need_bda) info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (device_.info.ray_tracing) {
            info.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }
        device_.share(info);
        if (vk.vkCreateBuffer(device_.info.device, &info, nullptr, &buffer_) != VK_SUCCESS) return E_OUTOFMEMORY;
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(device_.info.device, buffer_, &requirements);
        // Readback buffers prefer cached memory, which the CPU reads quickly. Upload buffers prefer VRAM the CPU
        // writes through Resizable BAR, where the GPU reads them quickly (D3D-96); without it, system memory.
        VkMemoryPropertyFlags host_preferred = 0;
        if (heap_.Type == D3D12_HEAP_TYPE_READBACK) host_preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        if (heap_.Type == D3D12_HEAP_TYPE_UPLOAD && device_.upload_in_vram)
            host_preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        memory_ = host ? device_.allocate(requirements,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          host_preferred, &memory_flags_, addressable || need_bda)
                       : device_.allocate(requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_flags_,
                                          addressable || need_bda);
        if (!memory_ || vk.vkBindBufferMemory(device_.info.device, buffer_, memory_, 0) != VK_SUCCESS)
            return E_OUTOFMEMORY;
        if (addressable) {
            VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            address.buffer = buffer_;
            va_ = vk.vkGetBufferDeviceAddress(device_.info.device, &address) - device_.arena.address;
            device_.log_va(heap_.Type == D3D12_HEAP_TYPE_READBACK ? "readback buffer" : "upload buffer", va_,
                           desc_.Width);
        }
        if (host) {
            if (vk.vkMapMemory(device_.info.device, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_) != VK_SUCCESS)
                return E_OUTOFMEMORY;
            std::memset(mapped_, 0, std::size_t(desc_.Width));  // committed resources start zeroed
        }
        if (need_bda && vk.vkGetBufferDeviceAddress) {
            VkBufferDeviceAddressInfo bda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            bda.buffer = buffer_;
            buffer_va_ = vk.vkGetBufferDeviceAddress(device_.info.device, &bda);
            gpu_va_ = buffer_va_;
        }
        if (device_.info.ray_tracing && initial_state == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE &&
            vk.vkCreateAccelerationStructureKHR && vk.vkGetAccelerationStructureDeviceAddressKHR) {
            VkAccelerationStructureCreateInfoKHR asci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            asci.buffer = buffer_;
            asci.offset = 0;
            asci.size = desc_.Width;
            asci.type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR;
            if (vk.vkCreateAccelerationStructureKHR(device_.info.device, &asci, nullptr, &accel_) == VK_SUCCESS) {
                VkAccelerationStructureDeviceAddressInfoKHR dai{
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
                dai.accelerationStructure = accel_;
                const std::uint64_t as_va = vk.vkGetAccelerationStructureDeviceAddressKHR(device_.info.device, &dai);
                if (as_va) gpu_va_ = as_va;
                device_.context->register_acceleration_structure(gpu_va_, accel_);
                if (buffer_va_ && buffer_va_ != gpu_va_)
                    device_.context->register_acceleration_structure(buffer_va_, accel_);
            }
        }
        binding.buffer = buffer_;
        binding.size = desc_.Width;
        binding.host = cpu_writable() ? static_cast<const std::uint8_t*>(mapped_) : nullptr;
        binding.host_readable = (heap_.Type == D3D12_HEAP_TYPE_READBACK ||
                                 (heap_.Type == D3D12_HEAP_TYPE_CUSTOM &&
                                  heap_.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK));
        binding.va = va_;
        device_.context->add_resource(id(), binding);
        // Committed resources start zeroed unless the heap says otherwise; the host cannot reach device-local memory,
        // so the next submission fills it (D3D-71).
        if (!host && !(heap_flags_ & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED))
            device_.context->zero_on_next_submit(id(), buffer_);
        return S_OK;
    }

    // A buffer on the caller's own pages (Xbox device slot 133, create_buffer_at_host_address): `address` is imported
    // with VK_EXT_external_memory_host and the buffer bound to it at offset 0, so the CPU's writes there are the GPU's
    // reads with no copy. Its console VA is `address` itself, the VA the title chose, so vertex, index and immediate
    // writes the title names by that VA find this buffer (VulkanContext::buffer_at). A shader that adds heap_base to
    // that VA does not reach it: the title's own V#s into these pages need the single address space of
    // docs/d3d12_vulkan.md section 3.3, which this does not decide. The pages stay the caller's: nothing is zeroed.
    HRESULT create_host_buffer(void* address) {
        auto& vk = device_.context->vk;
        const VkDeviceSize alignment = device_.info.host_pointer_alignment;
        if (!device_.get_host_pointer_properties || !alignment) return E_NOTIMPL;
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        if (!address || !desc_.Width || desc_.Width > UINT64_MAX - 2 * alignment) return E_INVALIDARG;
        // The import starts at the page that holds `address`, and the buffer is bound at the offset inside it: the
        // title places small buffers inside a page too (a 16-byte UAV at a 64-byte-aligned address on an RTX 5090,
        // found in review), which an import of `address` itself would refuse.
        const std::uintptr_t base = start & ~static_cast<std::uintptr_t>(alignment - 1);
        void* const base_address = reinterpret_cast<void*>(base);
        const VkDeviceSize offset = start - base;
        const VkDeviceSize bytes = (offset + desc_.Width + alignment - 1) & ~(alignment - 1);
        // An imported buffer's VA is its host address, and it joins the same BufferVaIndex as default-heap buffers,
        // whose VAs are offsets in [0, arena size). A host range that reaches into that window (low host addresses,
        // as under Wine) would shadow arena buffers in the index's nearest-start lookup, so it takes the fallback
        // (found in review).
        if (start < device_.arena.size) {
            log_once("a title-placed buffer's host address overlaps the arena's VA window; not imported, size",
                     int(desc_.Width > INT_MAX ? INT_MAX : desc_.Width));
            return E_NOTIMPL;
        }
        // The whole imported range must be committed, possibly across several allocations (the retail buffer spans
        // three mapped windows); a page not committed yet (xmem commits on first touch) or a write-combined one (on
        // an RTX 5090 such an import succeeded but GPU reads returned wrong bytes, found in review) is not imported,
        // and the ordinary-buffer fallback answers instead.
        bool importable = true;
        for (VkDeviceSize at = 0; at < bytes;) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(static_cast<const char*>(base_address) + at, &region, sizeof(region)) ||
                region.State != MEM_COMMIT || (region.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE))) {
                importable = false;
                break;
            }
            const auto region_end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
            at = region_end - base;
        }
        if (!importable) return E_NOTIMPL;
        const bool need_bda = device_.info.buffer_device_address || device_.arena.buffer != VK_NULL_HANDLE;
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
        info.size = desc_.Width;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if (device_.info.conditional_rendering) info.usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
        if (need_bda) info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        device_.share(info);
        if (vk.vkCreateBuffer(device_.info.device, &info, nullptr, &buffer_) != VK_SUCCESS) return E_OUTOFMEMORY;
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(device_.info.device, buffer_, &requirements);
        VkMemoryHostPointerPropertiesEXT host{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (device_.get_host_pointer_properties(device_.info.device,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base_address,
                                                &host) != VK_SUCCESS)
            return E_NOTIMPL;
        const std::uint32_t types = host.memoryTypeBits & requirements.memoryTypeBits;
        // An offset off the buffer's own alignment cannot be bound: the fallback answers.
        if (!types || (offset % requirements.alignment) || bytes < offset + requirements.size) return E_NOTIMPL;
        VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import.pHostPointer = base_address;
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        // Host-visible and coherent first, as the caller's pages are; then any type both the import and the
        // buffer take.
        for (const VkMemoryPropertyFlags want :
             {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
              VkMemoryPropertyFlags(0)}) {
            for (std::uint32_t i = 0; i < device_.memory.memoryTypeCount && !memory_; ++i) {
                if (!(types & (1u << i)) || (device_.memory.memoryTypes[i].propertyFlags & want) != want) continue;
                VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                              need_bda ? static_cast<const void*>(&flags) : &import};
                allocate.allocationSize = bytes;
                allocate.memoryTypeIndex = i;
                if (vk.vkAllocateMemory(device_.info.device, &allocate, nullptr, &memory_) == VK_SUCCESS)
                    memory_flags_ = device_.memory.memoryTypes[i].propertyFlags;
                else
                    memory_ = VK_NULL_HANDLE;
            }
            if (memory_) break;
        }
        if (!memory_) return E_OUTOFMEMORY;
        if (vk.vkBindBufferMemory(device_.info.device, buffer_, memory_, offset) != VK_SUCCESS) return E_OUTOFMEMORY;
        mapped_ = address;
        host_import_ = true;
        va_ = start;
        if (need_bda && vk.vkGetBufferDeviceAddress) {
            VkBufferDeviceAddressInfo bda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            bda.buffer = buffer_;
            buffer_va_ = vk.vkGetBufferDeviceAddress(device_.info.device, &bda);
            gpu_va_ = buffer_va_;
        }
        device_.log_va("title-placed buffer", va_, desc_.Width);
        palmetto::ResourceBinding binding{};
        binding.buffer = buffer_;
        binding.size = desc_.Width;
        binding.host = static_cast<const std::uint8_t*>(address);
        binding.host_readable = true;
        binding.va = va_;
        device_.context->add_resource(id(), binding);
        return S_OK;
    }

    HRESULT create_sparse_buffer() {
        constexpr std::uint64_t tile_bytes = palmetto::tile_size_in_bytes;
        if (!desc_.Width || desc_.Width > UINT64_MAX - tile_bytes ||
            desc_.SampleDesc.Count != 1 || desc_.SampleDesc.Quality != 0) return E_INVALIDARG;
        auto& vk = device_.context->vk;
        auto sparse = std::make_shared<palmetto::SparseResource>(device_.info.device, vk);
        sparse->tile_count = (desc_.Width + tile_bytes - 1) / tile_bytes;
        const auto info = device_.sparse_buffer_info(sparse->tile_count * tile_bytes);
        if (vk.vkCreateBuffer(device_.info.device, &info, nullptr, &sparse->buffer) != VK_SUCCESS)
            return E_OUTOFMEMORY;
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(device_.info.device, sparse->buffer, &requirements);
        sparse->memory_types = requirements.memoryTypeBits;
        if (tile_bytes % requirements.alignment ||
            !(sparse->memory_types & device_.sparse_buffer_memory_types)) return E_NOTIMPL;
        VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        address.buffer = sparse->buffer;
        va_ = vk.vkGetBufferDeviceAddress(device_.info.device, &address) - device_.arena.address;
        palmetto::ResourceBinding binding{};
        binding.buffer = sparse->buffer;
        binding.size = desc_.Width;
        binding.va = va_;
        binding.sparse_buffer = true;
        device_.context->add_sparse_resource(id(), binding, std::move(sparse));
        return S_OK;
    }

    HRESULT create_sparse_image() {
        auto& vk = device_.context->vk;
        const auto format = device_.texture_format(desc_);
        TextureCreateInfo create(desc_, format, false);
        create.info.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                             VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
        HRESULT support = device_.check_image_support(create.info);
        if (FAILED(support)) return support == E_INVALIDARG ? E_NOTIMPL : support;
        device_.share(create.info);
        auto sparse = std::make_shared<palmetto::SparseResource>(device_.info.device, vk);
        const auto result = vk.vkCreateImage(device_.info.device, &create.info, nullptr, &sparse->image);
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) return E_NOTIMPL;
        if (result != VK_SUCCESS) return E_OUTOFMEMORY;

        VkMemoryRequirements requirements{};
        vk.vkGetImageMemoryRequirements(device_.info.device, sparse->image, &requirements);
        if (requirements.alignment != palmetto::tile_size_in_bytes ||
            palmetto::memory_type_order(device_.memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                       0, palmetto::tile_size_in_bytes).empty())
            return E_NOTIMPL;
        std::uint32_t count = 0;
        vk.vkGetImageSparseMemoryRequirements(device_.info.device, sparse->image, &count, nullptr);
        if (count != 1) return E_NOTIMPL;  // exactly one color aspect, with no separately bound metadata
        VkSparseImageMemoryRequirements image_requirements{};
        vk.vkGetImageSparseMemoryRequirements(device_.info.device, sparse->image, &count, &image_requirements);
        constexpr auto shape = palmetto::element_tile_shape(false, 4);
        const auto& properties = image_requirements.formatProperties;
        const auto granularity = properties.imageGranularity;
        if (properties.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || granularity.width != shape.width ||
            granularity.height != shape.height || granularity.depth != 1 ||
            !image_requirements.imageMipTailFirstLod || image_requirements.imageMipTailSize)
            return E_NOTIMPL;

        sparse->image_extent = {create.info.extent.width, create.info.extent.height};
        sparse->tile_count = (1 + (sparse->image_extent.width - 1) / shape.width) *
                             std::uint64_t(1 + (sparse->image_extent.height - 1) / shape.height);
        sparse->memory_types = requirements.memoryTypeBits;
        image_ = sparse->image;
        sparse_image_ = true;
        format_ = format.format;
        aspects_ = format.aspects;
        image_address_ = device_.next_image_address.fetch_add(256);
        binding_.image = image_;
        binding_.format = format_;
        binding_.aspects = aspects_;
        binding_.width = sparse->image_extent.width;
        binding_.height = sparse->image_extent.height;
        // An unbound tile reads as zero. Keep the layout stable once the image can share memory with an alias.
        binding_.not_zeroed = true;
        binding_.simultaneous_access = true;
        binding_.sampled_in_general = true;
        device_.context->add_sparse_resource(id(), binding_, std::move(sparse));
        return S_OK;
    }

    // A placed buffer is a range of the heap's arena memory: it has no VkBuffer of its own and its console VA is
    // where it sits in the arena. The arena is host-visible, so an upload or readback place maps that same range.
    HRESULT create_placed_buffer(std::shared_ptr<const palmetto::TileHeapAllocation> heap_range) {
        if (!device_.arena.mapped) return E_OUTOFMEMORY;
        heap_range_ = std::move(heap_range);
        if (cpu_visible()) mapped_ = device_.arena.mapped + va_;
        std::memset(device_.arena.mapped + va_, 0, std::size_t(desc_.Width));  // a fresh place reads as zeroed
        palmetto::ResourceBinding binding{};
        binding.size = desc_.Width;
        binding.buffer = device_.arena.buffer;
        binding.buffer_offset = va_;
        binding.va = va_;
        binding.host = cpu_writable() ? device_.arena.mapped + va_ : nullptr;
        binding.host_readable = (heap_.Type == D3D12_HEAP_TYPE_READBACK ||
                                 (heap_.Type == D3D12_HEAP_TYPE_CUSTOM &&
                                  heap_.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK));
        device_.context->add_resource(id(), binding);
        return S_OK;
    }

    // ---- pooled and deferred image memory (docs/streamed_image_backing.md) --------------------------------------
    //
    // The set that shares a block is the one upstream measured as safe: an optimal-tiled image that is neither an
    // attachment nor CPU-visible, no larger than an eighth of a block, and one Vulkan does not require a dedicated
    // allocation for. Everything else keeps its own VkDeviceMemory, as before.
    bool poolable(VkImage image, const VkMemoryRequirements& requirements) const {
        if (!device_.image_memory_pool || !device_.pool_block_bytes) return false;
        if (cpu_visible() || host_transfer_) return false;
        if (desc_.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            return false;
        if (requirements.size > device_.pool_block_bytes / 8) return false;
        if (requirements.alignment > device_.pool_block_bytes) return false;
        return !requires_dedicated_allocation(image);
    }

    // True also when the driver cannot be asked: then the image keeps its own allocation, which is always valid.
    bool requires_dedicated_allocation(VkImage image) const {
        if (!device_.image_requirements2) return true;
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkImageMemoryRequirementsInfo2 query{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        query.image = image;
        VkMemoryRequirements2 result{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        result.pNext = &dedicated;
        device_.image_requirements2(device_.info.device, &query, &result);
        return dedicated.requiresDedicatedAllocation == VK_TRUE;
    }

    // Gives the image its backing: a range of a pooled block where poolable(), else its own VkDeviceMemory. Called at
    // creation, or later for a deferred image. False when neither the pool nor the driver could back it (the caller
    // then fails the creation or refuses the submission rather than use an image without memory).
    bool bind_image_backing(VkImage image, const VkMemoryRequirements& requirements) {
        auto& vk = device_.context->vk;
        if (poolable(image, requirements)) {
            // One type per candidate, best first; a type whose block does not exist yet is only asked for one when the
            // driver has room (the pool reports a null block otherwise and the next type gets its turn).
            for (const std::uint32_t type : palmetto::memory_type_order(
                     device_.memory, requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     device_.pool_block_bytes)) {
                auto range = device_.image_memory_pool->allocate(type, requirements.size, requirements.alignment,
                                                                 device_.pool_block_bytes);
                if (!range) continue;
                if (vk.vkBindImageMemory(device_.info.device, image, range->memory(), range->offset()) != VK_SUCCESS)
                    continue;  // dropped: the range goes back to its block
                memory_flags_ = device_.memory.memoryTypes[type].propertyFlags;
                if (palmetto::spilled_to_host(0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory_flags_))
                    device_.note_spill_once(requirements.size, type);
                pooled_range_ = std::shared_ptr<palmetto::ImageMemoryPool<VkDeviceMemory>::Range>(std::move(range));
                device_.pool_ranges_live.fetch_add(1, std::memory_order_relaxed);
                device_.record_image_allocation(desc_, requirements.size, memory_flags_, cpu_visible(), true);
                return true;
            }
        }
        // Its own allocation, the default path: device-local first, another type when device-local is full (D3D-299's
        // one-time spill line), exactly as every image before the pool.
        memory_ = device_.allocate(requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_flags_);
        if (!memory_) return false;
        if (vk.vkBindImageMemory(device_.info.device, image, memory_, 0) != VK_SUCCESS) return false;
        device_.record_image_allocation(desc_, requirements.size, memory_flags_, cpu_visible(), false);
        return true;
    }

    // Gives a deferred image its memory now: its first host write, or the queue hook before the first submission that
    // could use it. False when the driver cannot back it; the caller refuses the submission then.
    bool ensure_backing() {
        if (backing_bound_) return true;
        std::lock_guard lock(device_.deferred_images_mutex);
        return bind_backing_locked();
    }

    // The same with device_.deferred_images_mutex already held (the queue hook walks the list under it).
    bool bind_backing_locked() {
        if (backing_bound_) return true;
        if (!bind_image_backing(image_, deferred_requirements_)) {
            static std::once_flag logged;
            std::call_once(logged, [] {
                std::fprintf(stderr,
                             "[rt] d3d12: a deferred image could not be backed, so the next submission is refused "
                             "instead of using an image with no memory\n");
            });
            return false;
        }
        backing_bound_ = true;
        deferred_ = false;
        std::erase(device_.deferred_images, this);
        device_.deferred_waiting.fetch_sub(1, std::memory_order_relaxed);
        device_.deferred_bound_count.fetch_add(1, std::memory_order_relaxed);
        device_.deferred_bound_bytes.fetch_add(deferred_requirements_.size, std::memory_order_relaxed);
        return true;
    }

    HRESULT create_texture(const Format& format, const std::vector<VkFormat>& castable = {},
                           bool defer_image_memory = false) {
        auto& vk = device_.context->vk;
        const bool volume = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const UINT depth = volume ? desc_.DepthOrArraySize : 1;
        const bool one_d = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        if (desc_.MipLevels == 0) desc_.MipLevels = full_mips(desc_.Width, desc_.Height, depth);
        TextureCreateInfo create(desc_, format, device_.color_attachment_usable(format.format));
        VkImageCreateInfo& info = create.info;
        if (!castable.empty()) {
            // D3D12 always allows a view in the resource's own format, whether or not the castable list names
            // it. Vulkan requires every view format to be in the image's format list, so add it when missing.
            view_formats_ = castable;
            if (std::find(view_formats_.begin(), view_formats_.end(), format.format) == view_formats_.end())
                view_formats_.push_back(format.format);
            if (format.uint_backed) {
                // An unsigned-backed UAV texture needs both its formats whatever the list names (TextureCreateInfo).
                for (VkFormat pair : {format.format, format.uint_backed})
                    if (std::find(view_formats_.begin(), view_formats_.end(), pair) == view_formats_.end())
                        view_formats_.push_back(pair);
            }
            info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            create.format_list.viewFormatCount = std::uint32_t(view_formats_.size());
            create.format_list.pViewFormats = view_formats_.data();
            create.format_list.pNext = info.pNext;
            info.pNext = &create.format_list;
        }
        const std::uint32_t samples = info.samples;
        // Textures the CPU reads and writes (WriteToSubresource/ReadFromSubresource) copy on the host when they can.
        // (A D24 texture stored as D32 goes through the staged copy: the host copy would not convert. Nor would it
        // repack a 96-bit texture's 12-byte texels.)
        host_transfer_ = cpu_visible() && samples == 1 && !format.d24_as_float && !format.rgb96 &&
                         device_.host_transfer_format(format.format);
        rgb96_ = format.rgb96;
        if (host_transfer_) info.usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
        HRESULT support = device_.check_image_support(info);
        if (support == E_INVALIDARG && host_transfer_) {
            host_transfer_ = false;  // staging copies do not require host-transfer usage
            info.usage &= ~VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
            support = device_.check_image_support(info);
        }
        if (FAILED(support)) return support;
        device_.share(info);
        const VkResult created = vk.vkCreateImage(device_.info.device, &info, nullptr, &image_);
        if (created != VK_SUCCESS) {
            if (created == VK_ERROR_OUT_OF_HOST_MEMORY || created == VK_ERROR_OUT_OF_DEVICE_MEMORY)
                return E_OUTOFMEMORY;
            if (!view_formats_.empty()) {
                log_once("castable format list refused by Vulkan; count", int(view_formats_.size()));
                return E_INVALIDARG;
            }
            return E_OUTOFMEMORY;
        }
        VkMemoryRequirements requirements{};
        vk.vkGetImageMemoryRequirements(device_.info.device, image_, &requirements);
        // Deferred backing (PALMETTO_IMAGE_DEFERRED): a title-placed texture is created with no memory and gets it at
        // its first host write or before the first submission that could use it (docs/streamed_image_backing.md).
        // Attachments and CPU-visible textures are backed at creation as before, as is one Vulkan requires a
        // dedicated allocation for.
        if (defer_image_memory && deferred_image_backing_enabled() && !cpu_visible() && !host_transfer_ &&
            !(desc_.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) &&
            !requires_dedicated_allocation(image_)) {
            deferred_ = true;
            backing_bound_ = false;
            deferred_requirements_ = requirements;
            device_.add_deferred_image(this);
        } else if (!bind_image_backing(image_, requirements)) {
            return E_OUTOFMEMORY;
        }

        palmetto::ResourceBinding binding{};
        binding.image = image_;
        binding.typeless = format.typeless && format.aspects == VK_IMAGE_ASPECT_COLOR_BIT;  // views name their format
        binding.castable_view_formats = !view_formats_.empty();  // a castable list made the image mutable
        binding.uint_backed = format.uint_backed;
        binding.rgb96 = format.rgb96;
        binding.depth24_as_float = format.d24_as_float;
        binding.depth24 = format.d24;
        binding.format = format.format;
        binding.aspects = format.aspects;
        binding.width = std::uint32_t(desc_.Width);
        binding.height = one_d ? 1u : desc_.Height;
        binding.mip_levels = desc_.MipLevels;
        binding.array_layers = info.arrayLayers;
        binding.depth = depth;
        binding.volume = volume;
        binding.line = one_d;
        binding.samples = samples;
        binding.color_attachment = (info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        binding.sampled = (info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
        binding.simultaneous_access = (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
        binding.not_zeroed = (heap_flags_ & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) != 0;
        // With the bindless set, a view's descriptor has one layout for as long as it lives (bind_image).
        binding.sampled_in_general = device_.bindless != nullptr;
        format_ = format.format;
        uint_backed_ = format.uint_backed;
        aspects_ = format.aspects;
        image_address_ = device_.next_image_address.fetch_add(256);
        binding_ = binding;
        if (host_transfer_) {
            // Moved to GENERAL and zero-filled on the host now, so the CPU can copy into it before any command
            // list has touched it (a first GPU use would otherwise zero-fill over what the CPU wrote).
            if (!host_initialize()) return E_OUTOFMEMORY;
            device_.context->add_resource(id(), binding, VK_IMAGE_LAYOUT_GENERAL);
            return S_OK;
        }
        // Never used yet: the first command that touches a part zero-fills it and moves it to the layout of the
        // state that command expects (vulkan_replay.h, initialize), so no submission is needed here.
        device_.context->add_resource(id(), binding, VK_IMAGE_LAYOUT_UNDEFINED);
        return S_OK;
    }

    // D3D12 reports the placement alignment of a description that left it 0: 64 KiB, or 4 MiB for a multisampled
    // texture. One the description asked for is kept. Measured on the PC runtime and WARP (d3d12_resource_desc_test).
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override {
        D3D12_RESOURCE_DESC desc = desc_;
        if (!desc.Alignment) {
            desc.Alignment = (desc.SampleDesc.Count > 1 && !is_tiled_layout(desc.Layout))
                                 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                 : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        }
        return desc;
    }

    // Both sparse resources and the committed fallback report the reserved layout and have no heap of their own.
    void make_reserved(D3D12_TEXTURE_LAYOUT layout) {
        reserved_ = true;
        desc_.Layout = layout;
    }
    bool reserved() const { return reserved_; }
    // A committed resource: neither placed in a heap nor reserved. It owns its memory, like a heap does.
    bool committed() const { return !placed_ && !reserved_; }

    // D3D12 answers only for committed and placed resources (E_INVALIDARG for reserved ones, measured on the PC
    // runtime).
    // Buffers only, as on D3D12; textures have no GPU VA (images are reached through descriptors). An acceleration
    // structure reports its real device address (instance descriptors and builds hand it to the driver); other
    // buffers report their console VA into the GPU address arena. A title-placed texture on the console's placed path
    // (device slot 133) answers the title's placement address instead of the PC's texture null: the console's placed
    // pages are CPU-addressable and the title writes the texels through this address (docs/3d_textures.md). One bounded
    // line names the answer and the proxy's vtable pointer, so the title's call site can be tied to this object.
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {
        if (!accel_ && !va_ && console_placed_va_) {
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr,
                             "[rt] d3d12: a title-placed texture answers GetGPUVirtualAddress with its console placement "
                             "address 0x%llx; proxy vtable %p %ux%u format %d; the committed stand-in is not backed by "
                             "the title's pages\n",
                             static_cast<unsigned long long>(console_placed_va_),
                             *reinterpret_cast<void* const*>(this), desc_.Width, desc_.Height, int(desc_.Format));
            return console_placed_va_;
        }
        return accel_ ? gpu_va_ : va_;
    }

    //
    // The node masks read back as the one node of a single adapter (0 becomes 1), and the heap flags as the runtime
    // resolves them: the caller's, without the two creation-only bits (CREATE_NOT_RESIDENT, CREATE_NOT_ZEROED), plus
    // the DENY bits of the resource classes this resource is not (buffers 0xC0, textures without render-target or
    // depth use 0x44, the others 0x84). Measured on the PC runtime and WARP (d3d12_node_mask_test).
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) override {
        if (reserved_) return E_INVALIDARG;
        if (properties) {
            *properties = heap_;
            if (!properties->CreationNodeMask) properties->CreationNodeMask = 1;
            if (!properties->VisibleNodeMask) properties->VisibleNodeMask = 1;
        }
        if (flags) *flags = resolved_heap_flags();
        return S_OK;
    }

    D3D12_HEAP_FLAGS resolved_heap_flags() const {
        const UINT deny_buffers = D3D12_HEAP_FLAG_DENY_BUFFERS, deny_rt_ds = D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES,
                   deny_non_rt_ds = D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
        UINT deny = deny_buffers | deny_rt_ds;  // a texture without render-target or depth use
        if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
            deny = deny_rt_ds | deny_non_rt_ds;
        else if (desc_.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            deny = deny_buffers | deny_non_rt_ds;
        const UINT creation_only = D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
        // A placed resource reports its heap's flags as they are (an ALLOW_ALL heap reads back 0): the DENY bits
        // are the runtime's choice only for the implicit heap of a committed resource (d3d12_heap_properties_test).
        if (placed_) return D3D12_HEAP_FLAGS(UINT(heap_flags_) & ~creation_only);
        return D3D12_HEAP_FLAGS((UINT(heap_flags_) & ~creation_only) | deny);
    }

    // Buffers on upload and readback heaps are mapped persistently; the ranges are hints, as on D3D12, except that
    // the host runtime refuses a non-empty read range that ends past the buffer. A texture in a CPU-visible heap has
    // no pointer to give (its layout is the driver's), so D3D12 only lets it be mapped with a null pointer, before
    // WriteToSubresource/ReadFromSubresource.
    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE* read, void** data) override {
        if (image_) {
            VkImageSubresourceRange range{};
            return !data && cpu_visible() && subresource != palmetto::all_subresources &&
                           palmetto::image_range(binding_, subresource, range)
                       ? S_OK
                       : E_INVALIDARG;
        }
        if (!mapped_ || subresource != 0) return E_INVALIDARG;
        if (read && read->Begin < read->End && read->End > desc_.Width) return E_INVALIDARG;
        if (data) *data = mapped_;
        return S_OK;
    }
    void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE*) override {}

    // CPU copies into and out of a texture subresource. D3D12 completes them before returning and leaves it to
    // the application to keep the GPU off the subresource meanwhile, so a blocking copy is the whole contract.
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT subresource, const D3D12_BOX* box, const void* data,
                                                 UINT row_pitch, UINT depth_pitch) override {
        return cpu_copy(true, subresource, box, const_cast<void*>(data), row_pitch, depth_pitch);
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void* data, UINT row_pitch, UINT depth_pitch, UINT subresource,
                                                  const D3D12_BOX* box) override {
        return cpu_copy(false, subresource, box, data, row_pitch, depth_pitch);
    }

private:
    // One subresource region in copy units: rows of texels, or of 4x4 blocks for block-compressed formats.
    struct Region {
        VkImageSubresourceLayers layers;
        D3D12_BOX box;
        palmetto::BlockFormat block;
        std::uint32_t rows;
        std::uint64_t row_bytes;
        std::uint32_t slices;
    };

    // The region a copy names, or false when D3D12 would reject it: an unknown subresource, a box whose right,
    // bottom or back edge lies past the mip's, or one that cuts a compressed block anywhere but at the mip edge.
    // An empty or inverted box that passes those checks is no copy at all: `rows` and `slices` are 0, and the host
    // runtime answers S_OK without touching a byte (d3d12_subresource_copy_test, RTX 3060 / 610.88).
    bool region(UINT subresource, const D3D12_BOX* box, Region& out) const {
        VkImageSubresourceRange range{};
        if (subresource == palmetto::all_subresources || !palmetto::image_range(binding_, subresource, range)) return false;
        if (binding_.samples > 1) return false;  // D3D12 has no CPU copies of multisampled textures; Vulkan neither
        const std::uint32_t mip = range.baseMipLevel;
        const std::uint32_t width = std::max(1u, binding_.width >> mip), height = std::max(1u, binding_.height >> mip);
        const std::uint32_t depth = binding_.volume ? std::max(1u, binding_.depth >> mip) : 1;
        out.box = box ? *box : D3D12_BOX{0, 0, 0, width, height, depth};
        const D3D12_BOX& b = out.box;
        if (b.right > width || b.bottom > height || b.back > depth) return false;
        out.block = palmetto::copy_block_format(binding_, range.aspectMask);
        if (!out.block.bytes) return false;
        const std::uint32_t k = out.block.block;
        if (b.left % k || b.top % k || (b.right % k && b.right != width) || (b.bottom % k && b.bottom != height))
            return false;
        out.layers = {range.aspectMask, mip, range.baseArrayLayer, 1};
        if (b.left >= b.right || b.top >= b.bottom || b.front >= b.back) {
            out.rows = 0;
            out.row_bytes = 0;
            out.slices = 0;
            return true;
        }
        out.rows = (b.bottom - b.top + k - 1) / k;
        out.row_bytes = std::uint64_t((b.right - b.left + k - 1) / k) * out.block.bytes;
        out.slices = b.back - b.front;
        return true;
    }

    HRESULT cpu_copy(bool write, UINT subresource, const D3D12_BOX* box, void* data, UINT row_pitch,
                     UINT depth_pitch, bool from_import = false) {
        if (!image_ || !data) return E_INVALIDARG;
        Region r{};
        if (!region(subresource, box, r)) return E_INVALIDARG;
        if (!r.rows || !r.slices) return S_OK;  // an empty box: nothing to copy
        // The pitches are not checked, as on the host runtime: rows or slices that overlap (a pitch smaller than a
        // row, or 0) are copied in order, row by row and slice by slice, so on a read the last one wins.
        if (!cpu_visible() && !from_import)
            log_once(write ? "WriteToSubresource on a texture outside a CPU-visible heap; heap type"
                           : "ReadFromSubresource on a texture outside a CPU-visible heap; heap type",
                     heap_.Type);
        auto* bytes = static_cast<std::byte*>(data);
        // Between the application's pitches and `pitch`, the one a copy reads or writes.
        const auto pack = [&](std::byte* to, std::uint64_t pitch) {
            for (std::uint32_t z = 0; z < r.slices; ++z)
                for (std::uint32_t y = 0; y < r.rows; ++y)
                    std::memcpy(to + (std::uint64_t(z) * r.rows + y) * pitch,
                                bytes + std::uint64_t(z) * depth_pitch + std::uint64_t(y) * row_pitch, r.row_bytes);
        };
        const auto unpack = [&](const std::byte* from, std::uint64_t pitch) {
            for (std::uint32_t z = 0; z < r.slices; ++z)
                for (std::uint32_t y = 0; y < r.rows; ++y)
                    std::memcpy(bytes + std::uint64_t(z) * depth_pitch + std::uint64_t(y) * row_pitch,
                                from + (std::uint64_t(z) * r.rows + y) * pitch, r.row_bytes);
        };
        // On the host only while the subresource is in GENERAL: it is from creation, and stays there until a
        // barrier moves it (D3D12's COMMON is GENERAL). Any other layout goes through the staging copy, which
        // moves it as a CopyTextureRegion would.
        if (host_transfer_ && device_.context->layout(id(), subresource) == VK_IMAGE_LAYOUT_GENERAL) {
            std::vector<std::byte> packed(r.row_bytes * r.rows * r.slices);
            if (write) pack(packed.data(), r.row_bytes);
            if (!host_copy(write, r, packed.data())) return E_FAIL;
            if (!write) unpack(packed.data(), r.row_bytes);
            return S_OK;
        }
        return staged_copy(write, subresource, r, pack, unpack);
    }

    // Tightly packed `memory` to or from the region, with VK_EXT_host_image_copy.
    bool host_copy(bool write, const Region& r, void* memory) {
        const VkOffset3D offset{std::int32_t(r.box.left), std::int32_t(r.box.top), std::int32_t(r.box.front)};
        const VkExtent3D extent{r.box.right - r.box.left, r.box.bottom - r.box.top, r.slices};
        const VkDevice dev = device_.info.device;
        if (write) {
            VkMemoryToImageCopyEXT copy{VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT};
            copy.pHostPointer = memory;
            copy.imageSubresource = r.layers;
            copy.imageOffset = offset;
            copy.imageExtent = extent;
            VkCopyMemoryToImageInfoEXT info{VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT};
            info.dstImage = image_;
            info.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
            info.regionCount = 1;
            info.pRegions = &copy;
            return device_.copy_memory_to_image(dev, &info) == VK_SUCCESS;
        }
        VkImageToMemoryCopyEXT copy{VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY_EXT};
        copy.pHostPointer = memory;
        copy.imageSubresource = r.layers;
        copy.imageOffset = offset;
        copy.imageExtent = extent;
        VkCopyImageToMemoryInfoEXT info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO_EXT};
        info.srcImage = image_;
        info.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        info.regionCount = 1;
        info.pRegions = &copy;
        return device_.copy_image_to_memory(dev, &info) == VK_SUCCESS;
    }

    // Every subresource of a new host-transfer image to GENERAL, zero-filled unless the heap said not to.
    bool host_initialize() {
        const VkDevice dev = device_.info.device;
        VkHostImageLayoutTransitionInfoEXT transition{VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT};
        transition.image = image_;
        transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        transition.subresourceRange = {binding_.aspects, 0, binding_.mip_levels, 0,
                                       binding_.volume ? VK_REMAINING_ARRAY_LAYERS : binding_.array_layers};
        if (device_.transition_image_layout(dev, 1, &transition) != VK_SUCCESS) return false;
        if (binding_.not_zeroed) return true;
        const std::uint32_t planes = palmetto::two_planes(binding_) ? 2 : 1;
        const std::uint32_t per_plane = binding_.mip_levels * binding_.array_layers;
        std::vector<std::byte> zero;
        for (UINT subresource = 0; subresource < planes * per_plane; ++subresource) {
            Region r{};
            if (!region(subresource, nullptr, r)) return false;
            zero.assign(r.row_bytes * r.rows * r.slices, std::byte{0});
            if (!host_copy(true, r, zero.data())) return false;
        }
        return true;
    }

    // A host-visible buffer at D3D12's footprint pitch, copied to or from the subresource by a CopyTextureRegion
    // replayed on the device's queue, then waited for. The replay handles the layout (and a first use's zero fill)
    // exactly as it does for the application's own copies.
    template<class Pack, class Unpack>
    HRESULT staged_copy(bool write, UINT subresource, const Region& r, const Pack& pack, const Unpack& unpack) {
        auto& vk = device_.context->vk;
        const VkDevice dev = device_.info.device;
        const std::uint64_t pitch = (r.row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) /
                                    D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        palmetto::ResourceBinding staging{};
        staging.size = pitch * r.rows * r.slices;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = staging.size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;  // storage: D24 depth conversion
        device_.share(info);
        if (vk.vkCreateBuffer(dev, &info, nullptr, &staging.buffer) != VK_SUCCESS) return E_OUTOFMEMORY;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        const auto release = [&](HRESULT hr) {
            if (memory) vk.vkFreeMemory(dev, memory, nullptr);  // unmaps it
            vk.vkDestroyBuffer(dev, staging.buffer, nullptr);
            return hr;
        };
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(dev, staging.buffer, &requirements);
        memory = device_.allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  write ? 0 : VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (!memory || vk.vkBindBufferMemory(dev, staging.buffer, memory, 0) != VK_SUCCESS ||
            vk.vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            return release(E_OUTOFMEMORY);
        if (write) pack(static_cast<std::byte*>(mapped), pitch);

        // The staging buffer is a resource only for the length of this call; its ID is its binding's address,
        // which no COM resource can share.
        const auto staging_id = reinterpret_cast<palmetto::ResourceId>(&staging);
        const std::uint32_t k = r.block.block;
        const palmetto::CopyLocation texture{id(), false, subresource, 0, 0, 0, 0, 0};
        const palmetto::CopyLocation placed{staging_id, true, 0, 0, std::uint32_t(r.row_bytes / r.block.bytes) * k,
                                           r.rows * k, r.slices, std::uint32_t(pitch)};
        palmetto::CopyTexture copy{};
        if (write) {
            copy = {texture, r.box.left, r.box.top, r.box.front, placed, false, {}};
        } else {
            copy = {placed, 0, 0, 0, texture, true,
                    {r.box.left, r.box.top, r.box.front, r.box.right, r.box.bottom, r.box.back}};
        }
        palmetto::CommandList list;
        if (!list.copy_texture(copy) || !list.close()) return release(E_INVALIDARG);
        device_.context->add_resource(staging_id, staging);
        palmetto::ReplayResult result = palmetto::ReplayResult::unsupported;
        {
            std::lock_guard lock(device_.staging_mutex);
            if (!device_.staging_queue) {
                device_.staging_queue = std::make_unique<palmetto::VulkanQueue>(*device_.context, device_.info.queue,
                                                                               device_.info.queue_family);
            }
            if (device_.staging_queue->ok()) {
                const palmetto::CommandList* lists[] = {&list};
                result = device_.staging_queue->execute(lists);
                if (result == palmetto::ReplayResult::success && !device_.staging_queue->idle())
                    result = palmetto::ReplayResult::unsupported;
            }
        }
        device_.context->remove_resource(staging_id);
        if (result != palmetto::ReplayResult::success) {
            log_once("WriteToSubresource/ReadFromSubresource staging copy failed; replay result", int(result));
            return release(E_FAIL);
        }
        if (!write) unpack(static_cast<const std::byte*>(mapped), pitch);
        return release(S_OK);
    }

    bool cpu_visible() const {
        if (heap_.Type == D3D12_HEAP_TYPE_UPLOAD || heap_.Type == D3D12_HEAP_TYPE_READBACK) return true;
        return heap_.Type == D3D12_HEAP_TYPE_CUSTOM && heap_.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
               heap_.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    }

    // Only these heaps hold bytes the CPU writes. READBACK is CPU-visible but GPU-written, and DEFAULT arena memory
    // is also GPU-written; both can hold stale bytes at replay time, so host_at() must not read them as inputs.
    bool cpu_writable() const {
        if (heap_.Type == D3D12_HEAP_TYPE_UPLOAD) return true;
        return heap_.Type == D3D12_HEAP_TYPE_CUSTOM &&
               (heap_.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
                heap_.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE);
    }

    DeviceState& device_;
    D3D12_RESOURCE_DESC desc_;
    D3D12_HEAP_PROPERTIES heap_;
    D3D12_HEAP_FLAGS heap_flags_;
    VkImage image_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkMemoryPropertyFlags memory_flags_ = 0;
    VkAccelerationStructureKHR accel_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    std::vector<VkFormat> view_formats_;  // creation-time castable list; empty means no restriction
    std::vector<DXGI_FORMAT> castable_formats_;  // the same list as the application gave it
    VkFormat uint_backed_ = VK_FORMAT_UNDEFINED;
    std::uint64_t image_address_ = 0;  // what a T# names the texture by
    bool rgb96_ = false;
    VkImageAspectFlags aspects_ = 0;
    std::uint64_t buffer_va_ = 0;
    std::uint64_t gpu_va_ = 0;
    bool host_import_ = false;  // memory_ is the caller's pages (create_host_buffer): freed at once, not deferred
    // Pooled image memory (docs/streamed_image_backing.md): memory_ is null and this range names the block, offset and
    // size the image is bound into. Released through the deferred queue, so another image cannot reuse the bytes while
    // a submission that read them is still running.
    std::shared_ptr<palmetto::ImageMemoryPool<VkDeviceMemory>::Range> pooled_range_;
    // Deferred backing: an image created with no memory yet, bound by bind_backing_locked() at its first host write or
    // by the queue hook before the first submission. Guarded by device_.deferred_images_mutex.
    bool deferred_ = false;
    bool backing_bound_ = true;
    VkMemoryRequirements deferred_requirements_{};
    bool reserved_ = false;
    bool sparse_image_ = false;
    bool placed_ = false;
    palmetto::ResourceBinding binding_{};  // a texture's, as registered with the context
    bool host_transfer_ = false;          // created with VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT
    D3D12_GPU_VIRTUAL_ADDRESS va_ = 0;
    VkDeviceSize arena_offset_ = 0;
    bool in_arena_ = false;
    // The title VA this resource stands in for (device slot 133), when its arena range was registered as an alias:
    // 0 for every other resource (docs/page_address_space.md).
    std::uint64_t console_alias_ = 0;
    // A placed buffer's heap range, held as a D3D12 placed resource holds its heap: the app may release the heap
    // first, and the range must not go to another allocation while this buffer lives.
    std::shared_ptr<const palmetto::TileHeapAllocation> heap_range_;
    // The title's pages of a title-placed texture (device slot 133, PALMETTO_TEXTURE_IMPORT): imported once into the
    // committed image before the first submission that can sample it (docs/texture_import.md). `placed_pages_` is the
    // console placement VA the title asked for; `placed_source_pages_` is where the bytes are read from on this host -
    // 0 when the console VA itself is the source (the path before this port), or the same offset in the CPU
    // reservation paired with the GPU-view window the texture was placed in (docs/peer_port_movie.md section 11).
    std::uint64_t placed_pages_ = 0;
    std::uint64_t placed_source_pages_ = 0;
    // The console VA a title-placed texture answers from GetGPUVirtualAddress: the placement address the title asked
    // for (docs/3d_textures.md). 0 for every other resource.
    std::uint64_t console_placed_va_ = 0;
    unsigned placed_attempts_ = 0;  // bounded retries while the title's fill has not run yet
    bool placed_imported_ = false;
    bool placed_skipped_ = false;
    bool placed_wait_logged_ = false;  // docs/texture_source.md: the all-zero-head wait is logged once per texture
    // The per-submission refresh (docs/peer_port_movie.md section 8, port/src/plane_refresh.h): the bytes the import put
    // in the image, kept as the shadow the next submission compares the title's pages against; the subresource shape the
    // narrow re-upload needs; and whether the guard-page tracker was armed on the pages as the pre-filter.
    std::vector<std::uint8_t> placed_shadow_;
    std::uint32_t placed_row_bytes_ = 0;
    std::uint32_t placed_rows_ = 0;
    bool placed_watch_armed_ = false;
    bool placed_refresh_tracking_ = false;
};

// Drains the title-placed textures whose pages are ready to import. Called by the queue before it records a
// submission (VulkanQueue::execute_now -> VulkanContext::import_placed_textures), so the image the replay samples
// already holds the title's bytes. Each entry holds a reference from the device shim.
    // Diagnostic (docs/frame_capture.md, the colour path): the compose pass's pixel shader fetches T# descriptors itself
    // out of the published matrix block (the s8 table's slot 0 is a V# over that block) instead of from a table slot. Scan
    // the block for decodable T#s and give each one a layer image from the title-placed resource its address names, so the
    // shader's sampled lookup finds it. Opt-in (PALMETTO_TEXTURE_BIND_FETCHED); bounded, logged, and only adds set-0
    // registrations.
void DeviceState::bind_fetched_textures(std::uint64_t table_title_va) {
        if (!context || !arena.mapped || !table_title_va) return;
        std::uint64_t mirror = 0, available = 0;
        if (!context->console_va_span(table_title_va, palmetto::ConsoleVaKind::descriptors, mirror, available)) return;
        if (mirror >= arena.size || available < 32) return;
        const auto* slot0 = reinterpret_cast<const std::uint32_t*>(arena.mapped + mirror);
        const palmetto::gfx10::BufferWords matrix_words{slot0[0], slot0[1], slot0[2], slot0[3]};
        palmetto::gfx10::Buffer matrix{};
        if (!palmetto::gfx10::decode(matrix_words, matrix) || !matrix.address) return;
        const std::uint64_t declared = matrix.stride ? std::uint64_t(matrix.stride) * matrix.num_records
                                                     : std::uint64_t(matrix.num_records);
        const std::uint64_t want = std::min<std::uint64_t>(declared, 16384);
        if (!want || matrix.address >= arena.size || want > arena.size - matrix.address) return;
        const auto* bytes = arena.mapped + matrix.address;
        // The published block is handed to the shader once per frame and the table address repeats: scan a given matrix
        // block once, not once per draw.
        {
            static std::mutex scanned_mutex;
            static std::unordered_set<std::uint64_t> scanned;
            std::lock_guard scan_lock(scanned_mutex);
            if (!scanned.insert(matrix.address).second) return;
            if (scanned.size() > 4096) scanned.clear();
        }
        unsigned bound = 0;
        for (std::uint64_t at = 0; at + 32 <= want && bound < 16; at += 8) {
            std::uint32_t words_at[8] = {};
            std::memcpy(words_at, bytes + at, sizeof(words_at));
            const palmetto::gfx10::ImageWords image_words{words_at[0], words_at[1], words_at[2], words_at[3],
                                                          words_at[4], words_at[5], words_at[6], words_at[7]};
            palmetto::gfx10::Image image{};
            if (!palmetto::gfx10::is_image(image_words) || !palmetto::gfx10::decode(image_words, image)) continue;
            if (!image.address) continue;
            Resource* placed = nullptr;
            {
                std::lock_guard resources_lock(placed_resources_mutex);
                const auto found = placed_resources.find(image.address);
                if (found != placed_resources.end()) placed = found->second;
            }
            if (!placed) {
                // No created resource at the T# address: say whether the title pages there carry bytes at all, so "the data
                // was never produced" can be told from "the data exists but no layer image was built for it".
                static std::mutex probed_mutex;
                static std::unordered_set<std::uint64_t> probed;
                bool first = false;
                {
                    std::lock_guard probe_lock(probed_mutex);
                    first = probed.insert(image.address).second;
                    if (probed.size() > 512) probed.clear();
                }
                if (first) {
                    std::uint32_t got = 0;
                    std::uint8_t pages[65536] = {};
                    std::uint64_t nonzero = 0, peak = 0;
                    if (read_title_memory(image.address, sizeof(pages), pages, got) && got) {
                        for (std::uint32_t i = 0; i < got; ++i) {
                            if (pages[i]) ++nonzero;
                            peak = std::max<std::uint64_t>(peak, pages[i]);
                        }
                        std::fprintf(stderr,
                                     "[rt] d3d12: fetched T#: block 0x%llx + 0x%llx names title VA 0x%llx (format %u "
                                     "%ux%u) with no created resource; its pages are readable, %llu of %u bytes "
                                     "non-zero, peak %llu\n",
                                     static_cast<unsigned long long>(matrix.address),
                                     static_cast<unsigned long long>(at),
                                     static_cast<unsigned long long>(image.address), image.format, image.width,
                                     image.height, static_cast<unsigned long long>(nonzero), got,
                                     static_cast<unsigned long long>(peak));
                    } else {
                        std::fprintf(stderr,
                                     "[rt] d3d12: fetched T#: block 0x%llx + 0x%llx names title VA 0x%llx (format %u "
                                     "%ux%u) with no created resource; its pages are NOT readable\n",
                                     static_cast<unsigned long long>(matrix.address),
                                     static_cast<unsigned long long>(at),
                                     static_cast<unsigned long long>(image.address), image.format, image.width,
                                     image.height);
                    }
                }
                continue;
            }
            std::lock_guard lock(descriptor_mutex);
            if (image_entries.find(palmetto::make_tsharp_key(image_words.data())) != image_entries.end()) continue;
            if (!placed->image()) continue;
            if (!bind_image(image_words, image, placed->image(), placed->format(), placed->aspects(), false, false,
                            placed->uint_backed(), placed->id(), placed->placed_read_pages(),
                            placed->placed_subresource_bytes()))
                continue;
            ++bound;
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 16)
                std::fprintf(stderr,
                             "[rt] d3d12: fetched T#: block 0x%llx + 0x%llx names title VA 0x%llx (format %u %ux%u); bound "
                             "the placed resource 0x%llx as a layer image\n",
                             static_cast<unsigned long long>(matrix.address), static_cast<unsigned long long>(at),
                             static_cast<unsigned long long>(image.address), image.format, image.width, image.height,
                             static_cast<unsigned long long>(placed->id()));
        }
    }


void DeviceState::import_placed_textures() {
    std::vector<Resource*> pending;
    {
        std::lock_guard lock(placed_texture_mutex);
        pending.swap(placed_textures_pending);
    }
    std::vector<Resource*> retry;
    for (Resource* resource : pending) {
        if (resource->import_placed_pages()) {
            retry.push_back(resource);  // keep the shim's reference for the next submission
            continue;
        }
        // Imported once: with the refresh switch on the resource is tracked and keeps this list's reference for the
        // walk (bounded by the shadow budget below); otherwise the reference goes as it did before the port.
        const auto plan = plane_refresh_plan();
        bool tracked = false;
        if (plan.enabled() && resource->placed_refresh_available()) {
            std::lock_guard lock(placed_refresh_mutex);
            constexpr std::uint64_t kShadowBudget = std::uint64_t(64) << 20;  // the shadows' bytes, this process
            constexpr std::size_t kMaxTracked = 16;
            if (placed_refresh.size() < kMaxTracked &&
                placed_refresh_shadow_bytes + resource->placed_shadow_bytes() <= kShadowBudget) {
                resource->set_placed_refresh_tracking();
                placed_refresh.push_back(resource);
                placed_refresh_shadow_bytes += resource->placed_shadow_bytes();
                if (resource->placed_watch_armed()) ++placed_refresh_counters.watched;
                tracked = true;
            }
        }
        if (!tracked) resource->Release();
    }
    if (!retry.empty()) {
        std::lock_guard lock(placed_texture_mutex);
        placed_textures_pending.insert(placed_textures_pending.end(), retry.begin(), retry.end());
    }
    // docs/peer_port_movie.md section 8: the same hook is the per-submission refresh of the textures tracked above, so
    // the pages the title rewrote are in their stand-ins before this submission is recorded. A no-op with the switch
    // off, and with nothing tracked.
    refresh_placed_textures();
}

void DeviceState::untrack_placed_refresh(Resource* resource) {
    bool found = false;
    {
        std::lock_guard lock(placed_refresh_mutex);
        const auto at = std::find(placed_refresh.begin(), placed_refresh.end(), resource);
        if (at != placed_refresh.end()) {
            placed_refresh.erase(at);
            placed_refresh_shadow_bytes -= resource->placed_shadow_bytes();
            if (resource->placed_watch_armed()) --placed_refresh_counters.watched;
            resource->stop_placed_refresh();
            found = true;
        }
    }
    // Released outside the lock: the release may destroy the resource, whose destructor takes other device locks.
    if (found) resource->Release();  // the reference the pending list took when it queued the import
}

void DeviceState::refresh_placed_textures() {
    const auto plan = plane_refresh_plan();
    if (!plan.enabled()) return;
    std::vector<Resource*> tracked;
    {
        std::lock_guard lock(placed_refresh_mutex);
        tracked = placed_refresh;
    }
    if (tracked.empty()) return;
    // The tracker's verdict covers every armed range, the descriptor-mirror gate's included: "changed" costs a compare
    // of every tracked texture, "unchanged" skips all of them, "unavailable" (nothing armed, a range refused, the trip
    // record overflowed) falls back to the hash compare the peers use for memory they cannot watch.
    rt::xmem::WriteWatchPoll verdict = rt::xmem::WriteWatchPoll::unavailable;
    if (plan.arms_tracker() && rt::xmem::page_watch_poll) verdict = rt::xmem::page_watch_poll();
    palmetto::RefreshBudget budget;
    std::vector<Resource*> untrack;
    std::uint64_t walked = 0, compared = 0, compare_bytes = 0, unchanged = 0, changed = 0, unreadable = 0, deferred = 0,
                 tracker_skips = 0, uploaded = 0, narrow = 0, full = 0;
    const std::size_t count = tracked.size();
    for (std::size_t i = 0; i < count; ++i) {
        Resource* const resource = tracked[(placed_refresh_next + i) % count];
        ++walked;
        const std::uint64_t bytes = resource->placed_shadow_bytes();
        const bool fits = bytes != 0 && budget.take(bytes);
        // The tracker is global, but arming can fail for one texture (including an alias-view overflow) while another
        // remains covered. An unarmed texture must always compare; a global "unchanged" is not evidence about it.
        const rt::xmem::WriteWatchPoll resource_verdict = resource->placed_watch_armed()
                                                              ? verdict
                                                              : rt::xmem::WriteWatchPoll::unavailable;
        switch (palmetto::texture_refresh_step(plan.mode, resource_verdict, fits)) {
        case palmetto::TextureRefreshStep::skip_tracker_unchanged:
            ++tracker_skips;
            continue;
        case palmetto::TextureRefreshStep::defer_budget:
            ++deferred;
            continue;
        case palmetto::TextureRefreshStep::compare:
            break;
        }
        ++compared;
        compare_bytes += bytes;
        const Resource::RefreshResult result = resource->refresh_placed_pages();
        switch (result.outcome) {
        case Resource::RefreshOutcome::unchanged:
            ++unchanged;
            break;
        case Resource::RefreshOutcome::changed:
            ++changed;
            ++uploaded;
            if (result.narrow) ++narrow; else ++full;
            break;
        case Resource::RefreshOutcome::unreadable:
            ++unreadable;
            break;
        case Resource::RefreshOutcome::failed:
            ++unreadable;  // the shape no longer matches the import: the texture leaves the walk
            untrack.push_back(resource);
            break;
        }
    }
    placed_refresh_next = count ? (placed_refresh_next + 1) % count : 0;
    // Our own compares read the title's pages, and in watch mode each read trips its guard: re-arm without taking a
    // change verdict (the change was already consumed by the poll above).
    if (compared && plan.arms_tracker() && rt::xmem::page_watch_rearm) rt::xmem::page_watch_rearm();
    for (Resource* const resource : untrack) untrack_placed_refresh(resource);
    {
        std::lock_guard lock(placed_refresh_mutex);
        auto& c = placed_refresh_counters;
        ++c.submissions;
        c.tracked += walked;
        c.compared += compared;
        c.compare_bytes += compare_bytes;
        c.changed += changed;
        c.unchanged += unchanged;
        c.unreadable += unreadable;
        c.deferred += deferred;
        c.tracker_skips += tracker_skips;
        c.uploaded += uploaded;
        c.narrow_uploads += narrow;
        c.full_uploads += full;
    }
    ++placed_refresh_walks;
    if (plan.trace && (placed_refresh_walks == 1 || placed_refresh_walks == 256 || placed_refresh_walks % 4096 == 0)) {
        char report[512];
        std::lock_guard lock(placed_refresh_mutex);
        palmetto::write_plane_refresh_report(placed_refresh_counters, report, sizeof report);
        std::fprintf(stderr, "[rt] d3d12: %s (walk %llu)\n", report,
                     static_cast<unsigned long long>(placed_refresh_walks));
        // docs/peer_port_movie.md section 11.4: the GPU-view pair's own counters, at the same milestone. Null hooks
        // mean this binary links no xmem (a GPU test): nothing printed.
        if (rt::xmem::gpu_view_pairs && rt::xmem::gpu_view_resolved && rt::xmem::gpu_view_refused &&
            rt::xmem::gpu_view_cpu_map_pages) {
            std::fprintf(stderr,
                         "[rt] d3d12: GPU-view pairing: %llu pair(s) recorded | %llu placed-texture VA(s) resolved "
                         "through the pair, %llu refused (a GPU view with no pair) | %llu CPU-view page(s) mapped "
                         "(walk %llu)\n",
                         static_cast<unsigned long long>(rt::xmem::gpu_view_pairs()),
                         static_cast<unsigned long long>(rt::xmem::gpu_view_resolved()),
                         static_cast<unsigned long long>(rt::xmem::gpu_view_refused()),
                         static_cast<unsigned long long>(rt::xmem::gpu_view_cpu_map_pages()),
                         static_cast<unsigned long long>(placed_refresh_walks));
        }
    }
}


// PALMETTO_IMAGE_MEM_POOL=1 (docs/streamed_image_backing.md): the blocks small textures share. The block size follows
// upstream's measured choice, 64 MiB capped at a sixteenth of a device-local heap, so a 4 GB card's 256 MiB BAR window
// would still get blocks. The callbacks allocate and free VkDeviceMemory; the pool only decides which ranges.
void DeviceState::start_image_memory_pool() {
    if (!image_memory_pool_enabled() || !context || !context->loaded()) return;
    auto& vk = context->vk;
    if (!vk.vkAllocateMemory || !vk.vkFreeMemory) return;
    VkDeviceSize block = VkDeviceSize(64) << 20;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; ++i) {
        const VkMemoryType& type = memory.memoryTypes[i];
        if (!(type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || type.heapIndex >= memory.memoryHeapCount)
            continue;
        block = std::min(block, memory.memoryHeaps[type.heapIndex].size / 16);
    }
    block &= ~VkDeviceSize(0xffff);
    if (block < (VkDeviceSize(1) << 20)) {
        std::fprintf(stderr,
                     "[rt] d3d12: image memory pool off: no device-local heap is big enough for a block (largest "
                     "candidate %llu KiB)\n",
                     static_cast<unsigned long long>(block >> 10));
        return;
    }
    pool_block_bytes = block;
    image_memory_pool = std::make_unique<palmetto::ImageMemoryPool<VkDeviceMemory>>(
        [this](std::uint32_t type, std::uint64_t bytes) -> VkDeviceMemory {
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = bytes;
            allocate.memoryTypeIndex = type;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (context->vk.vkAllocateMemory(info.device, &allocate, nullptr, &memory) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            record_allocation(bytes);
            pool_blocks_created.fetch_add(1, std::memory_order_relaxed);
            pool_bytes_created.fetch_add(bytes, std::memory_order_relaxed);
            return memory;
        },
        [this](VkDeviceMemory memory) {
            if (context) context->vk.vkFreeMemory(info.device, memory, nullptr);
        });
    std::fprintf(stderr,
                 "[rt] d3d12: image memory pool on: blocks of %llu MiB shared by small non-attachment textures; images "
                 "up to %llu MiB may share one\n",
                 static_cast<unsigned long long>(block >> 20), static_cast<unsigned long long>(block >> 23));
}

// The queue's hook (called from VulkanQueue::execute_now before the lists are prepared): every deferred image still
// waiting for its first write or GPU use gets its memory here. False refuses the submission instead of recording a
// command that names an image with no memory.
bool DeviceState::bind_deferred_images() {
    std::lock_guard lock(deferred_images_mutex);
    while (!deferred_images.empty()) {
        const std::size_t before = deferred_images.size();
        Resource* resource = deferred_images.front();
        if (!resource->bind_backing_locked()) return false;
        // The binding takes the image out of the list; an entry that stayed (already bound) would spin here.
        if (deferred_images.size() == before) {
            deferred_images.erase(deferred_images.begin());
            deferred_waiting.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    return true;
}

void DeviceState::add_deferred_image(Resource* resource) {
    std::lock_guard lock(deferred_images_mutex);
    deferred_images.push_back(resource);
    deferred_waiting.fetch_add(1, std::memory_order_relaxed);
}

void DeviceState::remove_deferred_image(Resource* resource) {
    std::lock_guard lock(deferred_images_mutex);
    if (std::erase(deferred_images, resource)) deferred_waiting.fetch_sub(1, std::memory_order_relaxed);
}

// DXGI -> Vulkan for vertex attributes, with each element's size for D3D12_APPEND_ALIGNED_ELEMENT.
bool vertex_format(DXGI_FORMAT format, VkFormat& out, UINT& bytes) {
    struct Row {
        DXGI_FORMAT dxgi;
        VkFormat vk;
        UINT bytes;
    };
    static constexpr Row rows[] = {
        {DXGI_FORMAT_R32G32B32A32_FLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {DXGI_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT, 16},
        {DXGI_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SINT, 16},
        {DXGI_FORMAT_R32G32B32_FLOAT, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {DXGI_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_UINT, 12},
        {DXGI_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_SINT, 12},
        {DXGI_FORMAT_R32G32_FLOAT, VK_FORMAT_R32G32_SFLOAT, 8},
        {DXGI_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT, 8},
        {DXGI_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SINT, 8},
        {DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, 4},
        {DXGI_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, 4},
        {DXGI_FORMAT_R32_SINT, VK_FORMAT_R32_SINT, 4},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8},
        {DXGI_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, 8},
        {DXGI_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, 8},
        {DXGI_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT, 8},
        {DXGI_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SINT, 8},
        {DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, 4},
        {DXGI_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UNORM, 4},
        {DXGI_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_SNORM, 4},
        {DXGI_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_UINT, 4},
        {DXGI_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_SINT, 4},
        {DXGI_FORMAT_R16_FLOAT, VK_FORMAT_R16_SFLOAT, 2},
        {DXGI_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, 2},
        {DXGI_FORMAT_R16_SNORM, VK_FORMAT_R16_SNORM, 2},
        {DXGI_FORMAT_R16_UINT, VK_FORMAT_R16_UINT, 2},
        {DXGI_FORMAT_R16_SINT, VK_FORMAT_R16_SINT, 2},
        {DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4},
        {DXGI_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, 4},
        {DXGI_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT, 4},
        {DXGI_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SINT, 4},
        {DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 4},
        {DXGI_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, 2},
        {DXGI_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_SNORM, 2},
        {DXGI_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_UINT, 2},
        {DXGI_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_SINT, 2},
        {DXGI_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1},
        {DXGI_FORMAT_R8_SNORM, VK_FORMAT_R8_SNORM, 1},
        {DXGI_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, 1},
        {DXGI_FORMAT_R8_SINT, VK_FORMAT_R8_SINT, 1},
        {DXGI_FORMAT_R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4},
        {DXGI_FORMAT_R10G10B10A2_UINT, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4},
        {DXGI_FORMAT_R11G11B10_FLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4},
    };
    for (const Row& row : rows)
        if (row.dxgi == format) {
            out = row.vk;
            bytes = row.bytes;
            return true;
        }
    return false;
}

VkBlendFactor blend_factor(D3D12_BLEND blend) {
    switch (blend) {
    case D3D12_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case D3D12_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case D3D12_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case D3D12_BLEND_INV_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3D12_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3D12_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case D3D12_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case D3D12_BLEND_DEST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case D3D12_BLEND_INV_DEST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3D12_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case D3D12_BLEND_BLEND_FACTOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case D3D12_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case D3D12_BLEND_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case D3D12_BLEND_INV_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case D3D12_BLEND_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case D3D12_BLEND_INV_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case D3D12_BLEND_ALPHA_FACTOR: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case D3D12_BLEND_INV_ALPHA_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blend_op(D3D12_BLEND_OP op) {
    switch (op) {
    case D3D12_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case D3D12_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case D3D12_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
    case D3D12_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

// D3D12 and Vulkan list the logic ops in different orders, so each is named.
VkLogicOp logic_op(D3D12_LOGIC_OP op) {
    switch (op) {
    case D3D12_LOGIC_OP_CLEAR: return VK_LOGIC_OP_CLEAR;
    case D3D12_LOGIC_OP_SET: return VK_LOGIC_OP_SET;
    case D3D12_LOGIC_OP_COPY: return VK_LOGIC_OP_COPY;
    case D3D12_LOGIC_OP_COPY_INVERTED: return VK_LOGIC_OP_COPY_INVERTED;
    case D3D12_LOGIC_OP_NOOP: return VK_LOGIC_OP_NO_OP;
    case D3D12_LOGIC_OP_INVERT: return VK_LOGIC_OP_INVERT;
    case D3D12_LOGIC_OP_AND: return VK_LOGIC_OP_AND;
    case D3D12_LOGIC_OP_NAND: return VK_LOGIC_OP_NAND;
    case D3D12_LOGIC_OP_OR: return VK_LOGIC_OP_OR;
    case D3D12_LOGIC_OP_NOR: return VK_LOGIC_OP_NOR;
    case D3D12_LOGIC_OP_XOR: return VK_LOGIC_OP_XOR;
    case D3D12_LOGIC_OP_EQUIV: return VK_LOGIC_OP_EQUIVALENT;
    case D3D12_LOGIC_OP_AND_REVERSE: return VK_LOGIC_OP_AND_REVERSE;
    case D3D12_LOGIC_OP_AND_INVERTED: return VK_LOGIC_OP_AND_INVERTED;
    case D3D12_LOGIC_OP_OR_REVERSE: return VK_LOGIC_OP_OR_REVERSE;
    case D3D12_LOGIC_OP_OR_INVERTED: return VK_LOGIC_OP_OR_INVERTED;
    default: return VK_LOGIC_OP_COPY;
    }
}

// D3D12_COMPARISON_FUNC (NEVER = 1 .. ALWAYS = 8) and D3D12_STENCIL_OP (KEEP = 1 .. DECR = 8) list their values
// in Vulkan's order, starting at 1 instead of 0.
VkCompareOp compare_op(D3D12_COMPARISON_FUNC func) {
    const int value = int(func);
    return value >= 1 && value <= 8 ? VkCompareOp(value - 1) : VK_COMPARE_OP_ALWAYS;
}
VkStencilOp stencil_op(D3D12_STENCIL_OP op) {
    const int value = int(op);
    return value >= 1 && value <= 8 ? VkStencilOp(value - 1) : VK_STENCIL_OP_KEEP;
}

VkStencilOpState stencil_face(const D3D12_DEPTH_STENCILOP_DESC& face, UINT8 read_mask, UINT8 write_mask) {
    VkStencilOpState state{};
    state.failOp = stencil_op(face.StencilFailOp);
    state.passOp = stencil_op(face.StencilPassOp);
    state.depthFailOp = stencil_op(face.StencilDepthFailOp);
    state.compareOp = compare_op(face.StencilFunc);
    state.compareMask = read_mask;
    state.writeMask = write_mask;
    return state;
}

bool equal_semantic(const std::string& a, const char* b) {
    if (!b || a.size() != std::strlen(b)) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
// A heap (section 3.3): a range of the GPU address arena, as large as the game asked for, so a placed buffer can
// name any 64 KiB-aligned offset in it. Textures placed in it still get their own VkDeviceMemory; the offset is only
// their key, which keeps optimal-tiling images out of the arena's memory type.
// Live Heap objects. A console call names a heap by an address rather than with an ID3D12Heap*, and
// Object<Heap>::from dereferences whatever it is given, so membership is checked before any tag read. Entries are
// removed when the object dies, so a recycled address is not mistaken for a live heap.
inline std::mutex& heap_registry_mutex() {
    static std::mutex m;
    return m;
}
inline std::unordered_set<const void*>& heap_registry_set() {
    static std::unordered_set<const void*> set;
    return set;
}
inline bool is_our_heap(const void* p) {
    std::lock_guard lock(heap_registry_mutex());
    return heap_registry_set().count(p) != 0;
}

class Heap final
    : public Child<Heap, ID3D12HeapStubs, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Heap> {
public:
    // {5d20c7a4-8e13-4b6d-9f27-3a1c94e0b7d8}
    static constexpr GUID kTag{0x5d20c7a4, 0x8e13, 0x4b6d, {0x9f, 0x27, 0x3a, 0x1c, 0x94, 0xe0, 0xb7, 0xd8}};

    Heap(DeviceState& device, const D3D12_HEAP_DESC& desc) : device_(device), desc_(desc) {
        { std::lock_guard lock(heap_registry_mutex()); heap_registry_set().insert(this); }
        VkDeviceSize offset = 0;
        if (device_.arena.mapped &&
            device_.arena_allocate(desc_.SizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, offset,
                                   DeviceState::arena_heap)) {
            allocation_ = std::shared_ptr<const palmetto::TileHeapAllocation>(
                new palmetto::TileHeapAllocation{device_.arena.memory, offset, desc_.SizeInBytes,
                                                device_.arena.memory_type},
                [&device](const palmetto::TileHeapAllocation* allocation) {
                    device.arena_release(allocation->offset, allocation->size, DeviceState::arena_heap);
                    delete allocation;
                });
        }
    }

    ~Heap() override {
        std::lock_guard lock(heap_registry_mutex());
        heap_registry_set().erase(this);
    }

    // An existing heap (OpenExistingHeapFromAddress): `memory` is the caller's committed range imported with
    // VK_EXT_external_memory_host, and the heap owns that VkDeviceMemory, not the range. Not in the arena.
    Heap(DeviceState& device, const D3D12_HEAP_DESC& desc, VkDeviceMemory memory, std::uint32_t memory_type,
         const void* address)
        : device_(device), desc_(desc), host_address_(address) {
        allocation_ = std::shared_ptr<const palmetto::TileHeapAllocation>(
            new palmetto::TileHeapAllocation{memory, 0, desc_.SizeInBytes, memory_type},
            [device = device_.info.device, free = device_.context->vk.vkFreeMemory]
            (const palmetto::TileHeapAllocation* allocation) {
                free(device, allocation->memory, nullptr);
                delete allocation;
            });
        // Imported heaps are heaps of this device too: create_placed looks the address up through this registry, so
        // without this insert an existing-heap placement falls into the "not a runtime heap" refusal instead of the
        // address branch that handles it.
        { std::lock_guard lock(heap_registry_mutex()); heap_registry_set().insert(this); }
    }

    // As the host reports it (measured against the PC runtime, d3d12_vulkan_device_test compare_heap_descs): the
    // alignment the heap was placed with (64 KiB for an Alignment of 0) and the one node for a node mask of 0, as the
    // layer's resources, descriptor heaps and queues already answer.
    D3D12_HEAP_DESC STDMETHODCALLTYPE GetDesc() override {
        D3D12_HEAP_DESC desc = desc_;
        desc.Alignment = alignment();
        desc.Properties.CreationNodeMask = 1;
        desc.Properties.VisibleNodeMask = 1;
        return desc;
    }

    bool reserved() const { return bool(allocation_); }
    // An existing heap: the caller's memory, not an arena range, so offset() is not a place in the arena.
    const void* host_address() const { return host_address_; }
    const std::shared_ptr<const palmetto::TileHeapAllocation>& arena_range() const { return allocation_; }
    VkDeviceSize offset() const { return allocation_->offset; }
    UINT64 bytes() const { return desc_.SizeInBytes; }
    UINT64 alignment() const { return desc_.Alignment ? desc_.Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; }
    const D3D12_HEAP_PROPERTIES& properties() const { return desc_.Properties; }
    D3D12_HEAP_FLAGS flags() const { return desc_.Flags; }
    std::shared_ptr<const palmetto::TileHeapAllocation> tiles(const palmetto::VulkanContext& context, bool image,
                                                           std::uint32_t memory_types) {
        const auto denied = image ? D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES : D3D12_HEAP_FLAG_DENY_BUFFERS;
        if (device_.context.get() != &context || desc_.Properties.Type != D3D12_HEAP_TYPE_DEFAULT ||
            (desc_.Flags & denied) || host_address_) return {};
        std::lock_guard lock(backing_mutex_);
        if (!image && device_.tiles_share_arena()) {
            if (tile_allocation_) return {};
            arena_tiles_ = true;
            return allocation_;
        }
        // Vulkan only guarantees sparse aliasing between compatible images. A texture tile heap has its own
        // backing, so a heap already used for buffer tiles or placed buffers cannot also map texture tiles.
        if (arena_tiles_ || (tile_allocation_ && image_tiles_ != image)) return {};
        if (placed_buffer_ || (image && placed_image_)) return {};
        if (!tile_allocation_) {
            std::uint32_t memory_type = 0;
            if (!image) memory_types = device_.sparse_buffer_memory_types;
            const VkMemoryRequirements requirements{desc_.SizeInBytes, palmetto::tile_size_in_bytes,
                                                    memory_types};
            const auto memory = device_.allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                                                  nullptr, !image, &memory_type);
            if (!memory) return {};
            image_tiles_ = image;
            tile_allocation_ = std::shared_ptr<const palmetto::TileHeapAllocation>(
                new palmetto::TileHeapAllocation{memory, 0, desc_.SizeInBytes, memory_type},
                [device = device_.info.device, free = device_.context->vk.vkFreeMemory]
                (const palmetto::TileHeapAllocation* allocation) {
                    free(device, allocation->memory, nullptr);
                    delete allocation;
                });
        }
        return tile_allocation_;
    }

    bool use_as_placed_resource(bool buffer) {
        std::lock_guard lock(backing_mutex_);
        // A placed resource cannot alias the separate sparse memory allocation.
        if (tile_allocation_ && (buffer || image_tiles_)) return false;
        (buffer ? placed_buffer_ : placed_image_) = true;
        return true;
    }

private:
    DeviceState& device_;
    D3D12_HEAP_DESC desc_;
    const void* host_address_ = nullptr;
    std::shared_ptr<const palmetto::TileHeapAllocation> allocation_;
    std::mutex backing_mutex_;
    std::shared_ptr<const palmetto::TileHeapAllocation> tile_allocation_;
    bool placed_buffer_ = false;
    bool placed_image_ = false;
    bool arena_tiles_ = false;
    bool image_tiles_ = false;
};

// The S#s a shader builds from immediates (literal_samplers in its index entry) are named by no heap slot, so
// nothing else gives them a sampler and a hash-table key: without this their lookup finds the fallback sampler
// (index 0) and the shader samples with the wrong filter and address modes (docs/design.md, Literal samplers).
// Registering is idempotent and also runs when the pipeline is created again.
void register_literal_samplers(DeviceState& device, const palmetto::ShaderEntry& entry) {
    for (const palmetto::gfx10::SamplerWords& words : entry.literal_samplers)
        if (!device.bind_sampler(words))
            log_once("a shader's literal S# got no sampler (no bindless set, or its sampler array is full); "
                     "literal S#s in that shader:",
                     int(entry.literal_samplers.size()));
}

// Among index entries for one program (containers with the same FSSM program; 789 groups in the 26,503-shader
// corpus), the one to build: the first that `fits` and declares no set-0 bindings (the cheaper pipeline); else the
// first that fits; else the first without bindings; else the first. Null when there is none. It lives in
// src/graphics_pso.h so the native graphics-stage branch below and the compute path choose by the same rule.
using rt::graphics_pso::pick_entry;

// Whether the input layout has an element for every ordinary (non-system-value) input of the vertex shader.
bool layout_feeds(const palmetto::ShaderEntry& vs, const D3D12_INPUT_LAYOUT_DESC& layout) {
    const UINT count = layout.pInputElementDescs ? layout.NumElements : 0;
    for (const auto& input : vs.input_signature) {
        if (input.system_value) continue;
        bool fed = false;
        for (UINT i = 0; i < count && !fed; ++i)
            fed = layout.pInputElementDescs[i].SemanticIndex == input.semantic_index &&
                  equal_semantic(input.semantic, layout.pInputElementDescs[i].SemanticName);
        if (!fed) return false;
    }
    return true;
}

// Known D3D12_PIPELINE_STATE_FLAGS bits: TOOL_DEBUG (0x1, kept for cached-PSO parity), bit 0x2,
// DYNAMIC_DEPTH_BIAS (0x4), and DYNAMIC_INDEX_BUFFER_STRIP_CUT (0x8). Higher bits fail with E_INVALIDARG
// on both Windows hardware D3D12 and WARP.
constexpr UINT kKnownPipelineStateFlags = 0xFu;

class RootSignature final
    : public Child<RootSignature, ID3D12RootSignatureStubs, ID3D12Object, ID3D12DeviceChild, ID3D12RootSignature> {
public:
    // {3e9b4a61-2d7c-4f18-b5a0-c71e8d92f036}
    static constexpr GUID kTag{0x3e9b4a61, 0x2d7c, 0x4f18, {0xb5, 0xa0, 0xc7, 0x1e, 0x8d, 0x92, 0xf0, 0x36}};
    RootSignature(RootArgumentLayout layout, std::string content)
        : layout_(std::move(layout)), content_(std::move(content)) {}
    const RootArgumentLayout& layout() const { return layout_; }
    // SHA-256 of the serialized root signature: a cached PSO matches root signatures with the same content.
    const std::string& content() const { return content_; }

private:
    RootArgumentLayout layout_;
    std::string content_;
};

// The ID3DBlob GetCachedBlob returns.
class CachedBlob final : public ID3DBlob {
public:
    explicit CachedBlob(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) {
            AddRef();
            *out = static_cast<ID3DBlob*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return bytes_.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return bytes_.size(); }

private:
    std::vector<std::uint8_t> bytes_;
    std::atomic<ULONG> refs_{1};
};

// A shader as the key sees it, without reading the whole program on every PSO: a DXBC container already carries a
// checksum of its contents (bytes 4..19), so that and the size stand for it. A container without one (all zero, as
// hand-built test containers have) or other bytecode is hashed with FNV-1a.
void key_shader(palmetto::cached_pso::KeyBuilder& key, const char* stage, const D3D12_SHADER_BYTECODE& code) {
    const auto* bytes = static_cast<const std::uint8_t*>(code.pShaderBytecode);
    const std::size_t size = bytes ? code.BytecodeLength : 0;
    key.tag(stage).u32(std::uint32_t(size)).u32(std::uint32_t(std::uint64_t(size) >> 32));
    if (size >= 20 && std::memcmp(bytes, "DXBC", 4) == 0 &&
        std::any_of(bytes + 4, bytes + 20, [](std::uint8_t b) { return b != 0; })) {
        key.bytes(bytes + 4, 16);
        return;
    }
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < size; ++i) h = (h ^ bytes[i]) * 0x100000001b3ull;
    key.u32(std::uint32_t(h)).u32(std::uint32_t(h >> 32));
}

void key_root_signature(palmetto::cached_pso::KeyBuilder& key, ID3D12RootSignature* signature);

// The parts of a compute description the PC runtime matches a cached PSO against (not Flags, measured).
palmetto::cached_pso::Key pipeline_key(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc) {
    palmetto::cached_pso::KeyBuilder key;
    key.tag("compute");
    key_root_signature(key, desc.pRootSignature);
    key_shader(key, "cs", desc.CS);
    key.u32(desc.NodeMask);
    return palmetto::sha256_hex(key.data());
}

palmetto::cached_pso::Key pipeline_key(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    palmetto::cached_pso::KeyBuilder key;
    key.tag("graphics");
    key_root_signature(key, desc.pRootSignature);
    key_shader(key, "vs", desc.VS);
    key_shader(key, "ps", desc.PS);
    key_shader(key, "ds", desc.DS);
    key_shader(key, "hs", desc.HS);
    key_shader(key, "gs", desc.GS);
    const D3D12_STREAM_OUTPUT_DESC& so = desc.StreamOutput;
    key.u32(so.pSODeclaration ? so.NumEntries : 0);
    for (UINT i = 0; so.pSODeclaration && i < so.NumEntries; ++i) {
        const D3D12_SO_DECLARATION_ENTRY& e = so.pSODeclaration[i];
        key.u32(e.Stream).text(e.SemanticName).u32(e.SemanticIndex).u32(e.StartComponent).u32(e.ComponentCount);
        key.u32(e.OutputSlot);
    }
    key.u32(so.pBufferStrides ? so.NumStrides : 0);
    for (UINT i = 0; so.pBufferStrides && i < so.NumStrides; ++i) key.u32(so.pBufferStrides[i]);
    key.u32(so.RasterizedStream);
    const D3D12_BLEND_DESC& blend = desc.BlendState;
    key.u32(blend.AlphaToCoverageEnable).u32(blend.IndependentBlendEnable);
    for (const D3D12_RENDER_TARGET_BLEND_DESC& t : blend.RenderTarget)
        key.u32(t.BlendEnable).u32(t.LogicOpEnable).u32(t.SrcBlend).u32(t.DestBlend).u32(t.BlendOp).u32(t.SrcBlendAlpha)
            .u32(t.DestBlendAlpha).u32(t.BlendOpAlpha).u32(t.LogicOp).u32(t.RenderTargetWriteMask);
    key.u32(desc.SampleMask);
    const D3D12_RASTERIZER_DESC& r = desc.RasterizerState;
    std::uint32_t bias_clamp = 0, slope = 0;
    std::memcpy(&bias_clamp, &r.DepthBiasClamp, 4);
    std::memcpy(&slope, &r.SlopeScaledDepthBias, 4);
    key.u32(r.FillMode).u32(r.CullMode).u32(r.FrontCounterClockwise).u32(std::uint32_t(r.DepthBias)).u32(bias_clamp);
    key.u32(slope).u32(r.DepthClipEnable).u32(r.MultisampleEnable).u32(r.AntialiasedLineEnable);
    key.u32(r.ForcedSampleCount).u32(r.ConservativeRaster);
    const D3D12_DEPTH_STENCIL_DESC& d = desc.DepthStencilState;
    key.u32(d.DepthEnable).u32(d.DepthWriteMask).u32(d.DepthFunc).u32(d.StencilEnable).u32(d.StencilReadMask);
    key.u32(d.StencilWriteMask);
    for (const D3D12_DEPTH_STENCILOP_DESC& face : {d.FrontFace, d.BackFace})
        key.u32(face.StencilFailOp).u32(face.StencilDepthFailOp).u32(face.StencilPassOp).u32(face.StencilFunc);
    const D3D12_INPUT_LAYOUT_DESC& input = desc.InputLayout;
    const UINT input_count =
        input.pInputElementDescs ? std::min<UINT>(input.NumElements, D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) : 0;
    key.u32(input_count);
    for (UINT i = 0; i < input_count; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& e = input.pInputElementDescs[i];
        key.text(e.SemanticName).u32(e.SemanticIndex).u32(e.Format).u32(e.InputSlot).u32(e.AlignedByteOffset);
        key.u32(e.InputSlotClass).u32(e.InstanceDataStepRate);
    }
    key.u32(desc.IBStripCutValue).u32(desc.PrimitiveTopologyType).u32(desc.NumRenderTargets);
    for (const DXGI_FORMAT format : desc.RTVFormats) key.u32(format);
    key.u32(desc.DSVFormat).u32(desc.SampleDesc.Count).u32(desc.SampleDesc.Quality).u32(desc.NodeMask);
    return palmetto::sha256_hex(key.data());
}

// CachedPSO as the PC runtime checks it before creating the PSO (src/cached_pso.h has the measured answers).
HRESULT check_cached_pso(const palmetto::cached_pso::Identity& identity, const palmetto::cached_pso::Key& key,
                         const D3D12_CACHED_PIPELINE_STATE& cached) {
    if (!cached.pCachedBlob) return S_OK;
    if (!cached.CachedBlobSizeInBytes) return E_INVALIDARG;
    using palmetto::cached_pso::Check;
    switch (palmetto::cached_pso::check(identity, key,
                                       {static_cast<const std::uint8_t*>(cached.pCachedBlob), cached.CachedBlobSizeInBytes})) {
    case Check::accepted: return S_OK;
    case Check::other_adapter: return D3D12_ERROR_ADAPTER_NOT_FOUND;
    case Check::other_driver: return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
    default: return E_INVALIDARG;
    }
}

// The console's ID3D12PipelineState goes past the public interface, which ends at slot 8 (GetCachedBlob). The
// FidelityFX DLL calls slot 11 on every compute pipeline it creates (docs/d3d12_command_objects.md, "Pipeline slot 11
// sizes the compute scratch"). These three are declared here, ahead of Object's virtual destructor, so they are vtable
// slots 9, 10 and 11 under MSVC and GCC alike; declared in PipelineState itself they would come after the destructor,
// which takes one slot under MSVC and two under GCC. d3d12_fspx_compute_test calls slot 11 through the vtable.
class XboxPipelineStateStubs : public ID3D12PipelineStateStubs {
public:
    virtual HRESULT STDMETHODCALLTYPE xbox_slot9(UINT64, UINT64, UINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE xbox_slot10(UINT64, UINT64, UINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE xbox_slot11(UINT64, UINT64, void** out) = 0;
};

// A pipeline state. Compute only for now: the shader is found by the SHA-256 of its FSSM program in the
// translator's index, and the FSSM blob is copied into the arena as the shader's code base.
// A D3D12_PIPELINE_STATE_STREAM_DESC (ID3D12Device2::CreatePipelineState) as the descriptions the PSO creators take.
// The stream is a run of subobjects, each a D3D12_PIPELINE_STATE_SUBOBJECT_TYPE followed by its description at that
// description's alignment, each subobject starting at pointer alignment (the d3dx12.h layout, `alignas(void*)`). A
// subobject that is absent takes the runtime's default (the CD3DX12 defaults: depth test on with LESS, back-face
// culling, no blending, sample mask 0xFFFFFFFF, one sample). A type given twice, a truncated stream or an unknown
// type is E_INVALIDARG, as on the PC runtime; state this layer cannot honour yet is E_NOTIMPL with a log.
struct PipelineStream {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    D3D12_SHADER_BYTECODE ms{};
    D3D12_SHADER_BYTECODE as{};
    bool has_cs = false;
    bool has_graphics_stage = false;  // VS, PS, DS, HS, GS, AS or MS
};

HRESULT parse_pipeline_stream(const D3D12_PIPELINE_STATE_STREAM_DESC& desc, PipelineStream& out) {
    if (!desc.pPipelineStateSubobjectStream || !desc.SizeInBytes) return E_INVALIDARG;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC& g = out.graphics;
    for (auto& target : g.BlendState.RenderTarget) {
        target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    g.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    g.RasterizerState.DepthClipEnable = TRUE;
    g.DepthStencilState.DepthEnable = TRUE;
    g.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    g.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    g.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    g.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    g.DepthStencilState.FrontFace = g.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    g.SampleMask = UINT_MAX;
    g.SampleDesc = {1, 0};

    const auto* base = static_cast<const std::uint8_t*>(desc.pPipelineStateSubobjectStream);
    const std::size_t size = desc.SizeInBytes;
    const auto round_up = [](std::size_t v, std::size_t a) { return (v + a - 1) / a * a; };
    std::uint64_t seen = 0;
    std::size_t at = 0;
    while (at < size) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        if (size - at < sizeof(type)) return E_INVALIDARG;
        std::memcpy(&type, base + at, sizeof(type));
        // The versioned depth-stencil and rasterizer subobjects describe the same state as their base type, so the
        // stream contract counts any two of one family as duplicates; the payload below still follows `type`.
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE family = type;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1 ||
            type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2)
            family = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1 ||
                 type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2)
            family = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
        if (unsigned(type) >= 64 || (seen >> unsigned(family) & 1)) return E_INVALIDARG;
        seen |= std::uint64_t(1) << unsigned(family);
        bool ok = true;
        const auto take = [&](auto& value) {
            using T = std::remove_reference_t<decltype(value)>;
            const std::size_t payload = round_up(at + sizeof(type), alignof(T));
            if (payload > size || size - payload < sizeof(T)) {
                ok = false;
                return;
            }
            std::memcpy(&value, base + payload, sizeof(T));
            at = round_up(payload + sizeof(T), sizeof(void*));
        };
        const auto stage = [&](D3D12_SHADER_BYTECODE& code) {
            take(code);
            out.has_graphics_stage = true;
        };
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            take(g.pRootSignature);
            out.compute.pRootSignature = g.pRootSignature;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: stage(g.VS); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: stage(g.PS); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: stage(g.DS); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: stage(g.HS); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: stage(g.GS); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: stage(out.as); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: stage(out.ms); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            take(out.compute.CS);
            out.has_cs = true;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: take(g.StreamOutput); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: take(g.BlendState); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: take(g.SampleMask); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: take(g.RasterizerState); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: take(g.DepthStencilState); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: take(g.InputLayout); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: take(g.IBStripCutValue); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: take(g.PrimitiveTopologyType); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
            D3D12_RT_FORMAT_ARRAY formats{};
            take(formats);
            if (formats.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) return E_INVALIDARG;
            g.NumRenderTargets = formats.NumRenderTargets;
            std::copy(std::begin(formats.RTFormats), std::end(formats.RTFormats), std::begin(g.RTVFormats));
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: take(g.DSVFormat); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: take(g.SampleDesc); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            take(g.NodeMask);
            out.compute.NodeMask = g.NodeMask;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            take(g.CachedPSO);
            out.compute.CachedPSO = g.CachedPSO;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
            take(g.Flags);
            out.compute.Flags = g.Flags;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
            D3D12_DEPTH_STENCIL_DESC1 d{};
            take(d);
            if (ok && d.DepthBoundsTestEnable) {
                log_once("depth-bounds tests are not supported yet; stream subobject", int(type));
                return E_NOTIMPL;
            }
            g.DepthStencilState = {d.DepthEnable, d.DepthWriteMask, d.DepthFunc, d.StencilEnable, d.StencilReadMask,
                                   d.StencilWriteMask, d.FrontFace, d.BackFace};
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: {
            D3D12_DEPTH_STENCIL_DESC2 d{};
            take(d);
            if (!ok) break;
            if (d.DepthBoundsTestEnable) {
                log_once("depth-bounds tests are not supported yet; stream subobject", int(type));
                return E_NOTIMPL;
            }
            // Separate front and back stencil masks are not modelled (one read and one write mask per PSO).
            if (d.StencilEnable && (d.FrontFace.StencilReadMask != d.BackFace.StencilReadMask ||
                                    d.FrontFace.StencilWriteMask != d.BackFace.StencilWriteMask)) {
                log_once("separate front and back stencil masks are not supported yet; stream subobject", int(type));
                return E_NOTIMPL;
            }
            const auto face = [](const D3D12_DEPTH_STENCILOP_DESC1& f) {
                return D3D12_DEPTH_STENCILOP_DESC{f.StencilFailOp, f.StencilDepthFailOp, f.StencilPassOp, f.StencilFunc};
            };
            g.DepthStencilState = {d.DepthEnable, d.DepthWriteMask, d.DepthFunc, d.StencilEnable,
                                   UINT8(d.FrontFace.StencilReadMask), UINT8(d.FrontFace.StencilWriteMask),
                                   face(d.FrontFace), face(d.BackFace)};
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: {
            // Their DepthBias is a float; the PSO path takes D3D12_RASTERIZER_DESC's integer one.
            FLOAT bias = 0;
            D3D12_RASTERIZER_DESC& r = g.RasterizerState;
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1) {
                D3D12_RASTERIZER_DESC1 d{};
                take(d);
                r = {d.FillMode, d.CullMode, d.FrontCounterClockwise, 0, d.DepthBiasClamp, d.SlopeScaledDepthBias,
                     d.DepthClipEnable, d.MultisampleEnable, d.AntialiasedLineEnable, d.ForcedSampleCount,
                     d.ConservativeRaster};
                bias = d.DepthBias;
            } else {
                D3D12_RASTERIZER_DESC2 d{};
                take(d);
                r = {d.FillMode, d.CullMode, d.FrontCounterClockwise, 0, d.DepthBiasClamp, d.SlopeScaledDepthBias,
                     d.DepthClipEnable, FALSE, FALSE, d.ForcedSampleCount, d.ConservativeRaster};
                bias = d.DepthBias;
            }
            r.DepthBias = INT(bias);
            if (ok && FLOAT(r.DepthBias) != bias) log_once("a fractional depth bias is truncated; bias x1000", int(bias * 1000));
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
            D3D12_VIEW_INSTANCING_DESC d{};
            take(d);
            if (ok && d.ViewInstanceCount) {
                log_once("view instancing is not supported yet; views", int(d.ViewInstanceCount));
                return E_NOTIMPL;
            }
            break;
        }
        default:
            log_once("unknown pipeline state stream subobject type:", int(type));
            return E_INVALIDARG;
        }
        if (!ok) return E_INVALIDARG;
    }
    if (out.has_cs && out.has_graphics_stage) return E_INVALIDARG;  // a compute stage with graphics stages
    if (!out.has_cs && !out.has_graphics_stage) return E_INVALIDARG;
    return S_OK;
}

class PipelineState final
    : public Child<PipelineState, XboxPipelineStateStubs, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable,
                   ID3D12PipelineState> {
public:
    // {8d2f6c14-b0e7-4a93-9c58-1f4a7e3b6d20}
    static constexpr GUID kTag{0x8d2f6c14, 0xb0e7, 0x4a93, {0x9c, 0x58, 0x1f, 0x4a, 0x7e, 0x3b, 0x6d, 0x20}};
    explicit PipelineState(DeviceState& device) : device_(device) {}

    ~PipelineState() override {
        auto& vk = device_.context->vk;
        const VkDevice dev = device_.info.device;
        // docs/deferred_compile.md: the task holds a reference to this PSO until its build has run, so a deferred
        // handle is either complete here or the build never ran; forget the id either way.
        device_.forget_deferred_compute(id());
        device_.forget_pipeline(id());
        if (registered_) device_.context->remove_pipeline(id());
        // A shared entry owns its Vulkan objects; the PSO only releases its reference, and the entry's destructor
        // destroys them when the last PSO using it is gone. Per-PSO variant pipelines go first: the shared layout must
        // outlive every pipeline built from it.
        {
            std::lock_guard lock(deferred_handle_mutex_);  // orders with the compile worker's handle write
            if (!shared_compute_) {
                if (pipeline_.pipeline) vk.vkDestroyPipeline(dev, pipeline_.pipeline, nullptr);
                if (pipeline_.layout) vk.vkDestroyPipelineLayout(dev, pipeline_.layout, nullptr);
                if (module_) vk.vkDestroyShaderModule(dev, module_, nullptr);
            }
        }
        for (const auto& [cull, built] : ps_variants_) vk.vkDestroyPipeline(dev, built, nullptr);  // PS-only control
        // docs/vertex_buffer_va.md section 22: the cull-only content-window variants (a failed build is cached as null).
        for (const auto& [pair, built] : cull_variants_)
            if (built) vk.vkDestroyPipeline(dev, built, nullptr);
        for (const auto& [unbound, built] : variants_) vk.vkDestroyPipeline(dev, built, nullptr);
        if (!shared_graphics_) {
            if (graphics_.pipeline) vk.vkDestroyPipeline(dev, graphics_.pipeline, nullptr);
            if (graphics_.layout) vk.vkDestroyPipelineLayout(dev, graphics_.layout, nullptr);
            for (VkShaderModule module : modules_) vk.vkDestroyShaderModule(dev, module, nullptr);
        }
        shared_graphics_.reset();  // last reference: destroys pipeline, modules and layout exactly once
        shared_compute_.reset();
        if (code_bytes_) device_.arena_release(code_base_, code_bytes_, DeviceState::arena_shader);
    }

    // Pipeline layouts declare the whole 144-byte block as one push-constant range. A device whose
    // maxPushConstantsSize is smaller (128 bytes is the Vulkan minimum, which some Intel and mobile drivers
    // report) needs the uniform-buffer tail #224 flags with push_constants_need_fallback, which is not built yet,
    // so pipelines are refused there with the limit named instead of breaking VUID-VkPushConstantRange-size-00298.
    bool push_constants_fit() const {
        const std::uint32_t limit = device_.info.max_push_constants_size;
        if (limit == 0 || limit >= sizeof(palmetto::ShaderUserData)) return true;  // 0: not queried
        log_once("pipelines need 144 bytes of push constants (uniform-buffer fallback not built yet); "
                 "maxPushConstantsSize",
                 int(limit));
        return false;
    }

    // Asks for the subgroup size the shader was translated for, where the device allows (src/subgroup_size.h): the
    // stage gets VkPipelineShaderStageRequiredSubgroupSizeCreateInfo in `storage` (which must outlive pipeline
    // creation) and, for compute, REQUIRE_FULL_SUBGROUPS. A size the device cannot give is reported, not hidden.
    void subgroup_size_for(const palmetto::ShaderEntry& entry, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo& storage,
                           VkPipelineShaderStageCreateInfo& stage) {
        const palmetto::SubgroupRequest request = palmetto::decide_subgroup_size(entry, device_.info.subgroup);
        if (request.outcome == palmetto::SubgroupOutcome::unavailable)
            report_subgroup_unavailable(entry, request, device_.info.subgroup);
        palmetto::apply_subgroup_request(request, storage, stage);
        subgroup_requests_.push_back(request);
    }

    std::uintptr_t id() const { return reinterpret_cast<std::uintptr_t>(static_cast<const ID3D12PipelineState*>(this)); }
    void set_cache_key(palmetto::cached_pso::Key key) { cache_key_ = std::move(key); }

    // False only for a description whose key is not content-based (a foreign root signature is hashed by address):
    // such a PSO is never shared through the recipe cache.
    void set_cache_key_deterministic(bool deterministic) { cache_key_deterministic_ = deterministic; }
    bool recipe_cache_ready() const { return !cache_key_.empty() && cache_key_deterministic_; }

    // Adopt a recipe cache entry created by an earlier identical description: the PSO gets the entry's VkPipeline and
    // VkPipelineLayout, a copy of its recipe (for the lazy variants), the replay-visible fields and its own copy of the
    // program bytes the shader's code base reads. The entry stays alive through shared_graphics_ until this PSO is
    // released; the last release destroys its Vulkan objects exactly once.
    HRESULT adopt_graphics_recipe(const std::shared_ptr<SharedGraphicsPipeline>& shared) {
        if (!shared) return E_FAIL;
        // The per-PSO code-base copy comes first: it can fail, and nothing is adopted until it is in place.
        VkDeviceSize code_base = 0, code_bytes = 0;
        if (!shared->code_bytes.empty()) {
            VkDeviceSize offset = 0;
            if (!device_.arena_allocate(shared->code_bytes.size(), 256, offset, DeviceState::arena_shader))
                return E_OUTOFMEMORY;
            std::memcpy(device_.arena.mapped + offset, shared->code_bytes.data(), shared->code_bytes.size());
            code_base = offset;
            code_bytes = shared->code_bytes.size();
        }
        graphics_ = palmetto::GraphicsPipeline{};
        graphics_.pipeline = shared->pipeline;
        graphics_.layout = shared->layout;
        graphics_.push_stages = shared->push_stages;
        graphics_.code_base = code_base;
        graphics_.bindless_set = shared->bindless_set;
        graphics_.color_formats = shared->color_formats;
        graphics_.depth_format = shared->depth_format;
        graphics_.depth_aspects = shared->depth_aspects;
        graphics_.vertex_slots = shared->vertex_slots;
        graphics_.zero_step_slots = shared->zero_step_slots;
        graphics_.strip_cut = shared->strip_cut;
        graphics_.mesh = shared->mesh;
        graphics_.table_windows = shared->table_windows;
        graphics_.samples = shared->samples;
        graphics_.has_pixel_stage = shared->has_pixel_stage;
        graphics_.ps_images = shared->ps_images;
        graphics_.vs_loads = shared->vs_loads;
        graphics_.console_ok = shared->console_ok;
        graphics_.console_parameter_count = shared->console_parameter_count;
        std::copy(std::begin(shared->console_parameter), std::end(shared->console_parameter),
                  std::begin(graphics_.console_parameter));
        graphics_.variant = [this](std::uint32_t unbound) { return variant(unbound); };
        graphics_.with_ps = [this](VkShaderModule ps, bool cull_none) { return variant_with_ps(ps, cull_none); };
        // Diagnostic (docs/vertex_buffer_va.md section 22): the same recipe with only its cull pair replaced, keyed by
        // (cull mode, front face) in its own cache. Only read by the env-gated content-window rule.
        graphics_.with_cull = [this](std::uint32_t cull_mode, bool front_ccw) {
            return variant_with_cull(cull_mode, front_ccw);
        };
        graphics_.describe_vertex_path = [shared](palmetto::VertexPathInfo& out) { out = shared->vertex_path; };
        recipe_ = shared->recipe;
        // The recipe's stage create infos chain their required-subgroup-size structs; point the copy at its own so a
        // lazily built variant reads this PSO's recipe, not the entry's.
        for (std::size_t i = 0; i < recipe_.stages.size() && i < std::size(recipe_.required_sizes); ++i)
            if (recipe_.stages[i].pNext == &shared->recipe.required_sizes[i])
                recipe_.stages[i].pNext = &recipe_.required_sizes[i];
        subgroup_requests_ = shared->subgroup_requests;
        code_base_ = code_base;
        code_bytes_ = code_bytes;
        shared_graphics_ = shared;
        device_.context->add_graphics_pipeline(id(), graphics_);
        registered_ = true;
        is_graphics_ = true;
        return S_OK;
    }

    HRESULT adopt_compute_recipe(const std::shared_ptr<SharedComputePipeline>& shared) {
        if (!shared || shared->program_bytes.empty()) return E_FAIL;
        VkDeviceSize offset = 0;
        if (!device_.arena_allocate(shared->program_bytes.size(), 256, offset, DeviceState::arena_shader))
            return E_OUTOFMEMORY;
        std::memcpy(device_.arena.mapped + offset, shared->program_bytes.data(), shared->program_bytes.size());
        pipeline_ = palmetto::ComputePipeline{};
        pipeline_.pipeline = shared->pipeline;
        pipeline_.layout = shared->layout;
        pipeline_.code_base = offset;
        pipeline_.bindless_set = shared->bindless_set;
        code_base_ = offset;
        code_bytes_ = shared->program_bytes.size();
        module_ = VK_NULL_HANDLE;
        shared_compute_ = shared;
        device_.context->add_pipeline(id(), pipeline_);
        registered_ = true;
        return S_OK;
    }

    // A null pointer is accepted with S_OK, as by the PC runtime (measured on an RTX 3060 and WARP).
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** blob) override {
        if (!blob) return S_OK;
        *blob = new CachedBlob(palmetto::cached_pso::encode(device_.pso_identity, cache_key_));
        return S_OK;
    }

    // Slots 9 and 10: console methods no caller is known to use; refused, and named once in the log.
    HRESULT STDMETHODCALLTYPE xbox_slot9(UINT64, UINT64, UINT64) override {
        log_once("console pipeline slot called (not implemented); slot", 9);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE xbox_slot10(UINT64, UINT64, UINT64) override {
        log_once("console pipeline slot called (not implemented); slot", 10);
        return E_NOTIMPL;
    }

    // Slot 11, called by the FidelityFX DLL as (0, 0, &obj) after it creates a pipeline. It reads one dword through
    // obj's slot 3 (the shape of ID3DBlob::GetBufferPointer), multiplies it by 1024, releases obj through slot 2, and
    // creates and binds a compute scratch buffer before a dispatch only when that value is non-zero (measured in the
    // DLL's code, docs/d3d12_command_objects.md). The answer is a blob holding 0: none of the 1.0.2.1 DLL's 29 programs
    // enables scratch (COMPUTE_PGM_RSRC2.SCRATCH_EN clear in all 29) or has a scratch instruction (measured), and a
    // translated module keeps its private data in SPIR-V, not in a console scratch buffer. With 0 the DLL creates no
    // scratch buffer and skips list slot 141. The arguments are logged once when they are not the measured zeros.
    HRESULT STDMETHODCALLTYPE xbox_slot11(UINT64 a, UINT64 b, void** out) override {
        if (a || b) log_once("console pipeline slot 11 with unexpected arguments; first", int(a ? a : b));
        if (!out) return E_POINTER;
        *out = static_cast<ID3DBlob*>(new CachedBlob(std::vector<std::uint8_t>(4, 0)));
        return S_OK;
    }

    bool compute() const { return registered_ && !is_graphics_; }
    bool graphics() const { return registered_ && is_graphics_; }
    const std::vector<palmetto::SubgroupRequest>& subgroup_requests() const { return subgroup_requests_; }

    // A VS (+ GS) (+ PS) pipeline for dynamic rendering. Vertex input comes from the input layout matched to the
    // VS's input signature by semantic name and index: location = the element's register, the translator's rule
    // (docs/design.md, "Vertex fetch"). Refused with a log: hull and domain stages, stream output, the centre
    // multisample pattern, two stages that both read their code base, and state that needs a Vulkan feature the
    // device lacks (including the geometryShader feature a GS needs). A GS takes triangles; the sample count may
    // be above one. Viewport, scissor, topology, vertex strides, stencil reference and blend factor are dynamic,
    // set from the list's state at each draw.
    // With `ms` (a pipeline state stream's MS subobject) it is an MS (+ PS) pipeline on VK_EXT_mesh_shader instead:
    // no VS, no vertex input or input assembly (the input layout, topology type and strip cut are ignored, as D3D12
    // does for mesh PSOs), drawn by DispatchMesh. An AS (amplification shader) is refused: the game has none.
    // `standin` is the diagnostic graphics stand-in (PALMETTO_STANDIN_PIPELINES, create_standin_graphics_pipeline): the
    // stages are the project's own kStandinVsSpirv/kStandinPsSpirv pair instead of a library translation, and no
    // shader library is needed. Set only by that entry point.
    HRESULT create_graphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const D3D12_SHADER_BYTECODE* ms = nullptr,
                            const D3D12_SHADER_BYTECODE* as = nullptr, bool standin = false) {
        auto& vk = device_.context->vk;
        const VkDevice dev = device_.info.device;
        const bool profile = pipeline_profile_enabled();
        if (profile) pipeline_profile_begin(true);
        const PipelineClock::time_point profile_started = profile ? PipelineClock::now() : PipelineClock::time_point{};
        // docs/deferred_compile.md: who enters pipeline creation, counted per caller thread
        // (PALMETTO_PIPELINE_CALL_STATS; a null stats pointer when the switch is off).
        const palmetto::PipelineCallStats::Scope call_scope(device_.pipeline_stats(), true,
                                                            palmetto::PipelineCallStats::Kind::creation);
        const bool mesh = ms && ms->pShaderBytecode && ms->BytecodeLength;
        // The per-process recipe cache (PALMETTO_PIPELINE_RECIPE_CACHE, off by default): an earlier identical description
        // (same content key) already built the pipeline, layout and modules, so adopt them instead of resolving the
        // shaders and building again. Only the plain-description path is keyed: the pipeline-state-stream path passes
        // its mesh/amplification stages separately and has no key, and the stand-in diagnostic is never shared.
        const bool recipe_cache =
            pipeline_recipe_cache_enabled() && recipe_cache_ready() && !standin && !mesh && !(as && as->pShaderBytecode);
        if (recipe_cache) {
            if (const std::shared_ptr<SharedGraphicsPipeline> shared = device_.find_graphics_recipe(cache_key_)) {
                if (profile) pipeline_profile_recipe_hit(true);
                const HRESULT adopted = adopt_graphics_recipe(shared);
                if (SUCCEEDED(adopted) && profile) pipeline_profile_created(true, profile_started, std::string());
                return adopted;
            }
        }
        if (as && as->pShaderBytecode && as->BytecodeLength) {
            log_once("amplification shaders are not supported (the game has none); bytes", int(as->BytecodeLength));
            return E_NOTIMPL;
        }
        if (mesh && (desc.VS.BytecodeLength || desc.HS.BytecodeLength || desc.DS.BytecodeLength ||
                     desc.GS.BytecodeLength || desc.StreamOutput.NumEntries))
            return E_INVALIDARG;  // D3D12: a mesh PSO has no vertex, tessellation or geometry stage
        if (!mesh && (!desc.VS.pShaderBytecode || !desc.VS.BytecodeLength)) return E_INVALIDARG;
        if (mesh && (!device_.info.mesh_shader || !vk.vkCmdDrawMeshTasksEXT)) {
            log_once("mesh-shader PSOs need VK_EXT_mesh_shader, which the device has not enabled; stage", 1);
            return E_NOTIMPL;
        }
        if (desc.NodeMask & ~1u) return E_INVALIDARG;
        if (static_cast<UINT>(desc.Flags) & ~kKnownPipelineStateFlags) return E_INVALIDARG;
        if (!device_.arena.mapped) {
            log_once("pipelines need the GPU address arena (bufferDeviceAddress); stage", 1);
            return E_NOTIMPL;
        }
        if (!push_constants_fit()) return E_NOTIMPL;
        if (!vk.graphics_complete()) {
            log_once("graphics pipelines need Vulkan 1.3 dynamic rendering and extended dynamic state; stage", 1);
            return E_NOTIMPL;
        }
        if (desc.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) return E_INVALIDARG;
        for (UINT i = desc.NumRenderTargets; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (desc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
        if (desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
            const Format dsv_fmt = vulkan_format(desc.DSVFormat, true);
            if (dsv_fmt.format == VK_FORMAT_UNDEFINED || dsv_fmt.typeless ||
                !(dsv_fmt.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
                log_once("depth-stencil format not supported:", desc.DSVFormat);
                return E_INVALIDARG;
            }
        }
        if (desc.InputLayout.NumElements > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) return E_INVALIDARG;
        if (desc.InputLayout.NumElements > 0 && !desc.InputLayout.pInputElementDescs) return E_INVALIDARG;
        for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
            const D3D12_INPUT_ELEMENT_DESC& element = desc.InputLayout.pInputElementDescs[i];
            if (!element.SemanticName) return E_INVALIDARG;
            if (element.InputSlot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) return E_INVALIDARG;
            if (element.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA &&
                element.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA)
                return E_INVALIDARG;
            if (element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA &&
                element.InstanceDataStepRate != 0)
                return E_INVALIDARG;
        }
        if (desc.BlendState.IndependentBlendEnable) {
            for (const D3D12_RENDER_TARGET_BLEND_DESC& rt : desc.BlendState.RenderTarget)
                if (rt.LogicOpEnable) return E_INVALIDARG;
        } else {
            const D3D12_RENDER_TARGET_BLEND_DESC& rt0 = desc.BlendState.RenderTarget[0];
            if (rt0.BlendEnable && rt0.LogicOpEnable) return E_INVALIDARG;
            if (rt0.LogicOpEnable && unsigned(rt0.LogicOp) > D3D12_LOGIC_OP_OR_INVERTED) return E_INVALIDARG;
        }
        if (desc.HS.BytecodeLength || desc.DS.BytecodeLength || desc.StreamOutput.NumEntries) {
            log_once("graphics pipeline stage not supported yet (hull/domain/stream output):", 0);
            return E_NOTIMPL;
        }
        // A geometry shader is the translator's Vulkan geometry stage (docs/design.md, "Geometry shaders"): it takes
        // triangles and emits a triangle strip. It needs the geometryShader feature, and the PSO's topology type must
        // be its input primitive, as D3D12 requires.
        const bool geometry = desc.GS.pShaderBytecode && desc.GS.BytecodeLength;
        if (geometry && !device_.info.features.geometryShader) {
            log_once("geometry shaders need the Vulkan geometryShader feature; stage", 0);
            return E_NOTIMPL;
        }
        if (geometry && desc.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) {
            log_once("a translated geometry shader takes triangles; topology type", desc.PrimitiveTopologyType);
            return E_INVALIDARG;
        }
        const UINT samples = desc.SampleDesc.Count;
        if (!samples || samples > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT || (samples & (samples - 1))) {
            log_once("invalid graphics pipeline sample count:", int(samples));
            return E_INVALIDARG;
        }
        // D3D12 takes quality 0 and the two pattern values (measured natively on an RX 7800 XT, a Radeon iGPU and
        // WARP: 0, D3D12_STANDARD_MULTISAMPLE_PATTERN and D3D12_CENTER_MULTISAMPLE_PATTERN succeed, 1 is
        // E_INVALIDARG, as NumQualityLevels is 1). The standard pattern is Vulkan's standard sample positions, so it
        // is quality 0 here. The centre pattern puts every sample at the pixel centre, which needs custom sample
        // locations: refused with a log until those are set.
        if (desc.SampleDesc.Quality == DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN) {
            log_once("D3D12_CENTER_MULTISAMPLE_PATTERN needs custom sample locations (not set yet); samples",
                     int(samples));
            return E_NOTIMPL;
        }
        if (desc.SampleDesc.Quality != 0 && desc.SampleDesc.Quality != DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN) {
            log_once("invalid graphics pipeline sample quality:", int(desc.SampleDesc.Quality));
            return E_INVALIDARG;
        }
        const palmetto::ShaderLibrary* library = device_.library();
        // A stand-in stage carries its module in the table, so it needs no library: a shader directory that failed to
        // load must answer with stand-ins and refusals, not abort the pipeline path.
        if (!library && !standin) return E_FAIL;

        struct Stage {
            const char* name;
            VkShaderStageFlagBits bit;
            const D3D12_SHADER_BYTECODE& code;
            const palmetto::ShaderEntry* entry = nullptr;
            std::vector<std::uint8_t> fssm;
            std::vector<std::uint32_t> words;
        };
        Stage stages[3] = {mesh ? Stage{"ms", VK_SHADER_STAGE_MESH_BIT_EXT, *ms}
                                : Stage{"vs", VK_SHADER_STAGE_VERTEX_BIT, desc.VS},
                           {"gs", VK_SHADER_STAGE_GEOMETRY_BIT, desc.GS},
                           {"ps", VK_SHADER_STAGE_FRAGMENT_BIT, desc.PS}};
        // stage_infos.size() indexes the recipe's per-stage required-size structs: one for every table entry.
        static_assert(std::size(stages) <= std::size(decltype(recipe_.required_sizes){}),
                      "GraphicsRecipe::required_sizes needs an entry per graphics stage");
        bool uses_bindless = false;  // a stage declares set-0 bindings: the layout holds the device's set 0
        std::vector<VkPipelineShaderStageCreateInfo> stage_infos;
        // Chained from stage_infos, which the recipe keeps for variants built at draw time: so they live in the recipe.
        auto& required_sizes = recipe_.required_sizes;
        const Stage* code_stage = nullptr;
        // D24_UNORM_S8_UINT stored as D32_SFLOAT_S8_UINT (no D24 on the GPU): the pixel stage writes the depth D24
        // would hold, biased in D24 units and quantised (src/depth24_fragment.h, D3D-309), and the rasterizer's own
        // bias is turned off. Multisampled targets keep the old path: a FragDepth write is per pixel.
        const bool d24_as_float = desc.DSVFormat != DXGI_FORMAT_UNKNOWN &&
                                  vulkan_format(desc.DSVFormat, true).format == VK_FORMAT_D24_UNORM_S8_UINT &&
                                  !device_.depth_usable(VK_FORMAT_D24_UNORM_S8_UINT);
        bool depth24_fragment = d24_as_float && desc.DepthStencilState.DepthEnable;
        if (depth24_fragment && samples > 1) {
            log_once("D24 depth stored as D32 keeps float depth bias and precision on multisampled targets; samples",
                     int(samples));
            depth24_fragment = false;
        }
        const palmetto::Depth24Bias depth24_bias{desc.RasterizerState.DepthBias,
                                                desc.RasterizerState.SlopeScaledDepthBias,
                                                desc.RasterizerState.DepthBiasClamp};
        for (Stage& stage : stages) {
            if (!stage.code.pShaderBytecode || !stage.code.BytecodeLength) continue;  // no PS: depth only
            const std::span<const std::uint8_t> bytecode{
                static_cast<const std::uint8_t*>(stage.code.pShaderBytecode), stage.code.BytecodeLength};
            stage.fssm = palmetto::fssm_program(bytecode);
            if (stage.fssm.empty()) {
                // The graphics stand-in's own module (see create_standin_graphics_pipeline): no library record and no
                // library lookup at all. `continue` skips the per-entry handling below (bindings, mesh limits, code
                // base, f16 halves, writable storage) - none of it applies to the default entry.
                if (standin) {
                    const std::vector<std::uint32_t>& words = standin_stage_words(stage.name);
                    if (words.empty()) {
                        log_once("the graphics stand-in has a VS+PS pair only; stage", int(stage.bit));
                        return E_NOTIMPL;
                    }
                    stage.words = words;
                    stage.entry = &standin_entry();
                    continue;
                }
                // The title's precompiled pipeline-library graphics programs are native RDNA2 programs, not FSSM/DXBC:
                // this is the graphics analogue of create_compute's lookup, under the stage's own name, and an
                // ambiguous program (translations that differ, no kind-0 state to choose between them) is refused
                // (src/graphics_pso.h). `stage.fssm` is the field the code base below is uploaded from, and for a
                // native stage the bytes the pipeline copies are the kind-6 program - exactly what create_compute
                // copies for its program.
                const bool vertex = stage.bit == VK_SHADER_STAGE_VERTEX_BIT;
                rt::graphics_pso::NativeStage native;
                std::string refusal;
                if (!rt::graphics_pso::resolve_native_stage(
                        bytecode, stage.name, *library,
                        [&](const palmetto::ShaderEntry& e) { return !vertex || layout_feeds(e, desc.InputLayout); },
                        &native, &refusal)) {
                    log_once("shader bytecode holds no FSSM or native FSPX kind-6 program; bytes",
                             int(stage.code.BytecodeLength));
                    return E_INVALIDARG;
                }
                if (!native.entry) {
                    std::fprintf(stderr, "[rt] d3d12: %s\n", refusal.c_str());
                    return E_INVALIDARG;
                }
                if (native.raw_key)
                    std::fprintf(stderr,
                                 "[rt] d3d12: %s program has no FSPX prologue; it was found by the SHA-256 of its "
                                 "program bytes (fspx_code_sha256): %s\n",
                                 stage.name, palmetto::sha256_hex(native.program.bytes).c_str());
                stage.entry = native.entry;
                stage.fssm = std::move(native.program.bytes);
            } else {
                const std::string hash = palmetto::sha256_hex(stage.fssm);
                // Containers with the same program can differ in their input signature or bindings: a VS must have
                // its inputs fed by this input layout (else the layout check below reports it).
                const bool vertex = stage.bit == VK_SHADER_STAGE_VERTEX_BIT;
                stage.entry = pick_entry(library->candidates(stage.name, hash), [&](const palmetto::ShaderEntry& e) {
                    return !vertex || layout_feeds(e, desc.InputLayout);
                });
                if (!stage.entry) {
                    std::fprintf(stderr, "[rt] d3d12: no translated %s for FSSM sha256 %s\n", stage.name,
                                 hash.c_str());
                    return E_INVALIDARG;
                }
            }
            if (stage.entry->binding_count) {
                if (!device_.bindless) {
                    std::fprintf(stderr, "[rt] d3d12: %s uses set-0 bindings; the device has no bindless set 0\n",
                                 stage.entry->name.c_str());
                    return E_NOTIMPL;
                }
                uses_bindless = true;
                register_literal_samplers(device_, *stage.entry);
            }
            if (stage.bit == VK_SHADER_STAGE_MESH_BIT_EXT) {
                // The SPIR-V's output arrays are sized from PSV0 (the index's max_vertices / max_primitives); Vulkan
                // has no per-pipeline limit to set, so a mesh shader beyond the device's is refused here
                // (docs/d3d12_vulkan.md, "Pipelines").
                if (const std::string refusal = palmetto::mesh_limit_refusal(*stage.entry, device_.mesh_limits());
                    !refusal.empty()) {
                    std::fprintf(stderr, "[rt] d3d12: %s\n", refusal.c_str());
                    return E_NOTIMPL;
                }
            }
            if (stage.entry->uses_code_base) {
                if (code_stage) {
                    log_once("two stages read their code base; one push-constant slot holds one (question 14)", 0);
                    return E_NOTIMPL;
                }
                code_stage = &stage;
            }
            stage.words = library->spirv(*stage.entry);
            if (stage.words.empty()) return E_FAIL;
            // Diagnostic (docs/page_content.md): what a translated stage actually does with descriptors and images, so a
            // frame that renders black can be told apart from a texture-content gap. Counts only; the drawn pipeline
            // reports the pixel stage's image count (GraphicsPipeline::ps_images).
            {
                unsigned samples = 0, reads = 0, writes = 0, loads = 0;
                const std::vector<std::uint32_t>& words = stage.words;
                for (std::size_t at = 5; at < words.size();) {
                    const std::uint32_t opcode = words[at] & 0xffffu;
                    const std::uint32_t length = words[at] >> 16;
                    if (!length) break;
                    if (opcode >= 86 && opcode <= 97) ++samples;
                    else if (opcode == 98) ++reads;
                    else if (opcode == 99) ++writes;
                    else if (opcode == 61) ++loads;
                    at += length;
                }
                const bool pixel = stage.bit == VK_SHADER_STAGE_FRAGMENT_BIT;
                const bool vertex = stage.bit == VK_SHADER_STAGE_VERTEX_BIT;
                if (pixel) {
                    graphics_.has_pixel_stage = true;
                    graphics_.ps_images = samples + reads;
                } else if (vertex) {
                    graphics_.vs_loads = loads;
                }
                static std::atomic<unsigned> vs_logged{0}, ps_logged{0};
                std::atomic<unsigned>& logged = pixel ? ps_logged : vs_logged;
                if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                    std::fprintf(stderr,
                                 "[rt] d3d12: pso stage %s %s: spv %zu word(s), %u image sample/gather, %u image read, "
                                 "%u image write, %u load(s), %zu output(s), %zu binding(s), %zu literal sampler(s), "
                                 "code base %d\n",
                                 stage.name, stage.entry->stage.c_str(), words.size(), samples, reads, writes, loads,
                                 stage.entry->outputs.size(), stage.entry->binding_count,
                                 stage.entry->literal_samplers.size(), stage.entry->uses_code_base ? 1 : 0);
            }
            const auto inner = palmetto::conservative_raster::inner_coverage_requirements(stage.words);
            if (!inner.valid) return E_INVALIDARG;
            if (inner.fully_covered &&
                (stage.bit != VK_SHADER_STAGE_FRAGMENT_BIT ||
                 device_.conservative_raster_tier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_3 ||
                 (inner.post_depth && !device_.conservative_post_depth_coverage))) {
                log_once("FullyCoveredEXT needs an enabled Tier 3 shader ABI and device properties; stage", int(stage.bit));
                return E_NOTIMPL;
            }
        }
        // Diagnostic (PALMETTO_SIG_CLASS=1, docs/root_arguments.md section 9): classify the root signature's console
        // parameter layout for the two pass pixel stages whose published read set is s8+s9 - the sibling pass
        // (fspx/lib_ps_00820) and the compose pass (fspx/lib_ps_00822). The question this answers without a boot-time
        // guess: is s9 a declared 32-bit constant parameter after the descriptor table, or is it beyond the signature
        // span (driver-defined on the console)? Types, console slots and dword counts only; one line per pipeline
        // shape.
        if ([] {
                static const bool on = [] {
                    char value[8] = {};
                    return GetEnvironmentVariableA("PALMETTO_SIG_CLASS", value, sizeof(value)) > 0 && value[0] != '\0' &&
                           value[0] != '0';
                }();
                return on;
            }()) {
            for (const Stage& stage : stages) {
                if (!stage.entry) continue;
                if (stage.entry->name.find("lib_ps_00820") == std::string::npos &&
                    stage.entry->name.find("lib_ps_00822") == std::string::npos)
                    continue;
                std::string pipeline;
                for (const Stage& other : stages) {
                    if (!other.entry) continue;
                    if (!pipeline.empty()) pipeline += "+";
                    pipeline += other.entry->name;
                }
                std::vector<rt::graphics_pso::ConsoleParameter> parameters;
                if (const RootArgumentLayout* root = root_signature_layout(desc.pRootSignature)) {
                    for (const auto& placed : root->parameters) {
                        rt::graphics_pso::ConsoleParameter entry;
                        entry.type = placed.type;
                        entry.values = placed.constant_count;
                        entry.first_sgpr = placed.console_offset;
                        entry.dwords = rt::rootsig::stage_dwords(placed.type, placed.constant_count);
                        parameters.push_back(entry);
                    }
                }
                std::string shape = pipeline;
                for (const auto& entry : parameters)
                    shape += " " + std::to_string(int(entry.type)) + "@+" + std::to_string(entry.first_sgpr) + "x" +
                             std::to_string(entry.dwords);
                static std::mutex class_mutex;
                static std::set<std::string> printed;
                {
                    const std::lock_guard lock(class_mutex);
                    if (!printed.insert(shape).second) continue;
                }
                rt::graphics_pso::ConsoleSignatureClass classified;
                rt::graphics_pso::classify_console_parameters(parameters, &classified);
                std::fprintf(stderr, "[rt] d3d12: signature class %s: %zu parameter(s):", pipeline.c_str(),
                             parameters.size());
                for (const auto& entry : parameters)
                    std::fprintf(stderr, " [type %d, %u constant value(s), console slot s%d, %u dword(s)]",
                                 int(entry.type), entry.values,
                                 entry.first_sgpr == rt::rootsig::kNoSgpr ? -1 : int(entry.first_sgpr), entry.dwords);
                if (classified.constant_after_table >= 0)
                    std::fprintf(stderr,
                                 " -> a 32-bit constant AFTER the descriptor table: parameter %d at console s%u, "
                                 "%u dword(s)\n",
                                 classified.constant_after_table, classified.constant_after_table_slot,
                                 classified.constant_after_table_dwords);
                else if (classified.table_index >= 0)
                    std::fprintf(stderr,
                                 " -> descriptor table at parameter %d and NO 32-bit constant after it (table-only after "
                                 "the table)\n",
                                 classified.table_index);
                else
                    std::fprintf(stderr, " -> no descriptor table parameter\n");
            }
        }
        // --f16-interp-fallback (SHADER-378): align/synthesize VS decoded f16 halves for this (VS, PS) pair before
        // creating the shader modules so colliding PS and high-param producers link within MoltenVK's 30 locations.
        // stages[] is {vs, gs, ps} since geometry shaders joined graphics PSOs (#2109): the PS is stages[2], and with a
        // GS the PS reads the GS's outputs, not the VS's, so the VS/PS halves link only applies without one.
        Stage& vs_stage = stages[0];
        Stage& gs_stage = stages[1];
        Stage& ps_stage = stages[2];
        if (vs_stage.entry && ps_stage.entry && !ps_stage.entry->f16_halves.empty() && gs_stage.entry) {
            log_once("--f16-interp-fallback halves are linked VS->PS only; this PSO has a geometry shader", 0);
        }
        if (vs_stage.entry && ps_stage.entry && !gs_stage.entry &&
            (!ps_stage.entry->f16_halves.empty() || !vs_stage.entry->f16_halves.empty())) {
            std::string link_error;
            if (!palmetto::link_f16_halves(vs_stage.words, *vs_stage.entry, ps_stage.words, *ps_stage.entry,
                                          &link_error, device_.f16_half_top_location)) {
                std::fprintf(stderr, "[rt] d3d12: %s\n", link_error.c_str());
                return E_FAIL;
            }
        }
        // A stage that declares a storage image or buffer it may write needs a device feature outside compute
        // (src/spirv_stores.h). Without it the pipeline would be created all the same and break
        // VUID-RuntimeSpirv-NonWritable-06340 / -06341, so the PSO is refused here with the stage named.
        for (const Stage& stage : stages) {
            if (!stage.entry) continue;
            const bool pixel = stage.bit == VK_SHADER_STAGE_FRAGMENT_BIT;
            const VkBool32 allowed = pixel ? device_.info.features.fragmentStoresAndAtomics
                                           : device_.info.features.vertexPipelineStoresAndAtomics;
            if (allowed || !palmetto::spirv_declares_writable_storage(stage.words)) continue;
            std::fprintf(stderr,
                         "[rt] d3d12: %s (%s) writes a storage image or buffer, which needs the Vulkan feature %s; "
                         "the device does not have it enabled, so the pipeline is refused\n",
                         stage.entry->name.c_str(), stage.name,
                         pixel ? "fragmentStoresAndAtomics" : "vertexPipelineStoresAndAtomics");
            return E_NOTIMPL;
        }
        for (Stage& stage : stages) {
            if (!stage.entry) continue;
            // After the f16 link above, which can also edit the pixel shader's words.
            if (depth24_fragment && stage.bit == VK_SHADER_STAGE_FRAGMENT_BIT) {
                std::string why;
                std::vector<std::uint32_t> patched = palmetto::depth24_patch_fragment(stage.words, depth24_bias, why);
                if (patched.empty()) {
                    std::fprintf(stderr, "[rt] d3d12: %s keeps float depth on a D24 target stored as D32: %s\n",
                                 stage.entry->name.c_str(), why.c_str());
                    depth24_fragment = false;
                } else {
                    stage.words = std::move(patched);
                }
            }
            VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module_info.codeSize = stage.words.size() * 4;
            module_info.pCode = stage.words.data();
            VkShaderModule module = VK_NULL_HANDLE;
            const PipelineClock::time_point module_started =
                profile ? PipelineClock::now() : PipelineClock::time_point{};
            const VkResult module_result = vk.vkCreateShaderModule(dev, &module_info, nullptr, &module);
            if (profile) pipeline_profile_module(true, pipeline_elapsed_us(module_started));
            if (module_result != VK_SUCCESS) return E_FAIL;
            modules_.push_back(module);
            VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            info.stage = stage.bit;
            info.module = module;
            info.pName = "main";
            subgroup_size_for(*stage.entry, required_sizes[stage_infos.size()], info);
            stage_infos.push_back(info);
        }
        if (depth24_fragment && !ps_stage.entry) {  // no pixel shader: a built-in one writes the depth
            const std::vector<std::uint32_t> words = palmetto::depth24_depth_only_fragment(depth24_bias);
            VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module_info.codeSize = words.size() * 4;
            module_info.pCode = words.data();
            VkShaderModule module = VK_NULL_HANDLE;
            const PipelineClock::time_point module_started =
                profile ? PipelineClock::now() : PipelineClock::time_point{};
            const VkResult module_result =
                words.empty() ? VK_ERROR_INITIALIZATION_FAILED : vk.vkCreateShaderModule(dev, &module_info, nullptr, &module);
            if (profile) pipeline_profile_module(true, pipeline_elapsed_us(module_started));
            if (module_result != VK_SUCCESS) return E_FAIL;
            modules_.push_back(module);
            VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            info.module = module;
            info.pName = "main";
            stage_infos.push_back(info);
        }

        // Input layout -> vertex input, by semantic. A mesh pipeline has none.
        std::vector<VkVertexInputAttributeDescription> attributes;
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputBindingDivisorDescriptionEXT> divisors;
        std::uint32_t divisor_slots = 0;
        UINT slot_end[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
        for (UINT i = 0; !mesh && i < desc.InputLayout.NumElements; ++i) {
            const D3D12_INPUT_ELEMENT_DESC& element = desc.InputLayout.pInputElementDescs[i];
            if (element.InputSlot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) return E_INVALIDARG;
            VkFormat format;
            UINT bytes;
            if (!vertex_format(element.Format, format, bytes)) {
                log_once("vertex attribute format not supported yet:", element.Format);
                return E_INVALIDARG;
            }
            const UINT alignment = std::min(bytes, 4u);
            const UINT offset = element.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT
                                    ? (slot_end[element.InputSlot] + alignment - 1) / alignment * alignment
                                    : element.AlignedByteOffset;
            slot_end[element.InputSlot] = offset + bytes;
            const palmetto::SignatureElement* input = nullptr;
            for (const auto& e : stages[0].entry->input_signature)
                if (e.system_value == 0 && e.semantic_index == element.SemanticIndex &&
                    equal_semantic(e.semantic, element.SemanticName))
                    input = &e;
            if (!input) continue;  // the shader does not read this element
            attributes.push_back({input->register_index, element.InputSlot, format, offset});
            const bool instance = element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
            // Step rate 0: every instance reads element 0, which a stride of 0 gives (set at each draw).
            if (instance && element.InstanceDataStepRate == 0) graphics_.zero_step_slots |= 1u << element.InputSlot;
            if (instance && element.InstanceDataStepRate > 1) {
                if (!device_.info.vertex_attribute_divisor) {
                    log_once("instance step rates above 1 need vertexAttributeInstanceRateDivisor; rate",
                             int(element.InstanceDataStepRate));
                    return E_NOTIMPL;
                }
                if (!(divisor_slots & (1u << element.InputSlot))) {
                    divisor_slots |= 1u << element.InputSlot;
                    divisors.push_back({element.InputSlot, element.InstanceDataStepRate});
                }
            }
            if (!(graphics_.vertex_slots & (1u << element.InputSlot))) {
                graphics_.vertex_slots |= 1u << element.InputSlot;
                bindings.push_back({element.InputSlot, 0,  // stride: dynamic, from IASetVertexBuffers
                                    instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
            }
        }
        for (const auto& e : stages[0].entry->input_signature) {
            if (e.system_value) continue;
            const bool fed = std::any_of(attributes.begin(), attributes.end(),
                                         [&](const auto& a) { return a.location == e.register_index; });
            if (!fed) {  // D3D12 rejects an input layout that leaves a shader input unfed
                std::fprintf(stderr, "[rt] d3d12: the input layout does not feed %s%u (register %u)\n",
                             e.semantic.c_str(), e.semantic_index, e.register_index);
                return E_INVALIDARG;
            }
        }
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        switch (mesh ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE : desc.PrimitiveTopologyType) {  // mesh: not used
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: assembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE: assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        default:
            log_once("primitive topology type not supported:", desc.PrimitiveTopologyType);
            return E_INVALIDARG;
        }
        if (!mesh) {  // a mesh PSO has no index buffer: its strip cut is ignored
            if (desc.IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED &&
                desc.IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF &&
                desc.IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF) {
                log_once("index-buffer strip cut value not supported:", desc.IBStripCutValue);
                return E_INVALIDARG;
            }
            graphics_.strip_cut = desc.IBStripCutValue;
        }

        const D3D12_RASTERIZER_DESC& r = desc.RasterizerState;
        if (r.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF &&
            r.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON) return E_INVALIDARG;
        if (r.ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON) {
            if (device_.conservative_raster_tier == D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED) {
                log_once("conservative rasterization needs enabled VK_EXT_conservative_rasterization and Tier 1 precision", 1);
                return E_INVALIDARG;
            }
            if (r.FillMode != D3D12_FILL_MODE_SOLID ||
                desc.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) return E_INVALIDARG;
            // Target-independent rasterization is a separate pipeline path; do not silently ignore its count.
            if (r.ForcedSampleCount) return E_NOTIMPL;
        }
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = r.FillMode == D3D12_FILL_MODE_WIREFRAME ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        raster.cullMode = r.CullMode == D3D12_CULL_MODE_FRONT  ? VK_CULL_MODE_FRONT_BIT
                          : r.CullMode == D3D12_CULL_MODE_BACK ? VK_CULL_MODE_BACK_BIT
                                                               : VK_CULL_MODE_NONE;
        // With the negative-height viewport, framebuffer winding matches D3D12's screen-space winding.
        raster.frontFace = r.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        // The D24-as-D32 pixel stage applies the bias itself, in D24 units (depth24_fragment above): the
        // rasterizer's bias stays off and zero, so its clamp needs no depthBiasClamp feature either.
        raster.depthBiasEnable = !depth24_fragment && (r.DepthBias || r.SlopeScaledDepthBias != 0.0f);
        if (raster.depthBiasEnable) {
            raster.depthBiasConstantFactor = float(r.DepthBias);
            raster.depthBiasClamp = r.DepthBiasClamp;
            raster.depthBiasSlopeFactor = r.SlopeScaledDepthBias;
        }
        raster.lineWidth = 1.0f;
        // No depth clipping: fragments outside the viewport's depth range are kept and their depth clamped to it, as
        // D3D12 clamps output depth to MinDepth..MaxDepth anyway (needs depthClamp, checked below).
        raster.depthClampEnable = r.DepthClipEnable ? VK_FALSE : VK_TRUE;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VkSampleCountFlagBits(samples);
        const VkSampleMask sample_mask = desc.SampleMask;
        multisample.pSampleMask = &sample_mask;
        multisample.alphaToCoverageEnable = desc.BlendState.AlphaToCoverageEnable;

        const D3D12_DEPTH_STENCIL_DESC& ds = desc.DepthStencilState;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = ds.DepthEnable;
        depth.depthWriteEnable = ds.DepthEnable && ds.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
        depth.depthCompareOp = compare_op(ds.DepthFunc);
        depth.stencilTestEnable = ds.StencilEnable;
        depth.front = stencil_face(ds.FrontFace, ds.StencilReadMask, ds.StencilWriteMask);
        depth.back = stencil_face(ds.BackFace, ds.StencilReadMask, ds.StencilWriteMask);

        std::vector<VkPipelineColorBlendAttachmentState> blends(desc.NumRenderTargets);
        for (UINT i = 0; i < desc.NumRenderTargets; ++i) {
            const D3D12_RENDER_TARGET_BLEND_DESC& b =
                desc.BlendState.RenderTarget[desc.BlendState.IndependentBlendEnable ? i : 0];
            blends[i].blendEnable = b.BlendEnable;
            blends[i].srcColorBlendFactor = blend_factor(b.SrcBlend);
            blends[i].dstColorBlendFactor = blend_factor(b.DestBlend);
            blends[i].colorBlendOp = blend_op(b.BlendOp);
            blends[i].srcAlphaBlendFactor = blend_factor(b.SrcBlendAlpha);
            blends[i].dstAlphaBlendFactor = blend_factor(b.DestBlendAlpha);
            blends[i].alphaBlendOp = blend_op(b.BlendOpAlpha);
            blends[i].colorWriteMask = b.RenderTargetWriteMask & 0xf;  // R, G, B, A in both APIs
            // B8G8R8X8 has no alpha, so blending reads destination alpha as 1 there (WARP and AMD's D3D12 driver),
            // where its BGRA8 Vulkan image would read the X byte. For colour SRC_ALPHA_SAT is min(As, 1 - Ad), so 0;
            // for alpha it is 1 in both APIs already.
            if (alpha_unused(desc.RTVFormats[i])) {
                const auto dest_alpha_one = [](VkBlendFactor& factor, bool colour) {
                    if (factor == VK_BLEND_FACTOR_DST_ALPHA) factor = VK_BLEND_FACTOR_ONE;
                    else if (factor == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA) factor = VK_BLEND_FACTOR_ZERO;
                    else if (colour && factor == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE) factor = VK_BLEND_FACTOR_ZERO;
                };
                dest_alpha_one(blends[i].srcColorBlendFactor, true);
                dest_alpha_one(blends[i].dstColorBlendFactor, true);
                dest_alpha_one(blends[i].srcAlphaBlendFactor, false);
                dest_alpha_one(blends[i].dstAlphaBlendFactor, false);
            }
        }
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = std::uint32_t(blends.size());
        blend.pAttachments = blends.data();
        // A logic op replaces blending for every target in Vulkan (one per pipeline), and D3D12 takes it from
        // RenderTarget[0]. Vulkan applies it to integer and normalized formats and skips float ones.
        const D3D12_RENDER_TARGET_BLEND_DESC& first = desc.BlendState.RenderTarget[0];
        if (desc.NumRenderTargets && first.LogicOpEnable) {
            if (unsigned(first.LogicOp) > D3D12_LOGIC_OP_OR_INVERTED) return E_INVALIDARG;
            blend.logicOpEnable = VK_TRUE;
            blend.logicOp = logic_op(first.LogicOp);
        }

        // State that needs a Vulkan feature the device did not enable (VulkanDeviceInfo::features) is refused, rather
        // than handed to the driver against the spec.
        const VkPhysicalDeviceFeatures& features = device_.info.features;
        const auto refuse = [&](const char* what, int value) {
            log_once(what, value);
            return E_NOTIMPL;
        };
        if (raster.polygonMode != VK_POLYGON_MODE_FILL && !features.fillModeNonSolid)
            return refuse("wireframe fill needs fillModeNonSolid, which the device lacks; fill mode", r.FillMode);
        if (raster.depthBiasEnable && raster.depthBiasClamp != 0.0f && !features.depthBiasClamp)
            return refuse("a depth-bias clamp needs depthBiasClamp, which the device lacks; clamp x1000",
                          int(raster.depthBiasClamp * 1000));
        for (std::size_t i = 1; i < blends.size(); ++i)
            if (std::memcmp(&blends[i], &blends[0], sizeof(blends[0])) != 0 && !features.independentBlend)
                return refuse("per-render-target blend state needs independentBlend, which the device lacks; target",
                              int(i));
        const auto src1 = [](VkBlendFactor f) {
            return f == VK_BLEND_FACTOR_SRC1_COLOR || f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ||
                   f == VK_BLEND_FACTOR_SRC1_ALPHA || f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        };
        for (const auto& b : blends)
            if (b.blendEnable && !features.dualSrcBlend &&
                (src1(b.srcColorBlendFactor) || src1(b.dstColorBlendFactor) || src1(b.srcAlphaBlendFactor) ||
                 src1(b.dstAlphaBlendFactor)))
                return refuse("SRC1 blend factors need dualSrcBlend, which the device lacks; factor",
                              int(b.srcColorBlendFactor));
        if (blend.logicOpEnable && !features.logicOp)
            return refuse("logic ops need logicOp, which the device lacks; op", int(first.LogicOp));
        if (raster.depthClampEnable && !features.depthClamp)
            return refuse("DepthClipEnable = FALSE needs depthClamp, which the device lacks; flag", 0);

        graphics_.color_formats.assign(desc.NumRenderTargets, VK_FORMAT_UNDEFINED);
        for (UINT i = 0; i < desc.NumRenderTargets; ++i) {
            if (desc.RTVFormats[i] == DXGI_FORMAT_UNKNOWN) continue;
            graphics_.color_formats[i] = vulkan_format(desc.RTVFormats[i], false).format;
            if (graphics_.color_formats[i] == VK_FORMAT_UNDEFINED) {
                log_once("render-target format not supported yet:", desc.RTVFormats[i]);
                return E_INVALIDARG;
            }
        }
        if (desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
            Format f = vulkan_format(desc.DSVFormat, true);
            if (f.format == VK_FORMAT_D24_UNORM_S8_UINT && !device_.depth_usable(f.format))
                f.format = VK_FORMAT_D32_SFLOAT_S8_UINT;  // as CreateCommittedResource does
            graphics_.depth_format = f.format;
            graphics_.depth_aspects = f.aspects;
        }
        if (samples > 1) {
            const auto& dinfo = device_.info;
            const bool has_color = std::any_of(graphics_.color_formats.begin(), graphics_.color_formats.end(),
                                               [](VkFormat f) { return f != VK_FORMAT_UNDEFINED; });
            const bool has_depth = graphics_.depth_format != VK_FORMAT_UNDEFINED;
            if (const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                    dinfo.get_instance_proc ? dinfo.get_instance_proc(dinfo.instance, "vkGetPhysicalDeviceProperties")
                                            : nullptr)) {
                VkPhysicalDeviceProperties props{};
                get_props(dinfo.physical, &props);
                VkSampleCountFlags supported = ~VkSampleCountFlags(0);
                if (has_color) supported &= props.limits.framebufferColorSampleCounts;
                if (has_depth && (graphics_.depth_aspects & VK_IMAGE_ASPECT_DEPTH_BIT))
                    supported &= props.limits.framebufferDepthSampleCounts;
                if (has_depth && (graphics_.depth_aspects & VK_IMAGE_ASPECT_STENCIL_BIT))
                    supported &= props.limits.framebufferStencilSampleCounts;
                if (!has_color && !has_depth) supported &= props.limits.framebufferNoAttachmentsSampleCounts;
                if (!(supported & samples)) {
                    log_once("multisample count not supported by framebuffer limits; samples", int(samples));
                    return E_INVALIDARG;
                }
            }
            if ((has_color || has_depth) && !device_.image_format_properties) {
                log_once("cannot query per-format multisample support; samples", int(samples));
                return E_INVALIDARG;
            }
            for (const VkFormat fmt : graphics_.color_formats) {
                if (fmt == VK_FORMAT_UNDEFINED) continue;
                VkImageFormatProperties img_props{};
                if (device_.image_format_properties(dinfo.physical, fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &img_props) != VK_SUCCESS ||
                    !(img_props.sampleCounts & samples)) {
                    log_once("multisample count not supported by color attachment format; samples", int(samples));
                    return E_INVALIDARG;
                }
            }
            if (has_depth) {
                VkImageFormatProperties img_props{};
                if (device_.image_format_properties(dinfo.physical, graphics_.depth_format, VK_IMAGE_TYPE_2D,
                                                    VK_IMAGE_TILING_OPTIMAL,
                                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0,
                                                    &img_props) != VK_SUCCESS ||
                    !(img_props.sampleCounts & samples)) {
                    log_once("multisample count not supported by depth/stencil attachment format; samples",
                             int(samples));
                    return E_INVALIDARG;
                }
            }
        }
        // Every stage this path accepts reads the user SGPRs, so the range covers the whole stage table, not only the
        // stages present. A stage added to the table (geometry, mesh) is covered without touching this.
        // (The geometry bit only where the device has the stage: a range may not name a stage it cannot run.)
        for (const Stage& stage : stages)
            if (stage.bit != VK_SHADER_STAGE_GEOMETRY_BIT || device_.info.features.geometryShader)
                graphics_.push_stages |= stage.bit;
        const VkPushConstantRange push{graphics_.push_stages, 0, sizeof(palmetto::ShaderUserData)};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        VkDescriptorSetLayout set0_layout = VK_NULL_HANDLE;
        if (uses_bindless) {
            set0_layout = device_.bindless->layout();
            layout_info.setLayoutCount = 1;
            layout_info.pSetLayouts = &set0_layout;
            graphics_.bindless_set = device_.bindless->set();
        }
        const PipelineClock::time_point layout_started = profile ? PipelineClock::now() : PipelineClock::time_point{};
        const VkResult layout_result = vk.vkCreatePipelineLayout(dev, &layout_info, nullptr, &graphics_.layout);
        if (profile) pipeline_profile_layout(true, pipeline_elapsed_us(layout_started));
        if (layout_result != VK_SUCCESS) return E_FAIL;

        // Diagnostic (docs/vertex_path.md): capture the vertex path the shim actually bound - the input layout and the
        // resolved shader modules' corpus identity - for the replay's env-gated dump. Copies only; nothing on the draw
        // path reads it.
        {
            const auto layout_attributes = attributes;
            const auto layout_bindings = bindings;
            const auto layout_divisors = divisors;
            const std::string dir = library ? library->directory().string() : std::string();
            const std::string vs_name = stages[0].entry ? stages[0].entry->name : std::string();
            const std::string ps_name = stages[2].entry ? stages[2].entry->name : std::string();
            // The root signature parameter table (docs/vertex_path.md): which push-constant dwords each root parameter
            // owns. Read-only; the dump uses it to say where each dword came from.
            std::vector<palmetto::VertexPathInfo::RootParameter> layout_parameters;
            if (const RootArgumentLayout* root_layout = root_signature_layout(desc.pRootSignature)) {
                if (root_layout->graphics_ok) {
                    for (const RootArgumentLayout::Parameter& parameter : root_layout->parameters) {
                        palmetto::VertexPathInfo::RootParameter entry{};
                        entry.type = std::uint32_t(parameter.type);
                        entry.user_offset = parameter.user_offset;
                        entry.dwords = parameter.type == rt::rootsig::ParamType::Constants
                                           ? parameter.constant_count
                                           : (parameter.type == rt::rootsig::ParamType::Table ||
                                                      parameter.type == rt::rootsig::ParamType::Descriptor
                                                  ? 2u
                                                  : 0u);
                        entry.constant_count = parameter.constant_count;
                        layout_parameters.push_back(entry);
                    }
                }
            }
            graphics_.describe_vertex_path = [layout_attributes, layout_bindings, layout_divisors, dir, vs_name,
                                              ps_name, layout_parameters](palmetto::VertexPathInfo& out) {
                out = palmetto::VertexPathInfo{};
                const auto copy = [](char* destination, std::size_t capacity, const std::string& source) {
                    if (capacity) std::snprintf(destination, capacity, "%s", source.c_str());
                };
                copy(out.vs_name, sizeof(out.vs_name), vs_name);
                copy(out.ps_name, sizeof(out.ps_name), ps_name);
                if (!dir.empty()) {
                    copy(out.vs_path, sizeof(out.vs_path), dir + "\\" + vs_name + ".spv");
                    copy(out.ps_path, sizeof(out.ps_path), dir + "\\" + ps_name + ".spv");
                }
                for (const auto& a : layout_attributes) {
                    if (out.attribute_count >= std::size(out.attribute)) break;
                    auto& entry = out.attribute[out.attribute_count++];
                    entry.location = a.location;
                    entry.format = a.format;
                    entry.offset = a.offset;
                    entry.binding = a.binding;
                }
                for (const auto& b : layout_bindings) {
                    if (out.binding_count >= std::size(out.binding)) break;
                    auto& entry = out.binding[out.binding_count++];
                    entry.binding = b.binding;
                    entry.stride = b.stride;
                    entry.step_rate = b.inputRate;
                    entry.divisor = 0;
                }
                for (const auto& d : layout_divisors)
                    for (std::uint32_t i = 0; i < out.binding_count; ++i)
                        if (out.binding[i].binding == d.binding) out.binding[i].divisor = d.divisor;
                for (const auto& parameter : layout_parameters) {
                    if (out.parameter_count >= std::size(out.parameter)) break;
                    out.parameter[out.parameter_count++] = parameter;
                }
            };
        }
        // Kept for variants: a draw that leaves a slot unbound needs this pipeline with that format undefined.
        graphics_.mesh = mesh;
        recipe_.mesh = mesh;
        recipe_.stages = std::move(stage_infos);
        recipe_.attributes = std::move(attributes);
        recipe_.bindings = std::move(bindings);
        recipe_.divisors = std::move(divisors);
        recipe_.assembly = assembly;
        recipe_.raster = raster;
        recipe_.conservative = r.ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
        recipe_.multisample = multisample;
        graphics_.samples = samples;
        recipe_.sample_mask = sample_mask;
        recipe_.depth = depth;
        recipe_.blends = std::move(blends);
        recipe_.blend = blend;
        // With the recipe cache on, the entry a later description adopts must own a built pipeline, so that arm keeps
        // its eager build (it is the opt-in sharing slice, not the bounded default). Otherwise the build is deferred
        // to the first bind (pipeline_budget.h, docs/memory_growth.md).
        if (device_.defer_graphics_build() && !recipe_cache) {
            lazy_graphics_ = true;  // built at the first bind (pipeline_budget.h, docs/memory_growth.md)
        } else {
            graphics_.pipeline = build(0);
            if (!graphics_.pipeline) return E_FAIL;
        }
        if (profile) {
            std::string label;
            if (pipeline_elapsed_us(profile_started) >
                pipeline_profile().slow_floor_us.load(std::memory_order_relaxed))
                label = pipeline_profile_label(stages[0].entry ? stages[0].entry->name.c_str() : nullptr,
                                               stages[2].entry ? stages[2].entry->name.c_str() : nullptr, desc, mesh);
            pipeline_profile_created(true, profile_started, label);
        }
        graphics_.variant = [this](std::uint32_t unbound) { return variant(unbound); };
        // Diagnostic (docs/frame_capture.md): the frame dump's PS-only control, the same recipe with another pixel
        // shader. Only read by the env-gated diagnostic; nothing on the normal draw path calls it.
        graphics_.with_ps = [this](VkShaderModule ps, bool cull_none) { return variant_with_ps(ps, cull_none); };
        // Diagnostic (docs/vertex_buffer_va.md section 22): the same recipe with only its cull pair replaced, keyed by
        // (cull mode, front face) in its own cache. Only read by the env-gated content-window rule.
        graphics_.with_cull = [this](std::uint32_t cull_mode, bool front_ccw) {
            return variant_with_cull(cull_mode, front_ccw);
        };

        if (code_stage) {
            VkDeviceSize offset = 0;
            if (!device_.arena_allocate(code_stage->fssm.size(), 256, offset, DeviceState::arena_shader)) return E_OUTOFMEMORY;
            std::memcpy(device_.arena.mapped + offset, code_stage->fssm.data(), code_stage->fssm.size());
            code_bytes_ = code_stage->fssm.size();
            code_base_ = offset;
            graphics_.code_base = offset;
        }
        // Which user-data dwords hold a descriptor table's 64-bit VA: the graphics layout packs every table as two
        // dwords (rt::rootsig::pack). The title's console descriptor-heap VAs and the VAs of its placed buffers overlap
        // in its address space, so the replay rebases a value that is in both by what the root parameter says it is
        // (docs/page_address_space.md). The console-layout slot (docs/root_arguments.md) needs no marking: it carries
        // the already arena-relative value the replay would produce, so the replay's 64-bit window scan must not treat
        // it as a title VA at all.
        if (const RootArgumentLayout* root = root_signature_layout(desc.pRootSignature)) {
            for (const auto& parameter : root->parameters) {
                if (parameter.type != rt::rootsig::ParamType::Table || parameter.user_offset + 1 >= 32) continue;
                graphics_.table_windows |= std::uint32_t(1) << parameter.user_offset;
                graphics_.table_windows |= std::uint32_t(1) << (parameter.user_offset + 1);
            }
            // The same parameters at their console slots (docs/root_arguments.md sections 4-5, docs/vertex_buffer_va.md
            // section 18): the replay converts a draw's recorded block into this layout when
            // PALMETTO_GRAPHICS_CONSOLE_ROOT is set, so a translated stage finds its table at s8 (one dword) and its
            // constants after it instead of only the PC table pair at s0/s1.
            graphics_.console_ok = root->console_ok;
            for (const auto& parameter : root->parameters) {
                if (graphics_.console_parameter_count >= std::size(graphics_.console_parameter)) break;
                auto& entry = graphics_.console_parameter[graphics_.console_parameter_count++];
                entry.type = std::uint32_t(parameter.type);
                entry.pc_offset = parameter.user_offset;
                entry.console_offset = parameter.console_offset;
                entry.constant_count = parameter.constant_count;
            }
        }
        // Everything that can fail is done. With the recipe cache on, the created objects move into a shared entry the
        // next identical description adopts; this PSO keeps its own recipe, code-base range and diagnostics.
        if (recipe_cache) {
            auto entry = std::make_shared<SharedGraphicsPipeline>();
            entry->vk = &device_.context->vk;
            entry->device = device_.info.device;
            entry->pipeline = graphics_.pipeline;
            entry->layout = graphics_.layout;
            entry->modules = std::move(modules_);
            entry->recipe = recipe_;
            entry->subgroup_requests = subgroup_requests_;
            entry->push_stages = graphics_.push_stages;
            entry->bindless_set = graphics_.bindless_set;
            entry->color_formats = graphics_.color_formats;
            entry->depth_format = graphics_.depth_format;
            entry->depth_aspects = graphics_.depth_aspects;
            entry->vertex_slots = graphics_.vertex_slots;
            entry->zero_step_slots = graphics_.zero_step_slots;
            entry->strip_cut = graphics_.strip_cut;
            entry->table_windows = graphics_.table_windows;
            entry->samples = graphics_.samples;
            entry->mesh = graphics_.mesh;
            entry->has_pixel_stage = graphics_.has_pixel_stage;
            entry->ps_images = graphics_.ps_images;
            entry->vs_loads = graphics_.vs_loads;
            entry->console_ok = graphics_.console_ok;
            entry->console_parameter_count = graphics_.console_parameter_count;
            std::copy(std::begin(graphics_.console_parameter), std::end(graphics_.console_parameter),
                      std::begin(entry->console_parameter));
            if (graphics_.describe_vertex_path) graphics_.describe_vertex_path(entry->vertex_path);
            if (code_stage) entry->code_bytes = code_stage->fssm;
            shared_graphics_ = device_.add_graphics_recipe(cache_key_, std::move(entry));
        }
        device_.context->add_graphics_pipeline(id(), graphics_);
        registered_ = true;
        is_graphics_ = true;
        if (!lazy_graphics_) device_.note_pipeline_built(this);  // an eager build is live from creation
        return S_OK;
    }

    HRESULT create_compute(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc) {
        auto& vk = device_.context->vk;
        const VkDevice dev = device_.info.device;
        const bool profile = pipeline_profile_enabled();
        if (profile) pipeline_profile_begin(false);
        const PipelineClock::time_point profile_started = profile ? PipelineClock::now() : PipelineClock::time_point{};
        // docs/deferred_compile.md: who enters pipeline creation, counted per caller thread
        // (PALMETTO_PIPELINE_CALL_STATS; a null stats pointer when the switch is off).
        const palmetto::PipelineCallStats::Scope call_scope(device_.pipeline_stats(), false,
                                                            palmetto::PipelineCallStats::Kind::creation);
        // The per-process recipe cache (PALMETTO_PIPELINE_RECIPE_CACHE): an earlier identical compute description (same
        // content key) already built the pipeline, layout and module, so adopt them. Off by default.
        const bool recipe_cache = pipeline_recipe_cache_enabled() && recipe_cache_ready();
        if (recipe_cache) {
            if (const std::shared_ptr<SharedComputePipeline> shared = device_.find_compute_recipe(cache_key_)) {
                if (profile) pipeline_profile_recipe_hit(false);
                const HRESULT adopted = adopt_compute_recipe(shared);
                if (SUCCEEDED(adopted) && profile) pipeline_profile_created(false, profile_started, std::string());
                return adopted;
            }
        }
        if (!desc.CS.pShaderBytecode || !desc.CS.BytecodeLength) return E_INVALIDARG;
        if (desc.NodeMask & ~1u) return E_INVALIDARG;
        if (static_cast<UINT>(desc.Flags) & ~kKnownPipelineStateFlags) return E_INVALIDARG;
        if (!device_.arena.mapped) {
            log_once("pipelines need the GPU address arena (bufferDeviceAddress); stage", 0);
            return E_NOTIMPL;
        }
        if (!push_constants_fit()) return E_NOTIMPL;
        // The game's FSSM program, or the FidelityFX DLL's FSPX kind-6 program: in a DXBC container's ACCC part at
        // slot 62, bare from a slot-67 description (docs/fspx.md, "Run-time lookup").
        const palmetto::ComputeProgram program = palmetto::compute_program(
            {static_cast<const std::uint8_t*>(desc.CS.pShaderBytecode), desc.CS.BytecodeLength});
        if (program.kind == palmetto::ComputeProgram::Kind::none) {
            log_once("compute shader bytecode holds no FSSM or FSPX program; bytes", int(desc.CS.BytecodeLength));
            return E_INVALIDARG;
        }
        const palmetto::ShaderLibrary* library = device_.library();
        static const palmetto::ShaderEntry* last_compute_entry = nullptr;
        if (!library) return E_FAIL;
        // A binding-free entry wins over one with bindings for the same program (pick_entry).
        // An FSPX program from a container is matched on its kind-0 state too; a bare kind-6 program whose
        // translations differ is ambiguous without that state and is refused (program_candidates).
        bool ambiguous = false;
        const palmetto::ShaderEntry* entry = pick_entry(library->program_candidates("cs", program, &ambiguous),
                                                       [](const palmetto::ShaderEntry&) { return true; });
        bool used_standin = false;  // the stand-in uses the last translated program, not this description: never shared
        if (!entry && last_compute_entry && standin_pipelines()) {
            // A stand-in, as #6915 uses for a placed buffer: the title gets a pipeline object it can hold, built from
            // the last compute program that did translate, so a missing translation stops the work rather than the
            // boot. Enabled only by PALMETTO_STANDIN_PIPELINES, and the log says so every time.
            static std::atomic<int> standin_logged{0};
            if (standin_logged.fetch_add(1, std::memory_order_relaxed) < 3)
                std::fprintf(stderr,
                             "[rt] d3d12: STAND-IN compute pipeline: the program has no translation, so the module of "
                             "%s is used; its dispatch will not do that program's work\n",
                             last_compute_entry->name.c_str());
            entry = last_compute_entry;
            used_standin = true;
        }
        if (!entry) {
            const bool fspx = program.kind == palmetto::ComputeProgram::Kind::fspx;
            // The title's own precompiled pipelines are deserialized from its pipeline library. Eleven of the 42 it
            // asks for carry bytes that do not match the extracted library records, so the exact program it passes is
            // dumped here for offline translation. PALMETTO_DUMP_PIPELINES=<dir> writes <sha256>.code and .state into
            // that dir - game data, so it must be a work folder outside any repository.
            if (const char* dir = std::getenv("PALMETTO_DUMP_PIPELINES"); dir && *dir) {
                const std::string stem = palmetto::sha256_hex(program.bytes);
                const std::string base = std::string(dir) + "\\" + stem;
                if (FILE* f = std::fopen((base + ".code").c_str(), "wb")) {
                    std::fwrite(program.bytes.data(), 1, program.bytes.size(), f);
                    std::fclose(f);
                }
                if (!program.state.empty()) {
                    if (FILE* f = std::fopen((base + ".state").c_str(), "wb")) {
                        std::fwrite(program.state.data(), 1, program.state.size(), f);
                        std::fclose(f);
                    }
                }
                std::fprintf(stderr, "[rt] d3d12: dumped an untranslated pipeline program to %s.code\n", stem.c_str());
            }
            if (ambiguous)
                std::fprintf(stderr, "[rt] d3d12: FSPX kind-6 program sha256 %s is ambiguous without its kind-0 "
                                     "state: its translations differ\n", palmetto::sha256_hex(program.bytes).c_str());
            else
                std::fprintf(stderr, "[rt] d3d12: no translated compute shader for %s sha256 %s\n",
                             fspx ? (program.state.empty() ? "FSPX kind-6" : "FSPX kind-6 and kind-0") : "FSSM",
                             palmetto::sha256_hex(program.bytes).c_str());
            return E_INVALIDARG;
        }
        last_compute_entry = entry;
        // Set 0 (bindless images, samplers, the descriptor hash table): the device keeps it, and a pipeline whose
        // shader declares bindings has it in its layout and has it bound at every dispatch.
        if (entry->binding_count && !device_.bindless) {
            std::fprintf(stderr, "[rt] d3d12: %s uses set-0 bindings; the device has no bindless set 0\n",
                         entry->name.c_str());
            return E_NOTIMPL;
        }
        if (entry->binding_count) register_literal_samplers(device_, *entry);
        // SHADER-309: a module over the device's maxComputeSharedMemorySize is invalid usage that spirv-val accepts
        // and RADV dies on (#1721); some drivers report 32768. Refuse it here instead of creating the pipeline.
        if (entry->lds_bytes) {
            const auto& dinfo = device_.info;
            if (const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                    dinfo.get_instance_proc ? dinfo.get_instance_proc(dinfo.instance, "vkGetPhysicalDeviceProperties")
                                            : nullptr)) {
                VkPhysicalDeviceProperties props{};
                get_props(dinfo.physical, &props);
                if (entry->lds_bytes > props.limits.maxComputeSharedMemorySize) {
                    std::fprintf(stderr,
                                 "[rt] d3d12: %s declares %u bytes of workgroup memory, over the device's "
                                 "maxComputeSharedMemorySize %u\n",
                                 entry->name.c_str(), unsigned(entry->lds_bytes),
                                 unsigned(props.limits.maxComputeSharedMemorySize));
                    return E_NOTIMPL;
                }
            }
        }
        const std::vector<std::uint32_t> words = library->spirv(*entry);
        if (words.empty()) {
            std::fprintf(stderr, "[rt] d3d12: %s.spv is missing or not SPIR-V\n", entry->name.c_str());
            return E_FAIL;
        }

        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = words.size() * 4;
        module_info.pCode = words.data();
        const PipelineClock::time_point module_started = profile ? PipelineClock::now() : PipelineClock::time_point{};
        const VkResult module_result = vk.vkCreateShaderModule(dev, &module_info, nullptr, &module_);
        if (profile) pipeline_profile_module(false, pipeline_elapsed_us(module_started));
        if (module_result != VK_SUCCESS) return E_FAIL;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(palmetto::ShaderUserData)};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        VkDescriptorSetLayout set0_layout = VK_NULL_HANDLE;
        if (entry->binding_count) {
            set0_layout = device_.bindless->layout();
            layout_info.setLayoutCount = 1;
            layout_info.pSetLayouts = &set0_layout;
            pipeline_.bindless_set = device_.bindless->set();
        }
        const PipelineClock::time_point layout_started = profile ? PipelineClock::now() : PipelineClock::time_point{};
        const VkResult layout_result = vk.vkCreatePipelineLayout(dev, &layout_info, nullptr, &pipeline_.layout);
        if (profile) pipeline_profile_layout(false, pipeline_elapsed_us(layout_started));
        if (layout_result != VK_SUCCESS) return E_FAIL;
        // The compute stage the driver build needs, with the device's subgroup answer decided here on the creating
        // thread (the decision is cheap and the replay reads `subgroup_requests_`), so the compile worker has no shared
        // state to touch but the driver call itself.
        compute_stage_ = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        compute_stage_.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute_stage_.module = module_;
        compute_stage_.pName = "main";
        subgroup_size_for(*entry, compute_required_size_, compute_stage_);
        // The code base: shaders read constant tables embedded in their own program through s_getpc_b64. The
        // translator's code offsets are into the bytes the key hashes (the FSSM blob, or the kind-6 program). It is a
        // host copy, independent of the driver compile, so it is done before the deferred branch can return.
        const std::vector<std::uint8_t>& code = program.bytes;
        VkDeviceSize offset = 0;
        if (!device_.arena_allocate(code.size(), 256, offset, DeviceState::arena_shader)) return E_OUTOFMEMORY;
        std::memcpy(device_.arena.mapped + offset, code.data(), code.size());
        code_bytes_ = code.size();
        code_base_ = offset;
        pipeline_.code_base = offset;

        // docs/deferred_compile.md: register the pipeline state and let a compile worker make the driver call, so a
        // title task-pool thread never blocks in the shader compiler. A queue at its bound falls back to the
        // synchronous build below, exactly as the default path does.
        if (device_.pipeline_defer_compile_enabled()) {
            pipeline_.pipeline = VK_NULL_HANDLE;
            device_.context->add_pipeline(id(), pipeline_);
            registered_ = true;
            compute_task_ = device_.defer_compute_build(this, [this] { return SUCCEEDED(build_compute_driver()); });
            if (compute_task_) {
                if (profile) pipeline_profile_created(false, profile_started, std::string("deferred"));
                return S_OK;
            }
        }
        if (const HRESULT built = build_compute_driver(); FAILED(built)) return built;
        if (profile) {
            std::string label;
            if (pipeline_elapsed_us(profile_started) >
                pipeline_profile().slow_floor_us.load(std::memory_order_relaxed))
                label = pipeline_profile_label_compute(entry->name.c_str(), entry->binding_count);
            pipeline_profile_created(false, profile_started, label);
        }
        // Everything that can fail is done. With the recipe cache on, the created objects move into a shared entry the
        // next identical description adopts; this PSO keeps its own code-base range.
        if (recipe_cache && !used_standin) {
            auto shared = std::make_shared<SharedComputePipeline>();
            shared->vk = &device_.context->vk;
            shared->device = device_.info.device;
            shared->pipeline = pipeline_.pipeline;
            shared->layout = pipeline_.layout;
            shared->module = module_;
            shared->bindless_set = pipeline_.bindless_set;
            shared->program_bytes = code;
            module_ = VK_NULL_HANDLE;  // ownership moved into the entry
            shared_compute_ = device_.add_compute_recipe(cache_key_, std::move(shared));
        }
        if (!registered_) {
            device_.context->add_pipeline(id(), pipeline_);
            registered_ = true;
        } else {
            // The deferred fallback registered the entry before its synchronous build; publish the real handle.
            device_.context->set_pipeline_handle(id(), pipeline_.pipeline, false);
        }
        return S_OK;
    }

private:
    // The driver half of create_compute (docs/deferred_compile.md): the one call that reaches nvgpucomp64, made on the
    // creating thread by the default path or on a compile worker by the deferred path. The module, layout, stage and
    // code base are already in place; this sets the handle on this state and publishes it to the replay map.
    HRESULT build_compute_driver() {
        auto& vk = device_.context->vk;
        const VkDevice dev = device_.info.device;
        const bool profile = pipeline_profile_enabled();
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = compute_stage_;
        info.layout = pipeline_.layout;
        const bool feedback = profile && device_.pipeline_creation_feedback;
        VkPipelineCreationFeedback pipeline_feedback{};
        VkPipelineCreationFeedbackCreateInfo feedback_create{
            VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, nullptr, &pipeline_feedback, 0, nullptr};
        if (feedback) info.pNext = &feedback_create;
        const PipelineClock::time_point driver_started = profile ? PipelineClock::now() : PipelineClock::time_point{};
        VkPipeline handle = VK_NULL_HANDLE;
        VkResult created = VK_SUCCESS;
        {
            // The measurement the slice starts from: who is inside the driver's compiler and for how long
            // (PALMETTO_PIPELINE_CALL_STATS); a null stats pointer when the switch is off.
            const palmetto::PipelineCallStats::Scope driver_scope(device_.pipeline_stats(), false,
                                                                  palmetto::PipelineCallStats::Kind::driver);
            created = vk.vkCreateComputePipelines(dev, device_.pipeline_cache(), 1, &info, nullptr, &handle);
        }
        if (profile) {
            int cache_state = -1;
            if (created == VK_SUCCESS && (pipeline_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT))
                cache_state = (pipeline_feedback.flags &
                               VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ? 1 : 0;
            pipeline_profile_driver(false, pipeline_elapsed_us(driver_started), cache_state);
        }
        maybe_save_pipeline_caches();
        if (created != VK_SUCCESS) {
            log_once("a compute pipeline could not be built by the driver; VkResult", int(created));
            return E_FAIL;
        }
        {
            std::lock_guard lock(deferred_handle_mutex_);  // orders with the destructor's handle read
            pipeline_.pipeline = handle;
        }
        if (registered_) device_.context->set_pipeline_handle(id(), handle, false);
        return S_OK;
    }
    // The graphics pipeline with the formats of the `unbound` slots (palmetto::GraphicsPipeline::variant) undefined.
    // `substitute_ps` (diagnostic, docs/frame_capture.md): the same pipeline with its fragment stage replaced by that
    // module, which is what the frame dump's PS-only control draws the title's own vertices through. Null keeps the
    // recipe's stages, and the create info then points at the recipe's own list, so the default build is unchanged.
    // `cull_none` (diagnostic, substituted variants only): turn back-face culling off, to tell a culled winding from
    // degenerate geometry.
    // Diagnostic (docs/vertex_buffer_va.md section 22): a cull-only variant replaces only the recipe's rasterizer cull
    // pair; `set` keeps every other build path byte-identical (a default-constructed override changes nothing).
    struct CullOverride {
        bool set = false;
        std::uint32_t mode = 0;  // VkCullModeFlagBits
        bool front_ccw = false;
    };
    VkPipeline build(std::uint32_t unbound, VkShaderModule substitute_ps = VK_NULL_HANDLE,
                     bool cull_none = false, CullOverride cull_override = {}) const {
        // The stride state is last: a pipeline without vertex bindings (a full-screen pass that builds its vertices
        // from SV_VertexID) leaves it out, since no vkCmdBindVertexBuffers2 would ever set it
        // (VUID-vkCmdDraw-pStrides-04913).
        static constexpr VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
            VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
        const GraphicsRecipe& r = recipe_;
        // Diagnostic only: the recipe's stages with the fragment module swapped and its required-subgroup-size chain
        // dropped (the substituted shader declares none). Empty when the recipe has no fragment stage.
        std::vector<VkPipelineShaderStageCreateInfo> substituted_stages;
        const VkPipelineShaderStageCreateInfo* stage_infos = r.stages.data();
        std::uint32_t stage_count = std::uint32_t(r.stages.size());
        if (substitute_ps) {
            substituted_stages = r.stages;
            bool replaced = false;
            for (auto& stage : substituted_stages) {
                if (stage.stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
                stage.module = substitute_ps;
                stage.pNext = nullptr;
                replaced = true;
            }
            if (!replaced) return VK_NULL_HANDLE;
            stage_infos = substituted_stages.data();
            stage_count = std::uint32_t(substituted_stages.size());
        }
        std::vector<VkFormat> colors = graphics_.color_formats;
        for (std::size_t slot = 0; slot < colors.size(); ++slot)
            if (unbound & (1u << slot)) colors[slot] = VK_FORMAT_UNDEFINED;
        const bool depth_bound = !(unbound & palmetto::GraphicsPipeline::unbound_depth);
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = std::uint32_t(colors.size());
        rendering.pColorAttachmentFormats = colors.data();
        if (depth_bound && (graphics_.depth_aspects & VK_IMAGE_ASPECT_DEPTH_BIT))
            rendering.depthAttachmentFormat = graphics_.depth_format;
        if (depth_bound && (graphics_.depth_aspects & VK_IMAGE_ASPECT_STENCIL_BIT))
            rendering.stencilAttachmentFormat = graphics_.depth_format;

        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineVertexInputDivisorStateCreateInfoEXT divisor_state{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT};
        if (!r.divisors.empty()) {
            divisor_state.vertexBindingDivisorCount = std::uint32_t(r.divisors.size());
            divisor_state.pVertexBindingDivisors = r.divisors.data();
            vertex_input.pNext = &divisor_state;
        }
        vertex_input.vertexBindingDescriptionCount = std::uint32_t(r.bindings.size());
        vertex_input.pVertexBindingDescriptions = r.bindings.data();
        vertex_input.vertexAttributeDescriptionCount = std::uint32_t(r.attributes.size());
        vertex_input.pVertexAttributeDescriptions = r.attributes.data();
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineMultisampleStateCreateInfo multisample = r.multisample;
        VkPipelineRasterizationStateCreateInfo raster = r.raster;
        if (cull_none) raster.cullMode = VK_CULL_MODE_NONE;  // diagnostic only: the substituted variant's cull axis
        // Diagnostic only (docs/vertex_buffer_va.md section 22): the cull-only variant's measured pair. Only the raster
        // state is copied above, so the recipe and the stages stay the title's own.
        if (cull_override.set) {
            raster.cullMode = static_cast<VkCullModeFlagBits>(cull_override.mode);
            raster.frontFace = cull_override.front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        }
        VkPipelineRasterizationConservativeStateCreateInfoEXT conservative{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT};
        if (r.conservative) {
            conservative.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
            conservative.extraPrimitiveOverestimationSize = 0.0f;
            conservative.pNext = raster.pNext;
            raster.pNext = &conservative;
        }
        multisample.pSampleMask = &r.sample_mask;
        VkPipelineColorBlendStateCreateInfo blend = r.blend;
        blend.attachmentCount = std::uint32_t(r.blends.size());
        blend.pAttachments = r.blends.data();
        // A mesh pipeline takes neither the topology nor the strides as dynamic state
        // (VUID-VkGraphicsPipelineCreateInfo-pDynamicStates-07065 and -07066).
        static constexpr VkDynamicState mesh_dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                                                 VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                                                 VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        if (r.mesh) {
            dynamic.dynamicStateCount = std::uint32_t(std::size(mesh_dynamic_states));
            dynamic.pDynamicStates = mesh_dynamic_states;
        } else {
            dynamic.dynamicStateCount = std::uint32_t(std::size(dynamic_states)) - (r.bindings.empty() ? 1 : 0);
            dynamic.pDynamicStates = dynamic_states;
        }

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        info.stageCount = stage_count;
        info.pStages = stage_infos;
        info.pVertexInputState = r.mesh ? nullptr : &vertex_input;  // ignored with a mesh stage; null for clarity
        info.pInputAssemblyState = r.mesh ? nullptr : &r.assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &r.depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = graphics_.layout;
        // The profile chains VkPipelineCreationFeedback (core in Vulkan 1.3) so a warm run's cache hits are measured,
        // not assumed. The chain is output-only and built only with the switch on.
        const bool profile = pipeline_profile_enabled();
        const bool feedback = profile && device_.pipeline_creation_feedback;
        VkPipelineCreationFeedback pipeline_feedback{};
        VkPipelineCreationFeedback stage_feedback[3]{};
        VkPipelineCreationFeedbackCreateInfo feedback_create{
            VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, nullptr, &pipeline_feedback, 0, nullptr};
        if (feedback) {
            feedback_create.pNext = info.pNext;
            feedback_create.pipelineStageCreationFeedbackCount = (std::min)(stage_count, 3u);
            feedback_create.pPipelineStageCreationFeedbacks = stage_feedback;
            info.pNext = &feedback_create;
        }
        VkPipeline pipeline = VK_NULL_HANDLE;
        const PipelineClock::time_point driver_started = profile ? PipelineClock::now() : PipelineClock::time_point{};
        const VkResult created = [&] {
            // docs/deferred_compile.md: the driver call's wall time per caller thread (PALMETTO_PIPELINE_CALL_STATS).
            // For a graphics pipeline this runs on the recording thread that binds it (the lazy build).
            const palmetto::PipelineCallStats::Scope driver_scope(device_.pipeline_stats(), true,
                                                                  palmetto::PipelineCallStats::Kind::driver);
            return device_.context->vk.vkCreateGraphicsPipelines(device_.info.device, device_.pipeline_cache(), 1, &info,
                                                                 nullptr, &pipeline);
        }();
        if (profile) {
            int cache_state = -1;
            if (created == VK_SUCCESS && (pipeline_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT))
                cache_state = (pipeline_feedback.flags &
                               VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ? 1 : 0;
            pipeline_profile_driver(true, pipeline_elapsed_us(driver_started), cache_state);
        }
        maybe_save_pipeline_caches();
        if (created != VK_SUCCESS) return VK_NULL_HANDLE;
        return pipeline;
    }

    // Built on first use and kept until the PSO is released (replay may call this from any queue).
    VkPipeline variant(std::uint32_t unbound) {
        std::lock_guard lock(variants_mutex_);
        if (const auto it = variants_.find(unbound); it != variants_.end()) return it->second;
        const VkPipeline pipeline = build(unbound);
        if (!pipeline) {
            log_once("a pipeline variant for unbound attachment slots could not be built; slots", int(unbound));
            return VK_NULL_HANDLE;
        }
        variants_.emplace(unbound, pipeline);
        return pipeline;
    }

    // Diagnostic (docs/frame_capture.md): the same pipeline with `ps` as its pixel shader, built once per cull mode and
    // kept until the PSO is released, like variant(). The frame dump's PS-only control is its only caller.
    VkPipeline variant_with_ps(VkShaderModule ps, bool cull_none) {
        std::lock_guard lock(variants_mutex_);
        const std::uint32_t key = cull_none ? 1u : 0u;
        if (const auto it = ps_variants_.find(key); it != ps_variants_.end()) return it->second;
        if (ps_variant_failed_) return VK_NULL_HANDLE;
        const VkPipeline pipeline = build(0, ps, cull_none);
        if (!pipeline) {
            ps_variant_failed_ = true;
            log_once("the PS-only control pipeline could not be built from this recipe (no fragment stage?)", 0);
            return VK_NULL_HANDLE;
        }
        ps_variants_.emplace(key, pipeline);
        return pipeline;
    }

    // Diagnostic (docs/vertex_buffer_va.md section 22): the title's own pipeline with only its rasterizer cull pair
    // replaced, built once per (cull mode, front face) and kept until the PSO is released, like variant(). Its own cache,
    // separate from the PS-substitution one above, so a real-pipeline cull variant can never collide with the
    // substituted control's module. A failed build is cached as a null entry, so a content window does not retry it on
    // every draw.
    VkPipeline variant_with_cull(std::uint32_t cull_mode, bool front_ccw) {
        std::lock_guard lock(variants_mutex_);
        const std::uint32_t key = palmetto::frame_dump::cull_variant_key(cull_mode, front_ccw);
        if (const auto it = cull_variants_.find(key); it != cull_variants_.end()) return it->second;
        CullOverride override_state;
        override_state.set = true;
        override_state.mode = cull_mode;
        override_state.front_ccw = front_ccw;
        const VkPipeline pipeline = build(0, VK_NULL_HANDLE, false, override_state);
        if (!pipeline)
            log_once("the cull-only content-window pipeline variant could not be built from this recipe", 0);
        cull_variants_.emplace(key, pipeline);
        return pipeline;
    }

    DeviceState& device_;
    palmetto::ComputePipeline pipeline_;
    palmetto::GraphicsPipeline graphics_;
    GraphicsRecipe recipe_;
    std::mutex variants_mutex_;
    std::map<std::uint32_t, VkPipeline> variants_;
    std::map<std::uint32_t, VkPipeline> ps_variants_;  // docs/frame_capture.md, the PS-only control by cull mode
    // docs/vertex_buffer_va.md section 22: the cull-only content-window variant by (cull mode, front face); a null value
    // is a cached failed build.
    std::map<std::uint32_t, VkPipeline> cull_variants_;
    bool ps_variant_failed_ = false;
    VkShaderModule module_ = VK_NULL_HANDLE;
    std::vector<VkShaderModule> modules_;
    VkDeviceSize code_base_ = 0;
    VkDeviceSize code_bytes_ = 0;
    bool registered_ = false;
    bool is_graphics_ = false;
    std::vector<palmetto::SubgroupRequest> subgroup_requests_;  // one per stage, in creation order
    palmetto::cached_pso::Key cache_key_;  // the CachedPSO identity (GetCachedBlob/check) and the recipe-cache key
    // The per-process recipe cache (PALMETTO_PIPELINE_RECIPE_CACHE): when this PSO adopted an earlier identical
    // description's shared entry, that entry owns the Vulkan objects and this pointer keeps it alive. Empty otherwise:
    // the PSO owns and destroys its own objects as before.
    std::shared_ptr<SharedGraphicsPipeline> shared_graphics_;
    std::shared_ptr<SharedComputePipeline> shared_compute_;
    bool cache_key_deterministic_ = false;  // false: a foreign root signature is hashed by address, so never share
    // The live-pipeline budget (pipeline_budget.h): this PSO's VkPipeline is built at its first bind (creation past the
    // cap, or a rebuild after an eviction). The build mutex serializes those builds; the budget lock is only taken
    // after it is released, so the two paths cannot deadlock.
    bool lazy_graphics_ = false;
    std::mutex build_mutex_;
    // Off-thread compute compilation (docs/deferred_compile.md): the stage description the worker needs, the queued
    // task, and the mutex that orders the worker's handle write against this PSO's destruction.
    VkPipelineShaderStageCreateInfo compute_stage_{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo compute_required_size_{};
    std::shared_ptr<palmetto::CompileQueue::Task> compute_task_;
    std::mutex deferred_handle_mutex_;

public:
    // The live-pipeline budget's hooks (pipeline_budget.h); the recording thread and the budget call them.
    // Build the graphics pipeline now if it has none. Called from graphics_pipeline_id() on the thread recording the
    // bind; on success the replay's copy of the entry gets the fresh handle.
    bool ensure_graphics_built() {
        if (!is_graphics_) return false;
        if (!lazy_graphics_) return graphics_.pipeline != VK_NULL_HANDLE;
        {
            std::lock_guard lock(build_mutex_);
            if (graphics_.pipeline != VK_NULL_HANDLE) return true;
            const VkPipeline built = build(0);
            if (!built) return false;
            graphics_.pipeline = built;
        }
        // Outside the build mutex: note_pipeline_built() takes the budget lock, and the eviction path takes this PSO's
        // build mutex while it holds that lock (the order is fixed: build mutex, released, then budget lock).
        device_.context->set_pipeline_handle(id(), graphics_.pipeline, true);
        return device_.note_pipeline_built(this);
    }

    // With DeviceState::pipeline_budget_mutex held: the budget chose this pipeline to release. The PSO forgets the
    // handle so its next bind rebuilds it; the replay's copy keeps the old handle until that rebuild (a recording that
    // still names it is inside the budget's grace and the deferred queue waits for the submissions already on the GPU).
    void evict_graphics_pipeline() {
        std::lock_guard lock(build_mutex_);
        const VkPipeline handle = graphics_.pipeline;
        if (handle == VK_NULL_HANDLE) return;
        graphics_.pipeline = VK_NULL_HANDLE;
        ++device_.pipelines_evicted;
        device_.context->defer_destroy(palmetto::DeferredDeletionQueue::Kind::pipeline, handle);
    }
};

// ---- the live-pipeline budget (pipeline_budget.h, docs/memory_growth.md) -------------------------------------
//
// The decision is taken at PSO creation: the first `cap` graphics PSOs are built there as the layer always did, every
// later one is built at its first bind. The budget then keeps at most `cap` live pipelines, evicting the least
// recently bound one on a bind past the cap. Eviction takes this state's budget lock first and the PSO's build lock
// second; ensure_graphics_built() releases the build lock before it takes the budget lock, so the order is acyclic.
bool DeviceState::defer_graphics_build() {
    const std::uint64_t created = graphics_pipelines_created.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!pipeline_lazy_build_enabled()) return false;
    const std::size_t cap = pipeline_live_cap();
    if (!cap) return false;
    // No eager build at all: a pipeline is built at its first bind. Measured (docs/memory_growth.md): the driver's
    // shader compiler reserves multi-GB arenas per compile context, so even eight eager builds at creation grew the
    // boot to 44 GB, while one built pipeline kept it at 11.5 GB. The trade-off is named in the page: a driver error
    // in vkCreateGraphicsPipelines now surfaces at the first bind, not at CreateGraphicsPipelineState.
    return true;
}

bool DeviceState::note_pipeline_built(PipelineState* state) {
    if (!state) return false;
    std::lock_guard lock(pipeline_budget_mutex);
    pipeline_owners[state->id()] = state;
    ++pipelines_built;
    const std::uint64_t victim = pipeline_budget.touch(state->id());
    if (!victim) return true;
    const auto it = pipeline_owners.find(victim);
    if (it == pipeline_owners.end() || !it->second) return true;
    it->second->evict_graphics_pipeline();
    pipeline_owners.erase(it);
    return true;
}

void DeviceState::forget_pipeline(std::uintptr_t id) {
    std::lock_guard lock(pipeline_budget_mutex);
    pipeline_owners.erase(id);
    pipeline_budget.remove(id);
}

std::string DeviceState::pipeline_census_text() {
    std::lock_guard lock(pipeline_budget_mutex);
    char text[224] = {};
    std::snprintf(text, sizeof text,
                  "graphics PSOs %llu created; %zu live, %llu built, %llu evicted, %llu rebuilt; cap %zu",
                  static_cast<unsigned long long>(graphics_pipelines_created.load(std::memory_order_relaxed)),
                  pipeline_budget.live(), static_cast<unsigned long long>(pipelines_built),
                  static_cast<unsigned long long>(pipelines_evicted),
                  static_cast<unsigned long long>(pipelines_rebuilt), pipeline_live_cap());
    return text;
}

// ---- off-thread compute-pipeline compilation (docs/deferred_compile.md) -----------------------------------
void DeviceState::start_compile_workers() {
    bool expected = false;
    if (!compile_queue_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    palmetto::PipelineCallStats* stats = pipeline_stats();
    compile_queue.start(pipeline_compile_worker_count(), [stats] {
        if (stats) stats->note_compile_worker();
    });
}

// Queue one compute-pipeline driver build. The task holds a reference to the pipeline state (and through its device
// reference to this device) until the build has run, so the module, layout and code base the closure reads outlive it.
// nullptr means "the caller builds now": the switch is off, or the queue is at its bound.
std::shared_ptr<palmetto::CompileQueue::Task> DeviceState::defer_compute_build(PipelineState* state,
                                                                              std::function<bool()> build) {
    if (!state || !build) return nullptr;
    start_compile_workers();
    state->AddRef();
    const std::uintptr_t id = state->id();
    auto task = compile_queue.submit([this, state, id, build = std::move(build)]() mutable {
        const bool ok = build();
        // docs/peer_port_streaming.md: a held submission whose pipeline this was is woken here, on the compile worker
        // that finished it, so the hold flusher can replay it without any title thread waiting for the build. No-op
        // with the hold switch off; the state reference still makes `this` valid.
        pipeline_build_finished(id);
        state->Release();
        return ok;
    });
    if (!task) {
        state->Release();
        deferred_fallback_count.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    {
        std::lock_guard lock(deferred_compute_mutex);
        deferred_compute_tasks[state->id()] = task;
    }
    return task;
}

void DeviceState::forget_deferred_compute(std::uintptr_t id) {
    std::lock_guard lock(deferred_compute_mutex);
    deferred_compute_tasks.erase(id);
}

void DeviceState::stop_compile_workers() {
    compile_queue.stop();
    std::lock_guard lock(deferred_compute_mutex);
    deferred_compute_tasks.clear();
}

// ---- the non-blocking submission hold (docs/peer_port_streaming.md) ----------------------------------------------
//
// The queue holds a submission that names an unfinished deferred pipeline (VulkanQueue::execute) and asks these two
// hooks which pipeline ids the lists name that are still unbuilt and whether a set of ids has finished. The map under
// deferred_compute_mutex is the same bookkeeping docs/deferred_compile.md already keeps for the submit-side wait, so
// the two answers agree with it by construction.
bool DeviceState::collect_pending_pipelines(std::span<const palmetto::CommandList* const> lists,
                                            std::vector<std::uintptr_t>& out) {
    if (!pipeline_defer_compile_enabled()) return false;
    bool pending = false;
    std::lock_guard lock(deferred_compute_mutex);
    for (const palmetto::CommandList* list : lists) {
        if (!list) continue;
        for (const palmetto::Command& command : list->commands()) {
            std::uintptr_t pipeline = 0;
            if (const auto* dispatch = std::get_if<palmetto::Dispatch>(&command)) {
                pipeline = dispatch->pipeline;
            } else if (const auto* indirect = std::get_if<palmetto::ExecuteIndirect>(&command)) {
                if (indirect->kind == palmetto::IndirectKind::dispatch) pipeline = indirect->pipeline;
            }
            if (!pipeline) continue;
            const auto it = deferred_compute_tasks.find(pipeline);
            if (it == deferred_compute_tasks.end() || it->second->finished()) continue;  // eager, or already built
            out.push_back(pipeline);
            pending = true;
        }
    }
    return pending;
}

bool DeviceState::pipelines_finished(std::span<const std::uintptr_t> ids) {
    std::lock_guard lock(deferred_compute_mutex);
    for (const std::uintptr_t id : ids) {
        const auto it = deferred_compute_tasks.find(id);
        if (it != deferred_compute_tasks.end() && !it->second->finished()) return false;
    }
    return true;
}

// Called on the compile worker that finished a build (docs/peer_port_streaming.md): the hold policy loses that
// pipeline as a dependency and the hold flusher is woken to replay whatever became ready. A no-op with the switch off.
void DeviceState::pipeline_build_finished(std::uintptr_t id) {
    if (!context || !context->pipeline_hold_enabled()) return;
    context->pipeline_hold().finished(id);
    notify_pipeline_hold();
}

void DeviceState::notify_pipeline_hold() {
    if (!context || !context->pipeline_hold_enabled()) return;
    {
        std::lock_guard lock(hold_flusher_mutex);
        hold_flusher_pending = true;
    }
    hold_flusher_cv.notify_one();
}

// The hold flusher: one layer thread that replays held submissions once their pipelines are built (and forces the old
// wait when a hold expires past its budget). It is the "submit side" of the hold, so no title thread is ever inside a
// compiler for it. The periodic wake notices a budget expiry and a device loss; the condition variable makes the
// normal path prompt (one wake per finished build while something is held).
void DeviceState::start_pipeline_hold_flusher() {
    std::lock_guard lock(hold_flusher_mutex);
    if (hold_flusher_started) return;
    hold_flusher_started = true;
    hold_flusher = std::thread([this] {
        if (palmetto::PipelineCallStats* stats = pipeline_stats()) stats->note_hold_flusher();
        std::unique_lock lock(hold_flusher_mutex);
        for (;;) {
            hold_flusher_cv.wait_for(lock, std::chrono::milliseconds(100),
                                     [this] { return hold_flusher_stop || hold_flusher_pending; });
            if (hold_flusher_stop) return;
            hold_flusher_pending = false;
            lock.unlock();
            if (context) context->holds().pump_ready();
            lock.lock();
        }
    });
}

void DeviceState::stop_pipeline_hold_flusher() {
    {
        std::lock_guard lock(hold_flusher_mutex);
        if (!hold_flusher_started) return;
        hold_flusher_stop = true;
    }
    hold_flusher_cv.notify_all();
    if (hold_flusher.joinable()) hold_flusher.join();
}

// The queue hook (vulkan_backend.h prepare_pipelines, called from VulkanQueue::execute_now before the lists are
// prepared): make a deferred compute pipeline a submitted list actually names ready, on the thread that submits. It
// claims and builds *that* pipeline when it is still queued rather than waiting for the FIFO queue ahead of it: with
// the measured deferred arm (2,742 compute pipelines deferred, ~320 built in 90 s) the plain wait blocks the
// submission for minutes, and the title's own 30-pass hang watchdog aborts the boot first (docs/3d_textures.md section
// 11). Called with no Vulkan lock held; the build publishes the handle under resource_mutex_ itself.
void DeviceState::prepare_deferred_compute_pipelines(std::span<const palmetto::CommandList* const> lists) {
    if (!pipeline_defer_compile_enabled()) return;
    for (const palmetto::CommandList* list : lists) {
        if (!list) continue;
        for (const palmetto::Command& command : list->commands()) {
            std::uintptr_t pipeline = 0;
            if (const auto* dispatch = std::get_if<palmetto::Dispatch>(&command)) {
                pipeline = dispatch->pipeline;
            } else if (const auto* indirect = std::get_if<palmetto::ExecuteIndirect>(&command)) {
                if (indirect->kind == palmetto::IndirectKind::dispatch) pipeline = indirect->pipeline;
            }
            if (!pipeline) continue;
            std::shared_ptr<palmetto::CompileQueue::Task> task;
            {
                std::lock_guard lock(deferred_compute_mutex);
                const auto it = deferred_compute_tasks.find(pipeline);
                if (it != deferred_compute_tasks.end()) task = it->second;
            }
            if (!task) continue;  // not deferred: the replay map already holds its handle
            // Prewarm (docs/vertex_buffer_va.md section 13): with the newest-first queue the worker has already built
            // the pipeline this submission names, so no submit-thread wait happens. A finished task is not counted as
            // a wait; the queue's own counters still record the build.
            if (pipeline_prewarm_enabled() &&
                task->state() == palmetto::CompileQueue::Task::State::done) {
                deferred_built_count.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Non-blocking hold (docs/peer_port_streaming.md): a batch released from a hold names pipelines the hold
            // was released on, so they are finished by then; counting each as a submit wait would put the flusher's
            // replay into the title-thread wait counter. With the hold switch off this branch does nothing.
            if (pipeline_hold_enabled() && task->finished()) {
                deferred_built_count.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const double started = palmetto::timeline::seconds();
            const bool built = compile_queue.wait_or_build(task);
            palmetto::timeline::line("compile wait submit tid=0x%llx built=%d us=%.0f",
                                     static_cast<unsigned long long>(palmetto::thread_hash()), built ? 1 : 0,
                                     (palmetto::timeline::seconds() - started) * 1e6);
            if (built) {
                deferred_built_count.fetch_add(1, std::memory_order_relaxed);
            } else if (deferred_failed_count.fetch_add(1, std::memory_order_relaxed) == 0) {
                std::fprintf(stderr,
                             "[rt] d3d12: a deferred compute pipeline did not build; the submission that names it is "
                             "refused\n");
            }
        }
    }
}

std::string DeviceState::deferred_compile_text() {
    const auto& q = compile_queue;
    char text[512] = {};
    std::snprintf(text, sizeof text,
                  "[rt] deferred compile: submitted %llu, built %llu, failed %llu, rejected(fallback) %llu, cancelled "
                  "%llu, pending %zu of %zu, peak outstanding %llu; build time %.1f ms (max single %.1f ms); submit "
                  "waits %llu (total %.1f ms, max single %.1f ms; built by the waiting thread %llu, %.1f ms, max "
                  "single %.1f ms); workers %u\n",
                  static_cast<unsigned long long>(q.submitted()), static_cast<unsigned long long>(q.built()),
                  static_cast<unsigned long long>(q.failed()), static_cast<unsigned long long>(q.rejected()),
                  static_cast<unsigned long long>(q.cancelled()), q.pending(), q.bound(),
                  static_cast<unsigned long long>(q.peak_outstanding()),
                  static_cast<double>(q.builds_us()) / 1000.0,
                  static_cast<double>(q.longest_build_us()) / 1000.0,
                  static_cast<unsigned long long>(q.waits()), static_cast<double>(q.wait_us()) / 1000.0,
                  static_cast<double>(q.longest_wait_us()) / 1000.0,
                  static_cast<unsigned long long>(q.wait_builds()),
                  static_cast<double>(q.wait_build_us()) / 1000.0,
                  static_cast<double>(q.longest_wait_build_us()) / 1000.0, q.workers());
    std::string out = text;
    out += q.wait_threads_text();
    if (context && context->pipeline_hold_enabled()) out += context->pipeline_hold_report();
    return out;
}

// ---- the per-thread pipeline-call report (PALMETTO_PIPELINE_CALL_STATS, docs/deferred_compile.md) ------------
void DeviceState::start_pipeline_call_stats() {
    std::lock_guard lock(call_stats_mutex);
    if (call_stats_started.load(std::memory_order_relaxed)) return;
    call_stats = new palmetto::PipelineCallStats();
    const auto period = std::chrono::milliseconds(static_cast<long long>(pipeline_call_stats_interval_s() * 1000.0));
    call_stats_thread = std::thread([this, period] {
        const auto slice = std::chrono::milliseconds(100);  // reacts to shutdown within a slice, not a full period
        auto next = std::chrono::steady_clock::now() + period;
        for (;;) {
            std::this_thread::sleep_for(slice);
            if (call_stats_stop.load(std::memory_order_relaxed)) return;
            if (std::chrono::steady_clock::now() < next) continue;
            next += period;
            const std::string text = call_stats->report([this] { return deferred_compile_text(); });
            std::fputs(text.c_str(), stderr);
            std::fflush(stderr);
        }
    });
    call_stats_started.store(true, std::memory_order_release);
}

void DeviceState::stop_pipeline_call_stats() {
    {
        std::lock_guard lock(call_stats_mutex);
        if (!call_stats_started.load(std::memory_order_relaxed)) return;
        call_stats_stop.store(true, std::memory_order_relaxed);
    }
    if (call_stats_thread.joinable()) call_stats_thread.join();
    delete call_stats;
    call_stats = nullptr;
    call_stats_started.store(false, std::memory_order_relaxed);
}

void key_root_signature(palmetto::cached_pso::KeyBuilder& key, ID3D12RootSignature* signature) {
    if (!signature) {
        key.tag("no root signature");
    } else if (const RootSignature* own = RootSignature::from<RootSignature>(signature)) {
        key.tag("root signature").text(own->content().c_str());
    } else {
        const auto address = reinterpret_cast<std::uintptr_t>(signature);
        key.tag("foreign root signature").bytes(&address, sizeof(address));
    }
}

// A view of a texture as Create*View describes it, before the resource's own shape is applied.
struct TextureView {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // UNKNOWN: the resource's
    palmetto::gfx10::ImageType type = palmetto::gfx10::ImageType::image_2d;
    UINT first_mip = 0;
    UINT mip_count = ~0u;  // ~0: to the last
    UINT first_layer = 0;
    UINT layer_count = ~0u;
    UINT mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    UINT plane = 0;
    float min_lod = 0.0f;
};

// In-memory pipeline library (docs/d3d12_vulkan.md sections 3.6 and 4). D3D12 cached blobs are driver-specific
// and ignored across runs; Serialize writes a versioned header so CreatePipelineLibrary recognizes our own blobs.
// As on the host runtime (d3d12_pipeline_library_host_test), a foreign, truncated or corrupt blob is E_INVALIDARG;
// one of ours from another version is D3D12_ERROR_DRIVER_VERSION_MISMATCH. A pipeline library is a device child,
// not a pageable object: the host answers E_NOINTERFACE for ID3D12Pageable.
class PipelineLibrary final
    : public Child<PipelineLibrary, ID3D12PipelineLibrary1Stubs, ID3D12Object, ID3D12DeviceChild,
                   ID3D12PipelineLibrary, ID3D12PipelineLibrary1> {
public:
    // {6e2a9d41-7b5f-4c83-8e14-2a9d6f3c5b10}
    static constexpr GUID kTag{0x6e2a9d41, 0x7b5f, 0x4c83, {0x8e, 0x14, 0x2a, 0x9d, 0x6f, 0x3c, 0x5b, 0x10}};

    struct Header {
        std::uint32_t magic;
        std::uint32_t version;
        std::uint32_t count;
        std::uint32_t reserved;
    };
    static constexpr std::uint32_t kMagic = 0x314C504Cu;  // "LPL1"
    static constexpr std::uint32_t kVersion = 1;

    ~PipelineLibrary() override {
        std::lock_guard lock(mutex_);
        for (auto& [name, pso] : entries_)
            if (pso) pso->Release();
        entries_.clear();
    }

    // S_OK for a blob of ours, D3D12_ERROR_DRIVER_VERSION_MISMATCH for one of ours from another version,
    // E_INVALIDARG for anything else (foreign, truncated or corrupt).
    static HRESULT check_blob(const void* data, SIZE_T size) {
        if (!data || size < sizeof(Header)) return E_INVALIDARG;
        Header h{};
        std::memcpy(&h, data, sizeof(h));
        if (h.magic != kMagic || h.reserved != 0) return E_INVALIDARG;
        return h.version == kVersion ? S_OK : D3D12_ERROR_DRIVER_VERSION_MISMATCH;
    }

    HRESULT STDMETHODCALLTYPE StorePipeline(LPCWSTR name, ID3D12PipelineState* pipeline) override {
        if (!name || !*name || !pipeline) return E_INVALIDARG;
        if (!PipelineState::from<PipelineState>(pipeline)) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        auto [it, inserted] = entries_.emplace(name, pipeline);
        if (!inserted) return E_INVALIDARG;
        pipeline->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                   REFIID riid, void** out) override {
        if (!out) return E_INVALIDARG;  // the host runtime's answer (not E_POINTER)
        *out = nullptr;
        if (!name || !*name || !desc) return E_INVALIDARG;
        return load_entry(name, false, true, riid, out);
    }

    HRESULT STDMETHODCALLTYPE LoadComputePipeline(LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                                  REFIID riid, void** out) override {
        if (!out) return E_INVALIDARG;  // the host runtime's answer (not E_POINTER)
        *out = nullptr;
        if (!name || !*name || !desc) return E_INVALIDARG;
        return load_entry(name, true, false, riid, out);
    }

    HRESULT STDMETHODCALLTYPE LoadPipeline(LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                           REFIID riid, void** out) override {
        if (!out) return E_INVALIDARG;  // the host runtime's answer (not E_POINTER)
        *out = nullptr;
        if (!name || !*name || !desc || !desc->SizeInBytes || !desc->pPipelineStateSubobjectStream) return E_INVALIDARG;
        return load_entry(name, false, false, riid, out);
    }

    SIZE_T STDMETHODCALLTYPE GetSerializedSize() override { return sizeof(Header); }

    HRESULT STDMETHODCALLTYPE Serialize(void* data, SIZE_T size) override {
        if (!data || size < sizeof(Header)) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        const Header h{kMagic, kVersion, static_cast<std::uint32_t>(entries_.size()), 0};
        std::memcpy(data, &h, sizeof(h));
        return S_OK;
    }

private:
    HRESULT load_entry(LPCWSTR name, bool require_compute, bool require_graphics, REFIID riid, void** out) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(name);
        if (it == entries_.end()) return E_INVALIDARG;
        PipelineState* ps = PipelineState::from<PipelineState>(it->second);
        if (!ps) return E_INVALIDARG;
        if (require_compute && !ps->compute()) return E_INVALIDARG;
        if (require_graphics && !ps->graphics()) return E_INVALIDARG;
        return it->second->QueryInterface(riid, out);
    }

    std::mutex mutex_;
    std::unordered_map<std::wstring, ID3D12PipelineState*> entries_;
};

// Process-wide backing for ID3D12Device9::CreateShaderCacheSession and ID3D12ShaderCacheSession (D3D-34).
// Sessions in the same process sharing (Identifier, Mode, disk_path) share their entries; a live session with a
// different Version returns DXGI_ERROR_ALREADY_EXISTS, while reopening after all sessions close clears and recreates
// the cache. MODE_MEMORY discards its entries when the final session is released; MODE_DISK persists entries to a
// versioned binary file via unique per-writer temp files and atomic replacement.
struct ShaderCacheKey {
    GUID identifier{};
    D3D12_SHADER_CACHE_MODE mode = D3D12_SHADER_CACHE_MODE_MEMORY;
    std::wstring disk_path;  // empty for MODE_MEMORY

    bool operator<(const ShaderCacheKey& other) const {
        const int cmp = std::memcmp(&identifier, &other.identifier, sizeof(GUID));
        if (cmp != 0) return cmp < 0;
        if (mode != other.mode) return mode < other.mode;
        return disk_path < other.disk_path;
    }
};

struct ShaderCacheStore {
    static constexpr std::uint32_t kDiskMagic = 0x3143534Cu;  // "LSC1"
    static constexpr std::uint32_t kDiskFormatVersion = 2;
    static constexpr UINT kDefaultMaxMemoryBytes = 1024u;
    static constexpr UINT kDefaultMaxMemoryEntries = 128u;
    static constexpr UINT kDefaultMaxValueFileBytes = 128u * 1024u * 1024u;

    struct DiskHeader {
        std::uint32_t magic;
        std::uint32_t format_version;
        std::uint32_t entry_count;
        std::uint32_t reserved;
        std::uint64_t cache_version;
    };

    std::mutex mutex;
    std::map<std::string, std::vector<std::uint8_t>> entries;
    std::size_t total_bytes = 0;
    std::uint32_t live_sessions = 0;
    UINT64 version = 0;
    bool delete_on_destroy = false;
    std::filesystem::path disk_path;

    void load_from_disk_locked(UINT64 expected_version, UINT max_file_bytes) {
        if (disk_path.empty()) return;
        std::error_code ec;
        const std::uintmax_t file_size = std::filesystem::file_size(disk_path, ec);
        if (ec || file_size < sizeof(DiskHeader)) return;
        std::ifstream in(disk_path, std::ios::binary);
        if (!in) return;
        DiskHeader header{};
        if (!in.read(reinterpret_cast<char*>(&header), sizeof(header))) return;
        if (header.magic != kDiskMagic || header.format_version != kDiskFormatVersion || header.reserved != 0) return;
        if (header.cache_version != expected_version) {
            in.close();
            std::filesystem::remove(disk_path, ec);
            return;
        }
        std::uint64_t remaining = static_cast<std::uint64_t>(file_size) - sizeof(DiskHeader);
        if (header.entry_count > remaining / 8u) return;
        std::map<std::string, std::vector<std::uint8_t>> loaded;
        std::size_t bytes = 0;
        for (std::uint32_t i = 0; i < header.entry_count; ++i) {
            if (remaining < 8u) return;
            std::uint32_t lens[2]{};
            if (!in.read(reinterpret_cast<char*>(lens), sizeof(lens))) return;
            remaining -= 8u;
            const std::uint64_t key_len = lens[0];
            const std::uint64_t val_len = lens[1];
            if (!key_len || !val_len || key_len > remaining || val_len > remaining - key_len) return;
            if (val_len > max_file_bytes || bytes > max_file_bytes - val_len) return;
            std::string k(static_cast<std::size_t>(key_len), '\0');
            std::vector<std::uint8_t> v(static_cast<std::size_t>(val_len));
            if (!in.read(k.data(), static_cast<std::streamsize>(key_len)) ||
                !in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(val_len)))
                return;
            remaining -= key_len + val_len;
            loaded.emplace(std::move(k), std::move(v));
            bytes += static_cast<std::size_t>(val_len);
        }
        entries = std::move(loaded);
        total_bytes = bytes;
    }

    bool save_to_disk_locked() const {
        if (disk_path.empty()) return true;
        std::error_code ec;
        std::filesystem::create_directories(disk_path.parent_path(), ec);
        static std::atomic<std::uint64_t> write_seq{0};
        const std::uint64_t seq = write_seq.fetch_add(1, std::memory_order_relaxed);
        wchar_t suffix[64]{};
        std::swprintf(suffix, std::size(suffix), L".%lu.%lu.%llu.tmp",
                      static_cast<unsigned long>(GetCurrentProcessId()),
                      static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(seq));
        std::filesystem::path tmp = disk_path;
        tmp += suffix;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            const DiskHeader header{kDiskMagic, kDiskFormatVersion, static_cast<std::uint32_t>(entries.size()), 0,
                                    version};
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            for (const auto& [k, v] : entries) {
                const std::uint32_t lens[2] = {static_cast<std::uint32_t>(k.size()),
                                               static_cast<std::uint32_t>(v.size())};
                out.write(reinterpret_cast<const char*>(lens), sizeof(lens));
                out.write(k.data(), static_cast<std::streamsize>(k.size()));
                out.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
            }
            if (!out.good()) return false;
        }
        if (!MoveFileExW(tmp.c_str(), disk_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
        return true;
    }

    void clear() {
        std::lock_guard lock(mutex);
        entries.clear();
        total_bytes = 0;
        if (!disk_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(disk_path, ec);
        }
    }
};

std::filesystem::path default_shader_cache_dir() {
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path(ec);
    if (ec || tmp.empty()) return {};
    return tmp / L"palmetto_shader_cache";
}

struct ShaderCacheRegistry {
    std::mutex mutex;
    std::map<ShaderCacheKey, std::shared_ptr<ShaderCacheStore>> stores;
    std::atomic<bool> disabled{false};

    HRESULT acquire(const ShaderCacheKey& key, UINT64 version, UINT max_file_bytes,
                    std::shared_ptr<ShaderCacheStore>& out) {
        if (disabled.load(std::memory_order_relaxed)) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        std::lock_guard lock(mutex);
        if (disabled.load(std::memory_order_relaxed)) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        auto it = stores.find(key);
        if (it != stores.end()) {
            if (it->second->version != version) return DXGI_ERROR_ALREADY_EXISTS;
            ++it->second->live_sessions;
            out = it->second;
            return S_OK;
        }
        auto store = std::make_shared<ShaderCacheStore>();
        store->version = version;
        if (key.mode == D3D12_SHADER_CACHE_MODE_DISK && !key.disk_path.empty()) {
            store->disk_path = key.disk_path;
            std::lock_guard store_lock(store->mutex);
            store->load_from_disk_locked(version, max_file_bytes);
        }
        store->live_sessions = 1;
        stores.emplace(key, store);
        out = std::move(store);
        return S_OK;
    }

    void mark_delete_on_destroy(const std::shared_ptr<ShaderCacheStore>& store) {
        std::lock_guard lock(mutex);
        if (store) store->delete_on_destroy = true;
    }

    void release(const ShaderCacheKey& key, const std::shared_ptr<ShaderCacheStore>& store) {
        std::lock_guard lock(mutex);
        if (!store) return;
        if (store->live_sessions > 0) --store->live_sessions;
        if (store->live_sessions == 0) {
            if (store->delete_on_destroy && !store->disk_path.empty()) {
                std::error_code ec;
                std::filesystem::remove(store->disk_path, ec);
            }
            // Discard both idle MEMORY stores (per D3D12 spec) and idle DISK stores (so reopening reloads from disk).
            const auto it = stores.find(key);
            if (it != stores.end() && it->second == store) stores.erase(it);
        }
    }

    void clear_all() {
        std::vector<std::shared_ptr<ShaderCacheStore>> snapshot;
        {
            std::lock_guard lock(mutex);
            snapshot.reserve(stores.size());
            for (const auto& [k, v] : stores) snapshot.push_back(v);
        }
        for (const auto& store : snapshot) store->clear();
        // Also remove inactive and prior-run disk caches stored in the default cache directory (non-USE_WORKING_DIR).
        const auto default_dir = default_shader_cache_dir();
        if (!default_dir.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(default_dir, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(default_dir, ec)) {
                    if (ec) break;
                    if (!entry.is_regular_file(ec)) continue;
                    const auto fname = entry.path().filename().wstring();
                    if (fname.rfind(L"palmetto_sc_", 0) == 0) std::filesystem::remove(entry.path(), ec);
                }
            }
        }
    }
};

ShaderCacheRegistry& process_shader_cache_registry() {
    static auto* registry = new ShaderCacheRegistry();
    return *registry;
}

std::filesystem::path shader_cache_disk_path(const D3D12_SHADER_CACHE_SESSION_DESC& desc,
                                             const VkPhysicalDeviceProperties& gpu_props) {
    std::error_code ec;
    std::filesystem::path dir = (desc.Flags & D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR)
                                    ? std::filesystem::current_path(ec)
                                    : default_shader_cache_dir();
    if (ec || dir.empty()) return {};
    wchar_t name[160]{};
    const auto& g = desc.Identifier;
    if (desc.Flags & D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED) {
        std::swprintf(name, std::size(name),
                      L"palmetto_sc_%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x_d%04x_%04x_%08x.bin",
                      static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
                      g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], gpu_props.vendorID,
                      gpu_props.deviceID, gpu_props.driverVersion);
    } else {
        std::swprintf(name, std::size(name), L"palmetto_sc_%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x.bin",
                      static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
                      g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    }
    return dir / name;
}

class ShaderCacheSession final
    : public Child<ShaderCacheSession, ID3D12ShaderCacheSessionStubs, ID3D12Object, ID3D12DeviceChild,
                   ID3D12ShaderCacheSession> {
public:
    // {9a4f1b2c-8d6e-43a7-b152-7e3c9f0d4a81}
    static constexpr GUID kTag{0x9a4f1b2c, 0x8d6e, 0x43a7, {0xb1, 0x52, 0x7e, 0x3c, 0x9f, 0x0d, 0x4a, 0x81}};

    ShaderCacheSession(const D3D12_SHADER_CACHE_SESSION_DESC& desc, const ShaderCacheKey& key,
                       ShaderCacheRegistry& registry, std::shared_ptr<ShaderCacheStore> store)
        : desc_(desc), key_(key), registry_(&registry), store_(std::move(store)) {}

    ~ShaderCacheSession() override {
        if (registry_) registry_->release(key_, store_);
    }

    HRESULT STDMETHODCALLTYPE FindValue(const void* key, UINT key_size, void* value, UINT* value_size) override {
        if (!key || !key_size || !value_size) return E_INVALIDARG;
        if (registry_ && registry_->disabled.load(std::memory_order_relaxed))
            return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        std::lock_guard lock(store_->mutex);
        const std::string k(static_cast<const char*>(key), key_size);
        const auto it = store_->entries.find(k);
        if (it == store_->entries.end()) return DXGI_ERROR_NOT_FOUND;
        const UINT stored_size = static_cast<UINT>(it->second.size());
        if (!value) {
            *value_size = stored_size;
            return S_OK;
        }
        if (*value_size < stored_size) {
            *value_size = stored_size;
            return DXGI_ERROR_MORE_DATA;
        }
        std::memcpy(value, it->second.data(), stored_size);
        *value_size = stored_size;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StoreValue(const void* key, UINT key_size, const void* value, UINT value_size) override {
        if (!key || !key_size || !value || !value_size) return E_INVALIDARG;
        if (registry_ && registry_->disabled.load(std::memory_order_relaxed))
            return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        std::lock_guard lock(store_->mutex);
        const std::string k(static_cast<const char*>(key), key_size);
        if (store_->entries.find(k) != store_->entries.end()) return DXGI_ERROR_ALREADY_EXISTS;
        if (desc_.Mode == D3D12_SHADER_CACHE_MODE_MEMORY) {
            const UINT max_entries = desc_.MaximumInMemoryCacheEntries ? desc_.MaximumInMemoryCacheEntries
                                                                       : ShaderCacheStore::kDefaultMaxMemoryEntries;
            const UINT max_bytes = desc_.MaximumInMemoryCacheSizeBytes ? desc_.MaximumInMemoryCacheSizeBytes
                                                                       : ShaderCacheStore::kDefaultMaxMemoryBytes;
            if (store_->entries.size() >= max_entries || value_size > max_bytes ||
                store_->total_bytes > max_bytes - value_size)
                return DXGI_ERROR_CACHE_FULL;
        } else {
            const UINT max_file_bytes = desc_.MaximumValueFileSizeBytes ? desc_.MaximumValueFileSizeBytes
                                                                        : ShaderCacheStore::kDefaultMaxValueFileBytes;
            if (value_size > max_file_bytes || store_->total_bytes > max_file_bytes - value_size)
                return DXGI_ERROR_CACHE_FULL;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(value);
        auto [it, inserted] = store_->entries.emplace(k, std::vector<std::uint8_t>(bytes, bytes + value_size));
        store_->total_bytes += value_size;
        if (desc_.Mode == D3D12_SHADER_CACHE_MODE_DISK && !store_->save_to_disk_locked()) {
            store_->total_bytes -= value_size;
            store_->entries.erase(it);
            return E_FAIL;
        }
        return S_OK;
    }

    void STDMETHODCALLTYPE SetDeleteOnDestroy() override {
        if (registry_) registry_->mark_delete_on_destroy(store_);
    }

#if defined(_MSC_VER) || !defined(_WIN32)
    D3D12_SHADER_CACHE_SESSION_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
#else
    D3D12_SHADER_CACHE_SESSION_DESC* STDMETHODCALLTYPE GetDesc(D3D12_SHADER_CACHE_SESSION_DESC* out) override {
        *out = desc_;
        return out;
    }
#endif

private:
    D3D12_SHADER_CACHE_SESSION_DESC desc_{};
    ShaderCacheKey key_{};
    ShaderCacheRegistry* registry_ = nullptr;
    std::shared_ptr<ShaderCacheStore> store_;
};

// The T# of a view: what the shader gets to know about it, in the structure the console's shaders read.
// False (with a log) for a view set 0 cannot show yet.
bool image_descriptor(const Resource& resource, const TextureView& view, palmetto::gfx10::Image& out) {
    namespace g = palmetto::gfx10;
    const D3D12_RESOURCE_DESC& desc = resource.desc();
    // Plane 1 of a depth-stencil texture is its stencil, viewed as X24_TYPELESS_G8_UINT (D24S8) or
    // X32_TYPELESS_G8X24_UINT (D32S8): eight unsigned bits in G, as D3D12 returns them. The T# is 8_UINT, the
    // format of the console's separate stencil surface, and bind_image views the stencil aspect for it.
    const bool stencil = view.plane == 1;
    if (view.plane > 1 || (stencil && !(resource.aspects() & VK_IMAGE_ASPECT_STENCIL_BIT))) {
        log_once("no such plane in this texture; plane", int(view.plane));
        return false;
    }
    // Each stencil view format belongs to its own DXGI family only, as in D3D12: the family is the texture's DXGI
    // format, not the Vulkan format behind it (a D24 texture may be D32_SFLOAT_S8_UINT underneath).
    const bool stencil_format_fits =
        (view.format == DXGI_FORMAT_X24_TYPELESS_G8_UINT &&
         (desc.Format == DXGI_FORMAT_R24G8_TYPELESS || desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT)) ||
        (view.format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT &&
         (desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS || desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT));
    if (stencil && !stencil_format_fits) {
        log_once("a stencil-plane view needs its texture family's stencil view format; format", int(view.format));
        return false;
    }
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const bool msaa = g::multisampled(view.type);
    const bool line = view.type == g::ImageType::image_1d;
    const auto dimension = line ? D3D12_RESOURCE_DIMENSION_TEXTURE1D :
                           view.type == g::ImageType::image_3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D :
                                                               D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    if (desc.Dimension != dimension || (line && view.layer_count != 1) || msaa != (desc.SampleDesc.Count > 1)) {
        log_once("view type incompatible with texture dimension or sample count; view type", int(view.type));
        return false;
    }
    if (msaa) {
        // The MS bindings expose typed colour SRVs. An MS view has one mip and can name an array slice range.
        const Format format = texture_storage_format(desc.Format, false);
        if (resource.aspects() != VK_IMAGE_ASPECT_COLOR_BIT || format.typeless ||
            (view.format != DXGI_FORMAT_UNKNOWN && view.format != desc.Format)) {
            log_once("multisample shader view requires a matching typed colour format:", int(desc.Format));
            return false;
        }
        // Four 3-bit selectors followed by D3D12's always-set bit.
        if ((view.mapping >> 12) != 1) {
            log_once("invalid multisample shader view component mapping:", int(view.mapping));
            return false;
        }
    }
    const VkFormat resource_format = resource.uint_backed() ? resource.uint_backed() : resource.format();
    VkFormat named = resource_format;
    if (volume) {
        const bool compatible = vulkan_format(desc.Format, false).typeless
            ? desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
              (view.format == DXGI_FORMAT_R8G8B8A8_UNORM || view.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            : view.format == DXGI_FORMAT_UNKNOWN || view.format == desc.Format;
        if (!compatible) {
            log_once("unsupported volume SRV format for this resource:", int(view.format));
            return false;
        }
    }
    // Depth and depth-stencil textures can only be viewed through their own family's depth-plane SRV format (or
    // UNKNOWN on typed depth textures): in Vulkan a depth image's view format must equal the image's own depth
    // format (VUID-VkImageViewCreateInfo-image-01762 / 09594), so colour view formats such as R16_FLOAT on D16 or
    // R32_UINT on D32 must be rejected. Each view format belongs to its own DXGI family only, whatever format
    // backs the texture (a D24 texture may be D32_SFLOAT_S8_UINT underneath).
    const bool depth_plane = (resource.aspects() & VK_IMAGE_ASPECT_DEPTH_BIT) && !stencil;
    if (depth_plane) {
        const bool depth_format_fits =
            (desc.Format == DXGI_FORMAT_D16_UNORM &&
             (view.format == DXGI_FORMAT_UNKNOWN || view.format == DXGI_FORMAT_R16_UNORM)) ||
            (desc.Format == DXGI_FORMAT_R16_TYPELESS && view.format == DXGI_FORMAT_R16_UNORM) ||
            (desc.Format == DXGI_FORMAT_D32_FLOAT &&
             (view.format == DXGI_FORMAT_UNKNOWN || view.format == DXGI_FORMAT_R32_FLOAT)) ||
            (desc.Format == DXGI_FORMAT_R32_TYPELESS && view.format == DXGI_FORMAT_R32_FLOAT) ||
            (desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT &&
             (view.format == DXGI_FORMAT_UNKNOWN || view.format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS)) ||
            (desc.Format == DXGI_FORMAT_R24G8_TYPELESS && view.format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS) ||
            (desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT &&
             (view.format == DXGI_FORMAT_UNKNOWN || view.format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)) ||
            (desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS && view.format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
        if (!depth_format_fits) {
            log_once("incompatible depth shader-resource view format:", int(view.format));
            return false;
        }
    }
    if (view.format != DXGI_FORMAT_UNKNOWN && !stencil && !depth_plane) {
        if ((view.format == DXGI_FORMAT_A8_UNORM) != (desc.Format == DXGI_FORMAT_A8_UNORM)) {
            log_once("incompatible typed A8 view format:", int(view.format));
            return false;
        }
        // Only a castable-format list that names the view's format lets a view cross the two families, as WARP and
        // AMD's D3D12 driver accept (relaxed casting).
        if (alpha_unused(view.format) != alpha_unused(desc.Format) && !resource.lists_castable_format(view.format)) {
            log_once("view format from another typeless family (B8G8R8X8 and B8G8R8A8):", int(view.format));
            return false;
        }
        if ((view.format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) != (desc.Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP)) {
            log_once("incompatible R9G9B9E5 view format:", int(view.format));
            return false;
        }
        named = texture_storage_format(view.format, false).format;
        if (g::is_depth_stencil_format(named)) {
            log_once("depth-stencil view format on a colour texture:", int(view.format));
            return false;
        }
    }
    // A 96-bit texture takes views of its own family only (and a four-channel texture none of it), and only the
    // TYPELESS one is cast between FLOAT, UINT and SINT. Its T# names the three-channel format: the swizzle below
    // then reads alpha as one, and bind_image views the RGBA32 image accordingly.
    const bool rgb96 = resource.rgb96();
    if (!stencil && !depth_plane) {
        const bool view_rgb96 = view.format != DXGI_FORMAT_UNKNOWN ? vulkan_format(view.format, false).rgb96 : rgb96;
        if (view_rgb96 != rgb96 ||
            (rgb96 && desc.Format != DXGI_FORMAT_R32G32B32_TYPELESS && named != resource.format())) {
            log_once("view format outside the texture's 96-bit family:", int(view.format));
            return false;
        }
        if (rgb96) named = rgb96_named_format(named);
    }
    if (named == VK_FORMAT_UNDEFINED || !resource.allows_view_format(named)) {
        log_once("view format not in the resource's castable list:", int(view.format));
        return false;
    }
    const std::uint32_t code = stencil ? g::stencil_format : g::format_from_vulkan(named);
    VkFormat view_format = VK_FORMAT_UNDEFINED;
    if (stencil) {
        view_format = resource.format();
    } else if (code && rgb96) {
        view_format = named;
    } else if (code) {
        view_format = g::vulkan_view_format(resource_format, code, resource.has_view_format_list());
    }
    if (view_format == VK_FORMAT_UNDEFINED) {
        log_once("view format not supported for this texture:", int(view.format));
        return false;
    }
    const UINT layers = volume ? 1 : desc.DepthOrArraySize;
    if (view.first_mip >= desc.MipLevels || view.first_layer >= layers) {
        log_once("view starts past the texture's mips or layers; first mip", int(view.first_mip));
        return false;
    }
    if (msaa && view.layer_count != UINT_MAX && view.layer_count > UINT(desc.DepthOrArraySize) - view.first_layer) {
        log_once("multisample shader view ends past the texture array; layers", int(view.layer_count));
        return false;
    }
    out = {};
    out.address = resource.image_address();
    out.format = code;
    out.type = view.type;
    out.width = std::uint32_t(desc.Width);
    out.height = desc.Height;
    out.depth = volume ? desc.DepthOrArraySize : 1;
    out.samples = desc.SampleDesc.Count;
    out.first_mip = view.first_mip;
    out.mip_count = std::min<UINT>(view.mip_count, desc.MipLevels - view.first_mip);
    out.first_layer = view.first_layer;
    out.layer_count = std::min<UINT>(view.layer_count, layers - view.first_layer);
    if (view.type == g::ImageType::cube) out.layer_count -= out.layer_count % 6;  // whole cubes
    out.resource_mips = desc.MipLevels;
    out.min_lod = view.min_lod;
    if (!out.mip_count || !out.layer_count) {
        log_once("view of no mips or no layers; type", int(view.type));
        return false;
    }
    // D3D12's component mapping picks, for each of R, G, B, A, a channel of the format or a constant; the T#
    // says the same about the hardware's channels, which the format places.
    // A stencil view's format has its eight bits in G and no R, B or A of its own (0, 0, 1 when read).
    // B8G8R8X8 has no alpha: a view in its family reads 1, and a B8G8R8A8 view of a B8G8R8X8 texture (through a
    // castable-format list) the stored byte. The view's format decides, as on WARP and AMD's D3D12 driver.
    // The T#'s format code is the same for both channel orders of a colour format (R8G8B8A8 and B8G8R8A8 are one
    // 8_8_8_8 code), and the image view keeps the image's format; a castable-format list may still ask for the other
    // order, so then the view's own format places the channels. Depth images keep their own format's channels.
    const bool other_order = view.format != DXGI_FORMAT_UNKNOWN && !stencil && !depth_plane && !rgb96 &&
                             named != view_format && !g::is_depth_stencil_format(view_format);
    g::Swizzle channels = g::format_swizzle(other_order ? named : view_format);
    if (stencil) channels = {g::select_zero, g::select_x, g::select_zero, g::select_one};
    else if (desc.Format == DXGI_FORMAT_A8_UNORM) channels = {g::select_zero, g::select_zero, g::select_zero, g::select_x};
    else if (alpha_unused(view.format != DXGI_FORMAT_UNKNOWN ? view.format : desc.Format)) channels[3] = g::select_one;
    for (UINT i = 0; i < 4; ++i) {
        const UINT from = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(i, view.mapping);
        if (msaa && from > D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1) {
            log_once("invalid multisample shader view component selector:", from);
            return false;
        }
        out.swizzle[i] = from < 4 ? channels[from]
                                  : from == D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 ? g::select_zero : g::select_one;
    }
    return true;
}

// The S# of a D3D12 sampler.
palmetto::gfx10::Sampler sampler_descriptor(const D3D12_SAMPLER_DESC& desc, DeviceState* state = nullptr) {
    namespace g = palmetto::gfx10;
    const auto address = [](D3D12_TEXTURE_ADDRESS_MODE mode) {
        switch (mode) {
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return g::AddressMode::mirror;
        case D3D12_TEXTURE_ADDRESS_MODE_CLAMP: return g::AddressMode::clamp_last_texel;
        case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return g::AddressMode::clamp_border;
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE: return g::AddressMode::mirror_once_last_texel;
        default: return g::AddressMode::wrap;
        }
    };
    const bool anisotropic = D3D12_DECODE_IS_ANISOTROPIC_FILTER(desc.Filter);
    const auto filter = [&](D3D12_FILTER_TYPE type) {
        if (anisotropic) return type == D3D12_FILTER_TYPE_LINEAR ? g::Filter::aniso_bilinear : g::Filter::aniso_point;
        return type == D3D12_FILTER_TYPE_LINEAR ? g::Filter::bilinear : g::Filter::point;
    };
    g::Sampler out{};
    out.address_u = address(desc.AddressU);
    out.address_v = address(desc.AddressV);
    out.address_w = address(desc.AddressW);
    out.mag_filter = filter(D3D12_DECODE_MAG_FILTER(desc.Filter));
    out.min_filter = filter(D3D12_DECODE_MIN_FILTER(desc.Filter));
    out.mip_filter = D3D12_DECODE_MIP_FILTER(desc.Filter) == D3D12_FILTER_TYPE_LINEAR ? g::MipFilter::linear
                                                                                    : g::MipFilter::point;
    out.max_anisotropy = anisotropic ? std::clamp<UINT>(desc.MaxAnisotropy, 1, 16) : 1;
    const D3D12_FILTER_REDUCTION_TYPE reduction = D3D12_DECODE_FILTER_REDUCTION(desc.Filter);
    // D3D12's comparison functions are 1 (NEVER) to 8 (ALWAYS); the S#'s are 0 to 7 in the same order.
    if (reduction == D3D12_FILTER_REDUCTION_TYPE_COMPARISON && desc.ComparisonFunc >= D3D12_COMPARISON_FUNC_NEVER)
        out.compare = g::CompareFunction(desc.ComparisonFunc - D3D12_COMPARISON_FUNC_NEVER);
    out.compare_never = reduction == D3D12_FILTER_REDUCTION_TYPE_COMPARISON &&
                        desc.ComparisonFunc == D3D12_COMPARISON_FUNC_NEVER;
    if (reduction == D3D12_FILTER_REDUCTION_TYPE_MINIMUM) out.filter_mode = g::FilterMode::minimum;
    if (reduction == D3D12_FILTER_REDUCTION_TYPE_MAXIMUM) out.filter_mode = g::FilterMode::maximum;
    out.min_lod = desc.MinLOD;
    out.max_lod = desc.MaxLOD;
    out.lod_bias = desc.MipLODBias;
    const FLOAT* border = desc.BorderColor;
    const bool black = border[0] == 0 && border[1] == 0 && border[2] == 0;
    if (black && border[3] == 0) {
        out.border_color = g::BorderColor::transparent_black;
    } else if (black && border[3] == 1) {
        out.border_color = g::BorderColor::opaque_black;
    } else if (border[0] == 1 && border[1] == 1 && border[2] == 1 && border[3] == 1) {
        out.border_color = g::BorderColor::opaque_white;
    } else {
        out.border_color = g::BorderColor::table;
        if (state) out.border_color_index = state->register_border_color(false, border);
    }
    return out;
}

// A query heap: one VkQueryPool, registered with the context so that lists can resolve it by ID
// (docs/d3d12_vulkan.md section 4.6). Occlusion heaps hold both OCCLUSION and BINARY_OCCLUSION queries; timestamp
// and copy-queue timestamp heaps are timestamp pools; pipeline-statistics heaps count all eleven Vulkan
// statistics, whose result order is the field order of D3D12_QUERY_DATA_PIPELINE_STATISTICS. Stream-output
// statistics heaps have no pool: without stream output every such query is zero.
class QueryHeap final
    : public Child<QueryHeap, ID3D12QueryHeapStubs, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable,
                   ID3D12QueryHeap> {
public:
    // {6e2b9a41-d83c-4f17-b5e9-0a7c3d1f8b62}
    static constexpr GUID kTag{0x6e2b9a41, 0xd83c, 0x4f17, {0xb5, 0xe9, 0x0a, 0x7c, 0x3d, 0x1f, 0x8b, 0x62}};

    explicit QueryHeap(DeviceState& device) : device_(device) {}

    ~QueryHeap() override {
        if (!registered_) return;
        device_.context->remove_query_pool(id());
        if (pool_) device_.context->vk.vkDestroyQueryPool(device_.info.device, pool_, nullptr);
    }

    palmetto::QueryHeapId id() const { return query_heap_id(this); }

    HRESULT create(const D3D12_QUERY_HEAP_DESC& desc) {
        namespace h = palmetto::d3d12_query_heap;
        auto& vk = device_.context->vk;
        if (!vk.vkCreateQueryPool || !vk.vkDestroyQueryPool) return E_NOTIMPL;
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryCount = desc.Count;
        switch (desc.Type) {
        case h::occlusion: info.queryType = VK_QUERY_TYPE_OCCLUSION; break;
        case h::timestamp:
        case h::copy_queue_timestamp:
            if (!(device_.context->timestamp_period() > 0)) {
                log_once("timestamp query heap: the queue writes no timestamps; type", desc.Type);
                return E_NOTIMPL;
            }
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            break;
        case h::so_statistics:
            // The layer has no stream output: a graphics PSO with StreamOutput entries is refused (E_NOTIMPL) and
            // SOSetTargets is not implemented, so no primitive ever reaches a stream and every query of this heap
            // counts 0 written and 0 needed. That needs no Vulkan pool: Begin/EndQuery run nothing and a resolve fills zeros. When stream
            // output lands, this becomes a VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT pool (D3D-302).
            return register_pool(desc);
        case h::pipeline_statistics:
            if (!device_.info.pipeline_statistics_query) {
                log_once("pipeline-statistics query heap: pipelineStatisticsQuery not enabled; type", desc.Type);
                return E_NOTIMPL;
            }
            info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            info.pipelineStatistics = kPipelineStatistics;
            break;
        default:
            // Video decode and mesh-shader statistics (PIPELINE_STATISTICS1) have no path yet.
            log_once("query heap type not supported:", desc.Type);
            return E_NOTIMPL;
        }
        if (vk.vkCreateQueryPool(device_.info.device, &info, nullptr, &pool_) != VK_SUCCESS) {
            pool_ = VK_NULL_HANDLE;
            return E_OUTOFMEMORY;
        }
        return register_pool(desc);
    }

private:
    HRESULT register_pool(const D3D12_QUERY_HEAP_DESC& desc) {
        palmetto::QueryPool pool{};
        pool.pool = pool_;
        pool.heap_type = desc.Type;
        pool.count = desc.Count;
        pool.precise = device_.info.occlusion_query_precise;
        pool.ended.assign(desc.Count, false);
        device_.context->add_query_pool(id(), pool);
        registered_ = true;
        return S_OK;
    }

    // VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT through COMPUTE_SHADER_INVOCATIONS_BIT (0x1-0x400):
    // IAVertices, IAPrimitives, VSInvocations, GSInvocations, GSPrimitives, CInvocations, CPrimitives,
    // PSInvocations, HSInvocations (patches), DSInvocations, CSInvocations.
    static constexpr VkQueryPipelineStatisticFlags kPipelineStatistics = 0x7ff;
    static_assert(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) == 11 * sizeof(UINT64));

    DeviceState& device_;
    VkQueryPool pool_ = VK_NULL_HANDLE;  // null for a stream-output heap (no Vulkan query behind it)
    bool registered_ = false;            // added to the context, so removed from it on destruction
};

// The Vulkan objects create_vulkan_device_standalone made, destroyed with the device.
struct OwnedRuntime {
    PFN_vkDestroyDevice destroy_device = nullptr;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
};

class Device final
    : public Object<Device, ID3D12Device11Stubs, ID3D12Object, ID3D12Device, ID3D12Device1, ID3D12Device2,
                    ID3D12Device3, ID3D12Device4, ID3D12Device5, ID3D12Device6, ID3D12Device7, ID3D12Device8,
                    ID3D12Device9, ID3D12Device10, ID3D12Device11> {
public:
    // {c58e1d27-3a96-4b4f-9e72-6d0a8b5f3c61}
    static constexpr GUID kTag{0xc58e1d27, 0x3a96, 0x4b4f, {0x9e, 0x72, 0x6d, 0x0a, 0x8b, 0x5f, 0x3c, 0x61}};

    explicit Device(const VulkanDeviceInfo& info) {
        // Every Vulkan device serves Xbox device slot 133, whichever entry made it: create_vulkan_device and the
        // standalone device palmetto --run uses both come through here (found in review: registering only in
        // create_vulkan_device left the retail path without it).
        rt::d3d12_xs::set_placed_at_va_handler(&create_buffer_at_host_address);
        // docs/present_wiring.md: publish the console PresentX path to the queue proxy, through the palmetto_register_*
        // hook that keeps the GPU-free proxy binaries free of this file (the pattern the line above uses for device slot
        // 133). Both device entries - create_vulkan_device and the standalone device palmetto --run uses - come through
        // this constructor; the presentation core creates its window and swapchain lazily on the first present, through
        // create_window_presenter.
        if (const XboxPresentBackend* present_backend =
                create_xbox_present_backend(static_cast<ID3D12Device*>(this))) {
            palmetto_register_xbox_present(const_cast<XboxPresentBackend*>(present_backend));
        }
        state_.info = info;
        queue_users_.resize(info.command_queues.size(), 0);
        compute_users_.resize(info.compute_queues.size(), 0);
        copy_users_.resize(info.copy_queues.size(), 0);
        for (std::uint32_t f : {info.queue_family, info.compute_family, info.copy_family})
            if (f != UINT32_MAX && std::find(state_.families.begin(), state_.families.end(), f) == state_.families.end())
                state_.families.push_back(f);
        const auto get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            info.get_instance_proc(info.instance, "vkGetDeviceProcAddr"));
        state_.format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceFormatProperties"));
        state_.image_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceImageFormatProperties"));
        state_.image_format_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceImageFormatProperties2"));
        if (!state_.image_format_properties2) {
            state_.image_format_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
                info.get_instance_proc(info.instance, "vkGetPhysicalDeviceImageFormatProperties2KHR"));
        }
        const auto memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceMemoryProperties"));
        const auto device_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties"));
        const auto queue_families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        if (!get_device || !state_.format_properties || !state_.image_format_properties || !memory_properties ||
            !device_properties || !queue_families)
            return;
        state_.r9g9b9e5_uav_emulated =
            state_.probe_uint_backed_uav(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, VK_FORMAT_R32_UINT);
        state_.b5g5r5a1_uav_emulated =
            state_.probe_uint_backed_uav(VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_FORMAT_R16_UINT);
        state_.rgba8_srgb_uav_emulated =
            state_.probe_uint_backed_uav(VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM);
        state_.bgra8_srgb_uav_emulated =
            state_.probe_uint_backed_uav(VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM);
        memory_properties(info.physical, &state_.memory);
        {
            char text[4] = {};
            size_t length = 0;
            const bool sysmem = getenv_s(&length, text, "PALMETTO_UPLOAD_SYSMEM") == 0 && length > 1 && text[0] != '0';
            state_.upload_in_vram = palmetto::resizable_bar(state_.memory) && !sysmem;
            std::fprintf(stderr, "[rt] d3d12: upload heaps in %s\n",
                         state_.upload_in_vram ? "VRAM (DEVICE_LOCAL | HOST_VISIBLE, Resizable BAR)"
                         : sysmem             ? "system memory (PALMETTO_UPLOAD_SYSMEM)"
                                              : "system memory (no Resizable BAR)");
        }
        for (const char* name : {"vkGetDeviceImageMemoryRequirements", "vkGetDeviceImageMemoryRequirementsKHR"})
            if (!state_.image_requirements)
                state_.image_requirements =
                    reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(get_device(info.device, name));
        // The dedicated-allocation question the pool must ask before it shares a block with an image (Vulkan 1.1, so
        // the core name is always there on the devices this layer runs on).
        state_.image_requirements2 =
            reinterpret_cast<PFN_vkGetImageMemoryRequirements2>(get_device(info.device, "vkGetImageMemoryRequirements2"));

        if (info.physical && info.get_instance_proc) {
            const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties"));
            if (get_props) {
                VkPhysicalDeviceProperties props{};
                get_props(info.physical, &props);
                state_.pso_identity.vendor = props.vendorID;
                state_.pso_identity.device = props.deviceID;
                state_.pso_identity.driver = props.driverVersion;
                std::memcpy(state_.pso_identity.cache_uuid, props.pipelineCacheUUID, VK_UUID_SIZE);
                state_.pipeline_creation_feedback = props.apiVersion >= VK_API_VERSION_1_3;
                if (state_.info.max_push_constants_size == 0) {
                    state_.info.max_push_constants_size = props.limits.maxPushConstantsSize;
                    state_.info.push_constants_need_fallback = (state_.info.max_push_constants_size < 144);
                }
                // The counts D3D12 offers with no outputs are the target-independent-rasterization ones: 1, 4, 8
                // and 16 (ForcedSampleCount is 0, 1, 4, 8 or 16; DirectX-Specs VulkanOn12.md "Sample-frequency MSAA
                // with no render targets bound": a driver reports at least 0b11101). 2x is not one of them, though
                // framebufferNoAttachmentsSampleCounts has it on every GPU here; reporting it would offer a count the
                // PC runtime refuses (its hosts report 0x1d).
                constexpr VkSampleCountFlags kD3D12SampleCounts =
                    VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT | VK_SAMPLE_COUNT_16_BIT;
                state_.no_output_sample_counts =
                    static_cast<UINT>(props.limits.framebufferNoAttachmentsSampleCounts & kD3D12SampleCounts);
                state_.f16_half_top_location = palmetto::f16_half_top_location(
                    props.limits.maxVertexOutputComponents, props.limits.maxFragmentInputComponents);
            }
            if (const auto get_props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                    info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties2"))) {
                VkPhysicalDeviceDepthStencilResolveProperties resolve{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
                VkPhysicalDeviceConservativeRasterizationPropertiesEXT conservative{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT};
                if (info.conservative_rasterization) resolve.pNext = &conservative;
                VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &resolve};
                get_props2(info.physical, &props);
                state_.conservative_raster_tier = D3D12_CONSERVATIVE_RASTERIZATION_TIER(palmetto::conservative_raster::tier(
                    info.conservative_rasterization, props.properties.limits.subPixelPrecisionBits,
                    conservative.primitiveOverestimationSize, conservative.degenerateTrianglesRasterized == VK_TRUE,
                    conservative.fullyCoveredFragmentShaderInputVariable == VK_TRUE, info.inner_coverage_abi));
                state_.conservative_post_depth_coverage = conservative.conservativeRasterizationPostDepthCoverage == VK_TRUE;
                state_.depth_resolve_modes = resolve.supportedDepthResolveModes;
                state_.stencil_resolve_modes = resolve.supportedStencilResolveModes;
                const palmetto::ArenaLimits limits = palmetto::query_arena_limits(get_props2, info.physical);
                if (!state_.info.max_buffer_size) state_.info.max_buffer_size = limits.max_buffer_size;
                if (!state_.info.max_memory_allocation_size)
                    state_.info.max_memory_allocation_size = limits.max_allocation_size;
            }
        }

        state_.context = std::make_unique<palmetto::VulkanContext>(get_device, info.device,
                                                                  info.separate_depth_stencil_layouts,
                                                                  info.conditional_rendering, info.ray_tracing,
                                                                  info.xdxr_acceleration_structures &&
                                                                      info.buffer_device_address);
        if (info.physical && state_.format_properties) {
            state_.context->set_color_attachment_format(
                [physical = info.physical, get_props = state_.format_properties](VkFormat format) {
                    VkFormatProperties properties{};
                    get_props(physical, format, &properties);
                    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
                });
        }
        if (info.buffer_device_address && state_.context->loaded() && !create_arena())
            std::fprintf(stderr, "[rt] d3d12: the GPU address arena could not be created\n");
        // Pooled and deferred image memory (docs/streamed_image_backing.md): both are off by default. The pool exists
        // only with PALMETTO_IMAGE_MEM_POOL=1; the binder is registered always so a deferred image is backed before a
        // submission can use it (a no-op when PALMETTO_IMAGE_DEFERRED is unset and no image was deferred).
        if (state_.context->loaded()) {
            state_.start_image_memory_pool();
            state_.context->set_deferred_image_binder([this] { return state_.bind_deferred_images(); });
            // docs/memory_growth.md: the live-pipeline budget's counters, printed at the bounded submission milestones.
            state_.context->set_pipeline_census([this] { return state_.pipeline_census_text(); });
        }
        // A queue refreshes the arena mirrors of the title's console descriptor heaps before each submission
        // (docs/page_address_space.md). Registered even without an arena: a heap created later decides whether it can
        // be mirrored, and refuses draws if it cannot.
        if (state_.context->loaded())
            state_.context->set_console_heap_sync([this] { state_.sync_console_heaps(); });
        // The title's page content (docs/page_content.md): the replay pulls the bytes behind a title VA, the data a
        // descriptor table's V#s name, and a copy's source range, into the arena stand-ins before a submission reads
        // them.
        state_.context->set_console_page_pull(
            [this](std::uint64_t va, std::uint64_t bytes) { console_pull_pages(va, bytes); },
            [this](std::uint64_t table_va, std::uint32_t max_descriptors) {
                console_pull_table(table_va, max_descriptors);
            },
            [this](std::uint64_t arena_va, std::uint64_t bytes) { console_pull_arena(arena_va, bytes); });
        // Title pages a command names directly (docs/vertex_buffer_va.md): a vertex/index view or WriteBufferImmediate
        // address whose VA no mapping resolves is imported over the title's own committed pages at first use. The
        // replay side decides which spans to offer and bounds the number and bytes; the device does the Vulkan import.
        state_.context->set_title_page_importer(
            [this](std::uint64_t va, std::uint64_t bytes, VkBuffer& buffer, VkDeviceSize& offset, const char*& why) {
                return import_title_pages(va, bytes, buffer, offset, why);
            });
        // Diagnostic (docs/frame_capture.md): the frame dump's sampled-texture readback reads a draw's descriptor table
        // from the console heap mirror and resolves its first T# to the layer image set 0 holds. Reads only.
        state_.context->set_sampled_table_probe(
            [this](std::uint64_t table_va, std::uint32_t max_descriptors, palmetto::SampledTable& out) {
                return state_.sampled_table(table_va, max_descriptors, out);
            });
        // Diagnostic (docs/frame_capture.md): the constants read-through reads the title's own memory at the V# base a
        // draw names. Reads only.
        state_.context->set_title_memory_reader(
            [this](std::uint64_t va, std::uint32_t bytes, std::uint8_t* out, std::uint32_t& read) {
                return state_.read_title_memory(va, bytes, out, read);
            });
        // Diagnostic (docs/vertex_path.md): the fetch-slot dump reads the descriptors a translated shader fetches
        // itself, at heap_base + a push-constant value. Reads only, through the arena's host mapping.
        state_.context->set_device_memory_reader(
            [this](std::uint64_t address, std::uint32_t bytes, std::uint8_t* out, std::uint32_t& read) {
                return state_.read_device_memory(address, bytes, out, read);
            });
        // Diagnostic (docs/vertex_path.md): the geometry fix writes the console driver-block vertex-stream V# into the
        // arena mirror; opt-in only (PALMETTO_VERTEX_STREAM_DESCRIPTOR).
        state_.context->set_device_memory_writer(
            [this](std::uint64_t address, std::uint32_t bytes, const std::uint8_t* data) {
                return state_.write_device_memory(address, bytes, data);
            });
        state_.context->set_driver_block_allocator(
            [this](std::uint32_t bytes, std::uint64_t& address) {
                return state_.allocate_driver_block(bytes, address);
            });
        // The console driver-block V#'s base is heap-relative (docs/vertex_buffer_va.md section 18.3). A view that
        // resolves inside the GPU address arena names its arena offset; one the layer imported over the title's own pages
        // at first use is staged into an arena range, because its device address cannot be formed from heap_base.
        state_.context->set_buffer_arena_address(
            [this](VkBuffer buffer, VkDeviceSize offset, std::uint64_t& address) {
                return state_.buffer_arena_address(buffer, offset, address);
            });
        state_.context->set_title_span_stager(
            [this](std::uint64_t va, std::uint64_t bytes, std::uint64_t& address) {
                return state_.stage_title_span(va, bytes, address);
            });
        // Diagnostic (docs/frame_capture.md, the colour path): bind layer images for the T#s the compose pass fetches
        // itself out of its published matrix block. Opt-in only.
        state_.context->set_fetched_texture_binder(
            [this](std::uint64_t table_va) { state_.bind_fetched_textures(table_va); });
        // A title-placed texture's page import (docs/texture_import.md): the queue calls this before it records a
        // submission, so the image holds the title's bytes before anything samples it.
        state_.context->set_placed_texture_import([this] { state_.import_placed_textures(); });
        // Diagnostic (docs/texture_source.md): the page-writer watch for sampled placed textures, ticked once per
        // submission (it scans every 64th). Off unless PALMETTO_TEXTURE_PAGE_WATCH is set.
        state_.context->set_page_watch_tick([this] { state_.page_watch_tick(); });
        // Deferred compute-pipeline compilation (docs/deferred_compile.md): a submission that names a deferred pipeline
        // waits for the compile worker here, before the lists are prepared, on the thread that submits. A no-op unless
        // PALMETTO_PIPELINE_DEFER_COMPILE is set.
        state_.context->set_prepare_pipelines(
            [this](std::span<const palmetto::CommandList* const> lists) {
                state_.prepare_deferred_compute_pipelines(lists);
            });
        // Non-blocking submission hold (docs/peer_port_streaming.md): a submission that names an unbuilt deferred
        // pipeline is held by the queue and replayed by the hold flusher, never waited for on the title's thread.
        // Registered only when PALMETTO_PIPELINE_HOLD is set; unset leaves the queue's pipeline paths exactly as above.
        if (state_.pipeline_hold_enabled()) {
            palmetto::PipelineHoldSettings settings;
            settings.enabled = true;
            settings.bound = state_.pipeline_hold_bound();
            settings.budget_us = state_.pipeline_hold_budget_ms() * 1000;
            state_.context->set_pipeline_hold(
                [this](std::span<const palmetto::CommandList* const> lists, std::vector<std::uintptr_t>& out) {
                    return state_.collect_pending_pipelines(lists, out);
                },
                [this](std::span<const std::uintptr_t> ids) { return state_.pipelines_finished(ids); },
                [this] { state_.notify_pipeline_hold(); }, settings);
            state_.start_pipeline_hold_flusher();
            std::fprintf(stderr,
                         "[rt] d3d12: pipeline submit hold: on (bound %zu, budget %llu ms); a submission that names an "
                         "unbuilt deferred pipeline is held and replayed by the hold flusher, not waited for\n",
                         settings.bound, static_cast<unsigned long long>(settings.budget_us / 1000));
        } else if (state_.pipeline_defer_compile_enabled()) {
            std::fprintf(stderr,
                         "[rt] d3d12: pipeline submit hold: off; the submission that names an unbuilt deferred pipeline "
                         "waits for it on the submitting thread\n");
        }
        if (info.conditional_rendering) {
            const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties"));
            const auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                info.get_instance_proc(info.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
            if (properties && families) {
                VkPhysicalDeviceProperties limits{};
                properties(info.physical, &limits);
                std::uint32_t count = 0;
                families(info.physical, &count, nullptr);
                std::vector<VkQueueFamilyProperties> queues(count);
                families(info.physical, &count, queues.data());
                if (info.copy_family < count && !(queues[info.copy_family].queueFlags & VK_QUEUE_COMPUTE_BIT))
                    state_.context->set_transfer_only_family(info.copy_family);
                if (info.queue_family < count && (queues[info.queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                    state_.context->configure_predication(get_device, state_.memory, limits.limits.maxStorageBufferRange, info.queue_family);
                    if (info.storage_buffer_8bit) state_.context->configure_predicated_copy(get_device, limits.limits);
                }
            }
        }
        if (info.host_image_copy) load_host_image_copy(get_device);
        if (info.host_pointer_alignment)
            state_.get_host_pointer_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
                get_device(info.device, "vkGetMemoryHostPointerPropertiesEXT"));
        // Depth <-> colour copies are staged through scratch buffers (vulkan_scratch.h), on every GPU.
        state_.context->enable_copy_scratch(get_device, [this](const VkMemoryRequirements& requirements) {
            return state_.allocate(requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        });
        if (info.buffer_device_address && info.physical && info.get_instance_proc) {
            if (const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                    info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties"))) {
                VkPhysicalDeviceProperties props{};
                get_props(info.physical, &props);
                state_.context->configure_counted_dispatch(get_device, state_.memory, props.limits);
            }
        }
        // The copies of a D24 texture stored as D32 convert on the GPU (vulkan_depth24.h), and so do the copies of
        // a 96-bit texture, which every GPU stores as RGBA32 (D3D-62): the converter is there whether or not the
        // GPU has a native D24_UNORM_S8_UINT. It costs nothing until a copy needs it (its pipeline is made on the
        // first job, and a queue takes the tracker lock for it only when a list has such a copy).
        {
            std::uint64_t max_storage_range = 1u << 27;  // the least Vulkan allows, if the limit cannot be read
            if (info.physical && info.get_instance_proc) {
                if (const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                        info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties"))) {
                    VkPhysicalDeviceProperties props{};
                    get_props(info.physical, &props);
                    max_storage_range = props.limits.maxStorageBufferRange;
                }
            }
            state_.context->enable_depth24(get_device, [this](const VkMemoryRequirements& requirements) {
                return state_.allocate(requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            }, max_storage_range);
        }
        if (info.bindless && state_.context->loaded() && !create_bindless(get_device))
            std::fprintf(stderr, "[rt] d3d12: the bindless set 0 could not be created\n");
        query_luid_and_clock(get_device);
        // Timestamps: what the one VkQueue every D3D12 queue submits to can write.
        VkPhysicalDeviceProperties properties{};
        device_properties(info.physical, &properties);
        state_.max_sampler_lod_bias = properties.limits.maxSamplerLodBias;
        std::uint32_t family_count = 0;
        queue_families(info.physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        queue_families(info.physical, &family_count, families.data());
        if (info.queue_family < family_count) {
            const auto& family = families[info.queue_family];
            if (family.timestampValidBits)
                state_.context->set_timestamp_period(properties.limits.timestampPeriod);
            // MoltenVK (1.4.2 and 1.4.3 measured) resolves a timestamp before the write that precedes it lands, unless
            // something orders the two (docs/d3d12_vulkan.md section 4.6).
            const auto get_props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                info.get_instance_proc ? info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties2")
                                       : nullptr);
            if (family.timestampValidBits && get_props2) {
                VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
                get_props2(info.physical, &props);
                if (driver.driverID == VK_DRIVER_ID_MOLTENVK) {
                    state_.context->set_order_timestamp_copies(true);
                    log_once("timestamp resolves wait for the timestamps before them (MoltenVK); driver",
                             int(driver.driverID));
                }
            }
            if (family.queueFlags & VK_QUEUE_COMPUTE_BIT)
                state_.context->set_occlusion_resolve_memory(state_.memory);
        }

        if (info.debug_utils) {
            palmetto::DebugLabelFunctions labels;
            const auto load = [&](auto& f, const char* name) {
                f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(info.get_instance_proc(info.instance, name));
            };
            load(labels.cmd_insert, "vkCmdInsertDebugUtilsLabelEXT");
            load(labels.cmd_begin, "vkCmdBeginDebugUtilsLabelEXT");
            load(labels.cmd_end, "vkCmdEndDebugUtilsLabelEXT");
            load(labels.queue_insert, "vkQueueInsertDebugUtilsLabelEXT");
            load(labels.queue_begin, "vkQueueBeginDebugUtilsLabelEXT");
            load(labels.queue_end, "vkQueueEndDebugUtilsLabelEXT");
            state_.context->enable_debug_labels(labels);
            if (!state_.context->debug_labels())
                std::fprintf(stderr, "[rt] d3d12: VK_EXT_debug_utils entry points missing; debug markers are off\n");
        }

        const bool sparse_aliasing = state_.context->vk.vkQueueBindSparse && info.features.sparseBinding &&
            info.features.sparseResidencyAliased && properties.sparseProperties.residencyNonResidentStrict &&
            info.queue_family < family_count && (families[info.queue_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT);
        state_.sparse_image_tiles = sparse_aliasing && info.features.sparseResidencyImage2D &&
                                    state_.context->vk.vkGetImageSparseMemoryRequirements;
        if (sparse_aliasing && state_.arena.memory && info.features.sparseResidencyBuffer) {
            auto& vk = state_.context->vk;
            const auto sparse_info = state_.sparse_buffer_info(palmetto::tile_size_in_bytes);
            VkBuffer sample = VK_NULL_HANDLE;
            if (vk.vkCreateBuffer(info.device, &sparse_info, nullptr, &sample) == VK_SUCCESS) {
                VkMemoryRequirements requirements{};
                vk.vkGetBufferMemoryRequirements(info.device, sample, &requirements);
                vk.vkDestroyBuffer(info.device, sample, nullptr);
                if (!(palmetto::tile_size_in_bytes % requirements.alignment))
                    state_.sparse_buffer_memory_types = requirements.memoryTypeBits;
            }
        }
    }

    // Set 0 of every translated shader that declares bindings: images, samplers and the descriptor hash table
    // (docs section 3.4). Each ExecuteCommandLists brings it up to date before its commands are written.
    bool create_bindless(PFN_vkGetDeviceProcAddr get_device) {
        palmetto::BindlessFeatures features{};
        features.update_after_bind = true;  // VulkanDeviceInfo::bindless promises it
        features.partially_bound = state_.info.bindless_partially_bound;
        const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            state_.info.get_instance_proc(state_.info.instance, "vkGetPhysicalDeviceProperties2"));
        if (get_properties) {
            VkPhysicalDeviceVulkan12Properties indexed{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
            VkPhysicalDeviceVulkan11Properties v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &indexed};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &v11};
            get_properties(state_.info.physical, &properties);
            features.ms_samples = {VK_SAMPLE_COUNT_4_BIT, state_.ms_fallback_samples(VK_FORMAT_R32_UINT),
                                   state_.ms_fallback_samples(VK_FORMAT_R32_SINT)};
            // Past maxPerSetDescriptors, ask the driver about the grown layout (D3D-219: Intel's Xe-LP reports 6159).
            const auto layout_support = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSupport>(
                get_device(state_.info.device, "vkGetDescriptorSetLayoutSupport"));
            std::function<bool(const palmetto::BindlessFeatures&)> layout_supported;
            // MoltenVK's vkGetDescriptorSetLayoutSupport compares every layout with maxPerSetDescriptors (1212, the
            // limit without argument buffers) and rejects even 1212 itself, while it creates and samples a 65536-image,
            // 4096-sampler update-after-bind set correctly (docs/d3d12_vulkan.md section 5). There size_bindings'
            // per-type update-after-bind limits, which MoltenVK takes from Metal's argument-buffer limits, decide.
            VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 driver_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
            get_properties(state_.info.physical, &driver_properties);
            if (driver.driverID == VK_DRIVER_ID_MOLTENVK)
                layout_supported = [](const palmetto::BindlessFeatures&) { return true; };
            else if (layout_support)
                layout_supported = [&](const palmetto::BindlessFeatures& candidate) {
                    const palmetto::bindless::SetLayout layout(candidate);
                    VkDescriptorSetLayoutSupport support{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
                    layout_support(state_.info.device, &layout.info, &support);
                    return support.supported == VK_TRUE;
                };
            if (!palmetto::bindless::size_bindings(features, properties.properties.limits, v11, indexed,
                                                  layout_supported))
                std::fprintf(stderr, "[rt] d3d12: the device's descriptor limits are below the bindless set's default "
                                     "sizes; it holds less\n");
        }
        namespace b = palmetto::bindless;
        std::fprintf(stderr, "[rt] d3d12: bindless set: %u 2D images, %u samplers, %u 2D-array, %u 3D, %u 1D, "
                             "%u storage indices in 2D\n",
                     features.counts[b::sampled_binding(palmetto::ImageDimension::dim_2d)],
                     features.counts[b::sampler_binding],
                     features.counts[b::sampled_binding(palmetto::ImageDimension::dim_2d_array)],
                     features.counts[b::sampled_binding(palmetto::ImageDimension::dim_3d)],
                     features.counts[b::sampled_binding(palmetto::ImageDimension::dim_1d)],
                     features.counts[b::storage_binding(palmetto::ImageDimension::dim_2d, 0)]);
        // D3D12 promises a shader holds a full shader-visible sampler heap. Past this many distinct S#s, CreateSampler
        // logs that the array is full and a shader reads the fallback (clamping) sampler.
        if (features.counts[b::sampler_binding] < D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE + 1)
            std::fprintf(stderr, "[rt] d3d12: the bindless set holds fewer samplers than a full shader-visible "
                                 "sampler heap (%u of %u besides the fallback): its limits or "
                                 "vkGetDescriptorSetLayoutSupport allow no more\n",
                         features.counts[b::sampler_binding] - 1, unsigned(D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE));
        state_.bindless = std::make_unique<palmetto::BindlessSet>(get_device, state_.info.device, state_.memory, features);
        if (!state_.bindless->ok() || !state_.context->vk.vkCreateSampler) {
            state_.bindless.reset();
            return false;
        }
        state_.context->set_bindless_release(
            [](void* user, std::uint32_t dim, std::uint32_t idx) {
                auto* st = static_cast<DeviceState*>(user);
                std::lock_guard lock(st->descriptor_mutex);
                if (st->bindless) st->bindless->remove_image(static_cast<palmetto::ImageDimension>(dim), idx);
            },
            &state_);
        state_.context->set_bindless_prepare([this](VkCommandBuffer command_buffer) {
            std::unique_lock lock(state_.descriptor_mutex);
            if (!state_.bindless->fallbacks_ready()) state_.bindless->record_fallback_init(command_buffer);
            auto& context = *state_.context;
            if (state_.bindless_legacy_idle && state_.bindless->flush_replaces_table()) {
                // The wait of before D3D-314, kept for comparing frame times: nothing that uses the set still runs
                // after it, so the generations flush() retires are free at once.
                {
                    std::lock_guard submit(context.submit_mutex());
                    context.vk.vkDeviceWaitIdle(state_.info.device);
                }
                state_.bindless_idle_waits.fetch_add(1, std::memory_order_relaxed);
                state_.bindless->release_retired();
            } else if (state_.bindless->has_retired()) {
                // Generations of the table that an earlier flush replaced: free those whose submissions are done.
                // Nothing waits; the rest stay for a later submit. completed_retire_ticket() takes submit_mutex_,
                // which comes after the resource, tracker and descriptor locks held here.
                state_.bindless->release_retired(context.completed_retire_ticket());
                // A queue whose work is stalled (a fence only the CPU signals, long async work) keeps its ticket from
                // completing, and each replacing flush adds a generation. Past the cap wait, once the oldest is in the
                // way, for the submissions up to its ticket (the queues' timelines, not the device), with the
                // descriptor lock released: the thread that signals such a fence may need it.
                constexpr std::size_t max_retired = 8;
                if (state_.bindless->retired_count() > max_retired) {
                    if (!state_.bindless_cap_logged.exchange(true))
                        std::fprintf(stderr, "[rt] d3d12: more than %zu retired bindless table generations: waiting "
                                             "for the queues that still use the oldest\n", max_retired);
                    const std::uint64_t oldest = state_.bindless->oldest_retired_ticket();
                    lock.unlock();
                    const std::uint64_t completed = context.wait_retire_ticket(oldest);
                    lock.lock();
                    state_.bindless->release_retired(completed);
                }
            }
            // Under tracker_mutex_ (held from here to the submit) no other ExecuteCommandLists has recorded work that
            // is not yet submitted, so every submission that can have bound the set being replaced has a ticket up to
            // the one read here.
            const bool flushed = state_.bindless->flush(command_buffer, context.submitted_retire_ticket());
            if (flushed && state_.bindless->last_flush_replaced()) context.note_bindless_upload();
            return flushed;
        });
        state_.context->set_bindless_current([this]() {
            std::lock_guard lock(state_.descriptor_mutex);
            return state_.bindless ? state_.bindless->set() : VK_NULL_HANDLE;
        });
        {
            char text[8] = {};
            size_t length = 0;
            state_.bindless_legacy_idle = getenv_s(&length, text, "PALMETTO_BINDLESS_IDLE_WAIT") == 0 && length > 1 &&
                                          text[0] != '0';
        }
        state_.context->set_bindless_commit([this](bool submitted) {
            std::lock_guard lock(state_.descriptor_mutex);
            if (state_.bindless) state_.bindless->table_upload_result(submitted);
        });
        return true;
    }

    // PALMETTO_ARENA_MB sets its size (measured default 1024 MiB, at most 4095, and no more than the device's
    // maxBufferSize and maxMemoryAllocationSize allow: src/arena_size.h). The old 256 MiB default was a guess; the
    // deferred-compile boot measured a 501.8 MiB peak and a full arena answering E_OUTOFMEMORY once the loader's
    // stage ran (docs/deferred_compile.md section 9). It holds every default-heap buffer, shader-visible descriptor
    // heap and shader program. M0-M3 keep it host-visible (section 3.3, memory type policy).
    bool create_arena() {
        auto& vk = state_.context->vk;
        if (!vk.vkGetBufferDeviceAddress) return false;
        char text[16] = {};
        size_t length = 0;
        const bool set = getenv_s(&length, text, "PALMETTO_ARENA_MB") == 0 && length > 1;
        const palmetto::ArenaLimits limits{state_.info.max_buffer_size, state_.info.max_memory_allocation_size};
        palmetto::ArenaSize size = palmetto::decide_arena_size(set ? text : nullptr, limits);
        auto& arena = state_.arena;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size.bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        // Default-heap buffers live here, and any of them can be a SetPredication buffer.
        if (state_.info.conditional_rendering) info.usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
        // With ray tracing, AS result, scratch and build-input buffers are default-heap ranges of the arena.
        if (state_.info.ray_tracing)
            info.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        state_.share(info);
        VkMemoryRequirements requirements{};
        const auto create_buffer = [&] {
            if (info.size <= kArenaReserved ||
                vk.vkCreateBuffer(state_.info.device, &info, nullptr, &arena.buffer) != VK_SUCCESS) {
                arena.buffer = VK_NULL_HANDLE;
                return false;
            }
            vk.vkGetBufferMemoryRequirements(state_.info.device, arena.buffer, &requirements);
            return true;
        };
        if (!create_buffer()) return false;
        // The allocation is requirements.size, which alignment can make larger than the buffer: past
        // maxMemoryAllocationSize, make the buffer smaller by that much once.
        if (limits.max_allocation_size && requirements.size > limits.max_allocation_size) {
            vk.vkDestroyBuffer(state_.info.device, arena.buffer, nullptr);
            size = palmetto::decide_arena_size(set ? text : nullptr, limits, requirements.size - info.size);
            info.size = size.bytes;
            if (!create_buffer()) return false;
            if (requirements.size > limits.max_allocation_size) {
                vk.vkDestroyBuffer(state_.info.device, arena.buffer, nullptr);
                arena.buffer = VK_NULL_HANDLE;
                return false;
            }
        }
        if (size.capped)
            std::fprintf(stderr, "[rt] d3d12: GPU address arena: %llu MiB of the %llu MiB asked for, capped by the "
                                 "device's %s\n",
                         static_cast<unsigned long long>(size.bytes >> 20),
                         static_cast<unsigned long long>(size.requested >> 20), palmetto::arena_cap_name(size.capped));
        arena.memory = state_.allocate(requirements,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, true, &arena.memory_type);
        void* mapped = nullptr;
        if (!arena.memory || vk.vkBindBufferMemory(state_.info.device, arena.buffer, arena.memory, 0) != VK_SUCCESS ||
            vk.vkMapMemory(state_.info.device, arena.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            // Without an arena every buffer keeps its own memory; a leftover handle would make them addressable
            // with heap base 0, so their VAs would be raw device addresses above 4 GiB.
            vk.vkDestroyBuffer(state_.info.device, arena.buffer, nullptr);
            if (arena.memory) vk.vkFreeMemory(state_.info.device, arena.memory, nullptr);
            arena.buffer = VK_NULL_HANDLE;
            arena.memory = VK_NULL_HANDLE;
            return false;
        }
        arena.mapped = static_cast<std::uint8_t*>(mapped);
        std::memset(arena.mapped, 0, kArenaReserved);  // the zero page unbound vertex and index buffers read
        VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        address.buffer = arena.buffer;
        arena.address = vk.vkGetBufferDeviceAddress(state_.info.device, &address);
        arena.size = info.size;
        arena.free[kArenaReserved] = arena.size - kArenaReserved;
        arena.peak_end = kArenaReserved;
        char log[4] = {};
        length = 0;
        arena.log = getenv_s(&length, log, "PALMETTO_ARENA_LOG") == 0 && length > 1 && log[0] != '0';
        state_.context->set_heap_base(arena.address);
        state_.context->set_zero_page(arena.buffer, 0, kArenaReserved);
        return true;
    }

    ~Device() override {
        // docs/deferred_compile.md: stop the call-report thread and the compile workers before the Vulkan device goes.
        // A queued task holds a reference to its pipeline state (and through it to this device), so the queue is empty
        // by the time the device can be destroyed.
        // docs/peer_port_streaming.md: the hold flusher is joined first - it releases held submissions, which must not
        // happen while the compile workers are being stopped; then the report thread and the compile workers.
        state_.stop_pipeline_hold_flusher();
        state_.stop_pipeline_call_stats();
        state_.stop_compile_workers();
        if (state_.context) {
            {
                std::lock_guard lock(state_.context->submit_mutex());
                state_.context->vk.vkDeviceWaitIdle(state_.info.device);
            }
            state_.context->drain_deleted_resources(UINT64_MAX);
            state_.context->set_bindless_release(nullptr, nullptr);
            state_.context->set_bindless_current(nullptr);  // the retired generations go with state_.bindless below
        }
        // Every range has gone back to its block by the drain above; the blocks go before the device that owns them
        // (docs/streamed_image_backing.md). A pool left with live ranges would mean a texture outlived the device,
        // which the child references prevent.
        if (state_.image_memory_pool) {
            if (image_mem_trace_enabled()) {
                const auto stats = state_.image_memory_pool->stats();
                std::fprintf(stderr,
                             "[rt] image-mem: pool at shutdown: %llu blocks, %.1f MiB, %llu ranges live (%.1f MiB)\n",
                             static_cast<unsigned long long>(stats.blocks),
                             double(stats.block_bytes) / (1024.0 * 1024.0),
                             static_cast<unsigned long long>(stats.ranges),
                             double(stats.range_bytes) / (1024.0 * 1024.0));
            }
            state_.image_memory_pool.reset();
        }
        state_.staging_queue.reset();  // it uses the context
        if (state_.context && state_.bindless) {
            // Every texture is gone by now (each held this device), and its views with it; samplers remain.
            std::lock_guard lock(state_.descriptor_mutex);
            for (const auto& [key, sampler] : state_.sampler_entries)
                state_.context->vk.vkDestroySampler(state_.info.device, sampler, nullptr);
            state_.sampler_entries.clear();
            state_.bindless.reset();
        }
        state_.report_arena();
        if (state_.context && state_.arena.buffer) {
            auto& vk = state_.context->vk;
            vk.vkDestroyBuffer(state_.info.device, state_.arena.buffer, nullptr);
            if (state_.arena.memory) vk.vkFreeMemory(state_.info.device, state_.arena.memory, nullptr);
        }
        // The VkBuffers imported over title pages at first use (docs/vertex_buffer_va.md): the pages stay the title's,
        // so only the buffers and their imported memory go here, before the device that owns them.
        if (state_.context) {
            std::lock_guard lock(state_.imported_title_mutex);
            for (const auto& [buffer, memory] : state_.imported_title_buffers) {
                state_.context->vk.vkDestroyBuffer(state_.info.device, buffer, nullptr);
                if (memory) state_.context->vk.vkFreeMemory(state_.info.device, memory, nullptr);
            }
            state_.imported_title_buffers.clear();
        }
        // The persistent cache's final write (PALMETTO_PIPELINE_CACHE): before the handle goes, while its blob is
        // readable. The periodic save already covers a run that is killed at its timeout.
        state_.close_pipeline_cache();
        if (state_.pipeline_cache_handle)
            state_.context->vk.vkDestroyPipelineCache(state_.info.device, state_.pipeline_cache_handle, nullptr);
        state_.context.reset();
        if (owned_.device) owned_.destroy_device(owned_.device, nullptr);
        if (owned_.instance) owned_.destroy_instance(owned_.instance, nullptr);
    }

    bool ok() const { return state_.context && state_.context->loaded(); }

    // ---- The title's page content (docs/page_content.md) ---------------------------------------------------------
    //
    // The slot-133 handler runs after rt::xmem::commit_physical_range (main.cpp, set_placed_at_va_pages_hook), so the
    // title's pages are committed and the CPU can read what it wrote; the GPU, however, reads the arena range that
    // stands in for the title VA (docs/page_address_space.md). These two callbacks copy the bytes across, on demand:
    // console_pull_pages for a buffer VA the draw names (vertex/index views, root descriptors), console_pull_table for
    // the V#s inside a descriptor table the draw's root arguments name. A pull is a host copy into the arena's mapped
    // memory, so it has to happen before the submission that reads it is recorded - prepare_draw does that.
    void console_pull_pages(std::uint64_t title_va, std::uint64_t bytes) {
        if (!title_pages_enabled() || !title_va || !bytes) return;
        std::uint64_t arena_va = 0, available = 0;
        if (!state_.context->console_va_span(title_va, palmetto::ConsoleVaKind::buffer, arena_va, available)) return;
        if (arena_va >= state_.arena.size) return;
        const std::uint64_t copy = std::min({bytes, available, state_.arena.size - arena_va});
        if (!copy) return;
        std::uint8_t* destination = state_.arena.mapped + arena_va;
        const auto source_base = static_cast<std::uintptr_t>(title_va);
        std::uint64_t done = 0, copied = 0;
        while (done < copy) {
            MEMORY_BASIC_INFORMATION info{};
            const auto source = reinterpret_cast<const void*>(source_base + done);
            if (::VirtualQuery(source, &info, sizeof(info)) != sizeof(info)) break;
            const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            std::uint64_t chunk = copy - done;
            if (region_end > source_base + done) chunk = std::min<std::uint64_t>(chunk, region_end - (source_base + done));
            // A page the title has not committed (xmem commits on first touch) is left as the stand-in has it.
            if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE))) {
                std::memcpy(destination + done, source, std::size_t(chunk));
                copied += chunk;
            }
            done += chunk ? chunk : 0x1000;
        }
        if (!copied) return;
        state_.context->count_console_pages(copied);
        const std::uint64_t pulls = state_.pages_pulled.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t total = state_.pages_pulled_bytes.fetch_add(copied, std::memory_order_relaxed) + copied;
        // Bounded progress diagnostic: the copy cost is only visible as a running total, since a boot is killed before
        // the device's release report.
        if (pulls % 65536 == 0)
            std::fprintf(stderr, "[rt] d3d12: page content: %llu pull(s), %.1f MiB copied\n",
                         static_cast<unsigned long long>(pulls), double(total) / double(1 << 20));
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
            // Whether the bytes are the title's own data, not stand-in zeros: a count of non-zero bytes in what was
            // copied (no byte values are logged).
            std::size_t nonzero = 0;
            for (std::uint64_t i = 0; i < copied; ++i)
                if (destination[i]) ++nonzero;
            std::fprintf(stderr,
                         "[rt] d3d12: page pull: title VA 0x%llx + 0x%llx -> arena VA 0x%llx, %zu non-zero byte(s)\n",
                         static_cast<unsigned long long>(title_va), static_cast<unsigned long long>(copied),
                         static_cast<unsigned long long>(arena_va), nonzero);
        }
    }

    // ---- Title pages a command names directly (docs/vertex_buffer_va.md) ---------------------------------------
    // Xbox unified memory: the title can bind a vertex or index view to pages it wrote itself, with no D3D12 resource
    // behind them, so no creation path registered the range. The measured range is MEM_COMMIT / PAGE_READWRITE and every
    // page of it is readable, and by the time stage-3 content draws the GPU address arena is full (a 36 MiB slot-133
    // stand-in is refused with E_OUTOFMEMORY in the same boot), so an arena stand-in cannot serve the span. The device
    // imports the pages with VK_EXT_external_memory_host, exactly as the caller's own pages are already imported for
    // device slot 133 and OpenExistingHeapFromAddress: the buffer is the title's memory, so the CPU's writes are the
    // GPU's reads and no copy is needed. The caller bounds how many ranges are imported and their total bytes.
    bool import_title_pages(std::uint64_t va, std::uint64_t bytes, VkBuffer& out_buffer, VkDeviceSize& out_offset,
                            const char*& why) {
        out_buffer = VK_NULL_HANDLE;
        out_offset = 0;
        why = "";
        if (!state_.context || !state_.context->loaded()) {
            why = "the Vulkan context is not loaded";
            return false;
        }
        if (!state_.get_host_pointer_properties || !state_.info.host_pointer_alignment) {
            why = "the device has no VK_EXT_external_memory_host import";
            return false;
        }
        const VkDeviceSize alignment = state_.info.host_pointer_alignment;
        if (!alignment || (alignment & (alignment - 1)) || !bytes || !va) {
            why = "the host pointer alignment or the span is unusable";
            return false;
        }
        const std::uintptr_t start = static_cast<std::uintptr_t>(va);
        if (start > (std::numeric_limits<std::uintptr_t>::max)() - bytes) {
            why = "the span wraps the address space";
            return false;
        }
        const std::uintptr_t base = start & ~static_cast<std::uintptr_t>(alignment - 1);
        const VkDeviceSize offset = start - base;
        if (offset + bytes > UINT64_MAX - alignment) {
            why = "the span wraps the address space";
            return false;
        }
        const VkDeviceSize allocation = (offset + bytes + alignment - 1) & ~(alignment - 1);
        // The whole imported range must be committed and not write-combined: the same rule the slot-133 import applies
        // to the caller's pages. A page not committed yet cannot be imported, and the span stays refused by name.
        for (VkDeviceSize at = 0; at < allocation;) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<const void*>(base + at), &region, sizeof(region)) ||
                region.State != MEM_COMMIT || (region.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE))) {
                why = "the pages are not committed host memory the GPU can import";
                return false;
            }
            const auto region_end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
            if (region_end <= base + at) {
                why = "the pages are not committed host memory the GPU can import";
                return false;
            }
            const std::uint64_t chunk = std::min<std::uint64_t>(allocation - at, region_end - (base + at));
            at += chunk ? chunk : alignment;
        }
        auto& vk = state_.context->vk;
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        const bool need_bda = state_.info.buffer_device_address;
        if (need_bda) info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        state_.share(info);
        VkBuffer buffer = VK_NULL_HANDLE;
        if (vk.vkCreateBuffer(state_.info.device, &info, nullptr, &buffer) != VK_SUCCESS) {
            why = "the buffer over the title's pages could not be created";
            return false;
        }
        const auto fail = [this, &buffer, &why](const char* reason) {
            state_.context->vk.vkDestroyBuffer(state_.info.device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            why = reason;
            return false;
        };
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(state_.info.device, buffer, &requirements);
        VkMemoryHostPointerPropertiesEXT host{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (state_.get_host_pointer_properties(state_.info.device,
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                               reinterpret_cast<const void*>(base), &host) != VK_SUCCESS)
            return fail("the driver reported no importable memory type for the pages");
        const std::uint32_t types = host.memoryTypeBits & requirements.memoryTypeBits;
        if (!types || (offset % requirements.alignment) || allocation < offset + requirements.size)
            return fail("the pages' alignment or room does not fit the buffer");
        VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import.pHostPointer = reinterpret_cast<void*>(base);
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.pNext = &import;
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        for (const VkMemoryPropertyFlags want :
             {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
              VkMemoryPropertyFlags(0)}) {
            for (std::uint32_t i = 0; i < state_.memory.memoryTypeCount && !memory; ++i) {
                if (!(types & (1u << i)) || (state_.memory.memoryTypes[i].propertyFlags & want) != want) continue;
                VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                              need_bda ? static_cast<const void*>(&flags)
                                                       : static_cast<const void*>(&import)};
                allocate.allocationSize = allocation;
                allocate.memoryTypeIndex = i;
                if (vk.vkAllocateMemory(state_.info.device, &allocate, nullptr, &memory) != VK_SUCCESS)
                    memory = VK_NULL_HANDLE;
            }
            if (memory) break;
        }
        if (!memory) return fail("importing the title's pages failed (no memory type took the range)");
        if (vk.vkBindBufferMemory(state_.info.device, buffer, memory, offset) != VK_SUCCESS) {
            vk.vkFreeMemory(state_.info.device, memory, nullptr);
            return fail("the buffer could not be bound to the title's pages");
        }
        {
            std::lock_guard lock(state_.imported_title_mutex);
            state_.imported_title_buffers.emplace_back(buffer, memory);
        }
        out_buffer = buffer;
        out_offset = offset;
        return true;
    }

    // A descriptor table's VA: read its slots from the console heap's mirror and pull the buffer data each V# names.
    // A V# whose base the page-address-space work rebased holds the arena VA, so the reverse map gives the title span
    // the bytes still live in; an unrebased V# base is a title VA already.
    void console_pull_table(std::uint64_t table_va, std::uint32_t max_descriptors) {
        if (!title_pages_enabled() || !table_va) return;
        std::uint64_t mirror = 0, available = 0;
        if (!state_.context->console_va_span(table_va, palmetto::ConsoleVaKind::descriptors, mirror, available)) return;
        if (mirror >= state_.arena.size) return;
        const std::uint32_t slots = std::min<std::uint32_t>(
            max_descriptors, std::uint32_t(std::min<std::uint64_t>(available, state_.arena.size - mirror) / kDescriptorSize));
        std::uint32_t decoded = 0, title_named = 0, other = 0;
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            const auto* words = reinterpret_cast<const std::uint32_t*>(state_.arena.mapped + mirror +
                                                                       VkDeviceSize(slot) * kDescriptorSize);
            palmetto::gfx10::Buffer buffer{};
            const palmetto::gfx10::BufferWords descriptor{words[0], words[1], words[2], words[3]};
            if (!palmetto::gfx10::decode(descriptor, buffer) || !buffer.address) continue;
            ++decoded;
            const std::uint64_t bytes = buffer.stride ? std::uint64_t(buffer.stride) * buffer.num_records
                                                      : std::uint64_t(buffer.num_records);
            if (!bytes) continue;
            std::uint64_t title = 0, room = 0;
            if (state_.context->console_arena_span(buffer.address, title, room)) {
                ++title_named;
                console_pull_pages(title, std::min<std::uint64_t>(bytes, 1u << 20));
                continue;
            }
            // Not yet rebased (a slot this submission did not refresh): the base is the title VA itself.
            if (state_.context->console_va_span(buffer.address, palmetto::ConsoleVaKind::buffer, title, room)) {
                ++title_named;
                console_pull_pages(buffer.address, std::min<std::uint64_t>(bytes, 1u << 20));
            } else {
                ++other;
            }
        }
        // Bounded diagnostic: whether the constants/images a draw reads still name the title's pages (pulled here) or
        // ordinary buffers the GPU already sees (nothing to pull).
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 12)
            std::fprintf(stderr,
                         "[rt] d3d12: page table: VA 0x%llx, %u slot(s), %u V#, %u title-named (pulled), %u other\n",
                         static_cast<unsigned long long>(table_va), slots, decoded, title_named, other);
    }

    // A copy's source range, by its arena VA (docs/page_content.md): a buffer -> image upload or a buffer copy reads
    // the arena stand-in, whose bytes are the title's pages only once they have been pulled. The reverse map finds the
    // title span; an arena VA no title range stands in for (an ordinary buffer the title filled through the API) is a
    // no-op.
    void console_pull_arena(std::uint64_t arena_va, std::uint64_t bytes) {
        if (!title_pages_enabled() || !arena_va || !bytes) return;
        const std::uint64_t requests = state_.copy_source_requests.fetch_add(1, std::memory_order_relaxed) + 1;
        std::uint64_t title = 0, available = 0;
        const bool resolved = state_.context->console_arena_span(arena_va, title, available);
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
            std::fprintf(stderr,
                         "[rt] d3d12: page pull (copy source): arena VA 0x%llx + 0x%llx, %s\n",
                         static_cast<unsigned long long>(arena_va), static_cast<unsigned long long>(bytes),
                         resolved ? "a title range (pulled)" : "not a title range (no pull needed)");
        if (!resolved) return;
        console_pull_pages(title, std::min(bytes, available));
        const std::uint64_t pulls = state_.copy_source_pulls.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t total =
            state_.copy_source_bytes.fetch_add(std::min(bytes, available), std::memory_order_relaxed) +
            std::min(bytes, available);
        // The copy-source pulls say whether the title's texture data comes through copies of its pages at all, so a
        // sampling pass that still renders black can be told apart from "nothing was pulled".
        if (pulls % 2048 == 0)
            std::fprintf(stderr, "[rt] d3d12: page pull (copy source) totals: %llu pull(s), %.1f MiB\n",
                         static_cast<unsigned long long>(pulls), double(total) / double(1 << 20));
        (void)requests;
    }
    const VulkanDeviceInfo& info() const { return state_.info; }
    // Acceleration-structure resources work: KHR builds, or the XDXR records (which need the GPU address arena).
    bool acceleration_structures() const {
        return state_.info.ray_tracing || (state_.info.xdxr_acceleration_structures && state_.info.buffer_device_address);
    }
    bool upload_in_vram() const { return state_.upload_in_vram; }
    palmetto::VulkanContext& context() { return *state_.context; }
    const palmetto::BindlessSet* bindless() const { return state_.bindless.get(); }
    const DeviceState& state() const { return state_; }
    IUnknown* holder() { return identity(); }
    bool bind_sampler(const palmetto::gfx10::SamplerWords& words) { return state_.bind_sampler(words); }
    void own(const OwnedRuntime& owned) { owned_ = owned; }
    void set_outer(IUnknown* outer) { outer_ = outer; }

    UINT STDMETHODCALLTYPE GetNodeCount() override { return 1; }

    // The Vulkan physical device's LUID, which is the DXGI adapter's on Windows. Zero (logged) when the driver has
    // none to report (deviceLUIDValid false).
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return state_.luid; }

    // The CUSTOM equivalent of a standard heap type, from the D3D12 documentation's table for the architecture
    // D3D12_FEATURE_ARCHITECTURE reports. NUMA (a discrete GPU): DEFAULT is GPU-only memory in L1, UPLOAD
    // write-combined and READBACK write-back system memory in L0, GPU_UPLOAD write-combined L1. UMA (an integrated
    // GPU, CacheCoherentUMA FALSE): the same pages, all in L0, as the host runtime answers on a Radeon 780M (AMD
    // 32.0.13030.13001). The pool only names the answer; memory is chosen by the CPU page property either way. CUSTOM
    // and unknown types have
    // no equivalent and get an all-zero answer (D3D12 reports them through the debug layer), with node 0 in both node
    // masks as the host runtime gives it (d3d12_fence_timing_test compares CUSTOM, an unknown type and node mask 2).
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT node_mask, D3D12_HEAP_TYPE type) override {
        D3D12_HEAP_PROPERTIES properties{};
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        if (node_mask > 1) {
            log_once("GetCustomHeapProperties node mask", int(node_mask));
            return properties;
        }
        switch (type) {
        case D3D12_HEAP_TYPE_DEFAULT:
            properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
            properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L1;
            break;
        case D3D12_HEAP_TYPE_UPLOAD:
            properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
            properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
            break;
        case D3D12_HEAP_TYPE_READBACK:
            properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
            properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
            break;
        case D3D12_HEAP_TYPE_GPU_UPLOAD:
            properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
            properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L1;
            break;
        default:
            log_once("GetCustomHeapProperties heap type without a custom equivalent:", int(type));
            return properties;
        }
        if (state_.uma) properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        return properties;
    }

    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(ID3D12Fence* const* fences, const UINT64* values,
                                                                UINT count, D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags,
                                                                HANDLE event) override {
        return set_event_on_multiple_fence_completion(*state_.context, fences, values, count, flags, event, identity());
    }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
        if (removed_.load(std::memory_order_acquire)) return DXGI_ERROR_DEVICE_REMOVED;
        return (state_.context && state_.context->is_device_lost()) ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
    }

    // Residency and power (D3D-33, docs/d3d12_vulkan.md section 4.1). Vulkan has no residency API: memory from
    // vkAllocateMemory stays usable by the device until vkFreeMemory, and paging under pressure is the kernel
    // driver's job (WDDM on Windows), invisible to the application. So every pageable object of this device is
    // always resident, MakeResident has nothing to do and Evict, which D3D12 documents as a hint the runtime may
    // defer, can be dropped. Only the arguments are checked; the objects themselves are not touched.
    HRESULT STDMETHODCALLTYPE MakeResident(UINT count, ID3D12Pageable* const* objects) override {
        return count && !objects ? E_INVALIDARG : S_OK;
    }

    HRESULT STDMETHODCALLTYPE Evict(UINT count, ID3D12Pageable* const* objects) override {
        return count && !objects ? E_INVALIDARG : S_OK;
    }

    // Device1: priorities only order what the OS evicts first under memory pressure. VK_EXT_pageable_device_local_memory
    // could pass them on, but it is optional and changes no result, so they are accepted and dropped. Any UINT is a
    // valid priority (the D3D12_RESIDENCY_PRIORITY values are named points, the low 24 bits a refinement). As on the
    // host runtime (d3d12_residency_test, RTX 3060 / 610.88), only objects that own their memory take one: committed
    // resources and heaps. A placed resource (a reserved one too, by the same rule; not measured), a descriptor heap, a query heap or a fence is E_INVALIDARG.
    // The host also removes its device then (DXGI_ERROR_INVALID_CALL); the layer doesn't, since a console title that
    // sets priorities on its placed resources would lose its device over a hint.
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(UINT count, ID3D12Pageable* const* objects,
                                                   const D3D12_RESIDENCY_PRIORITY* priorities) override {
        if (count && (!objects || !priorities)) return E_INVALIDARG;
        for (UINT i = 0; i < count; ++i) {
            if (from<Heap>(objects[i])) continue;
            const Resource* resource = from<Resource>(objects[i]);
            if (!resource || !resource->committed()) return E_INVALIDARG;
        }
        return S_OK;
    }

    // Device3: the asynchronous MakeResident. The objects are already resident, so the work is complete at once
    // and the fence is signalled from the CPU now. The caller's queue Wait on it must not hang, which is why the
    // signal is kept although the residency part is a no-op. DENY_OVERBUDGET cannot fail: nothing is paged in. A
    // null fence is accepted, as the host runtime does: there is nothing to signal.
    HRESULT STDMETHODCALLTYPE EnqueueMakeResident(D3D12_RESIDENCY_FLAGS flags, UINT count,
                                                  ID3D12Pageable* const* objects, ID3D12Fence* fence,
                                                  UINT64 value) override {
        if ((count && !objects) || (flags & ~D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET)) return E_INVALIDARG;
        return fence ? fence->Signal(value) : S_OK;
    }

    // Stable power state pins GPU clocks for profiling, a developer-mode feature on Windows. Vulkan has no clock
    // control (vendor tools do it outside the API), and the game's frame does not depend on the clocks, so the
    // request is accepted. Success is what D3D12 returns on a machine where the call is allowed.
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) override { return S_OK; }

    // Device6: lets the driver run background shader recompilation and profiling-guided optimisation. Vulkan
    // drivers do that, if at all, without an API to steer it, so there is nothing to start or stop. The completion
    // event is set at once, since no measurement is running, and no further measurements are wanted.
    HRESULT STDMETHODCALLTYPE SetBackgroundProcessingMode(D3D12_BACKGROUND_PROCESSING_MODE mode,
                                                          D3D12_MEASUREMENTS_ACTION action, HANDLE event,
                                                          BOOL* further_measurements) override {
        if (unsigned(mode) > unsigned(D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_PROFILING_BY_SYSTEM) ||
            unsigned(action) > unsigned(D3D12_MEASUREMENTS_ACTION_DISCARD_PREVIOUS))
            return E_INVALIDARG;
        if (further_measurements) *further_measurements = FALSE;
        if (event) SetEvent(event);
        return S_OK;
    }

    // STUB
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(UINT* count, D3D12_META_COMMAND_DESC*) override {
        if (!count) return E_INVALIDARG;
        *count = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(REFGUID, D3D12_META_COMMAND_PARAMETER_STAGE, UINT*,
                                                             UINT*, D3D12_META_COMMAND_PARAMETER_DESC*) override {
        return E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE CreateMetaCommand(REFGUID, UINT, const void*, SIZE_T, REFIID, void** out) override {
        if (out) *out = nullptr;
        return E_INVALIDARG;
    }

    // STUB
    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(
        D3D12_SERIALIZED_DATA_TYPE type, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER*) override {
        return type == D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE
                   ? D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED
                   : D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
    }

    // STUB
    void STDMETHODCALLTYPE RemoveDevice() override { removed_.store(true, std::memory_order_release); }

    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid,
                                                 void** out) override {
        constexpr auto method = "ID3D12Device::CreateCommandQueue";
        if (!desc) return d3d12_trace::result(method, E_INVALIDARG);
        // As the host runtime: no bundle queues, the three D3D12 priorities (realtime refused, below), one flag and
        // one node. A valid description without an output is E_UNEXPECTED there, not a capability check.
        if (desc->Type == D3D12_COMMAND_LIST_TYPE_BUNDLE || UINT(desc->Type) > D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE ||
            (desc->Priority != D3D12_COMMAND_QUEUE_PRIORITY_NORMAL &&
             desc->Priority != D3D12_COMMAND_QUEUE_PRIORITY_HIGH &&
             desc->Priority != D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) ||
            (desc->Flags & ~D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT) || desc->NodeMask > 1) {
            log_once("CreateCommandQueue refused (type, priority, flags or node mask), type", desc->Type);
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        // Global realtime priority needs SeIncreaseBasePriorityPrivilege enabled in the effective token, as on the
        // Windows D3D12 runtime; without it, this answer. Vulkan queue priorities (and VK_KHR_global_priority) are
        // fixed at vkCreateDevice time before CreateCommandQueue is called, so a privileged request leases from the
        // device's pre-created VkQueue pool like D3D12_COMMAND_QUEUE_PRIORITY_HIGH and logs the approximation once.
        if (desc->Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME && !has_global_realtime_privilege()) {
            log_once("CreateCommandQueue refused (global realtime priority), type", desc->Type);
            return d3d12_trace::result(method, DXGI_ERROR_ACCESS_DENIED);
        }
        if (!out) return d3d12_trace::result(method, E_UNEXPECTED);
        *out = nullptr;
        if (desc->Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc->Type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
            desc->Type != D3D12_COMMAND_LIST_TYPE_COPY) return d3d12_trace::result(method, E_INVALIDARG);
        if (desc->Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME)
            log_once("CreateCommandQueue: global realtime priority runs on a standard-priority Vulkan queue, type",
                     desc->Type);
        // Caller-owned devices that only provide the legacy queue cannot acquire more handles from their VkDevice.
        if (state_.info.command_queues.empty())
            return d3d12_trace::result(method, create_command_queue(*state_.context, state_.info.queue,
                                                                     state_.info.queue_family, *desc, riid, out,
                                                                     identity(), state_.clock));
        // A VkQueue of the type's own family first, then the graphics family; shared when every one is in use.
        const bool compute = desc->Type == D3D12_COMMAND_LIST_TYPE_COMPUTE;
        const bool copy = desc->Type == D3D12_COMMAND_LIST_TYPE_COPY;
        auto& own_users = compute ? compute_users_ : copy_users_;
        const auto& own_queues = compute ? state_.info.compute_queues : state_.info.copy_queues;
        const std::uint32_t own_family = compute ? state_.info.compute_family : state_.info.copy_family;
        const std::span<const std::uint32_t> own =
            compute || copy ? std::span<const std::uint32_t>(own_users) : std::span<const std::uint32_t>();
        std::vector<std::uint32_t>* users = nullptr;
        std::size_t slot = 0;
        std::uint32_t sharing = 0;  // other D3D12 queues already on the chosen VkQueue
        {
            std::lock_guard lock(queue_mutex_);
            const auto chosen = palmetto::pick_queue_slot({own, queue_users_});
            if (chosen.pool == SIZE_MAX) return d3d12_trace::result(method, E_OUTOFMEMORY);  // no VkQueue at all
            users = chosen.pool == 0 ? &own_users : &queue_users_;
            slot = chosen.index;
            sharing = (*users)[slot]++;
        }
        const bool own_pool = users != &queue_users_;
        if (sharing)
            log_once("CreateCommandQueue: every Vulkan queue of the family is in use; this D3D12 queue shares one with "
                     "the others on it (their batches run in submission order), type", desc->Type);
        // Child holds a device reference until after the lease has been released.
        std::shared_ptr<void> lease(nullptr, [this, users, slot](void*) {
            std::lock_guard lock(queue_mutex_);
            --(*users)[slot];
        });
        const VkQueue queue = own_pool ? own_queues[slot] : state_.info.command_queues[slot];
        const std::uint32_t family = own_pool ? own_family : state_.info.queue_family;
        return d3d12_trace::result(method, create_command_queue(*state_.context, queue, family, *desc, riid, out,
                                                                 identity(), state_.clock, std::move(lease)));
    }

    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** out) override {
        constexpr auto method = "ID3D12Device::CreateCommandAllocator";
        return d3d12_trace::result(method, create_command_allocator(type, riid, out, identity()));
    }

    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                                ID3D12CommandAllocator* allocator, ID3D12PipelineState* state,
                                                REFIID riid, void** out) override {
        constexpr auto method = "ID3D12Device::CreateCommandList";
        if (out) *out = nullptr;
        if (node_mask & ~1u) return d3d12_trace::result(method, E_INVALIDARG);  // one node, as the host answers
        // The initial pipeline state is set as SetPipelineState would set it (a bundle's is its first deferred call).
        HRESULT hr = create_command_list(type, allocator, riid, out, identity(), state);
        return d3d12_trace::result(method, hr);
    }

    // Device4: a list created closed, without an allocator.
    HRESULT STDMETHODCALLTYPE CreateCommandList1(UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                                 D3D12_COMMAND_LIST_FLAGS, REFIID riid, void** out) override {
        constexpr auto method = "ID3D12Device4::CreateCommandList1";
        // The host runtime's order ("command list1:" cases): one node and a type up to VIDEO_ENCODE (non-zero
        // D3D12_COMMAND_LIST_FLAGS are ignored by the host runtime), then a null output is E_UNEXPECTED.
        if (out) *out = nullptr;
        if ((node_mask & ~1u) || UINT(type) > UINT(D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE))
            return d3d12_trace::result(method, E_INVALIDARG);
        if (!out) return d3d12_trace::result(method, E_UNEXPECTED);
        ID3D12CommandAllocator* allocator = nullptr;
        HRESULT hr = create_command_allocator(type, IID_PPV_ARGS(&allocator), identity());
        if (FAILED(hr)) return d3d12_trace::result(method, hr);
        ID3D12GraphicsCommandList* list = nullptr;
        hr = create_command_list(type, allocator, IID_PPV_ARGS(&list), identity());
        allocator->Release();
        if (FAILED(hr)) return d3d12_trace::result(method, hr);
        list->Close();
        hr = list->QueryInterface(riid, out);
        list->Release();
        return d3d12_trace::result(method, hr);
    }

    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void** out) override {
        constexpr auto method = "ID3D12Device::CreateFence";
        // As the host runtime: known flags only, cross-adapter only when shared, and no non-monitored fences
        // (refused there even with both sharing flags). A null output is a capability check.
        constexpr UINT known = D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER |
                               D3D12_FENCE_FLAG_NON_MONITORED;
        if ((UINT(flags) & ~known) || (flags & D3D12_FENCE_FLAG_NON_MONITORED) ||
            ((flags & D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER) && !(flags & D3D12_FENCE_FLAG_SHARED))) {
            log_once("CreateFence refused, flags", int(flags));
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        if (!out) return d3d12_trace::result(method, S_FALSE);
        return d3d12_trace::result(method, create_fence(*state_.context, initial, flags, riid, out,
                                                        identity()));
    }

    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                                     ID3D12RootSignature* root_signature, REFIID riid,
                                                     void** out) override {
        if (out) *out = nullptr;
        // The host refuses these with E_INVALIDARG (compare_invalid_arguments, "command signature:"): one node, a
        // known type per argument, exactly one draw or dispatch and it comes last, a root signature for root
        // arguments, and a stride that is a multiple of 4 and holds every argument.
        if (!desc || desc->NumArgumentDescs == 0 || !desc->pArgumentDescs || (desc->NodeMask & ~1u))
            return E_INVALIDARG;
        UINT64 size = 0;  // 64-bit: a large Num32BitValuesToSet must not wrap past ByteStride
        for (UINT i = 0; i < desc->NumArgumentDescs; ++i) {
            const D3D12_INDIRECT_ARGUMENT_DESC& arg = desc->pArgumentDescs[i];
            switch (arg.Type) {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: size += sizeof(D3D12_DRAW_ARGUMENTS); break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: size += sizeof(D3D12_DRAW_INDEXED_ARGUMENTS); break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: size += sizeof(D3D12_DISPATCH_ARGUMENTS); break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS: size += sizeof(D3D12_DISPATCH_RAYS_DESC); break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH: size += sizeof(D3D12_DISPATCH_MESH_ARGUMENTS); break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: size += sizeof(D3D12_VERTEX_BUFFER_VIEW); break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: size += sizeof(D3D12_INDEX_BUFFER_VIEW); break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: size += UINT64{4} * arg.Constant.Num32BitValuesToSet; break;
            case compat::kIndirectArgumentIncrementingConstant: size += 4; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: size += sizeof(D3D12_GPU_VIRTUAL_ADDRESS); break;
            default: return E_INVALIDARG;
            }
            const bool draw = arg.Type <= D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH ||
                              arg.Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS ||
                              arg.Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
            const bool root_argument = !draw && arg.Type != D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW &&
                                       arg.Type != D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
            if (draw != (i + 1 == desc->NumArgumentDescs) || (root_argument && !root_signature)) return E_INVALIDARG;
        }
        if (desc->ByteStride % 4 || desc->ByteStride < size) return E_INVALIDARG;
        if (!out) return E_UNEXPECTED;  // the host's answer for a valid description and no output
        palmetto::IndirectKind kind;
        switch (desc->pArgumentDescs[desc->NumArgumentDescs - 1].Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: kind = palmetto::IndirectKind::draw; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: kind = palmetto::IndirectKind::draw_indexed; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: kind = palmetto::IndirectKind::dispatch; break;
        default: return E_NOTIMPL;
        }
        return create_command_signature(kind, desc->ByteStride, riid, out, identity());
    }

    void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* desc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* out) override {
        if (!out) return;
        *out = {};
        if (!desc) return;
        if (desc->Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL &&
            desc->Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
            return;
        if (desc->DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY &&
            desc->DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS)
            return;
        const bool is_tlas = (desc->Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL);
        if (!is_tlas && desc->NumDescs > 0) {
            if (desc->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY) {
                if (!desc->pGeometryDescs) return;
            } else {
                if (!desc->ppGeometryDescs) return;
                for (UINT i = 0; i < desc->NumDescs; ++i)
                    if (!desc->ppGeometryDescs[i]) return;
            }
        }
        auto& vk = state_.context->vk;
        if (state_.context->xdxr_acceleration_structures()) {
            // XDXR records: the layout depends only on the triangle or instance count and on whether the nodes are
            // BOX16 (MINIMIZE_MEMORY, bottom level; xdxr_build.h). Its size has room for the most nodes a tree of
            // that many elements can have; a build's own size, in its header, is what COMPACTED_SIZE reports. An
            // update refits (no sort); the build's scratch size covers it.
            palmetto::BuildAccelerationStructure build{};
            build.is_tlas = is_tlas;
            if (desc->Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY)
                build.build_flags |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
            if (build.is_tlas) {
                build.instance_count = desc->NumDescs;
            } else {
                for (UINT i = 0; i < desc->NumDescs; ++i) {
                    const D3D12_RAYTRACING_GEOMETRY_DESC* gd =
                        (desc->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS)
                            ? desc->ppGeometryDescs[i]
                            : &desc->pGeometryDescs[i];
                    if (gd->Type != D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) continue;
                    palmetto::RtTriangleGeometry t{};
                    t.vertex_count = gd->Triangles.VertexCount;
                    t.index_count = gd->Triangles.IndexCount;
                    t.index_type = gd->Triangles.IndexFormat == DXGI_FORMAT_UNKNOWN ? 0u : 1u;
                    build.triangles.push_back(t);
                }
            }
            palmetto::XdxrLayout layout;
            if (!layout.compute(palmetto::VulkanContext::xdxr_elements(build), build.is_tlas,
                                palmetto::VulkanContext::xdxr_box16(build)))
                return;
            out->ResultDataMaxSizeInBytes = layout.size;
            out->ScratchDataSizeInBytes = layout.scratch;
            out->UpdateScratchDataSizeInBytes = layout.scratch;
            return;
        }
        if (!state_.info.ray_tracing || !vk.vkGetAccelerationStructureBuildSizesKHR) return;

        std::vector<VkAccelerationStructureGeometryKHR> geoms;
        std::vector<std::uint32_t> max_prims;
        if (is_tlas) {
            VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            g.geometry.instances.arrayOfPointers =
                desc->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS ? VK_TRUE : VK_FALSE;
            geoms.push_back(g);
            max_prims.push_back(desc->NumDescs);
        } else {
            for (UINT i = 0; i < desc->NumDescs; ++i) {
                const D3D12_RAYTRACING_GEOMETRY_DESC* gd =
                    (desc->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS)
                        ? (desc->ppGeometryDescs ? desc->ppGeometryDescs[i] : nullptr)
                        : (desc->pGeometryDescs ? &desc->pGeometryDescs[i] : nullptr);
                if (!gd || gd->Type != D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) continue;
                VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                g.flags = (gd->Flags & D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE) ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0u;
                auto& tr = g.geometry.triangles;
                tr.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                tr.vertexFormat = (gd->Triangles.VertexFormat == DXGI_FORMAT_R32G32_FLOAT) ? VK_FORMAT_R32G32_SFLOAT
                                                                                           : VK_FORMAT_R32G32B32_SFLOAT;
                tr.vertexStride = gd->Triangles.VertexBuffer.StrideInBytes;
                tr.maxVertex = gd->Triangles.VertexCount ? (gd->Triangles.VertexCount - 1u) : 0u;
                tr.indexType = (gd->Triangles.IndexFormat == DXGI_FORMAT_R16_UINT)
                                   ? VK_INDEX_TYPE_UINT16
                                   : ((gd->Triangles.IndexFormat == DXGI_FORMAT_R32_UINT) ? VK_INDEX_TYPE_UINT32
                                                                                          : VK_INDEX_TYPE_NONE_KHR);
                geoms.push_back(g);
                const std::uint32_t prims =
                    (gd->Triangles.IndexFormat != DXGI_FORMAT_UNKNOWN && gd->Triangles.IndexCount >= 3)
                        ? (gd->Triangles.IndexCount / 3u)
                        : (gd->Triangles.VertexCount / 3u);
                max_prims.push_back(prims);
            }
        }
        if (geoms.empty()) return;

        VkAccelerationStructureBuildGeometryInfoKHR bgi{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bgi.type = is_tlas ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                           : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (desc->Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
            bgi.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        if (desc->Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE)
            bgi.flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        if (desc->Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD)
            bgi.flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        if (desc->Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY)   // as the build gets it
            bgi.flags |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
        bgi.geometryCount = static_cast<std::uint32_t>(geoms.size());
        bgi.pGeometries = geoms.data();

        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vk.vkGetAccelerationStructureBuildSizesKHR(state_.info.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                   &bgi, max_prims.data(), &sizes);
        out->ResultDataMaxSizeInBytes = sizes.accelerationStructureSize;
        out->ScratchDataSizeInBytes = sizes.buildScratchSize;
        out->UpdateScratchDataSizeInBytes = sizes.updateScratchSize;
    }

    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid,
                                                   void** out) override {
        constexpr auto method = "ID3D12Device::CreateDescriptorHeap";
        if (out) *out = nullptr;
        if (!desc) return d3d12_trace::result(method, E_INVALIDARG);
        // As the host runtime: a null output is a capability check, answered before the rules below (S_FALSE even
        // for a shader-visible RTV heap). Zero descriptors are allowed, RTV and DSV heaps are never shader-visible,
        // one flag and one node.
        if (!out) return d3d12_trace::result(method, S_FALSE);
        if ((desc->Flags & ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) || desc->NodeMask > 1 ||
            ((desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
             (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV))) {
            log_once("CreateDescriptorHeap refused (flags or node mask), type", desc->Type);
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        auto* heap = new DescriptorHeap(state_, *desc);
        heap->attach_device(identity());
        if (!heap->ok()) {
            heap->Release();
            return d3d12_trace::result(method, E_OUTOFMEMORY);
        }
        return d3d12_trace::result(method, hand_out(heap, riid, out));
    }

    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** out) override {
        if (out) *out = nullptr;
        // The host runtime's order (compare_invalid_arguments, "query heap:"): the description first, refused with
        // E_INVALIDARG for no queries, a node other than node 0 or a type past PIPELINE_STATISTICS1 (7; 6 has no
        // name in the SDK but the host takes it), then E_UNEXPECTED for a null output. A known type this layer
        // can't create stays E_NOTIMPL.
        if (!desc || !desc->Count || desc->NodeMask > 1 ||
            UINT(desc->Type) > UINT(D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1))
            return E_INVALIDARG;
        if (!out) return E_UNEXPECTED;
        auto* heap = new QueryHeap(state_);
        heap->attach_device(identity());
        if (const HRESULT hr = heap->create(*desc); FAILED(hr)) {
            heap->Release();
            return hr;
        }
        return hand_out(heap, riid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT node_mask, const void* blob, SIZE_T size, REFIID riid,
                                                  void** out) override {
        // The host runtime answers a null output with S_FALSE before it looks at the blob or the node mask
        // (compare_invalid_arguments, "root signature:"). Otherwise it refuses a node other than node 0, and a DXBC
        // container whose own size (the dword at +24) is not the size passed.
        if (!out) return S_FALSE;
        *out = nullptr;
        if (!blob || !size || (node_mask & ~1u)) return E_INVALIDARG;
        const auto* bytes = static_cast<const std::uint8_t*>(blob);
        if (size >= 28 && std::memcmp(bytes, "DXBC", 4) == 0) {
            std::uint32_t container_size = 0;
            std::memcpy(&container_size, bytes + 24, sizeof(container_size));
            if (container_size != size) return E_INVALIDARG;
        }
        // The repository's root-signature model (port/src/root_signature.h): parse the RTS0 blob, then pack
        // the parameters into the 32 user-data dwords. pack() refuses what it cannot place (more than 32 dwords:
        // the console spills those to memory, in a layout that is still open), and so does this.
        const rt::rootsig::DecodeResult parsed = rt::rootsig::parse_rts0(bytes, size);
        if (!parsed.ok) {
            std::fprintf(stderr, "[rt] d3d12: root signature not understood: %s\n", parsed.error.c_str());
            return E_INVALIDARG;
        }
        // Two layouts, one per bind point (RootArgumentLayout): graphics keeps pack(), compute takes the console
        // layout pack_stage() measured (D3D-448). The signature is refused only when neither can place it.
        const rt::rootsig::PackResult packed = rt::rootsig::pack(parsed.sig);
        const rt::rootsig::StagePackResult compute = rt::rootsig::pack_stage(parsed.sig, rt::rootsig::Stage::Compute);
        if (!packed.ok && !compute.ok) {
            std::fprintf(stderr, "[rt] d3d12: root signature refused: %s (compute: %s)\n", packed.error.c_str(),
                         compute.error.c_str());
            return E_INVALIDARG;
        }
        if (!packed.ok)
            std::fprintf(stderr, "[rt] d3d12: root signature usable for compute only: %s\n", packed.error.c_str());
        if (!compute.ok)
            std::fprintf(stderr, "[rt] d3d12: root signature usable for graphics only: %s\n", compute.error.c_str());
        RootArgumentLayout layout;
        layout.flags = parsed.sig.flags;
        layout.graphics_ok = packed.ok;
        layout.compute_ok = compute.ok;
        for (std::size_t i = 0; i < parsed.sig.params.size(); ++i) {
            std::uint32_t first = UINT32_MAX;
            for (std::uint32_t d = 0; packed.ok && d < packed.dword_count; ++d)
                if (packed.provenance[d].param_index == i) {
                    first = d;
                    break;
                }
            layout.parameters.push_back({parsed.sig.params[i].type, first, parsed.sig.params[i].num_values,
                                         compute.ok ? compute.first_sgpr[i] : rt::rootsig::kNoSgpr});
        }
        // The console graphics user-SGPR layout (docs/root_arguments.md). The graphics feed path writes the PC packing
        // (from s0, two dwords per table) today, but a translated vertex/pixel shader reads each root parameter at its
        // console slot - base s8, one dword per descriptor table - so it finds zero there. Both layouts are carried so
        // the feed path can fill the console slots from the same values (PALMETTO_GRAPHICS_CONSOLE_ROOT, diagnostic;
        // the mesh base s16 is not fed by this path yet).
        {
            rt::graphics_pso::ConsoleUserDataPlacement console;
            const bool placed = rt::graphics_pso::console_graphics_user_data(parsed.sig, false, &console);
            layout.console_ok = placed;
            layout.console_base = console.base;
            for (std::size_t i = 0; i < layout.parameters.size() && i < console.first_sgpr.size(); ++i)
                layout.parameters[i].console_offset = console.first_sgpr[i];
        }
        register_static_samplers(parsed.sig);
        auto* signature = new RootSignature(
            std::move(layout), palmetto::sha256_hex({static_cast<const std::uint8_t*>(blob), size}));
        signature->attach_device(identity());
        return hand_out(signature, riid, out);
    }

    // PC slot 11; the Xbox ABI reaches it through device slots 62 and 67 (the proxy forwards).
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid,
                                                         void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!desc) return E_INVALIDARG;
        palmetto::cached_pso::Key key = pipeline_key(*desc);
        if (const HRESULT hr = check_cached_pso(state_.pso_identity, key, desc->CachedPSO); FAILED(hr)) return hr;
        auto* pipeline = new PipelineState(state_);
        pipeline->attach_device(identity());
        // The recipe cache keys on this content key, so it has to be in place before create_compute asks for an entry.
        pipeline->set_cache_key(std::move(key));
        // A foreign root signature is hashed by address, not by content: never share such a PSO.
        pipeline->set_cache_key_deterministic(desc->pRootSignature == nullptr ||
                                              RootSignature::from<RootSignature>(desc->pRootSignature) != nullptr);
        if (const HRESULT hr = pipeline->create_compute(*desc); FAILED(hr)) {
            pipeline->Release();
            return hr;
        }
        return hand_out(pipeline, riid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid,
                                                          void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!desc) return E_INVALIDARG;
        palmetto::cached_pso::Key key = pipeline_key(*desc);
        if (const HRESULT hr = check_cached_pso(state_.pso_identity, key, desc->CachedPSO); FAILED(hr)) return hr;
        auto* pipeline = new PipelineState(state_);
        pipeline->attach_device(identity());
        // The recipe cache keys on this content key, so it has to be in place before create_graphics asks for an entry.
        pipeline->set_cache_key(std::move(key));
        // A foreign root signature is hashed by address, not by content: never share such a PSO.
        pipeline->set_cache_key_deterministic(desc->pRootSignature == nullptr ||
                                              RootSignature::from<RootSignature>(desc->pRootSignature) != nullptr);
        if (const HRESULT hr = pipeline->create_graphics(*desc); FAILED(hr)) {
            pipeline->Release();
            return hr;
        }
        return hand_out(pipeline, riid, out);
    }

    // Device1: in-memory pipeline library (docs/d3d12_vulkan.md sections 3.6 and 4).
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void* blob, SIZE_T size, REFIID riid, void** out) override {
        constexpr auto method = "ID3D12Device1::CreatePipelineLibrary";
        // A refused blob leaves *out as it was, as the host runtime does; a wrong interface nulls it (hand_out).
        if (size > 0) {
            const HRESULT checked = PipelineLibrary::check_blob(blob, size);
            if (FAILED(checked)) return d3d12_trace::result(method, checked);
        }
        if (!out) return d3d12_trace::result(method, S_FALSE);
        *out = nullptr;
        auto* lib = new PipelineLibrary();
        lib->attach_device(identity());
        return d3d12_trace::result(method, hand_out(lib, riid, out));
    }

    // Device9: process-wide shader cache sessions (in-memory and disk-backed) and control (D3D-34).
    HRESULT STDMETHODCALLTYPE CreateShaderCacheSession(const D3D12_SHADER_CACHE_SESSION_DESC* desc, REFIID riid,
                                                       void** out) override {
        constexpr auto method = "ID3D12Device9::CreateShaderCacheSession";
        if (out) *out = nullptr;
        if (!desc) return d3d12_trace::result(method, E_INVALIDARG);
        const GUID zero_guid{};
        if (std::memcmp(&desc->Identifier, &zero_guid, sizeof(GUID)) == 0)
            return d3d12_trace::result(method, E_INVALIDARG);
        if (desc->Mode != D3D12_SHADER_CACHE_MODE_MEMORY && desc->Mode != D3D12_SHADER_CACHE_MODE_DISK)
            return d3d12_trace::result(method, E_INVALIDARG);
        constexpr UINT kKnownFlags = D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED | D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR;
        if (UINT(desc->Flags) & ~kKnownFlags) return d3d12_trace::result(method, E_INVALIDARG);
        if (!out) return d3d12_trace::result(method, S_FALSE);
        std::wstring disk_path;
        if (desc->Mode == D3D12_SHADER_CACHE_MODE_DISK) {
            VkPhysicalDeviceProperties props{};
            if (state_.info.get_instance_proc && state_.info.instance && state_.info.physical) {
                if (const auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                        state_.info.get_instance_proc(state_.info.instance, "vkGetPhysicalDeviceProperties")))
                    get_props(state_.info.physical, &props);
            }
            const auto path = shader_cache_disk_path(*desc, props);
            if (path.empty()) return d3d12_trace::result(method, E_FAIL);
            disk_path = path.wstring();
        }
        const ShaderCacheKey key{desc->Identifier, desc->Mode, std::move(disk_path)};
        auto& registry = process_shader_cache_registry();
        const UINT max_file_bytes = desc->MaximumValueFileSizeBytes ? desc->MaximumValueFileSizeBytes
                                                                    : ShaderCacheStore::kDefaultMaxValueFileBytes;
        std::shared_ptr<ShaderCacheStore> store;
        if (const HRESULT hr = registry.acquire(key, desc->Version, max_file_bytes, store); FAILED(hr))
            return d3d12_trace::result(method, hr);
        auto* session = new ShaderCacheSession(*desc, key, registry, std::move(store));
        session->attach_device(identity());
        return d3d12_trace::result(method, hand_out(session, riid, out));
    }

    HRESULT STDMETHODCALLTYPE ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS kinds,
                                                 D3D12_SHADER_CACHE_CONTROL_FLAGS control) override {
        constexpr auto method = "ID3D12Device9::ShaderCacheControl";
        constexpr UINT kKnownKinds = D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CACHE_FOR_DRIVER |
                                     D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CONVERSIONS |
                                     D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED |
                                     D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED;
        constexpr UINT kKnownControls = D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE |
                                        D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE | D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR;
        if (!kinds || (UINT(kinds) & ~kKnownKinds) || !control || (UINT(control) & ~kKnownControls))
            return d3d12_trace::result(method, E_INVALIDARG);
        if ((control & D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE) && (control & D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE))
            return d3d12_trace::result(method, E_INVALIDARG);
        auto& registry = process_shader_cache_registry();
        if (kinds & D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED) {
            if (control & D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR) registry.clear_all();
            if (control & D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE)
                registry.disabled.store(true, std::memory_order_relaxed);
            if (control & D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE)
                registry.disabled.store(false, std::memory_order_relaxed);
        }
        return d3d12_trace::result(method, S_OK);
    }

    // ID3D12Device2: a PSO from a pipeline state stream. A CS makes a compute PSO, an MS a mesh PSO, a VS the same
    // graphics PSO CreateGraphicsPipelineState makes (parse_pipeline_stream).
    HRESULT STDMETHODCALLTYPE CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid,
                                                  void** out) override {
        constexpr const char* method = "ID3D12Device2::CreatePipelineState";
        if (!out) return d3d12_trace::result(method, E_POINTER);
        *out = nullptr;
        if (!desc) return d3d12_trace::result(method, E_INVALIDARG);
        PipelineStream stream;
        if (const HRESULT hr = parse_pipeline_stream(*desc, stream); FAILED(hr)) return d3d12_trace::result(method, hr);
        auto* pipeline = new PipelineState(state_);
        pipeline->attach_device(identity());
        const HRESULT hr = stream.has_cs ? pipeline->create_compute(stream.compute)
                                         : pipeline->create_graphics(stream.graphics, &stream.ms, &stream.as);
        if (FAILED(hr)) {
            pipeline->Release();
            return d3d12_trace::result(method, hr);
        }
        return d3d12_trace::result(method, hand_out(pipeline, riid, out));
    }

    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override {
        return kDescriptorSize;
    }

    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        d3d12_trace::record("ID3D12Device::CreateRenderTargetView");
        if (!destination.ptr) return;
        palmetto::TargetView view{};
        palmetto::ResourceId rid = 0;
        D3D12_RESOURCE_DESC rd{};
        const Resource* r = Resource::from<Resource>(resource);
        if (r) {
            rid = r->id();
            rd = const_cast<Resource*>(r)->GetDesc();
        } else if (resource && state_.context->has_resource(resource_id(resource))) {
            rid = resource_id(resource);
            rd = resource->GetDesc();
        }
        if (rid) {
            view = {rid, 0, 0, rd.DepthOrArraySize};
            if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                view = volume_rtv(rid, rd, desc);
            } else if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE1D) {
                view = line_target_view(rid, rd, desc->Texture1D.MipSlice, 0, 1);
            } else if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE1DARRAY) {
                view = line_target_view(rid, rd, desc->Texture1DArray.MipSlice,
                                        desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize);
            } else if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) {
                view = {rid, desc->Texture2D.MipSlice, 0, 1};
            } else if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMS) {
                view = {rid, 0, 0, 1};
            } else if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) {
                view = {rid, desc->Texture2DArray.MipSlice, desc->Texture2DArray.FirstArraySlice,
                        view_slices(rd, desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize)};
            } else if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY) {
                // Multisample textures have one mip, so only the slice range is chosen (as for the DSV).
                view = {rid, 0, desc->Texture2DMSArray.FirstArraySlice,
                        view_slices(rd, desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize)};
            } else if (desc) {
                log_once("render-target view dimension not supported:", desc->ViewDimension);
                view = {};
            }
            // A TYPELESS color texture of any family takes RTVs in its family's formats (D3D-275; RGBA8 only before).
            const bool typeless_color = vulkan_format(rd.Format, false).typeless &&
                                        !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            const bool typed_rgba8 = rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || rd.Format == DXGI_FORMAT_R8G8B8A8_UINT;
            // An RGBA8 or BGRA8 texture whose castable list names the RTV's format in the other channel order, or
            // across B8G8R8X8 and B8G8R8A8, gets an RTV in that format.
            const DXGI_FORMAT asked = desc && desc->Format != DXGI_FORMAT_UNKNOWN ? desc->Format : rd.Format;
            const bool cast_listed = r && asked != rd.Format && rgba8_any_order(rd.Format) && rgba8_any_order(asked) &&
                                     r->lists_castable_format(asked);
            // A typed BGRA8 or B8G8R8X8 texture with a castable list refuses an RTV in another format the list
            // doesn't name, as a typed RGBA8 texture does. Without a list it keeps the image's format, as before.
            const bool typed_bgra8_listed = r && !typed_rgba8 && rgba8_any_order(rd.Format) &&
                                            r->has_castable_formats();
            if (rd.Format == DXGI_FORMAT_A8_UNORM || (desc && desc->Format == DXGI_FORMAT_A8_UNORM)) {
                view = {};
            } else if (r && r->uint_backed() == VK_FORMAT_A1R5G5B5_UNORM_PACK16) {
                // An R16_UINT-backed B5G5R5A1 UAV texture (Format::uint_backed) renders through an A1R5G5B5 view, in
                // its own format only.
                if (desc && desc->Format != DXGI_FORMAT_UNKNOWN && desc->Format != rd.Format)
                    view = {};
                else
                    view.format = static_cast<palmetto::ColorViewFormat>(VK_FORMAT_A1R5G5B5_UNORM_PACK16);
            } else if (r && (r->uint_backed() == VK_FORMAT_R8G8B8A8_SRGB || r->uint_backed() == VK_FORMAT_B8G8R8A8_SRGB)) {
                // A UNORM-backed 8-bit sRGB UAV texture renders through an sRGB view (encoding as the host does), in its
                // own format only.
                if (desc && desc->Format != DXGI_FORMAT_UNKNOWN && desc->Format != rd.Format)
                    view = {};
                else
                    view.format = static_cast<palmetto::ColorViewFormat>(r->uint_backed());
            } else if (rd.Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP ||
                       (desc && desc->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP)) {
                if (r && r->uint_backed() == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 &&
                    (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
                    // The Series-S upscale output (docs/decomp_renderer.md section 12): the console's R9G9B9E5 render
                    // target is hosted as a mutable R32_UINT image because this GPU has no E5B9G9R9 attachment or
                    // storage, so there is no attachment view of the format the title's pipeline names. Refused by
                    // name; the resource's SRV and UAV views are served by the UAV-only emulation.
                    log_once("R9G9B9E5_SHAREDEXP render-target view refused: the console's render target is hosted as a "
                             "mutable R32_UINT image on this GPU; format",
                             int(asked));
                    view = {};
                } else if (rd.Format != DXGI_FORMAT_R9G9B9E5_SHAREDEXP ||
                           !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
                           (desc && desc->Format != DXGI_FORMAT_UNKNOWN && desc->Format != rd.Format)) {
                    view = {};
                }
            } else if (typeless_color || typed_rgba8 || cast_listed || typed_bgra8_listed) {
                const DXGI_FORMAT requested = asked;
                // A typed UNORM RGBA8 resource with a castable list may be viewed in a listed family format
                // (D3D-85's committed path, D3D-90's placed one): its image is mutable and lists the view
                // format, so the reinterpretation is the typeless path's. Only a UNORM image is advertised,
                // as that is what the clear path reinterprets.
                const bool listed = requested == rd.Format ||
                                    (r && r->has_view_format_list() &&
                                     r->allows_view_format(vulkan_format(requested, false).format));
                if (typeless_color || cast_listed ||
                    (requested != rd.Format && listed && rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM)) {
                    const bool msaa = rd.SampleDesc.Count > 1;
                    const bool plain_1d = !msaa && desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE1D;
                    const bool array_1d = !msaa && desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                    const bool plain_2d = !msaa && desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D;
                    const bool array_2d = !msaa && desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    const bool ms = msaa && desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMS;
                    const bool ms_array = msaa && desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    const UINT plane = plain_2d ? desc->Texture2D.PlaneSlice :
                        array_2d ? desc->Texture2DArray.PlaneSlice : 0;
                    const bool dim_ok = ((plain_1d || array_1d) && rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) ||
                                        ((plain_2d || array_2d || ms || ms_array) &&
                                         rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
                    // Subtraction keeps malformed layer ranges from overflowing during validation.
                    if (!dim_ok || plane || !view.resource || view.mip >= rd.MipLevels ||
                        !view.layer_count || view.first_layer >= rd.DepthOrArraySize ||
                        view.layer_count > rd.DepthOrArraySize - view.first_layer ||
                        !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
                        view = {};
                    } else {
                        // The view's own format, of the resource's family (B8G8R8X8 and B8G8R8A8 are two families
                        // that share Vulkan formats) or named by its castable list; the clear and the attachment
                        // view reinterpret the image in it.
                        const Format named = vulkan_format(requested, false);
                        const int family = palmetto::detail::typeless_view_family(named.format);
                        const bool same_family =
                            alpha_unused(requested) == alpha_unused(rd.Format) &&
                            family == palmetto::detail::typeless_view_family(vulkan_format(rd.Format, false).format);
                        if (named.typeless || named.rgb96 || !family || !(same_family || cast_listed))
                            view = {};
                        else
                            view.format = static_cast<palmetto::ColorViewFormat>(named.format);
                        if (view.resource) {
                            const auto format = vulkan_format(requested, false);
                            VkFormatProperties properties{};
                            state_.format_properties(state_.info.physical, format.format, &properties);
                            if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) view = {};
                        }
                    }
                } else if (requested != rd.Format) {
                    view = {}; // No typed-resource format casting is advertised by this backend.
                }
                if (!view.resource) log_once("unsupported typeless or RGBA8 RTV format/subresource:", int(requested));
            }
        } else if (resource) {
            stub("ID3D12Device::CreateRenderTargetView(resource from another device)");
        }
        std::memcpy(reinterpret_cast<void*>(destination.ptr), &view, sizeof(view));
    }

    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        d3d12_trace::record("ID3D12Device::CreateDepthStencilView");
        if (!destination.ptr) return;
        palmetto::TargetView view{};
        if (const Resource* r = Resource::from<Resource>(resource)) {
            const D3D12_RESOURCE_DESC rd = const_cast<Resource*>(r)->GetDesc();
            view = {r->id(), 0, 0, rd.DepthOrArraySize};
            if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE1D) {
                view = line_target_view(r->id(), rd, desc->Texture1D.MipSlice, 0, 1);
            } else if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE1DARRAY) {
                view = line_target_view(r->id(), rd, desc->Texture1DArray.MipSlice,
                                        desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize);
            } else if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D) {
                view = {r->id(), desc->Texture2D.MipSlice, 0, 1};
            } else if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMS) {
                view = {r->id(), 0, 0, 1};
            } else if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY) {
                view = {r->id(), desc->Texture2DArray.MipSlice, desc->Texture2DArray.FirstArraySlice,
                        view_slices(rd, desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize)};
            } else if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY) {
                // Multisample textures have one mip, so only the slice range is chosen.
                view = {r->id(), 0, desc->Texture2DMSArray.FirstArraySlice,
                        view_slices(rd, desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize)};
            } else if (desc) {
                log_once("depth-stencil view dimension not supported:", desc->ViewDimension);
                view = {};
            }
            if (r->desc().Format == DXGI_FORMAT_A8_UNORM || (desc && desc->Format == DXGI_FORMAT_A8_UNORM)) view = {};
        } else if (resource) {
            stub("ID3D12Device::CreateDepthStencilView(resource from another device)");
        }
        std::memcpy(reinterpret_cast<void*>(destination.ptr), &view, sizeof(view));
    }

    // A descriptor is its slot's bytes (a TargetView in RTV and DSV heaps, otherwise what the shader loads), so a
    // copy is a byte copy. D3D12 walks the destination and source ranges together, one descriptor at a time; a
    // null size array means every range holds one descriptor. Whichever side runs out first ends the copy.
    void STDMETHODCALLTYPE CopyDescriptors(UINT destination_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,
                                           const UINT* destination_sizes, UINT source_ranges,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* source_sizes,
                                           D3D12_DESCRIPTOR_HEAP_TYPE) override {
        if (!destinations || !sources) return;
        UINT d = 0, s = 0, d_at = 0, s_at = 0;  // the range on each side, and the position in it
        while (d < destination_ranges && s < source_ranges) {
            if (d_at >= (destination_sizes ? destination_sizes[d] : 1) || !destinations[d].ptr) {
                ++d;
                d_at = 0;
            } else if (s_at >= (source_sizes ? source_sizes[s] : 1) || !sources[s].ptr) {
                ++s;
                s_at = 0;
            } else {
                std::memmove(reinterpret_cast<std::uint8_t*>(destinations[d].ptr) + std::size_t(d_at++) * kDescriptorSize,
                             reinterpret_cast<const std::uint8_t*>(sources[s].ptr) + std::size_t(s_at++) * kDescriptorSize,
                             kDescriptorSize);
            }
        }
    }

    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE source,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE) override {
        if (!destination.ptr || !source.ptr) return;
        std::memmove(reinterpret_cast<void*>(destination.ptr), reinterpret_cast<const void*>(source.ptr),
                     std::size_t(count) * kDescriptorSize);
    }

    // A constant-buffer view is a 16-byte GFX10 raw buffer V# in the first half of the slot (zeros in the rest).
    // A null description, zero address, or zero size leaves the whole slot zeroed (null V#: reads return 0).
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        d3d12_trace::record("ID3D12Device::CreateConstantBufferView");
        if (!destination.ptr) return;
        std::memset(reinterpret_cast<void*>(destination.ptr), 0, kDescriptorSize);
        if (!desc || !desc->BufferLocation || !desc->SizeInBytes) return;
        palmetto::gfx10::Buffer buffer{};
        buffer.address = desc->BufferLocation;
        buffer.stride = 0;
        buffer.num_records = desc->SizeInBytes;
        buffer.format = palmetto::gfx10::format_from_vulkan(VK_FORMAT_R32_UINT);
        buffer.oob_select = palmetto::gfx10::OobSelect::raw;
        const palmetto::gfx10::BufferWords words = palmetto::gfx10::encode(buffer);
        std::memcpy(reinterpret_cast<void*>(destination.ptr), words.data(), sizeof(words));
    }

    // A shader-resource view of a texture is a T# in the slot (docs section 3.4): the shader loads those eight
    // dwords through its descriptor table and finds the image by them. A buffer SRV writes a 16-byte GFX10 V# in the
    // first half of the slot (raw, structured, or typed). A slot left as zeros is a null descriptor.
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* resource,
                                                    const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        d3d12_trace::record("ID3D12Device::CreateShaderResourceView");
        using palmetto::gfx10::ImageType;
        if (!destination.ptr) return;
        std::memset(reinterpret_cast<void*>(destination.ptr), 0, kDescriptorSize);
        const Resource* r = Resource::from<Resource>(resource);
        if (!r) {
            if (resource) stub("ID3D12Device::CreateShaderResourceView(resource from another device)");
            return;
        }
        if (r->desc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) {
            log_once("shader-resource view denied by resource flags; format", int(r->desc().Format));
            return;
        }
        if (!r->image()) {
            if (!desc) {
                log_once("buffer SRV requires a description; width", int(r->desc().Width));
                return;
            }
            if (desc->ViewDimension != D3D12_SRV_DIMENSION_BUFFER) {
                log_once("shader-resource view dimension not supported for buffer:", desc->ViewDimension);
                return;
            }
            const UINT64 base_va = const_cast<Resource*>(r)->GetGPUVirtualAddress();
            const UINT64 width = r->desc().Width;
            if (!base_va || !desc->Buffer.NumElements || (desc->Buffer.Flags & ~D3D12_BUFFER_SRV_FLAG_RAW)) return;
            palmetto::gfx10::Buffer buffer{};
            if (desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) {
                if (desc->Format != DXGI_FORMAT_R32_TYPELESS || desc->Buffer.StructureByteStride != 0) return;
                if (desc->Buffer.FirstElement > width / 4) return;
                const UINT64 byte_off = desc->Buffer.FirstElement * 4;
                const UINT64 byte_size = UINT64(desc->Buffer.NumElements) * 4;
                if (byte_size > width - byte_off || byte_size > UINT32_MAX) return;
                buffer.address = base_va + byte_off;
                buffer.stride = 0;
                buffer.num_records = std::uint32_t(byte_size);
                buffer.format = palmetto::gfx10::format_from_vulkan(VK_FORMAT_R32_UINT);
                buffer.oob_select = palmetto::gfx10::OobSelect::raw;
            } else if (desc->Buffer.StructureByteStride != 0) {
                if (desc->Format != DXGI_FORMAT_UNKNOWN || desc->Buffer.StructureByteStride > 16383) return;
                const UINT64 stride = desc->Buffer.StructureByteStride;
                if (desc->Buffer.FirstElement > width / stride) return;
                const UINT64 byte_off = desc->Buffer.FirstElement * stride;
                const UINT64 byte_size = UINT64(desc->Buffer.NumElements) * stride;
                if (byte_size > width - byte_off) return;
                buffer.address = base_va + byte_off;
                buffer.stride = std::uint32_t(stride);
                buffer.num_records = desc->Buffer.NumElements;
                buffer.format = palmetto::gfx10::format_from_vulkan(VK_FORMAT_R32_UINT);
                buffer.oob_select = palmetto::gfx10::OobSelect::structured;
            } else {
                const Format fmt = vulkan_format(desc->Format, false);
                if (fmt.format == VK_FORMAT_UNDEFINED || fmt.typeless || !(fmt.aspects & VK_IMAGE_ASPECT_COLOR_BIT)) {
                    log_once("buffer SRV format not supported:", int(desc->Format));
                    return;
                }
                const VkFormat named = fmt.rgb96 ? rgb96_named_format(fmt.format) : fmt.format;
                const std::uint32_t code = palmetto::gfx10::format_from_vulkan(named);
                const palmetto::BlockFormat bf = footprint_block(fmt, VK_IMAGE_ASPECT_COLOR_BIT);
                if (!code || bf.block != 1 || !bf.bytes || bf.bytes > 16383) {
                    log_once("buffer SRV format not supported:", int(desc->Format));
                    return;
                }
                const UINT64 stride = bf.bytes;
                if (desc->Buffer.FirstElement > width / stride) return;
                const UINT64 byte_off = desc->Buffer.FirstElement * stride;
                const UINT64 byte_size = UINT64(desc->Buffer.NumElements) * stride;
                if (byte_size > width - byte_off) return;
                buffer.address = base_va + byte_off;
                buffer.stride = std::uint32_t(stride);
                buffer.num_records = desc->Buffer.NumElements;
                buffer.format = code;
                const palmetto::gfx10::Swizzle channels = palmetto::gfx10::format_swizzle(named);
                for (unsigned i = 0; i < 4; ++i) {
                    const UINT from = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(i, desc->Shader4ComponentMapping);
                    buffer.swizzle[i] = from < 4 ? channels[from]
                                        : from == D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0
                                            ? palmetto::gfx10::select_zero
                                            : palmetto::gfx10::select_one;
                }
                buffer.oob_select = palmetto::gfx10::OobSelect::structured;
            }
            const palmetto::gfx10::BufferWords words = palmetto::gfx10::encode(buffer);
            std::memcpy(reinterpret_cast<void*>(destination.ptr), words.data(), sizeof(words));
            return;
        }
        TextureView view{};
        if (!desc) {
            const bool array = r->desc().DepthOrArraySize > 1;
            if (r->desc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && !array) {
                view.type = ImageType::image_1d;
                view.layer_count = 1;
            } else if (r->desc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                view.type = ImageType::image_3d;
            else if (r->desc().SampleDesc.Count > 1)
                view.type = array ? ImageType::image_2d_msaa_array : ImageType::image_2d_msaa;
            else if (array)
                view.type = ImageType::image_2d_array;
        } else {
            view.format = desc->Format;
            view.mapping = desc->Shader4ComponentMapping;
            switch (desc->ViewDimension) {
            case D3D12_SRV_DIMENSION_TEXTURE1D:
                view.type = ImageType::image_1d;
                view.first_mip = desc->Texture1D.MostDetailedMip;
                view.mip_count = desc->Texture1D.MipLevels;
                view.layer_count = 1;
                view.min_lod = desc->Texture1D.ResourceMinLODClamp;
                break;
            case D3D12_SRV_DIMENSION_TEXTURE3D:
                view.type = palmetto::gfx10::ImageType::image_3d;
                view.first_mip = desc->Texture3D.MostDetailedMip;
                view.mip_count = desc->Texture3D.MipLevels;
                view.min_lod = desc->Texture3D.ResourceMinLODClamp;
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2DMS:
                view.type = ImageType::image_2d_msaa;
                view.layer_count = 1;
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
                view.type = ImageType::image_2d_msaa_array;
                view.first_layer = desc->Texture2DMSArray.FirstArraySlice;
                view.layer_count = desc->Texture2DMSArray.ArraySize;
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2D:
                view.first_mip = desc->Texture2D.MostDetailedMip;
                view.mip_count = desc->Texture2D.MipLevels;
                view.layer_count = 1;
                view.plane = desc->Texture2D.PlaneSlice;
                view.min_lod = desc->Texture2D.ResourceMinLODClamp;
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
                view.type = ImageType::image_2d_array;
                view.first_mip = desc->Texture2DArray.MostDetailedMip;
                view.mip_count = desc->Texture2DArray.MipLevels;
                view.first_layer = desc->Texture2DArray.FirstArraySlice;
                view.layer_count = desc->Texture2DArray.ArraySize;
                view.plane = desc->Texture2DArray.PlaneSlice;
                view.min_lod = desc->Texture2DArray.ResourceMinLODClamp;
                break;
            case D3D12_SRV_DIMENSION_TEXTURECUBE:
                view.type = ImageType::cube;
                view.first_mip = desc->TextureCube.MostDetailedMip;
                view.mip_count = desc->TextureCube.MipLevels;
                view.layer_count = 6;
                view.min_lod = desc->TextureCube.ResourceMinLODClamp;
                break;
            case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
                view.type = ImageType::cube;
                view.first_mip = desc->TextureCubeArray.MostDetailedMip;
                view.mip_count = desc->TextureCubeArray.MipLevels;
                view.first_layer = desc->TextureCubeArray.First2DArrayFace;
                view.layer_count = desc->TextureCubeArray.NumCubes < ~0u / 6 ? desc->TextureCubeArray.NumCubes * 6 : ~0u;
                view.min_lod = desc->TextureCubeArray.ResourceMinLODClamp;
                break;
            default:
                log_once("shader-resource view dimension not supported:", desc->ViewDimension);
                return;
            }
        }
        write_image_descriptor(*r, view, destination, false);
    }

    // An unordered-access view of a buffer is a V# in the slot (docs section 3.4): the shader loads those four dwords
    // through its descriptor table and reaches the bytes by the console VA in them. A structured view (format
    // UNKNOWN and a stride) is `stride` and a record count, a raw one (R32_TYPELESS with the RAW flag) is a byte count,
    // a typed one (any other format) is the format's GFX10_FORMAT code with the element size as the stride and an
    // element count: the V# a buffer_load_format / buffer_store_format / buffer_atomic reads (OOB_SELECT 1, so the
    // index is checked against NUM_RECORDS, docs/buffer_bounds.md). A slot left as zeros is a null descriptor.
    // A UAV counter (docs section 3.4, "UAV counters") is a second, raw V# of 4 bytes in the slot's upper 16 bytes: the
    // counter resource's address plus CounterOffsetInBytes. Append and consume are atomics a shader does on it.
    void write_buffer_uav(ID3D12Resource* resource, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                          D3D12_CPU_DESCRIPTOR_HANDLE destination) {
        namespace g = palmetto::gfx10;
        const D3D12_RESOURCE_DESC& rd = Resource::from<Resource>(resource)->desc();
        if (!(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
            log_once("unordered-access view of a buffer created without ALLOW_UNORDERED_ACCESS; flags", int(rd.Flags));
            return;
        }
        if (!desc || desc->ViewDimension != D3D12_UAV_DIMENSION_BUFFER) {
            log_once("unordered-access view of a buffer needs a buffer view description; dimension",
                     desc ? int(desc->ViewDimension) : -1);
            return;
        }
        const D3D12_BUFFER_UAV& view = desc->Buffer;
        const bool structured = desc->Format == DXGI_FORMAT_UNKNOWN;
        const bool raw = desc->Format == DXGI_FORMAT_R32_TYPELESS && (view.Flags & D3D12_BUFFER_UAV_FLAG_RAW);
        const bool typed = !structured && !raw;
        std::uint32_t format_code = 0, element_bytes = 0;
        g::Swizzle channels = g::identity_swizzle;
        if (typed) {
            // D3D12: a typed view has no RAW flag; its format is one of the typed-UAV formats the layer can name.
            const Format mapped = vulkan_format(desc->Format, false);
            const auto* layout = mapped.format != VK_FORMAT_UNDEFINED && !mapped.typeless && !mapped.rgb96 &&
                                         !(view.Flags & D3D12_BUFFER_UAV_FLAG_RAW)
                                     ? palmetto::uav_clear::layout(mapped.format)
                                     : nullptr;
            format_code = layout ? g::format_from_vulkan(mapped.format) : 0;
            if (!format_code || format_code >= 128) {  // the V#'s FORMAT is 7 bits
                log_once("typed buffer unordered-access view format not supported; format", int(desc->Format));
                return;
            }
            element_bytes = layout->bytes;
            channels = g::format_swizzle(mapped.format);
        }
        const UINT64 unit = structured ? view.StructureByteStride : typed ? element_bytes : 4;
        if (!unit || unit > 0x3fff || view.FirstElement > rd.Width / unit ||
            (UINT64(view.FirstElement) + view.NumElements) > rd.Width / unit ||
            UINT64(view.NumElements) * (structured || typed ? 1 : 4) > ~0u) {
            log_once("unordered-access view of a buffer outside the resource or with a bad stride; elements",
                     int(view.NumElements));
            return;
        }
        // D3D12: a counter belongs to a structured view and sits in a buffer that allows unordered access, 4096-byte
        // aligned with room for its 4 bytes; with no counter resource the offset is 0.
        if (counter || view.CounterOffsetInBytes) {
            const Resource* c = counter ? Resource::from<Resource>(counter) : nullptr;
            const bool valid = c && !c->image() && structured &&
                               (c->desc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
                               !(view.CounterOffsetInBytes % D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT) &&
                               UINT64(view.CounterOffsetInBytes) + 4 <= c->desc().Width;
            if (!valid) {
                log_once("unordered-access view counter refused (a structured view, a buffer that allows unordered "
                         "access, a 4096-aligned offset inside it); offset",
                         int(view.CounterOffsetInBytes));
                return;
            }
        }
        const UINT64 va = resource->GetGPUVirtualAddress();
        const UINT64 counter_va = counter ? counter->GetGPUVirtualAddress() : 0;
        if (!va || (counter && !counter_va)) return;
        g::Buffer buffer{};
        buffer.address = va + view.FirstElement * unit;
        buffer.stride = structured || typed ? std::uint32_t(unit) : 0;
        buffer.num_records = std::uint32_t(structured || typed ? view.NumElements : UINT64(view.NumElements) * 4);
        buffer.format = format_code;
        buffer.swizzle = channels;
        buffer.oob_select = typed ? g::OobSelect::structured
                                  : structured ? g::OobSelect::structured_with_offset : g::OobSelect::raw;
        const g::BufferWords words = g::encode(buffer);
        std::memcpy(reinterpret_cast<void*>(destination.ptr), words.data(), sizeof(words));
        if (counter) {
            g::Buffer counter_buffer{};
            counter_buffer.address = counter_va + view.CounterOffsetInBytes;
            counter_buffer.num_records = 4;
            counter_buffer.oob_select = g::OobSelect::raw;
            const g::BufferWords counter_words = g::encode(counter_buffer);
            std::memcpy(reinterpret_cast<char*>(destination.ptr) + sizeof(words), counter_words.data(),
                        sizeof(counter_words));
        }
    }

    // An unordered-access view of a texture is a T# too, of one mip; set 0 also holds it as a storage image.
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* resource, ID3D12Resource* counter,
                                                     const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        d3d12_trace::record("ID3D12Device::CreateUnorderedAccessView");
        if (!destination.ptr) return;
        std::memset(reinterpret_cast<void*>(destination.ptr), 0, kDescriptorSize);
        const Resource* r = Resource::from<Resource>(resource);
        if (!r) {
            if (resource) stub("ID3D12Device::CreateUnorderedAccessView(resource from another device)");
            return;
        }
        if (!r->image()) return write_buffer_uav(resource, counter, desc, destination);
        if (counter) {
            log_once("unordered-access view of a texture with a counter resource; dimension", int(r->desc().Dimension));
            return;
        }
        if (!(r->desc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
            log_once("unordered-access view of a texture created without ALLOW_UNORDERED_ACCESS; flags",
                     int(r->desc().Flags));
            return;
        }
        TextureView view{};
        view.mip_count = 1;
        if (!desc) {
            const bool array = r->desc().DepthOrArraySize > 1;
            if (r->desc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && !array) {
                view.type = palmetto::gfx10::ImageType::image_1d;
                view.layer_count = 1;
            } else if (array)
                view.type = palmetto::gfx10::ImageType::image_2d_array;
        } else {
            view.format = desc->Format;
            switch (desc->ViewDimension) {
            case D3D12_UAV_DIMENSION_TEXTURE1D:
                view.type = palmetto::gfx10::ImageType::image_1d;
                view.first_mip = desc->Texture1D.MipSlice;
                view.layer_count = 1;
                break;
            case D3D12_UAV_DIMENSION_TEXTURE2D:
                view.first_mip = desc->Texture2D.MipSlice;
                view.layer_count = 1;
                view.plane = desc->Texture2D.PlaneSlice;
                break;
            case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
                view.type = palmetto::gfx10::ImageType::image_2d_array;
                view.first_mip = desc->Texture2DArray.MipSlice;
                view.first_layer = desc->Texture2DArray.FirstArraySlice;
                view.layer_count = desc->Texture2DArray.ArraySize;
                view.plane = desc->Texture2DArray.PlaneSlice;
                break;
            default:
                log_once("unordered-access view dimension not supported:", desc->ViewDimension);
                return;
            }
        }
        // B8G8R8X8 has no typed UAV, so no format of its family gives an unordered-access view.
        if (alpha_unused(r->desc().Format) || alpha_unused(view.format)) {
            log_once("unordered-access view of a B8G8R8X8 format; format", int(view.format));
            return;
        }
        write_image_descriptor(*r, view, destination, true);
        state_.context->note_shader_image(r->id());
    }

    // A sampler is an S# in the first 16 bytes of the slot; the shader finds the sampler by those four dwords.
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* desc,
                                         D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        d3d12_trace::record("ID3D12Device::CreateSampler");
        if (!destination.ptr) return;
        std::memset(reinterpret_cast<void*>(destination.ptr), 0, kDescriptorSize);
        if (!desc) return;
        const palmetto::gfx10::SamplerWords words = palmetto::gfx10::encode(sampler_descriptor(*desc, &state_));
        std::memcpy(reinterpret_cast<void*>(destination.ptr), words.data(), sizeof(words));
        state_.bind_sampler(words);
    }

    // CreateSampler with a UINT border colour and non-normalized coordinates (D3D-28). A description the PC runtime
    // refuses leaves the slot as it was, as the runtime itself does (without the debug layer) on an RTX 4090 and
    // on WARP: docs/d3d12_vulkan.md section 3.2.
    void STDMETHODCALLTYPE CreateSampler2(const D3D12_SAMPLER_DESC2* desc,
                                          D3D12_CPU_DESCRIPTOR_HANDLE destination) override {
        d3d12_trace::record("ID3D12Device11::CreateSampler2");
        if (!destination.ptr || !desc) return;
        const char* refusal = sampler2_refusal(*desc);
        if (refusal) {
            log_once(refusal, desc->Flags);
            return;
        }
        const palmetto::gfx10::SamplerWords words = sampler2_words(*desc);
        std::memset(reinterpret_cast<void*>(destination.ptr), 0, kDescriptorSize);
        std::memcpy(reinterpret_cast<void*>(destination.ptr), words.data(), sizeof(words));
        state_.bind_sampler(words);
    }

    // The S# of a D3D12_SAMPLER_DESC2 that sampler2_refusal accepts.
    palmetto::gfx10::SamplerWords sampler2_words(const D3D12_SAMPLER_DESC2& desc) {
        const bool uint_border = desc.Flags & D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR;
        D3D12_SAMPLER_DESC base{desc.Filter,     desc.AddressU,      desc.AddressV,       desc.AddressW,
                                desc.MipLODBias, desc.MaxAnisotropy, desc.ComparisonFunc, {},
                                desc.MinLOD,     desc.MaxLOD};
        if (!uint_border) std::memcpy(base.BorderColor, desc.FloatBorderColor, sizeof(base.BorderColor));
        palmetto::gfx10::Sampler sampler = sampler_descriptor(base, uint_border ? nullptr : &state_);
        if (uint_border) {
            // The S#'s border types are the float ones', plus the layer's integer mark (kIntegerBorderPtr), which
            // gives the integer border its own key and VK_BORDER_COLOR_INT_* in Vulkan.
            namespace g = palmetto::gfx10;
            const UINT* c = desc.UintBorderColor;
            const bool black = c[0] == 0 && c[1] == 0 && c[2] == 0;
            if (black && c[3] == 0) {
                sampler.border_color = g::BorderColor::transparent_black;
            } else if (black && c[3] == 1) {
                sampler.border_color = g::BorderColor::opaque_black;
            } else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) {
                sampler.border_color = g::BorderColor::opaque_white;
            } else {
                sampler.border_color = g::BorderColor::table;
                sampler.border_color_index = state_.register_border_color(true, c);
            }
            sampler.integer_border = true;
        }
        sampler.unnormalized = desc.Flags & D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
        return palmetto::gfx10::encode(sampler);
    }

    // A root signature's static samplers take no root argument: a shader that uses one holds its S# by value
    // (docs/d3d12_vulkan.md section 3.5). So each gets a sampler in set 0 and a key in the hash table here, as
    // CreateSampler gives a heap slot's S#; without them the shader's lookup misses and it samples through the
    // fallback sampler. A static sampler is a D3D12_SAMPLER_DESC2 whose border colour is one of five named ones
    // (D3D12_STATIC_BORDER_COLOR; the two UINT ones can be in a blob of any version, the flags only in 1.2), or
    // transparent black with the UINT flag.
    void register_static_samplers(const rt::rootsig::RootSignature& signature) {
        for (const rt::rootsig::StaticSampler& s : signature.static_samplers) {
            D3D12_SAMPLER_DESC2 desc{};
            desc.Filter = D3D12_FILTER(s.filter);
            desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE(s.address_u);
            desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE(s.address_v);
            desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE(s.address_w);
            desc.MipLODBias = s.mip_lod_bias;
            desc.MaxAnisotropy = s.max_anisotropy;
            desc.ComparisonFunc = D3D12_COMPARISON_FUNC(s.comparison_func);
            desc.MinLOD = s.min_lod;
            desc.MaxLOD = s.max_lod;
            desc.Flags = D3D12_SAMPLER_FLAGS(s.flags);
            const bool white = s.border_color == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE ||
                               s.border_color == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE_UINT;
            if (s.border_color == D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK_UINT ||
                s.border_color == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE_UINT) {
                desc.Flags |= D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR;
                desc.UintBorderColor[0] = desc.UintBorderColor[1] = desc.UintBorderColor[2] = white ? 1u : 0u;
                desc.UintBorderColor[3] = 1;
            } else if (s.border_color == D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK &&
                       (desc.Flags & D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR)) {
                // The integer border the enum has no value for: the 1.2 flag on transparent black is UINT (0,0,0,0),
                // which D3D12 serializes and creates. UintBorderColor is already zero.
            } else if (s.border_color > D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE_UINT ||
                       (desc.Flags & D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR)) {
                // A value past the enum, or the UINT flag with the float opaque black or white (D3D12 refuses to
                // serialize that pair): not a sampler D3D12 defines.
                log_once("static sampler not registered: border colour and flags do not go together; border colour",
                         int(s.border_color));
                continue;
            } else {
                desc.FloatBorderColor[0] = desc.FloatBorderColor[1] = desc.FloatBorderColor[2] = white ? 1.0f : 0.0f;
                desc.FloatBorderColor[3] =
                    s.border_color == D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK ? 0.0f : 1.0f;
            }
            if (sampler2_refusal(desc)) {
                log_once("static sampler not registered: a sampler CreateSampler2 refuses; flags", int(desc.Flags));
                continue;
            }
            if (!state_.bind_sampler(sampler2_words(desc)))
                log_once("static sampler not registered: no room in the bindless set; register",
                         int(s.shader_register));
        }
    }

    // Why the PC runtime refuses a D3D12_SAMPLER_DESC2, or null. The non-normalized rules are its debug layer's
    // messages, which match Vulkan's for unnormalizedCoordinates; AddressW is free in both.
    static const char* sampler2_refusal(const D3D12_SAMPLER_DESC2& desc) {
        if (desc.Flags & ~UINT(D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR | D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES))
            return "CreateSampler2 refused: unknown sampler flags; flags";
        if (!(desc.Flags & D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES)) return nullptr;
        if (D3D12_DECODE_MIN_FILTER(desc.Filter) != D3D12_DECODE_MAG_FILTER(desc.Filter) ||
            D3D12_DECODE_MIP_FILTER(desc.Filter) != D3D12_FILTER_TYPE_POINT ||
            D3D12_DECODE_IS_ANISOTROPIC_FILTER(desc.Filter) ||
            D3D12_DECODE_FILTER_REDUCTION(desc.Filter) == D3D12_FILTER_REDUCTION_TYPE_COMPARISON)
            return "CreateSampler2 refused: non-normalized sampler filter; flags";
        const auto clamps = [](D3D12_TEXTURE_ADDRESS_MODE m) {
            return m == D3D12_TEXTURE_ADDRESS_MODE_CLAMP || m == D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        };
        if (!clamps(desc.AddressU) || !clamps(desc.AddressV))
            return "CreateSampler2 refused: non-normalized sampler address mode; flags";
        if (desc.MinLOD != 0 || desc.MaxLOD != 0 || desc.MipLODBias != 0)
            return "CreateSampler2 refused: non-normalized sampler LOD; flags";
        return nullptr;
    }

    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
                                                      const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                                      const D3D12_CLEAR_VALUE* clear, REFIID riid,
                                                      void** out) override {
        return create_committed_validated("ID3D12Device::CreateCommittedResource", heap, heap_flags, desc, state, clear,
                                          0, nullptr, riid, out);
    }

    // Reserved buffers use sparse binding where available. Textures and fallback buffers are fully backed.
    // The description rules are D3D12's: buffers are ROW_MAJOR, textures 64KB_UNDEFINED_SWIZZLE with a tile shape
    // (64KB_STANDARD_SWIZZLE needs StandardSwizzle64KBSupported, which CheckFeatureSupport reports as FALSE). The
    // Xbox tiled layout word 0x101 (slot 76, section 4.2) is accepted as 64KB_UNDEFINED_SWIZZLE; that the console
    // uses it for reserved resources too is an assumption.
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                                     const D3D12_CLEAR_VALUE* clear, REFIID riid,
                                                     void** out) override {
        return create_reserved("ID3D12Device::CreateReservedResource", desc, state, clear, 0, nullptr, riid, out);
    }

    // Device4. Protected sessions are not supported.
    HRESULT STDMETHODCALLTYPE CreateReservedResource1(const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                                      const D3D12_CLEAR_VALUE* clear,
                                                      ID3D12ProtectedResourceSession* session, REFIID riid,
                                                      void** out) override {
        constexpr auto method = "ID3D12Device4::CreateReservedResource1";
        if (out) *out = nullptr;
        if (!desc) return d3d12_trace::result(method, E_INVALIDARG);
        if (session) return d3d12_trace::result(method, refuse_protected_session("CreateReservedResource1"));
        return create_reserved(method, desc, state, clear, 0, nullptr, riid, out);
    }

    // Device10: an initial layout instead of a state (resources start in their first use's layout here anyway), and a
    // castable-format list forwarded to the backing texture.
    HRESULT STDMETHODCALLTYPE CreateReservedResource2(const D3D12_RESOURCE_DESC* desc, D3D12_BARRIER_LAYOUT layout,
                                                      const D3D12_CLEAR_VALUE* clear,
                                                      ID3D12ProtectedResourceSession* session,
                                                      UINT32 castable_formats, const DXGI_FORMAT* castable, REFIID riid,
                                                      void** out) override {
        constexpr auto method = "ID3D12Device10::CreateReservedResource2";
        if (out) *out = nullptr;
        if (!desc) return d3d12_trace::result(method, E_INVALIDARG);
        if (session) return d3d12_trace::result(method, refuse_protected_session("CreateReservedResource2"));
        if (const char* why = barrier_layout_refusal(*desc, layout)) {
            log_once((std::string(method) + " refused (" + why + "), layout").c_str(), int(layout));
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        return create_reserved(method, desc, D3D12_RESOURCE_STATE_COMMON, clear, castable_formats, castable, riid, out);
    }

    // Tiling of a reserved resource or a tiled (64KB_UNDEFINED_SWIZZLE) texture (src/resource_tiling.h). Anything
    // else, as on the PC runtime, has no tiles: every output is zero and no subresource tiling is written.
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource* resource, UINT* total, D3D12_PACKED_MIP_INFO* packed,
                                             D3D12_TILE_SHAPE* shape, UINT* count, UINT first,
                                             D3D12_SUBRESOURCE_TILING* tilings) override {
        palmetto::ResourceTiling tiling;
        Resource* r = Resource::from<Resource>(resource);
        const bool ok = r && (r->reserved() || is_tiled_layout(r->GetDesc().Layout)) &&
                        resource_tiling(r->GetDesc(), tiling);
        if (total) *total = ok ? tiling.total_tiles : 0;
        if (packed)
            *packed = ok ? D3D12_PACKED_MIP_INFO{UINT8(tiling.standard_mips), UINT8(tiling.packed_mips),
                                                 tiling.packed_tiles, tiling.packed_start}
                         : D3D12_PACKED_MIP_INFO{};
        if (shape)
            *shape = ok ? D3D12_TILE_SHAPE{tiling.shape.width, tiling.shape.height, tiling.shape.depth}
                        : D3D12_TILE_SHAPE{};
        if (!count) return;
        const UINT available = ok && first < tiling.subresources.size() ? UINT(tiling.subresources.size() - first) : 0;
        const UINT n = tilings ? std::min(*count, available) : 0;
        for (UINT i = 0; i < n; ++i) {
            const palmetto::SubresourceTiling& s = tiling.subresources[first + i];
            tilings[i] = {s.width, UINT16(s.height), UINT16(s.depth), s.start};
        }
        *count = n;
    }

    // The 1/2/3 variants (D3D-28) are the same calls with newer arguments, answered as the PC runtime answers them
    // (probed on an RTX 4090; docs/d3d12_vulkan.md section 3.2 has the table). The initial state or layout is not
    // used: new textures are tracked as UNDEFINED and moved on first use, and buffers have no layout.
    HRESULT STDMETHODCALLTYPE CreateCommittedResource1(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
                                                       const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                                       const D3D12_CLEAR_VALUE* clear,
                                                       ID3D12ProtectedResourceSession* session, REFIID riid,
                                                       void** out) override {
        if (out) *out = nullptr;
        if (session) return refuse_protected_session("CreateCommittedResource1");
        return CreateCommittedResource(heap, heap_flags, desc, state, clear, riid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateCommittedResource2(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
                                                       const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state,
                                                       const D3D12_CLEAR_VALUE* clear,
                                                       ID3D12ProtectedResourceSession* session, REFIID riid,
                                                       void** out) override {
        if (out) *out = nullptr;
        if (!desc) return E_INVALIDARG;
        if (session) return refuse_protected_session("CreateCommittedResource2");
        if (texture_mip_region(*desc)) return E_INVALIDARG;
        const D3D12_RESOURCE_DESC base = resource_desc(*desc);
        return CreateCommittedResource(heap, heap_flags, &base, state, clear, riid, out);
    }

    HRESULT STDMETHODCALLTYPE CreateCommittedResource3(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
                                                       const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
                                                       const D3D12_CLEAR_VALUE* clear,
                                                       ID3D12ProtectedResourceSession* session, UINT32 castable_count,
                                                       const DXGI_FORMAT* castable, REFIID riid, void** out) override {
        constexpr auto method = "ID3D12Device10::CreateCommittedResource3";
        if (out) *out = nullptr;
        if (!desc) return d3d12_trace::result(method, E_INVALIDARG);
        if (session) return d3d12_trace::result(method, refuse_protected_session("CreateCommittedResource3"));
        if (texture_mip_region(*desc)) return d3d12_trace::result(method, E_INVALIDARG);
        // A count with a null array is no list (as on the PC runtime). The layer reports relaxed format casting, so
        // a real list is passed through; only buffers are refused.
        const D3D12_RESOURCE_DESC base = resource_desc(*desc);
        if (const char* why = barrier_layout_refusal(base, layout)) {
            log_once((std::string(method) + " refused (" + why + "), layout").c_str(), int(layout));
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        // D3D12_BARRIER_LAYOUT has no acceleration-structure value; an AS buffer's state comes through the
        // legacy CreateCommittedResource path. Buffers here start COMMON, as before.
        return create_committed_validated(method, heap, heap_flags, &base, D3D12_RESOURCE_STATE_COMMON, clear,
                                          castable_count, castable, riid, out);
    }

    // Device9. The creator ID only groups queues for tools; one creator is as good as another here.
    HRESULT STDMETHODCALLTYPE CreateCommandQueue1(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID, REFIID riid,
                                                  void** out) override {
        return CreateCommandQueue(desc, riid, out);
    }

    // Device8. A texture with a mip region gets the failure answer (every output all ones), as on the PC runtime;
    // a buffer's mip region is ignored.
    void STDMETHODCALLTYPE GetCopyableFootprints1(const D3D12_RESOURCE_DESC1* desc, UINT first, UINT count,
                                                  UINT64 base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows,
                                                  UINT64* row_sizes, UINT64* total) override {
        const D3D12_RESOURCE_DESC base_desc = desc ? resource_desc(*desc) : D3D12_RESOURCE_DESC{};
        const bool usable = desc && !texture_mip_region(*desc);
        GetCopyableFootprints(usable ? &base_desc : nullptr, first, count, base, layouts, rows, row_sizes, total);
    }

    // Heaps and placed resources (section 3.3). A heap reserves its size in the arena, so a buffer placed at a
    // 64 KiB-aligned offset is simply that offset into the arena, and its console VA is the heap's place plus it.
    // Existing-memory heaps and lifetime trackers. The host refuses a null address, a file mapping that is not a
    // handle and a null lifetime owner with E_INVALIDARG on WARP, NVIDIA and Intel (d3d12_existing_heap_test). Docs:
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-openexistingheapfromaddress
    //
    // OpenExistingHeapFromAddress answers as the host runtime does on an RTX 4090 and WARP (d3d12_existing_heap_test
    // open_cases): the address must be the base of a VirtualAlloc allocation that is committed read-write over its
    // whole size; an address inside it, a partly committed or reserved-only allocation, a read-only one and an
    // uncached one are E_INVALIDARG. The heap is the allocation's size, 64 KiB aligned, CUSTOM / L0 with WRITE_BACK
    // (WRITE_COMBINE for a PAGE_WRITECOMBINE range), flags SHARED |
    // SHARED_CROSS_ADAPTER | ALLOW_SHADER_ATOMICS. Its backing is that memory itself, imported with
    // VK_EXT_external_memory_host, so the GPU sees the caller's writes. Without the extension, or when the range is
    // not aligned to minImportedHostPointerAlignment or no memory type takes it, the call stays E_NOTIMPL, and
    // CheckFeatureSupport(EXISTING_HEAPS) reports the extension's absence. The console title opens the 64 KiB
    // graphics region it has just committed with XMemVirtualAlloc this way, right after its first queue and fence.
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(const void* address, REFIID riid, void** out) override {
        constexpr auto method = "ID3D12Device3::OpenExistingHeapFromAddress";
        if (out) *out = nullptr;
        if (!address || !out) return d3d12_trace::result(method, E_INVALIDARG);
        SIZE_T size = 0;
        DWORD protect = 0;
        if (!existing_allocation_size(address, size, protect)) return d3d12_trace::result(method, E_INVALIDARG);
        if (!state_.get_host_pointer_properties) {
            log_once("OpenExistingHeapFromAddress (no VK_EXT_external_memory_host; existing heaps are not supported)", 0);
            return d3d12_trace::result(method, E_NOTIMPL);
        }
        const VkDeviceSize alignment = state_.info.host_pointer_alignment;
        if ((reinterpret_cast<std::uintptr_t>(address) | size) & (alignment - 1)) {
            log_once("OpenExistingHeapFromAddress: range not aligned to minImportedHostPointerAlignment; alignment",
                     int(alignment));
            return d3d12_trace::result(method, E_NOTIMPL);
        }
        VkMemoryHostPointerPropertiesEXT host{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (state_.get_host_pointer_properties(state_.info.device,
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, address,
                                               &host) != VK_SUCCESS ||
            !host.memoryTypeBits) {
            log_once("OpenExistingHeapFromAddress: no memory type imports this host range", 0);
            return d3d12_trace::result(method, E_NOTIMPL);
        }
        VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import.pHostPointer = const_cast<void*>(address);
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint32_t memory_type = 0;
        // Host-visible and coherent first, as the caller's pages are; then any type the driver offers.
        for (const VkMemoryPropertyFlags want :
             {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
              VkMemoryPropertyFlags(0)}) {
            for (std::uint32_t i = 0; i < state_.memory.memoryTypeCount && !memory; ++i) {
                if (!(host.memoryTypeBits & (1u << i)) ||
                    (state_.memory.memoryTypes[i].propertyFlags & want) != want)
                    continue;
                VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
                allocate.allocationSize = size;
                allocate.memoryTypeIndex = i;
                if (state_.context->vk.vkAllocateMemory(state_.info.device, &allocate, nullptr, &memory) ==
                    VK_SUCCESS)
                    memory_type = i;
                else
                    memory = VK_NULL_HANDLE;
            }
            if (memory) break;
        }
        if (!memory) {
            log_once("OpenExistingHeapFromAddress: importing the host range failed; size in KiB", int(size / 1024));
            return d3d12_trace::result(method, E_OUTOFMEMORY);
        }
        D3D12_HEAP_DESC desc{};
        desc.SizeInBytes = size;
        desc.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        desc.Properties.CPUPageProperty = (protect & PAGE_WRITECOMBINE) ? D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE
                                                                        : D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        desc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
                     D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS;
        auto* heap = new Heap(state_, desc, memory, memory_type, address);
        heap->attach_device(identity());
        return d3d12_trace::result(method, hand_out(heap, riid, out));
    }

    // The size of the VirtualAlloc allocation `address` starts, and its protection, when the whole of it is committed
    // PAGE_READWRITE, or PAGE_READWRITE | PAGE_WRITECOMBINE: what the host runtime accepts (a write-combined range
    // opens as a WRITE_COMBINE heap). False for an address inside an allocation, a reserved or free page, another
    // protection (read-only, PAGE_NOCACHE), or two protections in one allocation.
    static bool existing_allocation_size(const void* address, SIZE_T& size, DWORD& protect) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(address, &region, sizeof(region)) || region.AllocationBase != address) return false;
        protect = region.Protect;
        if (protect != PAGE_READWRITE && protect != (PAGE_READWRITE | PAGE_WRITECOMBINE)) return false;
        size = 0;
        const char* at = static_cast<const char*>(address);
        while (VirtualQuery(at + size, &region, sizeof(region)) && region.AllocationBase == address) {
            if (region.State != MEM_COMMIT || region.Protect != protect) return false;
            size += region.RegionSize;
        }
        return size != 0;
    }

    // create_buffer_at_host_address (Xbox device slot 133). The heap the resource reports is CUSTOM / WRITE_BACK / L0,
    // what OpenExistingHeapFromAddress gives the same pages, since the CPU keeps writing them.
    // A D3D12_BARRIER_LAYOUT (what Xbox slot 133 passes, as PC CreatePlacedResource2 does) as the legacy initial state
    // of an equivalent committed resource. UNDEFINED (0xFFFFFFFF) and anything unlisted give COMMON.
    static D3D12_RESOURCE_STATES state_for_barrier_layout(std::uint32_t layout) {
        switch (layout) {
            case 1: return D3D12_RESOURCE_STATE_GENERIC_READ;
            case 2: return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case 3: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case 4: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case 5: return D3D12_RESOURCE_STATE_DEPTH_READ;
            case 6: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            case 7: return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case 8: return D3D12_RESOURCE_STATE_COPY_DEST;
            case 9: return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            case 10: return D3D12_RESOURCE_STATE_RESOLVE_DEST;
            default: return D3D12_RESOURCE_STATE_COMMON;
        }
    }

    HRESULT create_at_host_address(void* address, const D3D12_RESOURCE_DESC& desc, std::uint32_t layout, REFIID riid,
                                   void** out) {
        constexpr auto method = "Xbox CreatePlacedResourceX (device slot 133)";
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            // Bounded diagnostic (docs/texture_import.md): the address the title placed a texture at, next to the
            // addresses the XG texture computer's fill writes (PALMETTO_XG_TRACE), to see whether the title's texture
            // bytes land in a placed resource's pages at all.
            {
                // The dimension is D3D12_RESOURCE_DIMENSION (3 = TEXTURE2D), not a texture's 3D-ness; the logged
                // description is the one the proxy already translated (console-only flag bits masked out, layout
                // replaced), which is what this device validates.
                static std::atomic<unsigned> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 64)
                    std::fprintf(stderr,
                                 "[rt] d3d12: placed texture: address 0x%llx %ux%ux%u format %d dimension %d mips %u "
                                 "layers %u flags 0x%x samples %u layout 0x%x\n",
                                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)),
                                 desc.Width, desc.Height, desc.DepthOrArraySize, int(desc.Format), int(desc.Dimension),
                                 desc.MipLevels, desc.DepthOrArraySize, static_cast<unsigned>(desc.Flags),
                                 desc.SampleDesc.Count, static_cast<unsigned>(desc.Layout));
            }
            // A texture is not placed on the title's pages yet: it gets an ordinary committed DEFAULT texture of the
            // same description, as slot 76 gives one, so the title has its resource instead of asserting (found in
            // review). What the title writes at `address` does not reach it.
            log_once("a title-placed texture (device slot 133) is an ordinary committed texture, not the title's pages; "
                     "dimension",
                     int(desc.Dimension));
            // The title's barrier layout (slot 133's third argument, measured by @dracbat) is checked against the
            // description as CreateCommittedResource checks a state; one it refuses (or UNDEFINED) gives COMMON, so
            // the title still gets its texture. The image itself starts undefined either way: the first command that
            // uses it moves it to that command's layout (vulkan_replay.h, initialize), as for every texture here.
            const D3D12_HEAP_PROPERTIES fallback{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_STATES initial = state_for_barrier_layout(layout);
            // The console's depth-stencil + unordered-access pair is created as its depth-stencil texture here, with a
            // named line for the missing unordered-access half; every other description is created exactly as asked.
            D3D12_RESOURCE_DESC host_desc = desc;
            if (console_depth_uav_translated(host_desc)) {
                log_once("a console depth-stencil texture that also allows unordered access is created as its "
                         "depth-stencil resource; unordered-access views of it stay refused (the layer cannot make a "
                         "storage view of a depth format); flags",
                         int(desc.Flags));
                // The dropped half can also be the creation state (a barrier layout of UNORDERED_ACCESS): the host
                // resource has no unordered access, so it starts COMMON and the first use moves it, as every image
                // here does. This keeps the creation a single attempt instead of a refused one and a retry.
                if (UINT(initial) & UINT(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) initial = D3D12_RESOURCE_STATE_COMMON;
            }
            HRESULT hr = create_committed_validated(nullptr, &fallback, D3D12_HEAP_FLAG_NONE, &host_desc, initial,
                                                    nullptr, 0, nullptr, riid, out, true);
            if (FAILED(hr) && initial != D3D12_RESOURCE_STATE_COMMON)
                hr = create_committed_validated(nullptr, &fallback, D3D12_HEAP_FLAG_NONE, &host_desc,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr, 0, nullptr, riid, out, true);
            if (FAILED(hr)) {
                // The same bounded record for a description this device cannot create: the refusal reason is printed
                // once by create_committed_validated, so this line ties the shape to that answer.
                static std::atomic<unsigned> failed{0};
                if (failed.fetch_add(1, std::memory_order_relaxed) < 32)
                    std::fprintf(stderr,
                                 "[rt] d3d12: placed texture refused: %ux%ux%u format %d dimension %d mips %u "
                                 "layers %u flags 0x%x samples %u layout 0x%x hresult 0x%08x\n",
                                 desc.Width, desc.Height, desc.DepthOrArraySize, int(desc.Format), int(desc.Dimension),
                                 desc.MipLevels, desc.DepthOrArraySize, static_cast<unsigned>(desc.Flags),
                                 desc.SampleDesc.Count, static_cast<unsigned>(desc.Layout),
                                 static_cast<unsigned>(hr));
            }
            // Opt-in (docs/texture_import.md): the title writes its texture bytes into the pages at `address` (in the
            // measured linear mode a row-major CPU copy per subresource), so import them into this image before the
            // first submission that can sample it. Queued here, drained by the queue hook, so the title's writes are
            // already in place when the import runs.
            if (SUCCEEDED(hr) && out && *out) {
                if (Resource* stand_in = Resource::from<Resource>(static_cast<IUnknown*>(*out))) {
                    stand_in->set_placed_pages(reinterpret_cast<std::uintptr_t>(address));
                    // The console's placed texture is the title's pages, and the title asks GetGPUVirtualAddress for
                    // the address it placed the texture at and writes the texels through it (Xbox unified memory). The
                    // PC committed stand-in above is not those pages, but the console's answer is the placement address;
                    // the PC answer (none for a texture) made the lighting init's 256x256 clears memset a null pointer
                    // (docs/3d_textures.md).
                    stand_in->set_console_placed_va(
                        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address)));
                    {
                        // Diagnostic (docs/frame_capture.md): remember which resource lives at this title VA, so a T# the
                        // shader fetches itself can be bound from it.
                        std::lock_guard resources_lock(state_.placed_resources_mutex);
                        if (state_.placed_resources.size() < 4096)
                            state_.placed_resources.emplace(reinterpret_cast<std::uintptr_t>(address), stand_in);
                    }
                    // Diagnostic (docs/texture_source.md): from its creation, watch the pages of a placed texture big
                    // enough to be the frame's sampled 4K population, so the watch starts before the frame's draws. The
                    // armed draw's own sampled texture is registered later by the descriptor probe.
                    const std::uint64_t placed_bytes = stand_in->placed_subresource_bytes();
                    if (placed_bytes >= 16ull * 1024 * 1024)
                        state_.watch_placed_pages(reinterpret_cast<std::uintptr_t>(address), placed_bytes);
                    if (placed_texture_import_enabled()) {
                        stand_in->AddRef();
                        std::lock_guard lock(state_.placed_texture_mutex);
                        state_.placed_textures_pending.push_back(stand_in);
                    }
                }
            }
            return d3d12_trace::result(method, hr);
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_CUSTOM;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        auto* resource = new Resource(state_, desc, heap, D3D12_HEAP_FLAG_NONE);
        resource->attach_device(identity());
        const HRESULT hr = resource->create_host_buffer(address);
        if (hr == E_INVALIDARG) {
            resource->Release();
            return d3d12_trace::result(method, hr);
        }
        if (FAILED(hr)) {
            // The driver would not import these pages: RADV refuses a view of a pagefile-backed section, the form
            // xmem's physical pages take (found in review on an RX 9070 XT under Wine), and a device without
            // VK_EXT_external_memory_host has no import at all. The boot goes on with an ordinary DEFAULT buffer
            // of the same description, as #6915's stand-in did: the title gets a resource, but its writes at
            // `address` do not reach it.
            resource->Release();
            log_once("a title-placed buffer (device slot 133) could not import its pages, so it is an ordinary buffer "
                     "the title's writes do not reach; HRESULT",
                     int(hr));
            const D3D12_HEAP_PROPERTIES fallback{D3D12_HEAP_TYPE_DEFAULT};
            const HRESULT created = create_committed(&fallback, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COMMON, 0, nullptr, riid, out);
            // The stand-in is a default-heap buffer, so it is an arena range: its console VA is an arena offset and
            // heap_base + VA is its device address. Register the VA the title placed the pages at as an alias for it,
            // so the views and descriptors that name the pages by that VA resolve to the stand-in and every
            // shader-visible copy of the VA is rebased to the arena range (docs/page_address_space.md). The bytes are
            // still not the title's writes (the line above); that is the separate copy/page-import limitation.
            if (SUCCEEDED(created) && out && *out) {
                if (Resource* stand_in = Resource::from<Resource>(static_cast<IUnknown*>(*out)))
                    stand_in->alias_console_va(reinterpret_cast<std::uintptr_t>(address));
            }
            return d3d12_trace::result(method, created);
        }
        return d3d12_trace::result(method, hand_out(resource, riid, out));
    }

    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(HANDLE file_mapping, REFIID, void** out) override {
        constexpr auto method = "ID3D12Device3::OpenExistingHeapFromFileMapping";
        if (out) *out = nullptr;
        DWORD flags = 0;
        if (!file_mapping || !out || !GetHandleInformation(file_mapping, &flags))
            return d3d12_trace::result(method, E_INVALIDARG);
        log_once("OpenExistingHeapFromFileMapping (existing heaps are not supported)", 0);
        return d3d12_trace::result(method, E_NOTIMPL);
    }

    HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(ID3D12LifetimeOwner* owner, REFIID, void** out) override {
        constexpr auto method = "ID3D12Device5::CreateLifetimeTracker";
        if (out) *out = nullptr;
        if (!owner || !out) return d3d12_trace::result(method, E_INVALIDARG);
        log_once("CreateLifetimeTracker (not supported)", 0);
        return d3d12_trace::result(method, E_NOTIMPL);
    }

    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* desc, REFIID riid, void** out) override {
        if (out) *out = nullptr;
        if (!desc) return E_INVALIDARG;
        if (const char* why = heap_desc_refusal(*desc, state_.uma)) {
            log_once((std::string("CreateHeap refused (") + why + "), heap type").c_str(), int(desc->Properties.Type));
            return E_INVALIDARG;
        }
        if (!state_.arena.mapped) {
            log_once("heaps need the GPU address arena (bufferDeviceAddress); heap size in KiB:",
                     int(desc->SizeInBytes / 1024));
            return E_NOTIMPL;
        }
        auto* heap = new Heap(state_, *desc);
        heap->attach_device(identity());
        if (!heap->reserved()) {
            log_once("GPU address arena full (raise PALMETTO_ARENA_MB); heap size in KiB:", int(desc->SizeInBytes / 1024));
            heap->Release();
            return E_OUTOFMEMORY;
        }
        if (!out) {
            heap->Release();
            return S_FALSE;
        }
        return hand_out(heap, riid, out);
    }

    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* heap_interface, UINT64 heap_offset,
                                                   const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) override {
        return create_placed("ID3D12Device::CreatePlacedResource", heap_interface, heap_offset, desc, state, clear, 0,
                             nullptr, riid, out);
    }

    // Device8: the DESC1 placed variant without a castable list. A texture with a mip region gets the failure
    // answer, as on the PC runtime; a buffer's mip region is ignored.
    HRESULT STDMETHODCALLTYPE CreatePlacedResource1(ID3D12Heap* heap_interface, UINT64 heap_offset,
                                                    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state,
                                                    const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) override {
        constexpr auto method = "ID3D12Device8::CreatePlacedResource1";
        if (out) *out = nullptr;
        if (!desc) return d3d12_trace::result(method, E_INVALIDARG);
        if (texture_mip_region(*desc)) return d3d12_trace::result(method, E_INVALIDARG);
        const D3D12_RESOURCE_DESC base = resource_desc(*desc);
        return create_placed(method, heap_interface, heap_offset, &base, state, clear, 0, nullptr, riid, out);
    }

    // Device10: an initial layout instead of a state, and the castable-format list, which the placed texture's
    // Vulkan image takes exactly as the committed path's does. A count with a null array is no list (as on the
    // PC runtime); the layer reports RelaxedFormatCastingSupported, so a real list is passed through.
    HRESULT STDMETHODCALLTYPE CreatePlacedResource2(ID3D12Heap* heap_interface, UINT64 heap_offset,
                                                    const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
                                                    const D3D12_CLEAR_VALUE* clear, UINT32 castable_count,
                                                    const DXGI_FORMAT* castable, REFIID riid, void** out) override {
        constexpr auto method = "ID3D12Device10::CreatePlacedResource2";
        if (out) *out = nullptr;
        if (!desc) return d3d12_trace::result(method, E_INVALIDARG);
        if (texture_mip_region(*desc)) return d3d12_trace::result(method, E_INVALIDARG);
        const D3D12_RESOURCE_DESC base = resource_desc(*desc);
        if (const char* why = barrier_layout_refusal(base, layout)) {
            log_once((std::string(method) + " refused (" + why + "), layout").c_str(), int(layout));
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        // An initial layout replaces the state, so the state rules are not this call's.
        return create_placed(method, heap_interface, heap_offset, &base, D3D12_RESOURCE_STATE_COMMON, clear,
                             castable_count, castable, riid, out);
    }

    // The placed path every CreatePlacedResource variant shares. A castable-format list reaches the placed
    // texture's Vulkan image as the committed path's does (create_committed): a buffer with a list is
    // E_INVALIDARG, and so is a format with no Vulkan mapping.
    // The arguments are answered as the host runtime answers them (d3d12_vulkan_device_test's
    // compare_placed_reserved, RTX 3060 / 610.88): the description, initial state and clear value rules
    // CreateCommittedResource has, then the placement: a buffer at a 64 KiB-aligned offset, a texture at a
    // multiple of its GetResourceAllocationInfo alignment (4 KiB for a small texture), in a heap aligned at least
    // as much, and the whole resource inside the heap. Only then is a null output E_UNEXPECTED, with nothing made.
    HRESULT create_placed(const char* method, ID3D12Heap* heap_interface, UINT64 heap_offset,
                          const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                          UINT32 castable_count, const DXGI_FORMAT* castable, REFIID riid, void** out) {
        if (out) *out = nullptr;
        if (!is_our_heap(heap_interface)) {
            // The console names a heap by an address; reading a tag from that address is what crashed the retail
            // boot (docs/boot_audit.md B1). Refuse cleanly and name the value once.
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12: placed resource with a heap that is not a runtime heap (console address?); "
                             "the heap identifier is %llu-bit, low word 0x%08x\n",
                             64, static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(heap_interface) & 0xffffffffu));
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        auto* heap = Heap::from<Heap>(heap_interface);
        if (!heap || !desc) return d3d12_trace::result(method, E_INVALIDARG);
        if (heap->host_address()) {
            // An existing heap is not an arena range, and the placed path addresses resources by arena offset.
            log_once("a placed resource in an existing heap (OpenExistingHeapFromAddress) is not supported yet; "
                     "dimension",
                     int(desc->Dimension));
            return d3d12_trace::result(method, E_NOTIMPL);
        }
        if (castable_count && castable && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            log_once("castable formats on a placed buffer are invalid; count", int(castable_count));
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        // The description, state and clear value rules of a committed resource, but on a default heap: the heap's
        // DENY flags are checked against the resource below, and the host takes any initial state for a buffer in
        // an upload or readback heap (COPY_DEST in an upload heap, GENERIC_READ in a readback heap), where a
        // committed buffer must start in GENERIC_READ or COPY_DEST. UAV access is refused there all the same.
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const char* why = committed_resource_refusal(default_heap, D3D12_HEAP_FLAG_NONE, *desc, state, clear,
                                                     acceleration_structures(), state_.uma);
        const D3D12_HEAP_TYPE heap_type = heap->properties().Type;
        if (!why && (heap_type == D3D12_HEAP_TYPE_UPLOAD || heap_type == D3D12_HEAP_TYPE_READBACK) &&
            (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            why = "UAV access in an upload or readback heap";
        if (why) {
            log_once((std::string(method) + " refused (" + why + "), dimension").c_str(), int(desc->Dimension));
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        const bool tiled = is_tiled_layout(desc->Layout);
        const bool msaa = desc->SampleDesc.Count > 1;
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            if (heap->flags() & D3D12_HEAP_FLAG_DENY_BUFFERS) return d3d12_trace::result(method, E_INVALIDARG);
            if (heap_offset % D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
                log_once("placed buffer offset not 64 KiB aligned:", int(heap_offset));
                return d3d12_trace::result(method, E_INVALIDARG);
            }
            if (desc->Width > heap->bytes() || heap_offset > heap->bytes() - desc->Width)
                return d3d12_trace::result(method, E_INVALIDARG);
        } else {
            if (heap->properties().Type == D3D12_HEAP_TYPE_UPLOAD ||
                heap->properties().Type == D3D12_HEAP_TYPE_READBACK)
                return d3d12_trace::result(method, E_INVALIDARG);
            const bool rt_ds = (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
            if (rt_ds && (heap->flags() & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES))
                return d3d12_trace::result(method, E_INVALIDARG);
            if (!rt_ds && (heap->flags() & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES))
                return d3d12_trace::result(method, E_INVALIDARG);
            if (tiled && desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                return d3d12_trace::result(method, E_INVALIDARG);
            if (msaa && desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                return d3d12_trace::result(method, E_INVALIDARG);
            // Extents, mips, samples, layout and alignment, as GetResourceAllocationInfo answers them (the sample
            // quality is the description rules' above, as in CreateCommittedResource).
            D3D12_RESOURCE_DESC checked = *desc;
            checked.SampleDesc.Quality = 0;
            const D3D12_RESOURCE_ALLOCATION_INFO alloc = allocation(checked);
            if (alloc.SizeInBytes == UINT64_MAX || heap->alignment() < alloc.Alignment ||
                heap_offset % alloc.Alignment) {
                log_once("placed texture refused (size, alignment or room in the heap), dimension",
                         int(desc->Dimension));
                return d3d12_trace::result(method, E_INVALIDARG);
            }
            const UINT64 fit_bytes =
                (tiled || msaa)
                    ? alloc.SizeInBytes
                    : min_placed_texture_bytes(checked, state_.texture_format(checked), alloc.Alignment,
                                               alloc.SizeInBytes);
            if (fit_bytes > heap->bytes() || heap_offset > heap->bytes() - fit_bytes) {
                log_once("placed texture refused (size, alignment or room in the heap), dimension",
                         int(desc->Dimension));
                return d3d12_trace::result(method, E_INVALIDARG);
            }
            if (tiled && static_cast<UINT>(desc->Layout) == kXboxTiledLayout) {
                log_once("placed resource with the Xbox tiled layout, as 64KB_UNDEFINED_SWIZZLE; dimension",
                         int(desc->Dimension));
            }
        }
        if (!out) return d3d12_trace::result(method, E_UNEXPECTED);
        const bool buffer = desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
        if (buffer && !heap->use_as_placed_resource(true)) {
            log_once("placed buffer refused: heap uses a separate allocation for sparse tiles", 1);
            return d3d12_trace::result(method, E_NOTIMPL);
        }
        // Only buffers are addressable, as in D3D12: a placed texture keeps a null VA and its own Vulkan image, which
        // create_texture makes for 1D, 2D and 3D alike (arrays and mips included), as for a committed texture.
        const VkDeviceSize placed_va = desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? heap->offset() + heap_offset : 0;
        auto* resource = new Resource(state_, *desc, heap->properties(), heap->flags(), placed_va);
        resource->attach_device(identity());
        HRESULT hr = E_NOTIMPL;
        if (buffer) {
            hr = resource->create_placed_buffer(heap->arena_range());
        } else {
            hr = S_OK;
            const Format format = state_.texture_format(*desc);
            if (format.format == VK_FORMAT_UNDEFINED) {
                hr = E_INVALIDARG;
            } else {
                std::vector<VkFormat> cast_formats;
                hr = castable_view_formats(format, castable_count, castable, cast_formats);
                if (SUCCEEDED(hr)) hr = resource->create_texture(format, cast_formats);
                if (SUCCEEDED(hr)) resource->keep_castable_formats(castable_count, castable);
            }
        }
        if (SUCCEEDED(hr) && !buffer && !heap->use_as_placed_resource(false)) {
            log_once("placed resource refused: heap uses a separate allocation for sparse tiles", 1);
            hr = E_NOTIMPL;
        }
        if (FAILED(hr)) {
            resource->Release();
            return d3d12_trace::result(method, hr);
        }
        return d3d12_trace::result(method, hand_out(resource, riid, out));
    }

    // Footprints for copies through a buffer: rows padded to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (512 for planar
    // depth-stencil), subresources placed at D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, block-compressed sizes rounded up to whole blocks, depth-stencil
    // formats as two planes. d3d12_vulkan_device_test compares every output with the PC runtime's.
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* desc, UINT first, UINT count, UINT64 base,
                                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows,
                                                 UINT64* row_sizes, UINT64* total) override {
        const auto fail = [&] {
            for (UINT i = 0; i < count; ++i) {
                if (layouts) std::memset(&layouts[i], 0xff, sizeof(layouts[i]));
                if (rows) rows[i] = ~0u;
                if (row_sizes) row_sizes[i] = ~0ull;
            }
            if (total) *total = ~0ull;
        };
        if (!desc || !xr_bias_compatible(*desc) || rgb96_refused(*desc)) return fail();
        const auto align = [](UINT64 value, UINT64 to) { return (value + to - 1) / to * to; };
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            // The PC runtime refuses any buffer field that isn't its one valid value (all measured on WARP, Width,
            // Height, Format, MipLevels 0, Layout and Count 0 also on an NVIDIA GPU). It doesn't look at Flags, so a
            // buffer with the render-target flag still gets its footprint (D3D-122).
            if (first != 0 || count != 1 || !desc->Width || desc->Height != 1 || desc->DepthOrArraySize != 1 ||
                desc->MipLevels != 1 || desc->Format != DXGI_FORMAT_UNKNOWN ||
                desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR || desc->SampleDesc.Count != 1 ||
                desc->SampleDesc.Quality != 0 ||
                (desc->Alignment && desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT))
                return fail();
            if (layouts) layouts[0] = {base, {DXGI_FORMAT_UNKNOWN, UINT(desc->Width), 1, 1,
                                              UINT(align(desc->Width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT))}};
            if (rows) rows[0] = 1;
            if (row_sizes) row_sizes[0] = desc->Width;
            if (total) *total = desc->Width;
            return;
        }
        const bool volume = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const bool one_d = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        if (!volume && !one_d && desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            log_once("GetCopyableFootprints dimension not supported:", desc->Dimension);
            return fail();
        }
        if (one_d && desc->Height != 1) return fail();
        // Structural checks only, as measured on the PC runtime (D3D-122). Not allocation()'s: the footprint path
        // accepts extra mips, 2D ROW_MAJOR and Quality > 0 at one sample, and extent limits, alignment and
        // multisample quality differ between adapters, so those stay unchecked.
        const UINT samples = desc->SampleDesc.Count;
        const bool multisample_ok = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc->MipLevels == 1 &&
                                    desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR && !(samples & (samples - 1)) &&
                                    (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
        if (!desc->Width || !desc->Height || !desc->DepthOrArraySize || !samples || (samples > 1 && !multisample_ok))
            return fail();
        const Format format = texture_storage_format(desc->Format, true);  // typeless depth families are planar either way
        UINT mips = desc->MipLevels;
        const UINT depth = volume ? desc->DepthOrArraySize : 1;
        if (!mips)
            for (UINT64 size = std::max<UINT64>({desc->Width, desc->Height, depth}); size; size >>= 1) ++mips;
        const UINT planes = (format.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? 2 : 1;
        const UINT per_plane = mips * (volume ? 1 : desc->DepthOrArraySize);
        if (!format.format || UINT64(first) + count > UINT64(per_plane) * planes) return fail();

        // Subresources are aligned to D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT counting from BaseOffset, not from 0: the
        // PC runtime moves the whole layout by BaseOffset, so an unaligned base (513) keeps the same relative offsets
        // and the same total. Measured by compare_footprints in d3d12_vulkan_device_test.
        UINT64 offset = 0, end = 0;
        for (UINT i = 0; i < count; ++i) {
            const UINT index = first + i, plane = index / per_plane, mip = index % mips;
            const VkImageAspectFlags aspect =
                planes == 2 ? (plane ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT) : format.aspects;
            const palmetto::BlockFormat block = footprint_block(format, aspect);
            if (!block.bytes) return fail();
            // D3D12 takes any MipLevels, and the PC runtime's own shifts wrap past the type's width: a 16x16 texture
            // with 40 mips reports subresource 32 as 1x16 and 33 as 1x8, a volume's depth likewise (measured on an
            // RTX 5060 Ti, compare_footprints). The masks give the same answers without shifting a 32-bit (or the
            // 64-bit Width) value by its width or more, which C++ leaves undefined.
            const UINT width = std::max<UINT>(1, UINT(desc->Width >> (mip & 63)));
            const UINT height = one_d ? 1 : std::max<UINT>(1, desc->Height >> (mip & 31));
            const UINT blocks_wide = (width + block.block - 1) / block.block;
            const UINT blocks_high = (height + block.block - 1) / block.block;
            const UINT64 row_size = UINT64(blocks_wide) * block.bytes;
            // Measured against the PC runtime (d3d12_vulkan_device_test): planar depth-stencil rows are padded to
            // 512 bytes, every other format's to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256).
            const UINT pitch = UINT(align(row_size, planes == 2 ? 2 * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
                                                                : D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
            offset = align(offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            DXGI_FORMAT footprint_format = desc->Format;
            if (planes == 2) footprint_format = plane ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
            const UINT slices = std::max(1u, depth >> (mip & 31));
            if (layouts)
                layouts[i] = {base + offset,
                              {footprint_format, blocks_wide * block.block, blocks_high * block.block, slices, pitch}};
            if (rows) rows[i] = blocks_high;
            if (row_sizes) row_sizes[i] = row_size;
            const UINT64 total_rows = UINT64(blocks_high) * slices;
            end = offset + (total_rows - 1) * pitch + row_size;
            offset += total_rows * pitch;
        }
        if (total) *total = end;
    }

    // How much heap memory resources need and how to align them, for applications that suballocate heaps (section
    // 4.1). Buffers follow D3D12's fixed rule. A texture's size is what the Vulkan image it becomes needs, rounded up
    // to D3D12's alignment for it; the alignment itself is D3D12's, since placed textures bind their own memory
    // (section 4.2). Several resources are laid out one after another, each at its own alignment. An invalid
    // description gives SizeInBytes = UINT64_MAX, as on D3D12. d3d12_vulkan_device_test compares with the PC runtime.
    // A visible mask above 1 names a node the single-node adapter lacks: the same invalid answer, also for no
    // resources at all, and the per-resource array is left untouched (host runtime and WARP, measured).
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC* descs) override {
        if (visible_mask > 1) return kInvalidAllocation;
        return place(count, descs, [&](UINT i) { return descs[i]; }, nullptr);
    }
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo1(
        UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC* descs,
        D3D12_RESOURCE_ALLOCATION_INFO1* each) override {
        if (visible_mask > 1) return kInvalidAllocation;
        return place(count, descs, [&](UINT i) { return descs[i]; }, each);
    }
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo2(
        UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC1* descs,
        D3D12_RESOURCE_ALLOCATION_INFO1* each) override {
        if (visible_mask > 1) return kInvalidAllocation;
        return place(count, descs, [&](UINT i) {
            const D3D12_RESOURCE_DESC1& d = descs[i];
            if (d.SamplerFeedbackMipRegion.Width || d.SamplerFeedbackMipRegion.Height ||
                d.SamplerFeedbackMipRegion.Depth) {
                log_once("GetResourceAllocationInfo2 sampler feedback not supported, format", d.Format);
                return D3D12_RESOURCE_DESC{};  // an unknown dimension: invalid
            }
            return D3D12_RESOURCE_DESC{d.Dimension, d.Alignment, d.Width, d.Height, d.DepthOrArraySize,
                                       d.MipLevels, d.Format, d.SampleDesc, d.Layout, d.Flags};
        }, each);
    }

    template <typename FeatureData>
    static HRESULT write_supported_feature(const char* method, void* data, UINT size, BOOL supported) {
        if (size != sizeof(FeatureData)) return d3d12_trace::result(method, E_INVALIDARG);
        static_cast<FeatureData*>(data)->Supported = supported;
        return d3d12_trace::result(method, S_OK);
    }

    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT size) override {
        constexpr auto method = "ID3D12Device::CheckFeatureSupport";
        log_once("CheckFeatureSupport feature", feature);
        if (!data) return d3d12_trace::result(method, E_INVALIDARG);
        constexpr VkSampleCountFlags d3d12_multisample_sample_counts =
            VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT |
            VK_SAMPLE_COUNT_16_BIT | VK_SAMPLE_COUNT_32_BIT;
        switch (feature) {
        case D3D12_FEATURE_FEATURE_LEVELS: {
            if (size != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* levels = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(data);
            // The host (hardware and WARP) refuses an empty or missing list before reading it.
            if (!levels->NumFeatureLevels || !levels->pFeatureLevelsRequested)
                return d3d12_trace::result(method, E_INVALIDARG);
            D3D_FEATURE_LEVEL best = D3D_FEATURE_LEVEL(0);
            for (UINT i = 0; i < levels->NumFeatureLevels; ++i)
                if (levels->pFeatureLevelsRequested[i] <= D3D_FEATURE_LEVEL_12_1)
                    best = std::max(best, levels->pFeatureLevelsRequested[i]);
            levels->MaxSupportedFeatureLevel = best;  // 0 with DXGI_ERROR_UNSUPPORTED, as the host writes it
            if (!best) return d3d12_trace::result(method, DXGI_ERROR_UNSUPPORTED);
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_ARCHITECTURE:
        case D3D12_FEATURE_ARCHITECTURE1: {
            const bool v1 = feature == D3D12_FEATURE_ARCHITECTURE1;
            if (size != (v1 ? sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1) : sizeof(D3D12_FEATURE_DATA_ARCHITECTURE)))
                return d3d12_trace::result(method, E_INVALIDARG);
            auto* arch = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE1*>(data);
            const UINT node = arch->NodeIndex;
            if (node != 0) return d3d12_trace::result(method, E_INVALIDARG);
            // The host GPU's architecture (question 11): not tile-based; UMA on an integrated GPU, as the host runtime
            // answers there (Radeon 780M, AMD 32.0.13030.13001: UMA 1, CacheCoherentUMA 0), else a discrete one. The
            // console is UMA; the layer keeps upload heaps host-visible either way, so the game's upload path works.
            // CacheCoherentUMA stays FALSE: the AMD iGPU measured reports it so, and FALSE only steers an application
            // to write-combined uploads, which every UMA adapter accepts.
            arch->TileBasedRenderer = FALSE;
            arch->UMA = state_.uma ? TRUE : FALSE;
            arch->CacheCoherentUMA = FALSE;
            if (v1) arch->IsolatedMMU = TRUE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS*>(data);
            *opts = {};
            // Answer what the layer will actually honour, not what the GPU offers: docs/d3d12_vulkan.md 4.1 says a
            // false "supported" is worse than a false "unsupported" at boot, and the PSO paths refuse these same
            // features when they are missing (logic ops, per the mapping vkd3d-proton's
            // d3d12_device_caps_init_feature_options uses).
            opts->DoublePrecisionFloatShaderOps = state_.info.features.shaderFloat64;
            opts->OutputMergerLogicOp = state_.info.features.logicOp;
            opts->MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_NONE;
            opts->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
            opts->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;  // Bindless descriptor indexing supported
            // No VK_EXT_shader_stencil_export is enabled and the translator emits no stencil-ref write, so a pixel
            // shader that writes the stencil ref would be refused: answer unsupported, the direction 4.1 prefers.
            opts->PSSpecifiedStencilRefSupported = FALSE;
            opts->TypedUAVLoadAdditionalFormats = TRUE;
            // POPS: the translator only declares PixelInterlockOrderedEXT, enabled exactly when the GPU offers
            // VK_EXT_fragment_shader_interlock and its fragmentShaderPixelInterlock feature.
            opts->ROVsSupported = state_.info.fragment_shader_interlock;
            opts->ConservativeRasterizationTier = state_.conservative_raster_tier;
            opts->MaxGPUVirtualAddressBitsPerResource = 40;
            opts->StandardSwizzle64KBSupported = FALSE;
            opts->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
            opts->CrossAdapterRowMajorTextureSupported = FALSE;
            // A translated vertex shader writing a layer or viewport index needs both Vulkan 1.2 features enabled
            // (vkCreateShaderModule refuses the module without them, VUID-...-pCode-08740).
            opts->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation =
                state_.info.shader_output_layer && state_.info.shader_output_viewport_index;
            opts->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_1;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS1: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1*>(data);
            const DeviceState::SubgroupFacts& facts = state_.subgroup_facts();
            *opts = {};
            opts->WaveOps = facts.wave_ops;
            opts->WaveLaneCountMin = facts.lane_min;
            opts->WaveLaneCountMax = facts.lane_max;
            // D3D12 counts every lane of the device here, and Vulkan has no such number. This is one full wave
            // (section 4.1): a game that sizes work from it plans for less parallelism than the GPU has, not more.
            opts->TotalLaneCount = facts.lane_max;
            opts->ExpandedComputeResourceStates = TRUE;  // Vulkan has no resource states to expand
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS8: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS8)) return d3d12_trace::result(method, E_INVALIDARG);
            // Vulkan BC image extents are in texels; only the transfer footprints round up to blocks.
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS8*>(data);
            opts->UnalignedBlockTexturesSupported = state_.info.texture_compression_bc;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS2: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS2*>(data);
            *opts = {};
            opts->DepthBoundsTestSupported = FALSE;
            opts->ProgrammableSamplePositionsTier = D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS3: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS3*>(data);
            *opts = {};
            opts->CopyQueueTimestampQueriesSupported = FALSE;
            // Only typeless resources set VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT.
            opts->CastingFullyTypedFormatSupported = FALSE;
            opts->WriteBufferImmediateSupportFlags = D3D12_COMMAND_LIST_SUPPORT_FLAG_NONE;
            opts->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
            opts->BarycentricsSupported = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS4: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4*>(data);
            *opts = {};
            // GetResourceAllocationInfo implements D3D12_SMALL_MSAA_RESOURCE_PLACEMENT_ALIGNMENT (64 KiB).
            opts->MSAA64KBAlignedTextureSupported = TRUE;
            opts->SharedResourceCompatibilityTier = D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
            opts->Native16BitShaderOpsSupported = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS5: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5*>(data);
            *opts = {};
            opts->SRVOnlyTiledResourceTier3 = FALSE;
            opts->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
            opts->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS7: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7*>(data);
            *opts = {};
            // Tier 1 is mesh PSOs and DispatchMesh, on VK_EXT_mesh_shader. Amplification shaders are part of the tier
            // but refused at PSO creation (the game has none); OPTIONS9's mesh statistics and full-range render
            // target array index stay FALSE.
            opts->MeshShaderTier =
                state_.info.mesh_shader ? D3D12_MESH_SHADER_TIER_1 : D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
            opts->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS9: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS9)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS9*>(data);
            *opts = {};
            opts->MeshShaderPipelineStatsSupported = FALSE;
            opts->MeshShaderSupportsFullRangeRenderTargetArrayIndex = FALSE;
            opts->AtomicInt64OnTypedResourceSupported = FALSE;
            opts->AtomicInt64OnGroupSharedSupported = FALSE;
            opts->DerivativesInMeshAndAmplificationShadersSupported = FALSE;
            opts->WaveMMATier = D3D12_WAVE_MMA_TIER_NOT_SUPPORTED;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS11: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS11)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS11*>(data);
            *opts = {};
            opts->AtomicInt64OnDescriptorHeapResourceSupported = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS13: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS13)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS13*>(data);
            *opts = {};
            opts->UnrestrictedBufferTextureCopyPitchSupported = FALSE;
            opts->UnrestrictedVertexElementAlignmentSupported = FALSE;
            opts->InvertedViewportHeightFlipsYSupported = FALSE;
            opts->InvertedViewportDepthFlipsZSupported = FALSE;
            opts->TextureCopyBetweenDimensionsSupported = FALSE;
            opts->AlphaBlendFactorSupported = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS14: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS14)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS14*>(data);
            *opts = {};
            opts->AdvancedTextureOpsSupported = FALSE;
            opts->WriteableMSAATexturesSupported = FALSE;
            opts->IndependentFrontAndBackStencilRefMaskSupported = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS15: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS15)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS15*>(data);
            *opts = {};
            opts->TriangleFanSupported = FALSE;
            opts->DynamicIndexBufferStripCutSupported = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS16: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS16)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS16*>(data);
            *opts = {};
            opts->DynamicDepthBiasSupported = FALSE;
            opts->GPUUploadHeapSupported = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS12: {
            // Vulkan 1.3's VkImageFormatListCreateInfo gives a mutable image the same relaxed format casting that
            // D3D12's castable-format lists describe. The layer also implements enhanced barriers (Barrier).
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12)) return E_INVALIDARG;
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS12*>(data);
            *opts = {};
            opts->MSPrimitivesPipelineStatisticIncludesCulledPrimitives = D3D12_TRI_STATE_UNKNOWN;
            opts->EnhancedBarriersSupported = TRUE;
            opts->RelaxedFormatCastingSupported = TRUE;
            return S_OK;
        }
        case D3D12_FEATURE_D3D12_OPTIONS17: {
            // Non-normalized samplers are core Vulkan (unnormalizedCoordinates), with the restrictions
            // CreateSampler2 checks. No manual write tracking, as the RTX 4090 answers too.
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS17)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS17*>(data);
            *opts = {};
            opts->NonNormalizedCoordinateSamplersSupported = TRUE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS18: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS18)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS18*>(data);
            *opts = {};
            opts->RenderPassesValid = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_D3D12_OPTIONS19: {
            if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS19)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS19*>(data);
            *opts = {};
            opts->MismatchingOutputDimensionsSupported = FALSE;
            opts->SupportedSampleCountsWithNoOutputs = state_.no_output_sample_counts;
            opts->PointSamplingAddressesNeverRoundUp = FALSE;
            opts->RasterizerDesc2Supported = FALSE;
            opts->NarrowQuadrilateralLinesSupported = FALSE;
            opts->AnisoFilterWithPointMipSupported = state_.info.sampler_anisotropy ? TRUE : FALSE;
            opts->MaxSamplerDescriptorHeapSize = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
            opts->MaxSamplerDescriptorHeapSizeWithStaticSamplers = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
            opts->MaxViewDescriptorHeapSize = D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2;
            opts->ComputeOnlyCustomHeapSupported = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case compat::kFeatureOptions20: {
            if (size != sizeof(compat::FeatureDataOptions20)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<compat::FeatureDataOptions20*>(data);
            *opts = {};
            opts->ComputeOnlyWriteWatchSupported = FALSE;
            opts->RecreateAtTier = compat::kRecreateAtTierNotSupported;
            return d3d12_trace::result(method, S_OK);
        }
        case compat::kFeatureOptions21: {
            if (size != sizeof(compat::FeatureDataOptions21)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* opts = static_cast<compat::FeatureDataOptions21*>(data);
            *opts = {};
            opts->WorkGraphsTier = compat::kWorkGraphsTierNotSupported;
            opts->ExecuteIndirectTier = compat::kExecuteIndirectTier1_0;
            opts->SampleCmpGradientAndBiasSupported = FALSE;
            opts->ExtendedCommandInfoSupported = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
            if (size != sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* va = static_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT*>(data);
            // The console and PC BDA support 48-bit GPU virtual addresses (section 4.1).
            va->MaxGPUVirtualAddressBitsPerResource = 40;
            va->MaxGPUVirtualAddressBitsPerProcess = 48;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_FORMAT_INFO: {
            if (size != sizeof(D3D12_FEATURE_DATA_FORMAT_INFO)) return E_INVALIDARG;
            auto* info = static_cast<D3D12_FEATURE_DATA_FORMAT_INFO*>(data);
            // Plane count describes the DXGI layout even when the backend cannot create the texture.
            switch (info->Format) {
            case DXGI_FORMAT_R32G8X24_TYPELESS:
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            case DXGI_FORMAT_R24G8_TYPELESS:
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            case DXGI_FORMAT_NV12:
            case DXGI_FORMAT_P010:
            case DXGI_FORMAT_P016:
            case DXGI_FORMAT_NV11:
                info->PlaneCount = 2;
                break;
            case DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE:
            case DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE:
            case DXGI_FORMAT_A4B4G4R4_UNORM:
                info->PlaneCount = 1;
                break;
            default:
                // Native D3D12 accepts UNKNOWN but rejects R1_UNORM and the gaps in the enum.
                if (UINT(info->Format) > DXGI_FORMAT_B4G4R4A4_UNORM || info->Format == DXGI_FORMAT_R1_UNORM)
                    return E_INVALIDARG;
                info->PlaneCount = 1;
            }
            return S_OK;
        }
        case D3D12_FEATURE_FORMAT_SUPPORT: {
            if (size != sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* fs = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT*>(data);
            fs->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
            fs->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
            // The host runtime's format table (compare_invalid_arguments, "format support:"): a value it has no entry
            // for is E_FAIL, not unsupported: past B4G4R4A4_UNORM (115) up to 188, which includes the planar video
            // formats P208/V208/V408 (130-132), and from 192 up; 189-191 are the sampler-feedback formats and
            // A4B4G4R4. UNKNOWN is a buffer format (raw and structured buffers).
            const UINT format_value = static_cast<UINT>(fs->Format);
            if (format_value > static_cast<UINT>(DXGI_FORMAT_B4G4R4A4_UNORM) &&
                (format_value < 189 || format_value > 191))
                return d3d12_trace::result(method, E_FAIL);
            if (fs->Format == DXGI_FORMAT_UNKNOWN) {
                fs->Support1 = D3D12_FORMAT_SUPPORT1_BUFFER;
                return d3d12_trace::result(method, S_OK);
            }
            // The plane formats a shader reads a depth-stencil texture through (CreateShaderResourceView takes them on
            // R24G8/R32G8X24 and D24S8/D32S8 textures) have no image of their own, so the lookups below find nothing.
            // The host reports them as sampled 2D and cube textures with mips, the depth plane also filtered and
            // compared. Left out, as for the depth formats themselves: 1D, and multisample loads.
            const bool depth_plane = fs->Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
                                     fs->Format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            if (depth_plane || fs->Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT ||
                fs->Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) {
                const bool d24 = fs->Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
                                 fs->Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT;
                VkFormat family = vulkan_format(d24 ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                                                true).format;
                if (family == VK_FORMAT_D24_UNORM_S8_UINT && !state_.depth_usable(family))
                    family = VK_FORMAT_D32_SFLOAT_S8_UINT;
                VkFormatProperties props{};
                if (state_.format_properties) state_.format_properties(state_.info.physical, family, &props);
                if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                    return d3d12_trace::result(method, DXGI_ERROR_UNSUPPORTED);
                fs->Support1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
                               D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_MIP;
                if (depth_plane && (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
                    fs->Support1 |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_SHADER_GATHER |
                                    D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON |
                                    D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON;
                return d3d12_trace::result(method, S_OK);
            }
            const Format fmt = texture_storage_format(fs->Format, false);
            const Format depth_fmt = vulkan_format(fs->Format, true);
            VkFormat vk_fmt = fmt.format != VK_FORMAT_UNDEFINED ? fmt.format : depth_fmt.format;
            // Report what creation makes: a D24 texture on a GPU without Vulkan D24 (AMD) is a D32_SFLOAT_S8_UINT
            // image (texture_format), so its features are D32S8's. Asking D24 itself gave Support1 = 0 there,
            // although CreateCommittedResource(ALLOW_DEPTH_STENCIL) succeeds.
            if (vk_fmt == VK_FORMAT_D24_UNORM_S8_UINT && !state_.depth_usable(vk_fmt))
                vk_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
            if (vk_fmt == VK_FORMAT_UNDEFINED || !state_.format_properties) {
                return d3d12_trace::result(method, DXGI_ERROR_UNSUPPORTED);
            }
            if (fs->Format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
                // The host reports this display format as 2D, display, cast-within-bit-layout, back-buffer-cast and
                // video-processor output, and no shader reads. The layer has the 2D resource and the bit-compatible
                // view cast; it has no display scan-out or video processor, so it reports only what it provides.
                fs->Support1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;
                return S_OK;
            }
            VkFormatProperties props{};
            state_.format_properties(state_.info.physical, vk_fmt, &props);
            VkFormatFeatureFlags optimal = props.optimalTilingFeatures;
            VkFormatFeatureFlags buffer = props.bufferFeatures;
            if (fmt.rgb96) {
                // Textures are RGBA32 images that are sampled and copied, nothing else yet (no render target,
                // no unordered access: creation refuses them). Buffers hold the real 12-byte format; the PC
                // runtime (WARP) has no typed unordered access of these formats, in buffers either.
                optimal &= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
                VkFormatProperties narrow{};
                state_.format_properties(state_.info.physical, rgb96_named_format(vk_fmt), &narrow);
                buffer = narrow.bufferFeatures &
                         ~VkFormatFeatureFlags(VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT |
                                               VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT);
            }

            D3D12_FORMAT_SUPPORT1 s1 = D3D12_FORMAT_SUPPORT1_NONE;
            D3D12_FORMAT_SUPPORT2 s2 = D3D12_FORMAT_SUPPORT2_NONE;

            if (fs->Format == DXGI_FORMAT_A8_UNORM) {
                // A8 render targets are R8_UNORM images (the view moves the bits into alpha), so the format reports
                // what the host can actually attach; measured: the title sizes one on this title's output path.
                if (state_.color_attachment_usable(VK_FORMAT_R8_UNORM))
                    fs->Support1 |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
                if (optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
                    fs->Support1 = (fs->Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) |
                                   D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
                                   D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
                    if (optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                        fs->Support1 |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_SHADER_GATHER;
                    VkImageFormatProperties limits{};
                    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                    if (state_.image_format_properties &&
                        state_.image_format_properties(state_.info.physical, vk_fmt, VK_IMAGE_TYPE_2D,
                                                       VK_IMAGE_TILING_OPTIMAL, usage, 0, &limits) == VK_SUCCESS &&
                        limits.maxMipLevels > 1)
                        fs->Support1 |= D3D12_FORMAT_SUPPORT1_MIP;
                }
                return d3d12_trace::result(method, S_OK);
            }

            // D3D12 disallows typed buffer views for the RGBA8 and BGRA8 sRGB formats (including BGRX8).
            // Vulkan can still advertise buffer features for their backing formats.
            if (buffer != 0 && vk_fmt != VK_FORMAT_R8G8B8A8_SRGB && vk_fmt != VK_FORMAT_B8G8R8A8_SRGB)
                s1 |= D3D12_FORMAT_SUPPORT1_BUFFER;
            // Only what CreateGraphicsPipelineState's input layouts accept (vertex_format): R8/R16 SNORM and SINT
            // have Vulkan vertex support but no input-layout mapping yet.
            VkFormat vertex_vk = VK_FORMAT_UNDEFINED;
            UINT vertex_bytes = 0;
            if ((buffer & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) && vertex_format(fs->Format, vertex_vk, vertex_bytes))
                s1 |= D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER;
            if (fs->Format == DXGI_FORMAT_R16_UINT || fs->Format == DXGI_FORMAT_R32_UINT)
                s1 |= D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER;

            if (optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_TEXTURE1D | D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                      D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
                      D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
            }
            if (buffer & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
            }
            if (optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
            }
            if (optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
                // A D3D12 multisample resolve needs the same image usages as texture_create_info(): colour
                // attachment plus transfer source/destination (and the ordinary sampled usage).
                VkImageFormatProperties image_properties{};
                const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                const VkImageCreateFlags flags = fmt.typeless ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
                if (state_.image_format_properties &&
                    state_.image_format_properties(state_.info.physical, vk_fmt, VK_IMAGE_TYPE_2D,
                                                   VK_IMAGE_TILING_OPTIMAL, usage, flags, &image_properties) == VK_SUCCESS &&
                    (image_properties.sampleCounts & d3d12_multisample_sample_counts)) {
                    s1 |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET |
                          D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE;
                    if ((optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !fmt.typeless &&
                        fmt.aspects == VK_IMAGE_ASPECT_COLOR_BIT && palmetto::gfx10::format_from_vulkan(vk_fmt) &&
                        state_.bindless && state_.bindless->supports_ms_class(DeviceState::numeric_class(vk_fmt)))
                        s1 |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD;
                }
            }
            if (optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_BLENDABLE;
            }
            if (optimal & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                s1 |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
            }
            // B8G8R8X8 has no typed UAV in D3D12 (texture_format refuses it), whatever the BGRA8 image could do.
            if (!alpha_unused(fs->Format) && ((optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
                                              (buffer & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT))) {
                s1 |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
                // A typed UAV texture read is not a storage-image read: the translator reads it through the
                // texture's sampled binding with the T#'s numeric type (rdna2spirv.py image_read_typed), so a
                // format that can be stored can be read. Typed buffer UAVs share the store side's limits: the layer
                // creates no buffer UAV views yet, and rdna2spirv.py FORMATS decodes only some buffer formats.
                s2 |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE | D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
                // Image atomics go through the r32ui storage view, which an R32_UINT UAV view gets
                // (StorageViews::r32ui). Today that is 2D UAVs only: the translator's image atomics cover 2D and 3D,
                // the bindless set has r32ui slots for those two, and the layer creates no 3D UAV views yet. There
                // is no image_atomic_swap, so no exchange.
                if (vk_fmt == VK_FORMAT_R32_UINT && (optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT)) {
                    s2 |= D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
                          D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
                          D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
                          D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
                }
            }
            {
                // D3D12's per-format rules, as native D3D12 reports them (RX 9070 XT, AMD 26.11.2, and WARP):
                // the Vulkan feature bits above say what the image can do, not what D3D12 lets the format do.
                const Format& chosen = fmt.format != VK_FORMAT_UNDEFINED ? fmt : depth_fmt;
                const bool depth = (chosen.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
                const bool block_compressed = vk_fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && vk_fmt <= VK_FORMAT_BC7_SRGB_BLOCK;
                // Gathers read the sampled view like a sample does (OpImageGather); the host reports one with the other.
                if (s1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) s1 |= D3D12_FORMAT_SUPPORT1_SHADER_GATHER;
                // Comparison sampling and gathers (SampleCmp, GatherCmp with a comparison sampler) are D3D12's for the
                // depth-plane view formats only; R32_FLOAT and R16_UNORM are the views of D32 and D16 textures.
                if ((s1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) &&
                    (fs->Format == DXGI_FORMAT_R32_FLOAT || fs->Format == DXGI_FORMAT_R16_UNORM))
                    s1 |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON | D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON;
                // A mip chain where the image can have one (asked with its sampled or depth-stencil usage).
                if (s1 & (D3D12_FORMAT_SUPPORT1_TEXTURE1D | D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                          D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
                          D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) {
                    const VkImageUsageFlags usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                          : VK_IMAGE_USAGE_SAMPLED_BIT;
                    VkImageFormatProperties limits{};
                    if (state_.image_format_properties &&
                        state_.image_format_properties(state_.info.physical, vk_fmt, VK_IMAGE_TYPE_2D,
                                                       VK_IMAGE_TILING_OPTIMAL, usage,
                                                       chosen.typeless ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
                                                       &limits) == VK_SUCCESS &&
                        limits.maxMipLevels > 1)
                        s1 |= D3D12_FORMAT_SUPPORT1_MIP;
                    // Depth formats multisample as depth-stencil targets; the colour-attachment check above never
                    // sees them. Same usages as MULTISAMPLE_QUALITY_LEVELS and resource creation ask for.
                    if (depth && !chosen.typeless && (s1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) &&
                        state_.image_format_properties &&
                        state_.image_format_properties(state_.info.physical, vk_fmt, VK_IMAGE_TYPE_2D,
                                                       VK_IMAGE_TILING_OPTIMAL,
                                                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                       0, &limits) == VK_SUCCESS &&
                        (limits.sampleCounts & d3d12_multisample_sample_counts))
                        s1 |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET;
                }
                // No resolve of integer formats (ResolveSubresource takes float, UNORM and SNORM only).
                if (DeviceState::numeric_class(vk_fmt) != 0) s1 &= ~D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE;
                // Block-compressed textures are 2D, 3D or cubes: 1D has no 4x4 blocks.
                if (block_compressed) s1 &= ~D3D12_FORMAT_SUPPORT1_TEXTURE1D;
                // Depth formats are 2D and cube resources whose shaders read and write them through a colour view
                // (R32_FLOAT, R24_UNORM_X8_TYPELESS, ...): not 3D, not shader resources or UAVs themselves (NVIDIA
                // gives D32_SFLOAT and D16_UNORM STORAGE_IMAGE). Native D3D12 also allows 1D, but the layer refuses
                // 1D depth-stencil resources (CreateCommittedResource).
                if (depth) {
                    s1 &= ~(D3D12_FORMAT_SUPPORT1_TEXTURE1D | D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                            D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_SHADER_GATHER |
                            D3D12_FORMAT_SUPPORT1_BUFFER | D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER |
                            D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
                    s2 = D3D12_FORMAT_SUPPORT2_NONE;
                }
                // A TYPELESS format is only storage: views give it a type, so it is a texture with mips and
                // nothing a view would do (no buffer, shader, render-target or UAV use of its own).
                if (chosen.typeless) {
                    s1 &= D3D12_FORMAT_SUPPORT1_TEXTURE1D | D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                          D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
                          D3D12_FORMAT_SUPPORT1_MIP;
                    s2 = D3D12_FORMAT_SUPPORT2_NONE;
                }
            }
            if (fs->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP && state_.r9g9b9e5_uav_emulated) {
                s1 |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
                // Reads go through the sampled E5B9G9R9 view, like every typed UAV read (see above).
                s2 |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE | D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
            }
            if (fs->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                // Keep the native attachment and resolve bits. Blending and shader multisample loads remain masked.
                s1 &= ~(D3D12_FORMAT_SUPPORT1_BLENDABLE | D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL |
                        D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD);
            }
            fs->Support1 = s1;
            fs->Support2 = s2;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
            if (size != sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* levels = static_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS*>(data);
            // RTX 5070 / 621.14/621.24: a format outside the runtime's table (and R1_UNORM) fails with the caller's
            // output untouched; a known format clears it before the sample count is checked.
            const UINT format_id = UINT(levels->Format);
            if (levels->Format == DXGI_FORMAT_R1_UNORM ||
                !(format_id <= DXGI_FORMAT_B4G4R4A4_UNORM ||
                  (format_id >= DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE &&
                   format_id <= DXGI_FORMAT_A4B4G4R4_UNORM)))
                return d3d12_trace::result(method, E_FAIL);
            levels->NumQualityLevels = 0;
            // TILED_RESOURCE asks about tiled (reserved, 64KB_UNDEFINED_SWIZZLE) textures. Measured natively on an
            // RX 7800 XT, a Radeon iGPU and WARP: 1x and 4x keep the plain answer, every other count has none, and a
            // format without a tile shape (96-bit) has none at any count. That is GetResourceAllocationInfo's rule
            // here (tiled MSAA is 4x only). 128-bit formats at 4x: the AMD drivers answer 0, WARP and the RTX 5070 Ti
            // allocate them; this follows the latter, as the allocation path does.
            // RTX 5070 / 621.14: undefined flag bits are ignored; bad counts fail, known formats have 1x.
            const bool tiled = (levels->Flags & D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE) != 0;
            if (levels->SampleCount == 0 || levels->SampleCount > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT)
                return d3d12_trace::result(method, E_FAIL);
            Format format = texture_storage_format(levels->Format, false);
            if (levels->SampleCount == 1) {
                levels->NumQualityLevels = tiled && format.rgb96 ? 0 : 1;
                return d3d12_trace::result(method, S_OK);
            }
            if ((levels->SampleCount & (levels->SampleCount - 1)) != 0)
                return d3d12_trace::result(method, S_OK);
            const bool alpha_only = levels->Format == DXGI_FORMAT_A8_UNORM;
            if (alpha_only && levels->SampleCount != 1) return d3d12_trace::result(method, S_OK);
            if (levels->Format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM && levels->SampleCount != 1)
                return d3d12_trace::result(method, S_OK);  // single-sample 2D only; no quality levels
            if (format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
                D3D12_RESOURCE_DESC depth_desc{};
                depth_desc.Format = levels->Format;
                depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                format = state_.texture_format(depth_desc);  // includes the D24-to-D32 fallback used by creation
            }
            if (format.format == VK_FORMAT_UNDEFINED || !state_.image_format_properties)
                return d3d12_trace::result(method, S_OK);
            if (format.rgb96) return d3d12_trace::result(method, S_OK);  // no 96-bit render targets yet

            // Vulkan exposes one standard sample pattern per supported count, so each supported count maps to
            // one D3D12 quality level (quality index 0). Query the matching colour or depth/stencil attachment
            // usage, plus the transfer and sampled usages used by the resource implementation.
            VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT;
            if (format.aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
                if (!alpha_only) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            } else if (format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
                usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            else
                return d3d12_trace::result(method, DXGI_ERROR_UNSUPPORTED);
            const VkImageCreateFlags flags = format.typeless ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
            VkImageFormatProperties image_properties{};
            const VkResult result = state_.image_format_properties(state_.info.physical, format.format,
                                                                   VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                                   usage, flags, &image_properties);
            const VkSampleCountFlagBits samples = VkSampleCountFlagBits(levels->SampleCount);
            if (result == VK_SUCCESS && (image_properties.sampleCounts & samples))
                levels->NumQualityLevels = 1;
            if (tiled && levels->SampleCount != 1 && levels->SampleCount != 4) levels->NumQualityLevels = 0;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_SHADER_MODEL: {
            if (size != sizeof(D3D12_FEATURE_DATA_SHADER_MODEL)) return E_INVALIDARG;
            auto* model = static_cast<D3D12_FEATURE_DATA_SHADER_MODEL*>(data);
            // Measured against the host PC runtime: a request above the supported model is clamped down to it, 5.1
            // is echoed because it is D3D12's own baseline (WARP and the RTX 5070 both answer it with S_OK), and a
            // value below it is refused with E_INVALIDARG (0x5f crashed the host, so the test does not probe it).
            // Refuse the same way instead of echoing an unrepresentable value back: below 5.1, and between 5.1 and
            // 6.0, where no runtime defines a model (0x52-0x5f; E_INVALIDARG on the host runtime of an RTX 5070).
            const UINT requested = static_cast<UINT>(model->HighestShaderModel);
            if (requested < D3D_SHADER_MODEL_5_1 ||
                (requested > D3D_SHADER_MODEL_5_1 && requested < D3D_SHADER_MODEL_6_0))
                return E_INVALIDARG;
            if (model->HighestShaderModel > kHighestShaderModel) model->HighestShaderModel = kHighestShaderModel;
            return S_OK;
        }
        case D3D12_FEATURE_ROOT_SIGNATURE: {
            if (size != sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE)) return E_INVALIDARG;
            auto* root = static_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE*>(data);
            // 1.0 to 1.2 are defined (1.2 by number: not every SDK names it). The host refuses anything else and leaves
            // the value alone (RTX 4070 Laptop 621.24 and WARP), so only 1.2 is clamped.
            const UINT requested = static_cast<UINT>(root->HighestVersion);
            if (requested < static_cast<UINT>(D3D_ROOT_SIGNATURE_VERSION_1_0) || requested > 0x3) return E_INVALIDARG;
            if (root->HighestVersion > kHighestRootSignatureVersion)
                root->HighestVersion = kHighestRootSignatureVersion;
            return S_OK;
        }
        case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT: {
            if (size != sizeof(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT))
                return d3d12_trace::result(method, E_INVALIDARG);
            auto* prot = static_cast<D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT*>(data);
            prot->Support = D3D12_PROTECTED_RESOURCE_SESSION_SUPPORT_FLAG_NONE;
            if (prot->NodeIndex != 0) return d3d12_trace::result(method, E_INVALIDARG);
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT: {
            if (size != sizeof(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPE_COUNT))
                return d3d12_trace::result(method, E_INVALIDARG);
            auto* prot_count = static_cast<D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPE_COUNT*>(data);
            if (prot_count->NodeIndex != 0) return d3d12_trace::result(method, E_INVALIDARG);
            prot_count->Count = 0;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES: {
            if (size != sizeof(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES))
                return d3d12_trace::result(method, E_INVALIDARG);
            const auto* prot_types = static_cast<const D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES*>(data);
            // Zero protected resource session types exist, so only Count == 0 on node 0 matches TYPE_COUNT.
            if (prot_types->NodeIndex != 0 || prot_types->Count != 0)
                return d3d12_trace::result(method, E_INVALIDARG);
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_SHADER_CACHE: {
            if (size != sizeof(D3D12_FEATURE_DATA_SHADER_CACHE)) return d3d12_trace::result(method, E_INVALIDARG);
            static_cast<D3D12_FEATURE_DATA_SHADER_CACHE*>(data)->SupportFlags = D3D12_SHADER_CACHE_SUPPORT_NONE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
            if (size != sizeof(D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY))
                return d3d12_trace::result(method, E_INVALIDARG);
            auto* prio = static_cast<D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY*>(data);
            prio->PriorityForTypeIsSupported = FALSE;
            if (UINT(prio->CommandListType) > D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE ||
                (prio->Priority != D3D12_COMMAND_QUEUE_PRIORITY_NORMAL &&
                 prio->Priority != D3D12_COMMAND_QUEUE_PRIORITY_HIGH &&
                 prio->Priority != D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME))
                return d3d12_trace::result(method, E_INVALIDARG);
            // DIRECT/COMPUTE/COPY report NORMAL only: all VkQueues share priority 1.0f (HIGH has no distinct scheduling)
            // and GLOBAL_REALTIME is refused in CreateCommandQueue.
            const bool supported_type = prio->CommandListType == D3D12_COMMAND_LIST_TYPE_DIRECT ||
                                        prio->CommandListType == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
                                        prio->CommandListType == D3D12_COMMAND_LIST_TYPE_COPY;
            const bool supported_priority = prio->Priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            prio->PriorityForTypeIsSupported = (supported_type && supported_priority) ? TRUE : FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_EXISTING_HEAPS:
            return write_supported_feature<D3D12_FEATURE_DATA_EXISTING_HEAPS>(
                method, data, size, state_.get_host_pointer_properties ? TRUE : FALSE);
        case D3D12_FEATURE_SERIALIZATION: {
            if (size != sizeof(D3D12_FEATURE_DATA_SERIALIZATION)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* ser = static_cast<D3D12_FEATURE_DATA_SERIALIZATION*>(data);
            ser->HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
            if (ser->NodeIndex != 0) return d3d12_trace::result(method, E_INVALIDARG);
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_CROSS_NODE: {
            if (size != sizeof(D3D12_FEATURE_DATA_CROSS_NODE)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* cross = static_cast<D3D12_FEATURE_DATA_CROSS_NODE*>(data);
            cross->SharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
            cross->AtomicShaderInstructions = FALSE;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_DISPLAYABLE: {
            if (size != sizeof(D3D12_FEATURE_DATA_DISPLAYABLE)) return d3d12_trace::result(method, E_INVALIDARG);
            auto* disp = static_cast<D3D12_FEATURE_DATA_DISPLAYABLE*>(data);
            disp->DisplayableTexture = FALSE;
            disp->SharedResourceCompatibilityTier = D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
            return d3d12_trace::result(method, S_OK);
        }
        case D3D12_FEATURE_QUERY_META_COMMAND:
            // EnumerateMetaCommands reports 0 meta-commands, so any query is rejected (as on host D3D12).
            return d3d12_trace::result(method, E_INVALIDARG);
        case compat::kFeaturePredication:
            return write_supported_feature<compat::FeatureSupportFlag>(
                method, data, size, state_.info.conditional_rendering ? TRUE : FALSE);
        case compat::kFeatureApplicationSpecificDriverState:
            return write_supported_feature<compat::FeatureSupportFlag>(method, data, size, FALSE);
        case compat::kFeatureBytecodeBypassHashSupported:
            return write_supported_feature<compat::FeatureSupportFlag>(method, data, size, FALSE);
        default:
            // Not answered yet. The log above lists every query; answers are added as the trace asks for them.
            return d3d12_trace::result(method, E_INVALIDARG);
        }
    }

private:
    IUnknown* identity() { return outer_ ? outer_ : static_cast<ID3D12Device*>(this); }

    static constexpr D3D12_RESOURCE_ALLOCATION_INFO kInvalidAllocation{UINT64_MAX,
                                                                       D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
    // Saturates at the last multiple of `to` instead of wrapping, as the PC runtime's totals do.
    static UINT64 align_up(UINT64 value, UINT64 to) {
        return value > UINT64_MAX - (to - 1) ? UINT64_MAX / to * to : (value + to - 1) / to * to;
    }

    // Lays `count` resources out one after another; `each` (optional) gets every resource's offset, size and
    // alignment. As on the PC runtime, the total is rounded up to the largest alignment (1 for no resources) and
    // saturates near 2^64 rather than wrapping.
    template<class Desc, class Get>
    D3D12_RESOURCE_ALLOCATION_INFO place(UINT count, const Desc* descs, Get get,
                                         D3D12_RESOURCE_ALLOCATION_INFO1* each) {
        D3D12_RESOURCE_ALLOCATION_INFO total{0, 1};
        bool failed = count && !descs;
        for (UINT i = 0; i < count; ++i) {
            const D3D12_RESOURCE_ALLOCATION_INFO one = failed ? kInvalidAllocation : allocation(get(i));
            failed = failed || one.SizeInBytes == UINT64_MAX;
            if (failed) {
                if (each) each[i] = {UINT64_MAX, one.Alignment, UINT64_MAX};
                continue;
            }
            const UINT64 offset = align_up(total.SizeInBytes, one.Alignment);
            if (each) each[i] = {offset, one.Alignment, one.SizeInBytes};
            total = {one.SizeInBytes > UINT64_MAX - offset ? UINT64_MAX : offset + one.SizeInBytes,
                     std::max(total.Alignment, one.Alignment)};
        }
        return failed ? kInvalidAllocation
                      : D3D12_RESOURCE_ALLOCATION_INFO{align_up(total.SizeInBytes, total.Alignment), total.Alignment};
    }

    // One resource's size and alignment, or kInvalidAllocation for a description D3D12 (or this device) rejects.
    D3D12_RESOURCE_ALLOCATION_INFO allocation(const D3D12_RESOURCE_DESC& desc) const {
        constexpr UINT64 align_4k = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT,
                         align_64k = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                         align_msaa_64k = D3D12_SMALL_MSAA_RESOURCE_PLACEMENT_ALIGNMENT,
                         align_msaa_4m = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
        constexpr D3D12_RESOURCE_FLAGS attachment =
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            // Host D3D12 accepts ALLOW_RENDER_TARGET, ALLOW_CROSS_ADAPTER, UAV, DENY_SHADER_RESOURCE,
            // RAYTRACING_ACCELERATION_STRUCTURE and 0x400 on buffers; refuses ALLOW_DEPTH_STENCIL,
            // ALLOW_SIMULTANEOUS_ACCESS, VIDEO_DECODE_REFERENCE_ONLY and other flags (D3D-467).
            constexpr UINT buffer_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                          D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
                                          D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER |
                                          D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE | 0x400;
            if (!desc.Width || desc.Width > UINT64_MAX - align_64k || desc.Height != 1 || desc.DepthOrArraySize != 1 ||
                desc.MipLevels != 1 || desc.Format != DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 ||
                desc.SampleDesc.Quality != 0 || desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
                (desc.Alignment && desc.Alignment != align_64k) || (UINT(desc.Flags) & ~buffer_flags))
                return kInvalidAllocation;
            return {align_up(desc.Width, align_64k), align_64k};
        }
        const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const bool one_d = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        if (!volume && !one_d && desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_UNKNOWN)
                log_once("GetResourceAllocationInfo dimension not supported:", desc.Dimension);
            return kInvalidAllocation;
        }
        if (one_d && desc.Height != 1) return kInvalidAllocation;
        const UINT samples = desc.SampleDesc.Count, depth = volume ? desc.DepthOrArraySize : 1;
        const UINT64 max_extent =
            volume ? D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION : D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        const UINT max_layers =
            volume ? D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION : D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
        if (!desc.Width || !desc.Height || !desc.DepthOrArraySize || desc.Width > max_extent ||
            desc.Height > max_extent || desc.DepthOrArraySize > max_layers || !samples || samples > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT ||
            (samples & (samples - 1)) || (samples > 1 && desc.SampleDesc.Quality != 0) ||
            desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
            return kInvalidAllocation;
        const UINT16 full = full_mips(desc.Width, desc.Height, depth);
        D3D12_RESOURCE_DESC resolved = desc;
        if (!resolved.MipLevels) resolved.MipLevels = full;
        if (resolved.MipLevels > full) return kInvalidAllocation;
        const bool depth_stencil = desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if ((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) && !depth_stencil)
            return kInvalidAllocation;
        if (samples > 1 && (volume || one_d || resolved.MipLevels != 1 || !(desc.Flags & attachment) ||
                            (desc.Flags & attachment) == attachment ||
                            (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
            return kInvalidAllocation;
        // The PC runtime: simultaneous access only single-sampled and without depth-stencil, and no volume depth.
        if (((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) && (depth_stencil || samples > 1)) ||
            (volume && depth_stencil))
            return kInvalidAllocation;
        const bool tiled = is_tiled_layout(desc.Layout);
        if (tiled) {
            if (one_d || (samples > 1 && samples != 4)) return kInvalidAllocation;
            if (desc.Alignment && desc.Alignment != align_64k) return kInvalidAllocation;
        } else if (desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN) {
            log_once("GetResourceAllocationInfo layout as UNKNOWN:", desc.Layout);
        }

        const Format format = state_.texture_format(desc);
        if (!format.format) {
            log_once("GetResourceAllocationInfo format not supported yet:", desc.Format);
            return kInvalidAllocation;
        }
        VkMemoryRequirements need{};
        const TextureCreateInfo create(resolved, format, state_.color_attachment_usable(format.format));
        if (!state_.memory_requirements(create.info, need)) return kInvalidAllocation;
        if (tiled) {
            palmetto::ResourceTiling tiling;
            if (!resource_tiling(resolved, tiling)) return kInvalidAllocation;
            return {UINT64(tiling.total_tiles) * palmetto::tile_size_in_bytes, align_64k};
        }

        // D3D12's alignment: 64 KiB, or 4 MiB with MSAA, unless the description asks for a small one (4 KiB, 64 KiB
        // with MSAA). 4 KiB needs the top mip of one array slice to fit in 16 tiles of 4 KiB and the resource not to
        // be a render target or depth buffer; mips and array slices don't count. If it doesn't qualify, the PC
        // runtime calls the description invalid rather than answering with 64 KiB. The MSAA 64 KiB alignment has no
        // size limit there. Both measured in d3d12_vulkan_device_test.
        UINT64 alignment = 0;
        if (samples > 1) {
            if (desc.Alignment && desc.Alignment != align_msaa_64k && desc.Alignment != align_msaa_4m)
                return kInvalidAllocation;
            alignment = desc.Alignment ? desc.Alignment : align_msaa_4m;
        } else {
            if (desc.Alignment && desc.Alignment != align_4k && desc.Alignment != align_64k) return kInvalidAllocation;
            if (desc.Alignment == align_4k && ((desc.Flags & attachment) || !small_tiles_fit(desc, format, need.size)))
                return kInvalidAllocation;
            alignment = desc.Alignment ? desc.Alignment : align_64k;
        }
        return {align_up(need.size, alignment), alignment};
    }

    // Whether 16 tiles of 4 KiB (palmetto::small_tile_shape; 4096 bytes of a row for 1D) cover the top mip of one
    // array slice, the PC runtime's test for D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT.
    static bool small_tiles_fit(const D3D12_RESOURCE_DESC& desc, const Format& format, VkDeviceSize vulkan_size) {
        const palmetto::BlockFormat block = palmetto::block_format(format.format, VK_IMAGE_ASPECT_COLOR_BIT);
        const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        palmetto::TileShape shape = palmetto::small_tile_shape(volume, block.bytes);
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && shape.width) shape = {4096 / block.bytes, 1, 1};
        // shortcut: untileable element sizes (3- and 12-byte formats) keep the whole-resource 64 KiB test; measure
        // them on the PC runtime if a game asks for one.
        if (!shape.width) return vulkan_size <= D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        const auto tiles = [](UINT64 texels, UINT64 per_block, UINT64 per_tile) {
            return ((texels + per_block - 1) / per_block + per_tile - 1) / per_tile;
        };
        return tiles(desc.Width, block.block, shape.width) * tiles(desc.Height, block.block, shape.height) *
                   tiles(volume ? desc.DepthOrArraySize : 1, 1, shape.depth) <= 16;
    }

    // Strict lower bound on the heap bytes a non-tiled single-sample texture needs: sums uncompressed texel/block
    // payload across subresources and rounds to `alignment`, capped at `fallback` (Vulkan allocation size). Avoids
    // rejecting host-valid placed textures when Vulkan driver padding exceeds D3D12's GetResourceAllocationInfo.
    static UINT64 min_placed_texture_bytes(const D3D12_RESOURCE_DESC& desc, const Format& format, UINT64 alignment,
                                           UINT64 fallback) {
        const VkImageAspectFlags aspect =
            (format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : format.aspects;
        const palmetto::BlockFormat block = footprint_block(format, aspect);
        if (!block.bytes || !alignment) return fallback;
        const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const bool one_d = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        const UINT depth = volume ? desc.DepthOrArraySize : 1;
        const UINT slices = volume ? 1 : desc.DepthOrArraySize;
        const UINT mips = desc.MipLevels ? desc.MipLevels : full_mips(desc.Width, desc.Height, depth);
        UINT64 per_slice = 0;
        for (UINT m = 0; m < mips; ++m) {
            const UINT64 w = std::max<UINT64>(1, desc.Width >> m);
            const UINT64 h = one_d ? 1 : std::max<UINT64>(1, desc.Height >> m);
            const UINT64 d = volume ? std::max<UINT64>(1, depth >> m) : 1;
            const UINT64 bw = (w + block.block - 1) / block.block;
            const UINT64 bh = (h + block.block - 1) / block.block;
            if (bw > UINT64_MAX / bh) return fallback;
            const UINT64 blocks = bw * bh;
            if (blocks > UINT64_MAX / d) return fallback;
            const UINT64 count = blocks * d;
            if (count > UINT64_MAX / block.bytes) return fallback;
            const UINT64 mip_bytes = count * block.bytes;
            if (per_slice > UINT64_MAX - mip_bytes) return fallback;
            per_slice += mip_bytes;
        }
        if (slices && per_slice > UINT64_MAX / slices) return fallback;
        const UINT64 total = per_slice * slices;
        if (total > UINT64_MAX - (alignment - 1)) return fallback;
        return std::min(fallback, align_up(total, alignment));
    }

    // Writes the view's T# into the slot and makes sure set 0 and the hash table know it. A view that cannot be
    // shown leaves the slot a null descriptor.
    void write_image_descriptor(const Resource& resource, const TextureView& view,
                                D3D12_CPU_DESCRIPTOR_HANDLE destination, bool storage) {
        palmetto::gfx10::Image image{};
        if (!image_descriptor(resource, view, image)) return;
        const palmetto::gfx10::ImageWords words = palmetto::gfx10::encode(image);
        const bool registered = state_.bind_image(words, image, resource.image(), resource.format(),
                                                  resource.aspects(), storage, resource.has_view_format_list(),
                                                  resource.uint_backed(), resource.id(), resource.placed_read_pages(),
                                                  resource.placed_subresource_bytes());
        if (registered && resource.sparse_image()) state_.context->note_shader_image(resource.id());
        if (palmetto::gfx10::multisampled(image.type) && !registered) return;
        std::memcpy(reinterpret_cast<void*>(destination.ptr), words.data(), sizeof(words));
    }

    // Validation shared by CreateCommittedResource and CreateCommittedResource3; internal callers pass null `method`
    // to skip trace recording. `defer_image_memory` is the slot-133 texture path's request to create the stand-in
    // without memory and back it at first use (docs/streamed_image_backing.md); every other caller backs at creation.
    HRESULT create_committed_validated(const char* method, const D3D12_HEAP_PROPERTIES* heap,
                                       D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* desc,
                                       D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                       UINT32 castable_count, const DXGI_FORMAT* castable, REFIID riid, void** out,
                                       bool defer_image_memory = false) {
        const auto finish = [&](HRESULT hr) { return method ? d3d12_trace::result(method, hr) : hr; };
        if (out) *out = nullptr;
        if (!heap || !desc) return finish(E_INVALIDARG);
        if (const char* why = committed_resource_refusal(*heap, heap_flags, *desc, state, clear,
                                                         acceleration_structures(), state_.uma)) {
            log_once((std::string(method ? method : "CreateCommittedResource") + " refused (" + why + "), dimension")
                         .c_str(),
                     int(desc->Dimension));
            return finish(E_INVALIDARG);
        }
        // The sample quality is checked above as the host does it here: CreateCommittedResource accepts a quality on a
        // 1x texture while GetResourceAllocationInfo requires 0; the other texture checks stay shared with allocation.
        D3D12_RESOURCE_DESC checked = *desc;
        if (checked.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) checked.SampleDesc.Quality = 0;
        if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && allocation(checked).SizeInBytes == UINT64_MAX) {
            log_once((std::string(method ? method : "CreateCommittedResource") +
                      " refused (texture size, mips, samples or alignment), dimension")
                         .c_str(),
                     int(desc->Dimension));
            return finish(E_INVALIDARG);
        }
        return finish(create_committed(heap, heap_flags, desc, state, castable_count, castable, riid, out,
                                       defer_image_memory));
    }

    // Shared by CreateCommittedResource, CreateCommittedResource3 and CreateReservedResource*. A castable-format list
    // is only meaningful on textures; the layer reports RelaxedFormatCastingSupported, so each format is mapped and
    // handed to Vulkan as a VkImageFormatListCreateInfo. A nonzero count with a null list is no list, as on the PC
    // runtime.
    HRESULT create_committed(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
                             const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initial_state,
                             UINT32 castable_count, const DXGI_FORMAT* castable, REFIID riid, void** out,
                             bool defer_image_memory = false) {
        if (out) *out = nullptr;
        if (!heap || !desc) return E_INVALIDARG;
        if (castable_count && castable && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            log_once("castable formats on a buffer are invalid; count", int(castable_count));
            return E_INVALIDARG;
        }
        auto* resource = new Resource(state_, *desc, *heap, heap_flags);
        resource->attach_device(identity());
        HRESULT hr = E_NOTIMPL;
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            hr = desc->SampleDesc.Count == 1 && desc->SampleDesc.Quality == 0
                     ? resource->create_buffer(initial_state)
                     : E_INVALIDARG;
        } else if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
                   desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            log_once("committed resource dimension not supported:", desc->Dimension);
        } else if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && desc->Height != 1) {
            log_once("1D texture height must be 1:", int(desc->Height));
            hr = E_INVALIDARG;
        } else if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
                   (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
            log_once("volume depth-stencil resources not supported, flags:", int(desc->Flags));
        } else if (heap->Type == D3D12_HEAP_TYPE_UPLOAD || heap->Type == D3D12_HEAP_TYPE_READBACK) {
            hr = E_INVALIDARG;  // D3D12 places textures on default heaps unless they are row-major
        } else if (const UINT samples = desc->SampleDesc.Count;
                   !samples || samples > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT ||
                   (samples > 1 && desc->SampleDesc.Quality &&
                    desc->SampleDesc.Quality != DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN &&
                    desc->SampleDesc.Quality != DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN) ||
                   (samples > 1 && ((samples & (samples - 1)) || desc->MipLevels != 1 ||
                                    desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                                    !(desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                     D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))))) {
            hr = E_INVALIDARG;  // D3D12: nonzero power-of-two sample count up to 32; MSAA needs a single-mip 2D attachment
        } else {
            hr = S_OK;
            const Format format = state_.texture_format(*desc);
            if (format.format == VK_FORMAT_UNDEFINED) {
                log_once("texture format not supported yet:", desc->Format);
                hr = E_INVALIDARG;
            } else {
                std::vector<VkFormat> cast_formats;
                hr = castable_view_formats(format, castable_count, castable, cast_formats);
                if (SUCCEEDED(hr)) hr = resource->create_texture(format, cast_formats, defer_image_memory);
                if (SUCCEEDED(hr)) resource->keep_castable_formats(castable_count, castable);
            }
        }
        if (FAILED(hr)) {
            resource->Release();
            return hr;
        }
        if (!out) {  // a capability check: D3D12 allows a null output
            resource->Release();
            return S_FALSE;
        }
        return hand_out(resource, riid, out);
    }

    // GetAdapterLuid's answer, and how queues read the clocks: calibrated timestamps when the caller enabled the
    // extension and the GPU calibrates its DEVICE domain against QueryPerformanceCounter, else the queue family's
    // timestamp queries.
    void query_luid_and_clock(PFN_vkGetDeviceProcAddr get_device) {
        const VulkanDeviceInfo& info = state_.info;
        const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceProperties2"));
        if (properties2) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
            properties2(info.physical, &properties);
            static_assert(sizeof(LUID) == VK_LUID_SIZE);
            if (id.deviceLUIDValid) std::memcpy(&state_.luid, id.deviceLUID, sizeof(LUID));
            state_.uma = properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        }
        if (!state_.luid.LowPart && !state_.luid.HighPart) log_once("the Vulkan driver reports no device LUID;", 0);

        state_.clock.get_device_proc = get_device;
        const auto families_of = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        if (families_of) {
            std::uint32_t count = 0;
            families_of(info.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            families_of(info.physical, &count, families.data());
            if (info.queue_family < count)
                state_.clock.timestamp_valid_bits = families[info.queue_family].timestampValidBits;
        }
        if (!info.calibrated_timestamps) return;
        // The KHR names first; the EXT ones are aliases with the same signatures.
        auto domains_of = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
            info.get_instance_proc(info.instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
        auto calibrated = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
            get_device(info.device, "vkGetCalibratedTimestampsKHR"));
        if (!domains_of || !calibrated) {
            domains_of = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
                info.get_instance_proc(info.instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
            calibrated = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
                get_device(info.device, "vkGetCalibratedTimestampsEXT"));
        }
        if (!domains_of || !calibrated) return;
        std::uint32_t count = 0;
        domains_of(info.physical, &count, nullptr);
        std::vector<VkTimeDomainKHR> domains(count);
        domains_of(info.physical, &count, domains.data());
        const auto has = [&](VkTimeDomainKHR domain) {
            return std::find(domains.begin(), domains.end(), domain) != domains.end();
        };
        if (has(VK_TIME_DOMAIN_DEVICE_KHR) && has(VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR))
            state_.clock.calibrated = calibrated;
        else
            log_once("calibrated timestamps lack the DEVICE or QueryPerformanceCounter domain; domains:", int(count));
    }

    // D3D12's layout rules for reserved resources. Use sparse backing for buffers and compatible color textures.
    // The arguments are answered as the host runtime answers them (d3d12_vulkan_device_test's
    // compare_placed_reserved, RTX 3060 / 610.88): the description, initial state and clear value rules of a
    // committed resource on a default heap, the reserved layouts below, and only then E_UNEXPECTED for a null
    // output, with nothing made.
    HRESULT create_reserved(const char* method, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                            const D3D12_CLEAR_VALUE* clear, UINT32 castable_count, const DXGI_FORMAT* castable,
                            REFIID riid, void** out) {
        if (out) *out = nullptr;
        if (!desc) return d3d12_trace::result(method, E_INVALIDARG);
        if (castable_count && castable && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            log_once("castable formats on a reserved buffer are invalid; count", int(castable_count));
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        D3D12_RESOURCE_DESC backing = *desc;
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            if (desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) return d3d12_trace::result(method, E_INVALIDARG);
        } else {
            if (static_cast<UINT>(desc->Layout) == kXboxTiledLayout) {
                log_once("reserved resource with the Xbox tiled layout, as 64KB_UNDEFINED_SWIZZLE; dimension",
                         desc->Dimension);
            } else if (desc->Layout != D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE) {
                return d3d12_trace::result(method, E_INVALIDARG);  // 64KB_STANDARD_SWIZZLE too: CheckFeatureSupport reports it unsupported
            }
            backing.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            palmetto::ResourceTiling tiling;
            backing.MipLevels = std::max<UINT16>(desc->MipLevels, 1);  // any count: only the format is checked here
            if (!resource_tiling(backing, tiling)) {
                log_once("reserved texture format has no tile shape:", desc->Format);
                return d3d12_trace::result(method, E_INVALIDARG);
            }
            backing.MipLevels = desc->MipLevels;
        }
        if (const char* why = committed_resource_refusal(heap, D3D12_HEAP_FLAG_NONE, backing, state, clear,
                                                         acceleration_structures(), state_.uma)) {
            log_once((std::string(method) + " refused (" + why + "), dimension").c_str(), int(desc->Dimension));
            return d3d12_trace::result(method, E_INVALIDARG);
        }
        if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            // Extents, mips and samples, as the committed backing below checks them (GetResourceAllocationInfo).
            D3D12_RESOURCE_DESC checked = backing;
            checked.SampleDesc.Quality = 0;
            if (allocation(checked).SizeInBytes == UINT64_MAX) return d3d12_trace::result(method, E_INVALIDARG);
        }
        if (!out) return d3d12_trace::result(method, E_UNEXPECTED);
        ID3D12Resource* resource = nullptr;
        HRESULT hr = S_OK;
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && state_.sparse_buffer_memory_types) {
            auto* sparse = new Resource(state_, backing, heap, D3D12_HEAP_FLAG_NONE);
            sparse->attach_device(identity());
            hr = sparse->create_sparse_buffer();
            if (FAILED(hr)) {
                sparse->Release();
                return d3d12_trace::result(method, hr);
            }
            resource = sparse;
        } else if (state_.sparse_image_tiles && backing.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                   backing.Format == DXGI_FORMAT_R8G8B8A8_UNORM && backing.Width && backing.Width <= UINT32_MAX &&
                   backing.Height && backing.DepthOrArraySize == 1 && backing.MipLevels == 1 &&
                   backing.SampleDesc.Count == 1 && !backing.SampleDesc.Quality &&
                   backing.Flags == D3D12_RESOURCE_FLAG_NONE && !clear && !castable_count) {
            auto* sparse = new Resource(state_, backing, heap, D3D12_HEAP_FLAG_NONE);
            sparse->attach_device(identity());
            hr = sparse->create_sparse_image();
            if (SUCCEEDED(hr)) resource = sparse;
            else {
                sparse->Release();
                if (hr != E_NOTIMPL) return d3d12_trace::result(method, hr);
            }
        }
        if (!resource) {
            hr = create_committed_validated(nullptr, &heap, D3D12_HEAP_FLAG_NONE, &backing, D3D12_RESOURCE_STATE_COMMON,
                                            clear, castable_count, castable, IID_PPV_ARGS(&resource));
        }
        if (FAILED(hr)) return d3d12_trace::result(method, hr);
        Resource::from<Resource>(resource)->make_reserved(desc->Layout);
        hr = resource->QueryInterface(riid, out);
        resource->Release();
        return d3d12_trace::result(method, hr);
    }

    // The tiling of a reserved resource's description; false for formats without a tile shape (three- and
    // twelve-byte texels, two-plane depth-stencil).
    static bool resource_tiling(const D3D12_RESOURCE_DESC& desc, palmetto::ResourceTiling& out) {
        palmetto::TilingDesc tiling{};
        tiling.width = desc.Width;
        tiling.height = desc.Height;
        tiling.depth_or_layers = desc.DepthOrArraySize;
        tiling.mips = desc.MipLevels;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            tiling.kind = palmetto::TiledKind::buffer;
            return palmetto::resource_tiling(tiling, out);
        }
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            return false;
        tiling.kind = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? palmetto::TiledKind::texture3d
                                                                           : palmetto::TiledKind::texture2d;
        const Format format = vulkan_format(desc.Format, false);
        if (!format.format || (format.aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) return false;
        if (format.rgb96) return false;  // the tile shape of a 12-byte texel is not measured
        const palmetto::BlockFormat block = palmetto::block_format(format.format, format.aspects);
        tiling.element_bytes = block.bytes;
        tiling.block = block.block;
        tiling.samples = desc.SampleDesc.Count;
        return palmetto::resource_tiling(tiling, out);
    }

    // Host image copies are used only on images in GENERAL (D3D12's COMMON), so GENERAL must be a layout the
    // device copies from and to on the host. Vulkan requires it to be; check rather than assume.
    void load_host_image_copy(PFN_vkGetDeviceProcAddr get_device) {
        const VkDevice dev = state_.info.device;
        const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            state_.info.get_instance_proc(state_.info.instance, "vkGetPhysicalDeviceProperties2"));
        state_.format_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
            state_.info.get_instance_proc(state_.info.instance, "vkGetPhysicalDeviceFormatProperties2"));
        const auto to_image = reinterpret_cast<PFN_vkCopyMemoryToImageEXT>(get_device(dev, "vkCopyMemoryToImageEXT"));
        const auto to_memory = reinterpret_cast<PFN_vkCopyImageToMemoryEXT>(get_device(dev, "vkCopyImageToMemoryEXT"));
        const auto transition =
            reinterpret_cast<PFN_vkTransitionImageLayoutEXT>(get_device(dev, "vkTransitionImageLayoutEXT"));
        if (!properties2 || !state_.format_properties2 || !to_image || !to_memory || !transition) {
            std::fprintf(stderr, "[rt] d3d12: VK_EXT_host_image_copy entry points missing; using staging copies\n");
            return;
        }
        VkPhysicalDeviceHostImageCopyPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &host};
        properties2(state_.info.physical, &properties);
        std::vector<VkImageLayout> src(host.copySrcLayoutCount), dst(host.copyDstLayoutCount);
        host.pCopySrcLayouts = src.data();
        host.pCopyDstLayouts = dst.data();
        properties2(state_.info.physical, &properties);
        const auto general = [](const std::vector<VkImageLayout>& layouts) {
            return std::find(layouts.begin(), layouts.end(), VK_IMAGE_LAYOUT_GENERAL) != layouts.end();
        };
        if (!general(src) || !general(dst)) {
            std::fprintf(stderr, "[rt] d3d12: host image copies do not accept GENERAL; using staging copies\n");
            return;
        }
        state_.copy_memory_to_image = to_image;
        state_.copy_image_to_memory = to_memory;
        state_.transition_image_layout = transition;
    }

    // The layer reports no protected-session support, so a session is refused, as D3D12 refuses one on a device
    // without D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT.
    static HRESULT refuse_protected_session(const char* method) {
        log_once((std::string("protected resource session refused (not supported) by ") + method + ':').c_str(), 1);
        return E_INVALIDARG;
    }

    // D3D12_RESOURCE_DESC1 is D3D12_RESOURCE_DESC plus SamplerFeedbackMipRegion.
    static D3D12_RESOURCE_DESC resource_desc(const D3D12_RESOURCE_DESC1& desc) {
        return {desc.Dimension, desc.Alignment, desc.Width,      desc.Height, desc.DepthOrArraySize,
                desc.MipLevels, desc.Format,    desc.SampleDesc, desc.Layout, desc.Flags};
    }

    // A sampler-feedback mip region on a texture, which needs a DXGI_FORMAT_SAMPLER_FEEDBACK_* resource the layer
    // does not have. The PC runtime ignores the region on a buffer.
    static bool texture_mip_region(const D3D12_RESOURCE_DESC1& desc) {
        const D3D12_MIP_REGION& region = desc.SamplerFeedbackMipRegion;
        return desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && (region.Width || region.Height || region.Depth);
    }

    std::mutex queue_mutex_;
    // Live D3D12 queues per VkQueue of info.command_queues, compute_queues and copy_queues (pick_queue_slot).
    std::vector<std::uint32_t> queue_users_;
    std::vector<std::uint32_t> compute_users_;
    std::vector<std::uint32_t> copy_users_;
    DeviceState state_;
    std::atomic<bool> removed_{false};
    OwnedRuntime owned_;
    IUnknown* outer_ = nullptr;  // the Xbox-ABI proxy, which owns a reference to this device
};

}  // namespace

HRESULT create_vulkan_device(const VulkanDeviceInfo& info, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto* device = new Device(info);
    if (!device->ok()) {
        device->Release();
        return E_FAIL;
    }
    return hand_out(device, riid, out);
}

std::shared_ptr<const palmetto::TileHeapAllocation> tile_heap(ID3D12Heap* heap, const palmetto::VulkanContext& context,
                                                           bool image, std::uint32_t memory_types) {
    auto* object = Heap::from<Heap>(heap);
    return object ? object->tiles(context, image, memory_types) : nullptr;
}

bool is_sparse_image(ID3D12Resource* resource) {
    const auto* object = Resource::from<Resource>(resource);
    return object && object->sparse_image();
}

void set_device_outer(ID3D12Device* device, IUnknown* outer) {
    Device* d = Device::from<Device>(device);
    if (!d) return;
    // A caller passes what create_device_proxy returned, which is the proxy's DXGI pointer when the title asked
    // for IDXGIDevice1: the device's outer identity is the proxy itself either way.
    if (void* proxy = rt::d3d12_xs::device_proxy_primary(outer)) outer = static_cast<IUnknown*>(proxy);
    d->set_outer(outer);
    // Second job, for an outer that is the Xbox-ABI proxy: its IDXGIDevice1::GetAdapter (d3d12_xs_device.h) hands
    // out this layer's adapter, the one create_dxgi_factory enumerates, so a title on the Vulkan device never gets
    // a host DXGI object from it.
    if (!rt::d3d12_xs::is_device_proxy(outer)) return;
    rt::d3d12_xs::set_device_adapter_provider(outer, [](ID3D12Device* real, REFIID riid, void** out) {
        IDXGIFactory1* factory = nullptr;
        HRESULT hr = create_dxgi_factory(real, __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
        if (FAILED(hr)) return hr;
        IDXGIAdapter* adapter = nullptr;
        hr = factory->EnumAdapters(0, &adapter);
        factory->Release();
        if (FAILED(hr)) return hr;
        hr = adapter->QueryInterface(riid, out);
        adapter->Release();
        return hr;
    });
}

const RootArgumentLayout* root_signature_layout(ID3D12RootSignature* signature) {
    const RootSignature* s = RootSignature::from<RootSignature>(signature);
    return s ? &s->layout() : nullptr;
}

std::uintptr_t compute_pipeline_id(ID3D12PipelineState* pipeline) {
    const PipelineState* p = PipelineState::from<PipelineState>(pipeline);
    return p && p->compute() ? p->id() : 0;
}

std::uintptr_t graphics_pipeline_id(ID3D12PipelineState* pipeline) {
    PipelineState* p = PipelineState::from<PipelineState>(pipeline);
    if (!p || !p->graphics()) return 0;
    // The live-pipeline budget (pipeline_budget.h): a PSO created past the cap, or whose pipeline an eviction
    // released, is built here, on the recording thread and before this list is submitted.
    if (!p->ensure_graphics_built()) return 0;
    return p->id();
}

const std::vector<palmetto::SubgroupRequest>* pipeline_subgroup_requests(ID3D12PipelineState* pipeline) {
    const PipelineState* p = PipelineState::from<PipelineState>(pipeline);
    return p && (p->compute() || p->graphics()) ? &p->subgroup_requests() : nullptr;
}

IUnknown* descriptor_heap_info(ID3D12DescriptorHeap* heap, D3D12_DESCRIPTOR_HEAP_DESC& desc,
                               D3D12_GPU_VIRTUAL_ADDRESS& gpu_base) {
    if (!heap) return nullptr;
    if (ID3D12DescriptorHeap* real = rt::d3d12_xs::heap_proxy_real_heap(heap)) heap = real;
    const DescriptorHeap* d = DescriptorHeap::from<DescriptorHeap>(heap);
    if (!d) return nullptr;
    desc = d->raw_desc();
    gpu_base = d->gpu_base();
    return d->attached_device();
}

VkImage vulkan_image(ID3D12Resource* resource) {
    const Resource* r = Resource::from<Resource>(resource);
    return r ? r->image() : VK_NULL_HANDLE;
}

VkMemoryPropertyFlags vulkan_memory_flags(ID3D12Resource* resource) {
    const Resource* r = Resource::from<Resource>(resource);
    return r ? r->memory_flags() : 0;
}

VkBuffer vulkan_buffer(ID3D12Resource* resource) {
    const Resource* r = Resource::from<Resource>(resource);
    return r ? r->buffer() : VK_NULL_HANDLE;
}

const VulkanDeviceInfo* vulkan_device_info(ID3D12Device* device) {
    if (!device) return nullptr;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    if (auto* d = Device::from<Device>(real ? real : device)) return &d->state().info;
    return nullptr;
}

bool rebase_console_va(IUnknown* device, std::uint64_t title_va, std::uint64_t& arena_va, bool descriptor) {
    if (!device || !title_va) return false;
    // The caller holds the device identity (the command list's attached device): a proxy resolves to the Vulkan device
    // this module implements, any other object fails Device::from and the answer is "not resolvable", never a guess.
    auto* as_device = reinterpret_cast<ID3D12Device*>(device);
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(as_device);
    Device* d = Device::from<Device>(real ? real : as_device);
    if (!d) return false;
    std::uint64_t available = 0;
    return d->context().console_va_span(
        title_va, descriptor ? palmetto::ConsoleVaKind::descriptors : palmetto::ConsoleVaKind::buffer, arena_va,
        available);
}

HRESULT create_buffer_at_host_address(ID3D12Device* device, void* address, const D3D12_RESOURCE_DESC& desc,
                                      std::uint32_t layout, REFIID riid, void** out) {
    if (out) *out = nullptr;
    if (!device || !out) return E_POINTER;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    auto* d = Device::from<Device>(real ? real : device);
    if (!d) return E_NOTIMPL;
    return d->create_at_host_address(address, desc, layout, riid, out);
}

bool device_supports_ray_tracing(ID3D12Device* device) {
    if (!device) return false;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    if (auto* d = Device::from<Device>(real ? real : device)) return d->context().ray_tracing_supported();
    return false;
}

VkAccelerationStructureKHR vulkan_acceleration_structure(ID3D12Device* device, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (!device || !va) return VK_NULL_HANDLE;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    if (auto* d = Device::from<Device>(real ? real : device))
        return d->context().resolve_acceleration_structure(va);
    return VK_NULL_HANDLE;
}

bool color_resolve_format_is_integer(DXGI_FORMAT format) {
    return DeviceState::numeric_class(vulkan_format(format, false).format) != 0;
}

bool color_resolve_format_matches(DXGI_FORMAT format, ID3D12Resource* dst, ID3D12Resource* src) {
    if (!dst || !src) return false;
    const Format requested = vulkan_format(format, false);
    if (requested.typeless || requested.rgb96 || requested.aspects != VK_IMAGE_ASPECT_COLOR_BIT) return false;
    if (DeviceState::numeric_class(requested.format) != 0) return false;  // D3D12 resolves no integer format
    const auto matches = [&](ID3D12Resource* resource) {
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        return vulkan_format(desc.Format, desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL).format ==
               requested.format;
    };
    return matches(dst) && matches(src);
}

bool depth_stencil_resolve_supported(DXGI_FORMAT format, ID3D12Resource* dst, UINT dst_subresource, UINT x, UINT y,
                                     ID3D12Resource* src, UINT src_subresource, const D3D12_RECT* rect,
                                     D3D12_RESOLVE_MODE mode) {
    const Resource* d = Resource::from<Resource>(dst);
    const Resource* s = Resource::from<Resource>(src);
    if (!d || !s) return false;
    // The format names the depth format, or one plane by its shader-resource format. Measured on the PC runtime
    // (RTX 5070 and WARP): Close fails for AVERAGE with the depth format or a stencil plane; the depth plane's
    // own format (R32_FLOAT, R24_UNORM_X8_TYPELESS) takes AVERAGE as well as MIN and MAX.
    DXGI_FORMAT family = format;
    int named_plane = -1;  // -1: any plane of the depth format
    switch (format) {
    case DXGI_FORMAT_R32_FLOAT: family = DXGI_FORMAT_D32_FLOAT, named_plane = 0; break;
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: family = DXGI_FORMAT_D24_UNORM_S8_UINT, named_plane = 0; break;
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: family = DXGI_FORMAT_D24_UNORM_S8_UINT, named_plane = 1; break;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: family = DXGI_FORMAT_D32_FLOAT_S8X24_UINT, named_plane = 0; break;
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: family = DXGI_FORMAT_D32_FLOAT_S8X24_UINT, named_plane = 1; break;
    default: break;
    }
    const Format requested = vulkan_format(family, true);
    if (requested.typeless || !(requested.aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
    // The plane a subresource index names (0 depth, 1 stencil), or -1.
    const auto plane = [&](const Resource* r, UINT subresource) {
        const D3D12_RESOURCE_DESC& desc = r->desc();
        if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
            vulkan_format(desc.Format, true).format != requested.format)
            return -1;
        const UINT planes = (requested.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? 2 : 1;
        const UINT per_plane = UINT(desc.MipLevels) * desc.DepthOrArraySize;
        return per_plane && subresource / per_plane < planes ? int(subresource / per_plane) : -1;
    };
    const int resolved = plane(d, dst_subresource);
    if (resolved < 0 || resolved != plane(s, src_subresource) || (named_plane >= 0 && resolved != named_plane) ||
        s->desc().SampleDesc.Count <= 1 || d->desc().SampleDesc.Count != 1)
        return false;
    VkResolveModeFlagBits wanted = VK_RESOLVE_MODE_MIN_BIT;
    if (mode == D3D12_RESOLVE_MODE_MAX) wanted = VK_RESOLVE_MODE_MAX_BIT;
    else if (mode == D3D12_RESOLVE_MODE_AVERAGE && named_plane == 0) wanted = VK_RESOLVE_MODE_AVERAGE_BIT;
    else if (mode != D3D12_RESOLVE_MODE_MIN) return false;
    // The rendering scope that replays it resolves pixels in place (vulkan_replay.h).
    if (rect ? (LONG(x) != rect->left || LONG(y) != rect->top) : (x || y)) return false;
    const DeviceState& state = d->device_state();
    return (resolved ? state.stencil_resolve_modes : state.depth_resolve_modes) & wanted;
}

std::uint32_t device_max_push_constants_size(ID3D12Device* device) {
    if (!device) return 0;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    if (!real) real = device;
    if (auto* d = Device::from<Device>(real)) return d->info().max_push_constants_size;
    return 0;
}

bool device_push_constants_need_fallback(ID3D12Device* device) {
    if (!device) return false;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    if (!real) real = device;
    if (auto* d = Device::from<Device>(real)) return d->info().push_constants_need_fallback;
    return false;
}

bool device_upload_in_vram(ID3D12Device* device) {
    if (!device) return false;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    if (!real) real = device;
    if (auto* d = Device::from<Device>(real)) return d->upload_in_vram();
    return false;
}

bool device_sparse_buffer_tiles(ID3D12Device* device) {
    const Device* d = Device::from<Device>(device);
    return d && d->state().sparse_buffer_memory_types != 0;
}

std::uint64_t device_arena_size(ID3D12Device* device) {
    if (!device) return 0;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    if (!real) real = device;
    const Device* d = Device::from<Device>(real);
    return d ? d->state().arena.size : 0;
}

namespace {
const palmetto::BindlessSet* bindless_of(ID3D12Device* device) {
    if (!device) return nullptr;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    const Device* d = Device::from<Device>(real ? real : device);
    return d ? d->bindless() : nullptr;
}
} // namespace

VkDescriptorSet bindless_set(ID3D12Device* device) {
    const palmetto::BindlessSet* set = bindless_of(device);
    return set ? set->set() : VK_NULL_HANDLE;
}

BindlessStats bindless_stats(ID3D12Device* device) {
    BindlessStats stats;
    if (!device) return stats;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    const Device* d = Device::from<Device>(real ? real : device);
    if (!d || !d->bindless()) return stats;
    stats.generations_made = d->bindless()->generations_made();
    stats.retired_pending = d->bindless()->retired_count();
    stats.device_idle_waits = d->state().bindless_idle_waits.load(std::memory_order_relaxed);
    return stats;
}

VkDescriptorSetLayout bindless_set_layout(ID3D12Device* device) {
    const palmetto::BindlessSet* set = bindless_of(device);
    return set ? set->layout() : VK_NULL_HANDLE;
}

std::uint32_t bindless_hash_lookup(ID3D12Device* device, const std::uint32_t (&key)[8]) {
    const palmetto::BindlessSet* set = bindless_of(device);
    return set ? set->table().lookup(key) : 0;
}

BindlessRoom bindless_room(ID3D12Device* device) {
    const palmetto::BindlessSet* set = bindless_of(device);
    if (!set) return {};
    return {set->free_images(palmetto::ImageDimension::dim_2d), set->free_samplers()};
}

WindowPresenter::WindowPresenter(IUnknown* device, std::unique_ptr<palmetto::VulkanPresenter> presenter)
    : device_(device), presenter_(std::move(presenter)) {
    device_->AddRef();
}

WindowPresenter::~WindowPresenter() {
    presenter_.reset();  // before the device it runs on
    device_->Release();
}

namespace {
HRESULT present_result(VkResult result) {
    if (result == VK_SUCCESS) return S_OK;
    std::fprintf(stderr, "[rt] d3d12: present failed (VkResult %d)\n", int(result));
    if (result == VK_ERROR_DEVICE_LOST) return DXGI_ERROR_DEVICE_REMOVED;
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) return DXGI_ERROR_UNSUPPORTED;
    return E_FAIL;
}
}  // namespace

HRESULT WindowPresenter::present(ID3D12Resource* back_buffer, UINT sync_interval, const D3D12_RECT* src_rect,
                                 bool tear) {
    if (!back_buffer) return E_INVALIDARG;
    const palmetto::PresentRect rect = src_rect ? palmetto::PresentRect{src_rect->left, src_rect->top, src_rect->right,
                                                                       src_rect->bottom}
                                               : palmetto::PresentRect{};
    return present_result(presenter_->present(resource_id(back_buffer), sync_interval > 0, src_rect ? &rect : nullptr,
                                              tear));
}

bool WindowPresenter::tear_supported() const { return presenter_->tear_supported(); }
bool WindowPresenter::last_present_tore() const { return presenter_->last_present_tore(); }
std::uint64_t WindowPresenter::tear_presents() const { return presenter_->tear_presents(); }

HRESULT WindowPresenter::present_blank(UINT sync_interval) {
    return present_result(presenter_->present_blank(sync_interval > 0));
}

HRESULT WindowPresenter::present_planes(const palmetto::PresentPlanes& planes, UINT sync_interval, bool tear) {
    return present_result(presenter_->present_planes(planes, sync_interval > 0, tear));
}

bool WindowPresenter::hdr_active() const { return presenter_->hdr_active(); }
void WindowPresenter::force_hdr_encoding(bool on) { presenter_->force_hdr_encoding(on); }
std::uint64_t WindowPresenter::composed_presents() const { return presenter_->composed_presents(); }

// DISPLAYCONFIG_SDR_WHITE_LEVEL.SDRWhiteLevel is the white level in units of 80/1000 nits: 1000 is 80 nits (the scRGB 1.0),
// 2500 is 200 nits (MS Learn, DISPLAYCONFIG_SDR_WHITE_LEVEL). Zero is not a level.
float sdr_white_nits_from_level(unsigned long level) { return float(level) / 1000.0f * 80.0f; }

float WindowPresenter::display_sdr_white_nits() const {
    if (!window_) return 0.0f;
    // The GDI name of the monitor the window is on, matched to a display path's source, whose target has the setting.
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) return 0.0f;
    // The active configuration can change between the size query and the read (a hot-plug): ERROR_INSUFFICIENT_BUFFER asks
    // for the sizes again (the pattern Microsoft documents for QueryDisplayConfig).
    UINT32 path_count = 0, mode_count = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 8 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return 0.0f;
        paths.assign(path_count, {});
        modes.assign(mode_count, {});
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    }
    if (status != ERROR_SUCCESS) return 0.0f;  // the caller keeps the last good value
    for (UINT32 i = 0; i < path_count; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), paths[i].sourceInfo.adapterId,
                        paths[i].sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            std::wcscmp(source.viewGdiDeviceName, monitor.szDevice) != 0)
            continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL level{};
        level.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(level), paths[i].targetInfo.adapterId,
                        paths[i].targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&level.header) != ERROR_SUCCESS || level.SDRWhiteLevel == 0) return 0.0f;
        return sdr_white_nits_from_level(level.SDRWhiteLevel);
    }
    return 0.0f;
}

bool WindowPresenter::enable_capture() { return presenter_->enable_capture(); }

void WindowPresenter::set_abort(std::function<bool()> abort) { presenter_->set_abort(std::move(abort)); }

bool WindowPresenter::read_back(std::vector<std::uint8_t>& pixels, VkExtent2D* extent, VkFormat* format) {
    return presenter_->read_back(pixels, extent, format);
}

VkExtent2D WindowPresenter::extent() const { return presenter_->extent(); }
std::uint64_t WindowPresenter::presented() const { return presenter_->presented(); }

bool device_has_swapchain_colorspace(ID3D12Device* device) {
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    Device* d = Device::from<Device>(real ? real : device);
    return d && d->state().info.swapchain_colorspace;
}

std::unique_ptr<WindowPresenter> create_window_presenter(ID3D12Device* device, HWND window) {
    // A title holds the Xbox-ABI proxy, not the layer's device: look behind it, as the helpers above do.
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    Device* d = Device::from<Device>(real ? real : device);
    if (device && !d)
        std::fprintf(stderr, "[rt] d3d12: create_window_presenter: %p is neither the Xbox device proxy nor a device of "
                             "this layer; no presenter\n",
                     static_cast<void*>(device));
    if (!d || !window) return nullptr;
    const VulkanDeviceInfo& info = d->state().info;
    if (!info.presentation) {
        std::fprintf(stderr, "[rt] d3d12: the Vulkan device was created without presentation support\n");
        return nullptr;
    }
    const auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        info.get_instance_proc(info.instance, "vkCreateWin32SurfaceKHR"));
    if (!create_surface) return nullptr;
    VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hinstance = GetModuleHandleW(nullptr);
    surface_info.hwnd = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (create_surface(info.instance, &surface_info, nullptr, &surface) != VK_SUCCESS) return nullptr;
    // PALMETTO_PRESENT_NO_TEAR=1 keeps the swapchain FIFO-only (ImmediateThresholdPercent then does nothing).
    // Any non-empty value other than "0" turns it off.
    bool tear_allowed = true;
    {
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "PALMETTO_PRESENT_NO_TEAR") == 0 && value) {
            tear_allowed = value[0] == '\0' || std::strcmp(value, "0") == 0;
            std::free(value);
        }
    }
    auto presenter = std::make_unique<palmetto::VulkanPresenter>(*d->state().context, info.get_instance_proc,
                                                                info.instance, info.physical, info.queue,
                                                                info.queue_family, surface,  // owns the surface
                                                                info.swapchain_maintenance1 && tear_allowed,
                                                                [window]() -> VkExtent2D {
                                                                    RECT r{};
                                                                    if (GetClientRect(window, &r)) {
                                                                        return {static_cast<uint32_t>(r.right - r.left),
                                                                                static_cast<uint32_t>(r.bottom - r.top)};
                                                                    }
                                                                    return {0, 0};
                                                                },
                                                                [create_surface, hinstance = GetModuleHandleW(nullptr),
                                                                 hwnd = window, instance = info.instance]() -> VkSurfaceKHR {
                                                                    // A lost surface (display gone) is remade for the
                                                                    // same window; VulkanSwapchain never retained the
                                                                    // hwnd, this callback does.
                                                                    VkWin32SurfaceCreateInfoKHR surface_info{
                                                                        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
                                                                    surface_info.hinstance = hinstance;
                                                                    surface_info.hwnd = hwnd;
                                                                    VkSurfaceKHR made = VK_NULL_HANDLE;
                                                                    if (create_surface(instance, &surface_info, nullptr,
                                                                                       &made) != VK_SUCCESS)
                                                                        return VK_NULL_HANDLE;
                                                                    return made;
                                                                });
    if (!presenter->ok()) {
        std::fprintf(stderr, "[rt] d3d12: cannot present to this window from the device's queue family\n");
        return nullptr;
    }
    auto window_presenter = std::make_unique<WindowPresenter>(d->holder(), std::move(presenter));
    window_presenter->set_window(window);
    return window_presenter;
}

HRESULT register_samplers(ID3D12Device* device, std::span<const palmetto::gfx10::SamplerWords> samplers) {
    if (!device) return E_INVALIDARG;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    Device* d = Device::from<Device>(real ? real : device);
    if (!d) return E_INVALIDARG;
    bool all = true;
    for (const palmetto::gfx10::SamplerWords& words : samplers) all = d->bind_sampler(words) && all;
    return all ? S_OK : E_FAIL;
}

HRESULT create_dxgi_factory(ID3D12Device* device, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!device) return E_INVALIDARG;
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    Device* d = Device::from<Device>(real ? real : device);
    if (!d) return E_INVALIDARG;
    const auto& i = d->info();
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(i.get_instance_proc(i.instance, "vkGetDeviceProcAddr"));
    if (!gdpa) return E_FAIL;
    return rt::dxgi::create_factory(d->context(), i.get_instance_proc, i.instance, i.physical, gdpa, i.queue,
                                    i.queue_family, riid, out);
}

// Creates its own Vulkan instance and a Vulkan 1.3 device, or a 1.2 device with VK_KHR_dynamic_rendering and
// VK_KHR_synchronization2 (vulkan13_features.h). PALMETTO_VK_VALIDATION=1 adds VK_LAYER_KHRONOS_validation.
HRESULT create_vulkan_device_standalone(REFIID riid, void** out, QueueTestSettings queue_test_settings) {
    if (!out) return E_POINTER;
    *out = nullptr;
    static const HMODULE library = LoadLibraryA("vulkan-1.dll");
    const auto get = library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                                   reinterpret_cast<void*>(GetProcAddress(library, "vkGetInstanceProcAddr")))
                             : nullptr;
    if (!get) {
        std::fprintf(stderr, "[rt] d3d12: vulkan-1.dll not found\n");
        return E_FAIL;
    }
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    const auto enum_inst_exts = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        get(nullptr, "vkEnumerateInstanceExtensionProperties"));
    std::vector<const char*> inst_exts;
    bool surface_maintenance1_khr = false, surface_maintenance1_ext = false;  // enabled on the instance
    bool swapchain_colorspace_enabled = false;  // VK_EXT_swapchain_colorspace on the instance
    if (enum_inst_exts) {
        std::uint32_t ext_cnt = 0;
        enum_inst_exts(nullptr, &ext_cnt, nullptr);
        std::vector<VkExtensionProperties> available(ext_cnt);
        enum_inst_exts(nullptr, &ext_cnt, available.data());
        bool has_surface = false, has_win32 = false, has_caps2 = false;
        for (const auto& e : available) {
            if (!std::strcmp(e.extensionName, VK_KHR_SURFACE_EXTENSION_NAME)) has_surface = true;
            if (!std::strcmp(e.extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) has_win32 = true;
            if (!std::strcmp(e.extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) has_caps2 = true;
            if (!std::strcmp(e.extensionName, "VK_KHR_surface_maintenance1")) surface_maintenance1_khr = true;
            if (!std::strcmp(e.extensionName, "VK_EXT_surface_maintenance1")) surface_maintenance1_ext = true;
        }
        if (has_surface && has_win32) {
            inst_exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            inst_exts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
            // Lists the HDR10 (ST.2084) surface formats, which a PresentX plane in HDR10 can use (xbox_present.h); without it
            // the surface only offers sRGB-nonlinear ones.
            for (const auto& e : available)
                if (!std::strcmp(e.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
                    inst_exts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
                    swapchain_colorspace_enabled = true;
                    break;
                }
            // The surface half of VK_*_swapchain_maintenance1 (the present modes a swapchain can switch between).
            if (has_caps2 && (surface_maintenance1_khr || surface_maintenance1_ext)) {
                inst_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
                if (surface_maintenance1_khr) inst_exts.push_back("VK_KHR_surface_maintenance1");
                if (surface_maintenance1_ext) inst_exts.push_back("VK_EXT_surface_maintenance1");
            } else {
                surface_maintenance1_khr = surface_maintenance1_ext = false;
            }
        } else {
            surface_maintenance1_khr = surface_maintenance1_ext = false;
        }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "palmetto";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const VkValidationFeatureEnableEXT sync_validation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation_features.enabledValidationFeatureCount = 1;
    validation_features.pEnabledValidationFeatures = &sync_validation;
    char validation[8] = {};
    size_t length = 0;
    if (getenv_s(&length, validation, "PALMETTO_VK_VALIDATION") == 0 && length > 1 && validation[0] != '0') {
        // Ask for the layer only when the loader lists it, and say so when it does not: an instance that cannot be
        // made because of a missing layer left the caller with "Vulkan unavailable" and no hint that validation was
        // the cause. Wine does not list the Linux loader's layers to the program and refuses an explicit request for
        // one, so there only VK_INSTANCE_LAYERS (which Wine keeps out of the program's environment) loads it.
        bool layer_listed = false;
        if (const auto enumerate_layers = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
                get(nullptr, "vkEnumerateInstanceLayerProperties"))) {
            std::uint32_t layer_count = 0;
            enumerate_layers(&layer_count, nullptr);
            std::vector<VkLayerProperties> layers(layer_count);
            if (layer_count && enumerate_layers(&layer_count, layers.data()) >= VK_SUCCESS)
                for (std::uint32_t i = 0; i < layer_count; ++i)
                    layer_listed = layer_listed || std::strcmp(layers[i].layerName, layer) == 0;
        }
        if (layer_listed) {
            instance_info.pNext = &validation_features;
            instance_info.enabledLayerCount = 1;
            instance_info.ppEnabledLayerNames = &layer;
        } else {
            std::fprintf(stderr,
                         "[rt] d3d12: PALMETTO_VK_VALIDATION is set but %s is not listed by the Vulkan loader "
                         "(install the Vulkan SDK; under Wine set VK_INSTANCE_LAYERS=%s instead)\n",
                         layer, layer);
            return E_FAIL;
        }
    }
    // VK_EXT_debug_utils when the loader has it, for the game's debug markers (PIX events) in RenderDoc and the like.
    const char* debug_utils = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    bool has_debug_utils = false;
    if (const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            get(nullptr, "vkEnumerateInstanceExtensionProperties"))) {
        std::uint32_t extension_count = 0;
        enumerate(nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        enumerate(nullptr, &extension_count, extensions.data());
        for (const auto& e : extensions) has_debug_utils = has_debug_utils || std::strcmp(e.extensionName, debug_utils) == 0;
    }
    // One list: the surface extensions above (for DXGI presentation) and debug utils both go on the instance.
    const bool instance_has_surface = !inst_exts.empty();
    if (has_debug_utils) inst_exts.push_back(debug_utils);
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(inst_exts.size());
    instance_info.ppEnabledExtensionNames = inst_exts.empty() ? nullptr : inst_exts.data();
    OwnedRuntime owned{};
    if (create_instance(&instance_info, nullptr, &owned.instance) != VK_SUCCESS) {
        std::fprintf(stderr, "[rt] d3d12: vkCreateInstance (Vulkan 1.3) failed\n");
        return E_FAIL;
    }
    owned.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get(owned.instance, "vkDestroyInstance"));
    const auto fail = [&](const char* why) {
        std::fprintf(stderr, "[rt] d3d12: %s\n", why);
        owned.destroy_instance(owned.instance, nullptr);
        return E_FAIL;
    };

#define PALMETTO_INSTANCE(name) const auto name = reinterpret_cast<PFN_##name>(get(owned.instance, #name))
    PALMETTO_INSTANCE(vkEnumeratePhysicalDevices);
    PALMETTO_INSTANCE(vkGetPhysicalDeviceProperties);
    PALMETTO_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    PALMETTO_INSTANCE(vkGetPhysicalDeviceFeatures2);
    PALMETTO_INSTANCE(vkGetPhysicalDeviceProperties2);
    PALMETTO_INSTANCE(vkEnumerateDeviceExtensionProperties);
    PALMETTO_INSTANCE(vkCreateDevice);
    PALMETTO_INSTANCE(vkGetDeviceProcAddr);
#undef PALMETTO_INSTANCE
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(owned.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(owned.instance, &count, physicals.data());
    VulkanDeviceInfo info{};
    info.get_instance_proc = get;
    info.instance = owned.instance;
    info.separate_depth_stencil_layouts = true;
    info.debug_utils = has_debug_utils;
    VkPhysicalDeviceProperties chosen{};
    bool chosen_bc = false;
    VkPhysicalDeviceFeatures chosen_features{};
    VkPhysicalDeviceVulkan12Features chosen12{};
    bool chosen_draw_params = false;
    palmetto::Vulkan13Features v13, candidate13;
    // The first usable device of the highest vulkan_device_rank: a native driver over Microsoft's Dozen (which says
    // it is a discrete GPU, #915), then discrete over integrated; the test harnesses (vk_device_pick.h) pick the same.
    int chosen_rank = -1;
    for (VkPhysicalDevice candidate : physicals) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (!palmetto::vulkan_device_wanted(props.deviceName)) continue;
        candidate13.query(candidate, props.apiVersion, vkGetPhysicalDeviceFeatures2, vkEnumerateDeviceExtensionProperties);
        if (!candidate13.usable()) continue;
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f11};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        const bool bc = features.features.textureCompressionBC;
        const bool draw_params = f11.shaderDrawParameters;
        if (!f12.timelineSemaphore || !f12.separateDepthStencilLayouts || !f12.bufferDeviceAddress) continue;
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (std::uint32_t i = 0; i < family_count; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (const int rank = palmetto::vulkan_device_rank(static_cast<int>(props.deviceType), props.vendorID);
                rank > chosen_rank) {
                chosen_rank = rank;
                info.physical = candidate;
                info.queue_family = i;
                chosen = props;
                chosen_bc = bc;
                chosen_features = features.features;
                chosen12 = f12;
                chosen_draw_params = draw_params;
                v13.choose(candidate13);
            }
            break;
        }
    }
    if (!info.physical)
        return fail(palmetto::vulkan_device_filter().empty()
                        ? "no Vulkan 1.3 GPU (or 1.2 with VK_KHR_dynamic_rendering and VK_KHR_synchronization2) with "
                          "dynamicRendering, synchronization2, timelineSemaphore and separateDepthStencilLayouts"
                        : "no GPU named by PALMETTO_VK_DEVICE has Vulkan 1.3 (or 1.2 with VK_KHR_dynamic_rendering and "
                          "VK_KHR_synchronization2) with dynamicRendering, synchronization2, timelineSemaphore and "
                          "separateDepthStencilLayouts");
    if (!v13.core)
        std::fprintf(stderr, "[rt] d3d12: Vulkan 1.2 device: dynamicRendering and synchronization2 through their KHR "
                             "extensions; %s\n",
                     v13.extended_dynamic_state ? "extended dynamic state through VK_EXT_extended_dynamic_state(2)"
                                                : "no extended dynamic state, so no graphics pipelines");

    // VK_EXT_conditional_rendering for SetPredication, VK_KHR_swapchain for DXGI presentation, and optional
    // VK_KHR_acceleration_structure + VK_KHR_ray_query for hardware BVH builds (BLAS + TLAS).
    VkPhysicalDeviceConditionalRenderingFeaturesEXT conditional{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    bool has_swapchain = false;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rq_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    {
        std::uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(info.physical, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        vkEnumerateDeviceExtensionProperties(info.physical, nullptr, &extension_count, extensions.data());
        bool has_as = false, has_dho = false, has_rq = false;
        for (const auto& e : extensions) {
            if (!std::strcmp(e.extensionName, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME)) {
                VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &conditional};
                vkGetPhysicalDeviceFeatures2(info.physical, &features);
                info.conditional_rendering = conditional.conditionalRendering == VK_TRUE;
            } else if (!std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                has_swapchain = true;
            } else if (!std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
                has_as = true;
            } else if (!std::strcmp(e.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
                has_dho = true;
            } else if (!std::strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME)) {
                has_rq = true;
            } else if (!std::strcmp(e.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
                VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &mesh_features};
                vkGetPhysicalDeviceFeatures2(info.physical, &features);
                info.mesh_shader = mesh_features.meshShader == VK_TRUE;
            }
        }
        if (!info.mesh_shader)
            std::fprintf(stderr, "[rt] d3d12: the GPU has no VK_EXT_mesh_shader: mesh-shader PSOs are unsupported\n");
        if (has_as && has_dho && has_rq && chosen12.bufferDeviceAddress) {
            as_features.pNext = &rq_features;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &as_features};
            vkGetPhysicalDeviceFeatures2(info.physical, &features);
            info.ray_tracing = as_features.accelerationStructure == VK_TRUE && rq_features.rayQuery == VK_TRUE;
        }
        // The game's translated RT shaders read XDXR BVH4 records, which a VkAccelerationStructureKHR is not
        // (docs/d3d12_vulkan.md 4.8): build those, unless PALMETTO_RT_AS=khr asks for #743's KHR builds instead.
        char rt_as[8] = {};
        std::size_t rt_as_length = 0;
        const bool khr = getenv_s(&rt_as_length, rt_as, "PALMETTO_RT_AS") == 0 && rt_as_length > 1 &&
                         std::strcmp(rt_as, "khr") == 0;
        if (!khr && chosen12.bufferDeviceAddress) {
            info.xdxr_acceleration_structures = true;
            info.ray_tracing = false;  // no KHR extensions: AS resources keep their console VAs
        }
        if (!info.conditional_rendering)
            std::fprintf(stderr, "[rt] d3d12: the GPU has no VK_EXT_conditional_rendering: SetPredication is unsupported\n");
    }
    VkPhysicalDeviceConditionalRenderingFeaturesEXT enable_conditional{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    enable_conditional.conditionalRendering = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR enable_rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    enable_rq.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enable_as{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    enable_as.accelerationStructure = VK_TRUE;

    // Only meshShader: the game has no amplification (task) shaders, and multiview and shading-rate mesh features
    // are not used.
    VkPhysicalDeviceMeshShaderFeaturesEXT enable_mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    enable_mesh.meshShader = VK_TRUE;

    void* ext_chain = nullptr;
    if (info.mesh_shader) {
        enable_mesh.pNext = ext_chain;
        ext_chain = &enable_mesh;
    }
    if (info.ray_tracing) {
        enable_rq.pNext = ext_chain;
        enable_as.pNext = &enable_rq;
        ext_chain = &enable_as;
    }
    if (info.conditional_rendering) {
        enable_conditional.pNext = ext_chain;
        ext_chain = &enable_conditional;
    }

    // Subgroup size control: a pipeline stage asks for the size its shader was translated for (src/subgroup_size.h).
    palmetto::query_subgroup_size_caps(vkGetPhysicalDeviceProperties2, info.physical, v13.subgroup_size_control,
                                      v13.compute_full_subgroups, info.subgroup);
    // Translated vertex shaders read BaseVertex / BaseInstance (SPIR-V DrawParameters) to give v0 / v3 the
    // hardware's zero-based vertex and instance ids (docs/design.md, Inputs and outputs).
    VkPhysicalDeviceVulkan11Features enable11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                              v13.enable(ext_chain)};
    enable11.shaderDrawParameters = chosen_draw_params ? VK_TRUE : VK_FALSE;
    if (!chosen_draw_params) std::fprintf(stderr, "[rt] d3d12: the GPU has no shaderDrawParameters\n");
    VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &enable11};
    enable12.timelineSemaphore = VK_TRUE;
    enable12.separateDepthStencilLayouts = VK_TRUE;
    info.buffer_device_address = chosen12.bufferDeviceAddress == VK_TRUE;
    enable12.bufferDeviceAddress = info.buffer_device_address ? VK_TRUE : VK_FALSE;
    info.storage_buffer_8bit = chosen12.storageBuffer8BitAccess == VK_TRUE;
    enable12.storageBuffer8BitAccess = info.storage_buffer_8bit;
    // Where the GPU has them: what the bindless set 0 needs (VulkanDeviceInfo::bindless), and the sampler features
    // D3D12 samplers can ask for.
    info.bindless = chosen12.runtimeDescriptorArray && chosen_features.shaderSampledImageArrayDynamicIndexing &&
                    chosen_features.shaderStorageImageArrayDynamicIndexing &&
                    chosen12.descriptorBindingSampledImageUpdateAfterBind &&
                    chosen12.descriptorBindingStorageImageUpdateAfterBind &&
                    chosen12.descriptorBindingStorageBufferUpdateAfterBind;
    info.bindless_partially_bound = info.bindless && chosen12.descriptorBindingPartiallyBound;
    info.sampler_anisotropy = chosen_features.samplerAnisotropy;
    info.sampler_mirror_clamp_to_edge = chosen12.samplerMirrorClampToEdge;
    info.sampler_filter_minmax = chosen12.samplerFilterMinmax;
    enable12.runtimeDescriptorArray = info.bindless;
    enable12.descriptorBindingSampledImageUpdateAfterBind = info.bindless;
    enable12.descriptorBindingStorageImageUpdateAfterBind = info.bindless;
    enable12.descriptorBindingStorageBufferUpdateAfterBind = info.bindless;
    enable12.descriptorBindingPartiallyBound = info.bindless_partially_bound;
    enable12.samplerMirrorClampToEdge = info.sampler_mirror_clamp_to_edge;
    enable12.samplerFilterMinmax = info.sampler_filter_minmax;
    info.shader_output_layer = chosen12.shaderOutputLayer == VK_TRUE;
    info.shader_output_viewport_index = chosen12.shaderOutputViewportIndex == VK_TRUE;
    enable12.shaderOutputLayer = info.shader_output_layer;
    enable12.shaderOutputViewportIndex = info.shader_output_viewport_index;
    if (!info.bindless)
        std::fprintf(stderr, "[rt] d3d12: the GPU lacks a descriptor-indexing feature the bindless set needs\n");
    enable12.bufferDeviceAddress = VK_TRUE;
    info.buffer_device_address = true;
    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(info.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(info.physical, &family_count, families.data());
    const std::uint32_t queue_cap = max_queues_per_family(queue_test_settings);
    const std::uint32_t command_queue_count = std::min(queue_cap, families[info.queue_family].queueCount);
    std::vector<float> priorities(16, 1.0f);
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    {
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = info.queue_family;
        q.queueCount = command_queue_count;
        q.pQueuePriorities = priorities.data();
        queue_infos.push_back(q);
    }
    const auto preferred = palmetto::pick_queue_families(families, info.queue_family);
    // D3D12 permits UpdateTileMappings on DIRECT, COMPUTE and COPY queues. Vulkan requires
    // VK_QUEUE_SPARSE_BINDING_BIT on the VkQueue passed to vkQueueBindSparse. A dedicated
    // async family without that capability cannot implement all D3D12 queue operations;
    // fall back to the graphics family when sparse buffers are supported. QueueKind still
    // enforces the D3D12 COPY/COMPUTE stage restrictions on the more capable family.
    const bool sparse_buffers = chosen_features.sparseBinding && chosen_features.sparseResidencyBuffer &&
                                chosen_features.sparseResidencyAliased;
    auto picked = palmetto::sparse_capable_queue_families(families, preferred, sparse_buffers);
    if (graphics_family_only(queue_test_settings)) picked = {info.queue_family, info.queue_family, info.queue_family};
    std::uint32_t compute_count = 0, copy_count = 0;
    if (picked.compute != info.queue_family) {
        info.compute_family = picked.compute;
        compute_count = std::min(queue_cap, families[picked.compute].queueCount);
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = picked.compute;
        q.queueCount = compute_count;
        q.pQueuePriorities = priorities.data();
        queue_infos.push_back(q);
    }
    if (picked.copy != info.queue_family) {
        info.copy_family = picked.copy;
        if (picked.copy != picked.compute) {
            copy_count = std::min(queue_cap, families[picked.copy].queueCount);
            VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            q.queueFamilyIndex = picked.copy;
            q.queueCount = copy_count;
            q.pQueuePriorities = priorities.data();
            queue_infos.push_back(q);
        }
    }
    VkPhysicalDeviceFeatures enable{};
    enable.sparseBinding = chosen_features.sparseBinding;
    enable.sparseResidencyBuffer = chosen_features.sparseResidencyBuffer;
    enable.sparseResidencyImage2D = chosen_features.sparseResidencyImage2D;
    enable.sparseResidencyAliased = chosen_features.sparseResidencyAliased;
    info.texture_compression_bc = chosen_bc;
    enable.textureCompressionBC = chosen_bc ? VK_TRUE : VK_FALSE;  // the game's textures are BC1-7
    enable.shaderSampledImageArrayDynamicIndexing = info.bindless;
    enable.shaderStorageImageArrayDynamicIndexing = info.bindless;
    if (!chosen_features.shaderInt64)
        std::fprintf(stderr, "[rt] d3d12: the GPU has no shaderInt64; translated shaders (all declare Int64) will not load\n");
    enable.samplerAnisotropy = info.sampler_anisotropy;
    enable.occlusionQueryPrecise = chosen_features.occlusionQueryPrecise;  // exact D3D12 occlusion counts
    enable.pipelineStatisticsQuery = chosen_features.pipelineStatisticsQuery;
    info.occlusion_query_precise = chosen_features.occlusionQueryPrecise == VK_TRUE;
    info.pipeline_statistics_query = chosen_features.pipelineStatisticsQuery == VK_TRUE;
    if (!chosen_bc) std::fprintf(stderr, "[rt] d3d12: the GPU has no BC texture support\n");
    const char* conditional_extension = VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME;

    // Optional: VK_EXT_host_image_copy lets WriteToSubresource/ReadFromSubresource copy on the CPU.
    std::uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(info.physical, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(info.physical, nullptr, &extension_count, extensions.data());
    info.conservative_rasterization = std::any_of(extensions.begin(), extensions.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME) == 0;
    });
    const bool has_host_image_copy = std::any_of(extensions.begin(), extensions.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME) == 0;
    });
    VkPhysicalDeviceImageViewMinLodFeaturesEXT min_lod{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
    if (std::any_of(extensions.begin(), extensions.end(), [](const auto& e) {
            return std::strcmp(e.extensionName, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME) == 0;
        })) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &min_lod};
        vkGetPhysicalDeviceFeatures2(info.physical, &query);
        info.image_view_min_lod = min_lod.minLod == VK_TRUE;
        if (info.image_view_min_lod) {
            min_lod.pNext = *v13.next();
            *v13.next() = &min_lod;
        }
    }
    const char* divisor_extension = nullptr;
    for (const char* name :
         {VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME}) {
        if (std::any_of(extensions.begin(), extensions.end(),
                        [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; })) {
            divisor_extension = name;
            break;
        }
    }
    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR divisor_khr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT divisor_ext{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    if (divisor_extension) {
        const bool is_khr = std::strcmp(divisor_extension, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0;
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                        is_khr ? static_cast<void*>(&divisor_khr) : static_cast<void*>(&divisor_ext)};
        vkGetPhysicalDeviceFeatures2(info.physical, &query);
        info.vertex_attribute_divisor =
            (is_khr ? divisor_khr.vertexAttributeInstanceRateDivisor : divisor_ext.vertexAttributeInstanceRateDivisor) ==
            VK_TRUE;
        if (info.vertex_attribute_divisor) {
            if (is_khr) {
                divisor_khr.pNext = *v13.next();
                *v13.next() = &divisor_khr;
            } else {
                divisor_ext.pNext = *v13.next();
                *v13.next() = &divisor_ext;
            }
        }
    }
    VkPhysicalDeviceHostImageCopyFeaturesEXT host_copy{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT};
    if (has_host_image_copy) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &host_copy};
        vkGetPhysicalDeviceFeatures2(info.physical, &query);
    }
    const char* host_image_copy_name = VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME;
    info.host_image_copy = has_host_image_copy && host_copy.hostImageCopy;
    if (info.host_image_copy) {
        host_copy.pNext = *v13.next();  // after the conditional rendering features, if any
        *v13.next() = &host_copy;
    } else {
        std::fprintf(stderr, "[rt] d3d12: no VK_EXT_host_image_copy; CPU texture copies use a staging submit\n");
    }

    // The pixel-shader extensions translated shaders can declare (VulkanDeviceInfo::fragment_shader_barycentric and
    // the two after it). Each is queried, and enabled with its feature where the GPU has it.
    const auto has_device_extension = [&](const char* name) {
        return std::any_of(extensions.begin(), extensions.end(),
                           [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    if (has_device_extension(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &barycentric};
        vkGetPhysicalDeviceFeatures2(info.physical, &query);
        info.fragment_shader_barycentric = barycentric.fragmentShaderBarycentric == VK_TRUE;
        if (info.fragment_shader_barycentric) {
            barycentric.pNext = *v13.next();
            *v13.next() = &barycentric;
        }
    }
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    if (has_device_extension(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &interlock};
        vkGetPhysicalDeviceFeatures2(info.physical, &query);
        info.fragment_shader_interlock = interlock.fragmentShaderPixelInterlock == VK_TRUE;
        if (info.fragment_shader_interlock) {
            interlock.fragmentShaderSampleInterlock = VK_FALSE;  // the translator only declares the pixel interlock
            interlock.fragmentShaderShadingRateInterlock = VK_FALSE;
            interlock.pNext = *v13.next();
            *v13.next() = &interlock;
        }
    }
#ifdef VK_KHR_MAINTENANCE_8_EXTENSION_NAME
    VkPhysicalDeviceMaintenance8FeaturesKHR maintenance8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
    if (has_device_extension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &maintenance8};
        vkGetPhysicalDeviceFeatures2(info.physical, &query);
        info.maintenance8 = maintenance8.maintenance8 == VK_TRUE;
        if (info.maintenance8) {
            maintenance8.pNext = *v13.next();
            *v13.next() = &maintenance8;
        }
    }
#endif

#ifdef VK_KHR_SHADER_FMA_EXTENSION_NAME
    // VK_KHR_shader_fma: OpFmaKHR is a single-rounding multiply-add in SPIR-V (GLSL.std.450 Fma may round twice).
    // Translated shaders built with `rdna2spirv.py --fma khr` need it (index capability FMAKHR, feature
    // shaderFmaFloat32); when it is missing they must not be created, so use `--fma glsl` there.
    VkPhysicalDeviceShaderFmaFeaturesKHR shader_fma{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FMA_FEATURES_KHR};
    if (std::any_of(extensions.begin(), extensions.end(), [](const auto& e) {
            return std::strcmp(e.extensionName, VK_KHR_SHADER_FMA_EXTENSION_NAME) == 0;
        })) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &shader_fma};
        vkGetPhysicalDeviceFeatures2(info.physical, &query);
        info.shader_fma = shader_fma.shaderFmaFloat32 == VK_TRUE;
        if (info.shader_fma) {
            shader_fma.pNext = *v13.next();
            *v13.next() = &shader_fma;
        }
    }
#endif
    VkPhysicalDeviceShaderClockFeaturesKHR shader_clock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
    if (has_device_extension(VK_KHR_SHADER_CLOCK_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &shader_clock};
        vkGetPhysicalDeviceFeatures2(info.physical, &query);
        info.shader_subgroup_clock = shader_clock.shaderSubgroupClock == VK_TRUE;
        info.shader_device_clock = shader_clock.shaderDeviceClock == VK_TRUE;
        if (info.shader_subgroup_clock || info.shader_device_clock) {
            shader_clock.pNext = *v13.next();
            *v13.next() = &shader_clock;
        }
    }
    // What graphics PSOs may ask for: different blend state per render target, wireframe, a depth-bias clamp and
    // SRC1 blend factors. Enabled where the GPU has them; a PSO that needs a missing one is refused.
    enable.independentBlend = chosen_features.independentBlend;
    enable.fillModeNonSolid = chosen_features.fillModeNonSolid;
    enable.depthBiasClamp = chosen_features.depthBiasClamp;
    enable.dualSrcBlend = chosen_features.dualSrcBlend;
    enable.logicOp = chosen_features.logicOp;
    enable.depthClamp = chosen_features.depthClamp;
    enable.geometryShader = chosen_features.geometryShader;  // translated geometry shaders (195 in the corpus)
    // What translated shaders declare (the index's `features`): 64-bit integers in most of the corpus (25,803 of
    // 26,503 entries), doubles, 16-bit integers, clip and cull distances in a few vertex shaders, image stores
    // without a format (StorageImageWriteWithoutFormat), gather offsets (ImageGatherExtended) and LOD-clamped samples
    // (MinLod), and the layer or viewport a vertex shader writes (above, in Vulkan 1.2). Enabled where the GPU has
    // them; without one, vkCreateShaderModule reports the capability (VUID-...-pCode-08740).
    enable.shaderInt64 = chosen_features.shaderInt64;
    enable.shaderFloat64 = chosen_features.shaderFloat64;
    enable.shaderInt16 = chosen_features.shaderInt16;
    enable.shaderClipDistance = chosen_features.shaderClipDistance;
    enable.shaderCullDistance = chosen_features.shaderCullDistance;
    enable.shaderStorageImageWriteWithoutFormat = chosen_features.shaderStorageImageWriteWithoutFormat;
    enable.shaderStorageImageReadWithoutFormat = chosen_features.shaderStorageImageReadWithoutFormat;
    enable.shaderImageGatherExtended = chosen_features.shaderImageGatherExtended;
    enable.shaderResourceMinLod = chosen_features.shaderResourceMinLod;
    // A translated pixel shader that stores to an image or does an image atomic declares that storage image without
    // NonWritable (1,018 of the corpus's 1,038 image-atomic shaders are pixel shaders), and Vulkan allows that outside
    // a compute shader only with these two features: fragmentStoresAndAtomics for the pixel stage,
    // vertexPipelineStoresAndAtomics for vertex and geometry shaders. Enabled where the GPU has them; a graphics PSO
    // whose shader needs a missing one is refused (PipelineState::create_graphics).
    enable.fragmentStoresAndAtomics = chosen_features.fragmentStoresAndAtomics;
    enable.vertexPipelineStoresAndAtomics = chosen_features.vertexPipelineStoresAndAtomics;
    info.features = enable;

    // GetClockCalibration reads both clocks at once with calibrated timestamps (KHR, or the older EXT).
    const char* calibrated = nullptr;
    for (const char* name :
         {VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME}) {
        for (const auto& extension : extensions)
            if (std::strcmp(extension.extensionName, name) == 0) calibrated = name;
        if (calibrated) break;
    }
    info.calibrated_timestamps = calibrated != nullptr;

    // Switching a vsync swapchain to IMMEDIATE or FIFO_RELAXED per present (PresentX's ImmediateThresholdPercent):
    // the KHR extension (promoted from the EXT one, same structures) with the surface extension of the same name
    // on the instance, else the EXT one. Only enabled where the instance has the surface half and the GPU the feature.
    const char* swapchain_m1_name = nullptr;
#ifdef PALMETTO_SWAPCHAIN_MAINTENANCE1
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchain_m1{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    if (has_swapchain && instance_has_surface) {
        const auto device_has = [&](const char* name) {
            return std::any_of(extensions.begin(), extensions.end(),
                               [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
        };
        if (surface_maintenance1_khr && device_has("VK_KHR_swapchain_maintenance1"))
            swapchain_m1_name = "VK_KHR_swapchain_maintenance1";
        else if (surface_maintenance1_ext && device_has("VK_EXT_swapchain_maintenance1"))
            swapchain_m1_name = "VK_EXT_swapchain_maintenance1";
        if (swapchain_m1_name) {
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &swapchain_m1};
            vkGetPhysicalDeviceFeatures2(info.physical, &query);
            if (swapchain_m1.swapchainMaintenance1 == VK_TRUE) {
                swapchain_m1.pNext = *v13.next();
                *v13.next() = &swapchain_m1;
            } else {
                swapchain_m1_name = nullptr;
            }
        }
    }
#endif
    info.swapchain_maintenance1 = swapchain_m1_name != nullptr;

    // Optional: VK_EXT_custom_border_color lets samplers use custom float and integer border colours.
    const bool has_custom_border = std::any_of(extensions.begin(), extensions.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME) == 0;
    });
    VkPhysicalDeviceCustomBorderColorFeaturesEXT custom_border{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    if (has_custom_border) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &custom_border};
        vkGetPhysicalDeviceFeatures2(info.physical, &query);
    }
    const char* custom_border_name = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    info.custom_border_color = has_custom_border && custom_border.customBorderColors;
    info.custom_border_color_without_format = has_custom_border && custom_border.customBorderColorWithoutFormat;
    if (info.custom_border_color) {
        custom_border.pNext = *v13.next();   // where later feature structs go since #1706
        *v13.next() = &custom_border;
    } else {
        std::fprintf(stderr,
                     "[rt] d3d12: no VK_EXT_custom_border_color; custom sampler border colours fall back to transparent black\n");
    }

    // Optional: VK_EXT_external_memory_host lets OpenExistingHeapFromAddress import the caller's committed memory as
    // the heap's backing, so the GPU reads what the CPU writes there without a copy.
    if (std::any_of(extensions.begin(), extensions.end(), [](const auto& e) {
            return std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0;
        })) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_memory{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &host_memory};
        vkGetPhysicalDeviceProperties2(info.physical, &properties);
        info.host_pointer_alignment = host_memory.minImportedHostPointerAlignment;
    }

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enable12};
    device_info.queueCreateInfoCount = std::uint32_t(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.pEnabledFeatures = &enable;
    std::vector<const char*> device_extensions;
    v13.add_extensions(device_extensions);
    if (info.conservative_rasterization) device_extensions.push_back(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
    if (info.image_view_min_lod) device_extensions.push_back(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
    if (info.vertex_attribute_divisor) device_extensions.push_back(divisor_extension);
    if (info.conditional_rendering) device_extensions.push_back(conditional_extension);
    if (info.host_image_copy) device_extensions.push_back(host_image_copy_name);
    if (info.fragment_shader_barycentric)
        device_extensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    if (info.fragment_shader_interlock) device_extensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
#ifdef VK_KHR_MAINTENANCE_8_EXTENSION_NAME
    if (info.maintenance8) device_extensions.push_back(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
#endif
#ifdef VK_KHR_SHADER_FMA_EXTENSION_NAME
    if (info.shader_fma) device_extensions.push_back(VK_KHR_SHADER_FMA_EXTENSION_NAME);
#endif
    if (info.shader_subgroup_clock || info.shader_device_clock)
        device_extensions.push_back(VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
    if (calibrated) device_extensions.push_back(calibrated);
    // VK_KHR_swapchain requires VK_KHR_surface on the instance (VUID-vkCreateDevice-ppEnabledExtensionNames-01387).
    if (has_swapchain && instance_has_surface) device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (swapchain_m1_name) device_extensions.push_back(swapchain_m1_name);
    if (info.mesh_shader) device_extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    if (info.ray_tracing) {
        device_extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        device_extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        device_extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }
    info.presentation = has_swapchain && instance_has_surface;
    info.swapchain_colorspace = info.presentation && swapchain_colorspace_enabled;
    if (info.custom_border_color) device_extensions.push_back(custom_border_name);
    if (info.host_pointer_alignment) device_extensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    device_info.enabledExtensionCount = std::uint32_t(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.empty() ? nullptr : device_extensions.data();
    if (vkCreateDevice(info.physical, &device_info, nullptr, &owned.device) != VK_SUCCESS)
        return fail("vkCreateDevice failed");
    info.device = owned.device;
    owned.destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(vkGetDeviceProcAddr(owned.device, "vkDestroyDevice"));
    info.command_queues.resize(command_queue_count);
    const auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(vkGetDeviceProcAddr(owned.device, "vkGetDeviceQueue"));
    for (std::uint32_t i = 0; i < command_queue_count; ++i)
        get_queue(owned.device, info.queue_family, i, &info.command_queues[i]);
    if (compute_count) {
        // Copies on the async-compute family split its queues with the compute queues: one VkQueue per live queue.
        const bool split = info.copy_family == info.compute_family && compute_count > 1;
        const std::uint32_t for_compute = split ? compute_count / 2 : compute_count;
        for (std::uint32_t i = 0; i < compute_count; ++i) {
            VkQueue q = VK_NULL_HANDLE;
            get_queue(owned.device, info.compute_family, i, &q);
            (i < for_compute ? info.compute_queues : info.copy_queues).push_back(q);
        }
        if (info.copy_family == info.compute_family && !split) info.copy_family = UINT32_MAX;
    }
    for (std::uint32_t i = 0; i < copy_count; ++i) {
        VkQueue q = VK_NULL_HANDLE;
        get_queue(owned.device, info.copy_family, i, &q);
        info.copy_queues.push_back(q);
    }
    info.queue = info.command_queues[0];
    info.max_push_constants_size = chosen.limits.maxPushConstantsSize;
    info.push_constants_need_fallback = (info.max_push_constants_size < 144);
    std::fprintf(stderr, "[rt] d3d12: Vulkan device: %s (maxPushConstantsSize: %u bytes)\n", chosen.deviceName,
                 chosen.limits.maxPushConstantsSize);
    std::fprintf(stderr,
                 "[rt] d3d12: subgroup sizes %u..%u (default %u), requirable in stages 0x%x, subgroupSizeControl %s, "
                 "computeFullSubgroups %s\n",
                 info.subgroup.min_size, info.subgroup.max_size, info.subgroup.default_size,
                 unsigned(info.subgroup.required_stages), info.subgroup.subgroup_size_control ? "on" : "off",
                 info.subgroup.compute_full_subgroups ? "on" : "off");
    if (info.push_constants_need_fallback) {
        std::fprintf(stderr,
                     "[rt] d3d12: WARNING: GPU maxPushConstantsSize (%u bytes) is below the 144 bytes required by "
                     "the shader contract; uniform-buffer fallback required for the 16-byte tail\n",
                     chosen.limits.maxPushConstantsSize);
    }

    auto* device = new Device(info);
    device->own(owned);  // destroyed with the device from here on, even on failure
    if (!device->ok()) {
        device->Release();
        return E_FAIL;
    }
    return hand_out(device, riid, out);
}

bool has_global_realtime_privilege() {
    const int override_state = g_global_realtime_privilege_override.load(std::memory_order_relaxed);
    if (override_state >= 0) return override_state != 0;
    HANDLE token = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        // Fall back to the process token only when the thread is not impersonating (ERROR_NO_TOKEN); fail closed on
        // ERROR_CANT_OPEN_ANONYMOUS or any other thread-token error so an impersonated caller cannot inherit the
        // process token's privilege.
        if (GetLastError() != ERROR_NO_TOKEN || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
    }
    LUID luid{};
    BOOL enabled = FALSE;
    if (LookupPrivilegeValueW(nullptr, L"SeIncreaseBasePriorityPrivilege", &luid)) {
        PRIVILEGE_SET set{};
        set.PrivilegeCount = 1;
        set.Control = PRIVILEGE_SET_ALL_NECESSARY;
        set.Privilege[0].Luid = luid;
        set.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!PrivilegeCheck(token, &set, &enabled)) enabled = FALSE;
    }
    CloseHandle(token);
    return enabled != FALSE;
}

void set_global_realtime_privilege_override_for_test(int state) {
    g_global_realtime_privilege_override.store(state, std::memory_order_relaxed);
}

HRESULT create_console_descriptor_heap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                       D3D12_GPU_VIRTUAL_ADDRESS console_address, std::uint64_t console_size,
                                       REFIID riid, void** out) {
    if (out) *out = nullptr;
    if (!device || !desc) return E_INVALIDARG;
    // No output: a capability check, as CreateDescriptorHeap does.
    if (!out) return S_FALSE;
    if ((desc->Flags & ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) || desc->NodeMask > 1 ||
        ((desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
         (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV))) {
        return E_INVALIDARG;
    }
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    auto* d = Device::from<Device>(real ? real : device);
    if (!d) return E_INVALIDARG;

    // state() returns const DeviceState& and identity() is private; holder() is the accessor the class
    // already exposes for this hand-off.
    auto* heap = new DescriptorHeap(const_cast<DeviceState&>(d->state()), *desc, console_address, console_size);
    heap->attach_device(d->holder());
    if (!heap->ok()) {
        heap->Release();
        return E_OUTOFMEMORY;
    }
    return hand_out(heap, riid, out);
}

// Console device slot 57 (port/shims/d3d12_xs_device.cpp) creates a graphics pipeline from a deserialized record
// (port/src/graphics_record.h) that this tree cannot convert yet: the fixed-function state blob and the input
// layout's formats are undecoded, so the slot refuses and the title gets a null PSO. With PALMETTO_STANDIN_PIPELINES
// on, the slot hands the title a real graphics pipeline built from this project's own VS+PS pair instead - the same
// create_graphics path a translated pipeline takes, with the defaults below - so one boot can measure whether that
// refusal is what stops it. Diagnostic only: the stand-in's draws do not do the record's work. It needs no shader
// library, so it also answers when the shader directory failed to load.
HRESULT create_standin_graphics_pipeline(ID3D12Device* device, REFIID riid, void** out) {
    if (out) *out = nullptr;
    if (!device || !out) return E_POINTER;
    if (!standin_pipelines()) return E_NOTIMPL;  // the switch is the only way in; off keeps slot 57's refusal
    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device);
    Device* d = Device::from<Device>(real ? real : device);
    if (!d) return E_NOTIMPL;  // not the Vulkan-backed device: nothing here can build a stand-in
    // The fixed-function state is unknown (port/src/graphics_record.h, "The fixed-function state blob"), so these
    // are documented defaults, not measured values: one R8G8B8A8_UNORM render target, no depth-stencil, blending off,
    // sample count 1, triangle list, solid fill, back-face culling, depth clipping on. The caller logs every use.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.VS = {kStandinVsSpirv, sizeof(kStandinVsSpirv)};
    desc.PS = {kStandinPsSpirv, sizeof(kStandinPsSpirv)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    auto* pipeline = new PipelineState(const_cast<DeviceState&>(d->state()));
    pipeline->attach_device(d->holder());
    if (const HRESULT hr = pipeline->create_graphics(desc, nullptr, nullptr, /*standin=*/true); FAILED(hr)) {
        pipeline->Release();
        return hr;
    }
    return hand_out(pipeline, riid, out);
}

extern "C" void palmetto_register_console_heap(void* fn);

// Hand the console descriptor-heap entry point to the device proxy (port/shims/d3d12_xs_device.cpp). It cannot
// look the symbol up by name because nothing exports it from palmetto.exe.
extern "C" void palmetto_register_console_heap(void* fn);
namespace {
struct ConsoleHeapRegistration {
    ConsoleHeapRegistration() {
        palmetto_register_console_heap(reinterpret_cast<void*>(&create_console_descriptor_heap));
    }
} g_console_heap_registration;
}  // namespace

extern "C" void palmetto_register_graphics_standin(void* fn);

// The same hand-off for the graphics stand-in: the device proxy calls it from console slot 57, and the switch is
// checked here so a boot without PALMETTO_STANDIN_PIPELINES is byte-identical in behaviour to the baseline.
namespace {
struct GraphicsStandinRegistration {
    GraphicsStandinRegistration() {
        palmetto_register_graphics_standin(reinterpret_cast<void*>(&create_standin_graphics_pipeline));
    }
} g_graphics_standin_registration;
}  // namespace

std::filesystem::path configured_shader_directory(std::string* error, const std::filesystem::path& config) {
    try {
        constexpr wchar_t env_name[] = L"PALMETTO_SHADERS";
        if (const DWORD size = ::GetEnvironmentVariableW(env_name, nullptr, 0)) {
            std::wstring value(size, L'\0');
            const DWORD length = ::GetEnvironmentVariableW(env_name, value.data(), size);
            if (length && length < size) {
                value.resize(length);
                return std::filesystem::path(value);
            }
        }
        if (!config.empty()) return shader_directory_from_file(config);
        std::wstring exe(32768, L'\0');
        const DWORD length = ::GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (!length || length >= exe.size()) return {};
        exe.resize(length);
        return shader_directory_from_file(std::filesystem::path(exe).parent_path() / L"shader_path.txt");
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return {};
    }
}

}  // namespace rt::d3d12
