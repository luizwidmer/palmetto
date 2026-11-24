// Xbox-ABI proxy around a PC ID3D12Device. See d3d12_xs_device.h for the measured slots and the limits.
//
// How forwarding works: the proxy object's first word is a vtable built once from one thunk per slot. A thunk
// receives the proxy in RCX, replaces it with the PC device pointer from offset 8, and tail-calls the PC method at
// the same index through a 15-argument integer prototype. Every PC D3D12 device method takes integer/pointer
// arguments only, so the x64 registers and stack words line up exactly; methods that return a struct keep their
// hidden return pointer in RDX, which is passed through as the first argument unchanged. The measured exceptions
// are IUnknown (0-2), slot 14 (which wraps the heap), 62 and 76, plus the heap proxy's 9/10 conversion from the
// PC hidden-pointer form to the Xbox RAX form. Slots 8/75 (command queues), 36 (fences) and 32 (a shared fence) wrap
// what they create in queue and fence proxies built the same way, and slots 31/34/35/45/46/50 hand the real objects
// behind heap, queue and fence proxies to the real device (see the command queue and fence section).
//
// A thunk declares 14 argument words, but the title passes only as many as the method takes, so the caller's frame
// may end right after the four register homes. The compiler owns a function's incoming stack-argument slots and may
// write them, a tail call included. Every function a thunk hands its words to therefore takes them in the same
// positions (slot 8: desc, riid, out as a1-a3; slot 50: a1-a5; and so on). A tail call then writes back only the
// values it read from those slots. A helper that moved a word to another stack position would overwrite the title's
// frame. xs_proxy_mock_test's mock hit this in an optimized build, and its thunks now pass their words by pointer.
//
// The public HLE Freminet/ConsoleX reports five more Xbox device slots (a third-party observation, not a Microsoft
// ABI); the generic thunk routes them before it forwards anything: 48 CreateCommandQueueX, 73 CreateCommandListX,
// 78 SetFrameIntervalX, 79 ScheduleFrameEventX and 80 WaitFrameEventX. The slots can be moved with PALMETTO_XS_*_SLOT
// for a trace that disagrees. The frame-event calls answer from a per-device token counter: there is no vblank source
// behind the device before a presenter exists, so WaitFrameEventX returns at once with a fresh non-zero token.
#include "d3d12_xs_device.h"
#include "d3d12_trace.h"
// docs/present_wiring.md: the presentation core (xbox_present.cpp) publishes the console PresentX path through this
// header's registration hook. Only its hook-only layer is included, so this translation unit - which is also compiled
// into the GPU-free proxy test binaries that link neither Vulkan nor xbox_present.cpp - stays free of the Vulkan layer;
// with nothing registered the hook refuses honestly.
#define PALMETTO_XBOX_PRESENT_HOOK_ONLY
#include "xbox_present.h"
#undef PALMETTO_XBOX_PRESENT_HOOK_ONLY
// create_console_descriptor_heap lives in d3d12_vulkan_device.cpp, which the GPU-free test binaries do not link.
// That file registers its own function here at startup, so this proxy can call it directly when it is present and
// fall back to the PC CreateDescriptorHeap when it is not. (A GetProcAddress lookup does not work: the symbol is not
// exported from palmetto.exe, so it silently resolved to nothing and every console-addressed heap was created without
// its console address, which is what makes the title assert on GetCPUDescriptorHandleForHeapStart().)
namespace rt::d3d12 {
using CreateConsoleHeapFn = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*,
                                            D3D12_GPU_VIRTUAL_ADDRESS, std::uint64_t, REFIID, void**);
inline CreateConsoleHeapFn& console_heap_slot() {
    static CreateConsoleHeapFn fn = nullptr;
    return fn;
}
}  // namespace rt::d3d12

// Called by d3d12_vulkan_device.cpp's static initializer.
extern "C" void palmetto_register_console_heap(void* fn) {
    rt::d3d12::console_heap_slot() = reinterpret_cast<rt::d3d12::CreateConsoleHeapFn>(fn);
}

// The graphics stand-in creator lives in d3d12_vulkan_device.cpp, which the GPU-free test binaries do not link
// either; it registers here at startup the same way. Console device slot 57 calls it after its record probe, and the
// creator itself checks PALMETTO_STANDIN_PIPELINES (answering E_NOTIMPL with the switch off), so a boot without the
// switch keeps the probe's refusal exactly.
namespace rt::d3d12 {
using CreateGraphicsStandinFn = HRESULT(WINAPI*)(ID3D12Device*, REFIID, void**);
inline CreateGraphicsStandinFn& graphics_standin_slot() {
    static CreateGraphicsStandinFn fn = nullptr;
    return fn;
}
}  // namespace rt::d3d12

// Called by d3d12_vulkan_device.cpp's static initializer.
extern "C" void palmetto_register_graphics_standin(void* fn) {
    rt::d3d12::graphics_standin_slot() = reinterpret_cast<rt::d3d12::CreateGraphicsStandinFn>(fn);
}

// The fence dispatcher belongs to the Vulkan layer (d3d12_com.cpp), which the GPU-free test binaries do not link:
// it registers here at startup the function that makes a fence value advance reach a console fence slot-12 cell
// (docs/fence_slot12.md). With none registered the cell moves only when the fence is advanced through its proxy.
namespace rt::d3d12_xs {
namespace {
FenceValueWatcherFn g_fence_value_watcher = nullptr;
}  // namespace
FenceValueWatcherFn fence_value_watcher() {
    return g_fence_value_watcher;
}
}  // namespace rt::d3d12_xs

// Called by d3d12_com.cpp's static initializer.
extern "C" void palmetto_register_fence_value_watcher(void* fn) {
    rt::d3d12_xs::g_fence_value_watcher = reinterpret_cast<rt::d3d12_xs::FenceValueWatcherFn>(fn);
}



#include "../src/graphics_pso.h"     // slot 57: the record -> PC graphics pipeline builder (docs/graphics_pso.md)
#include "../src/shader_library.h"  // palmetto::fspx_payload_from_parts for slot 67

#include <windows.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>  // _ReturnAddress, for the PALMETTO_XS_CALLERS probe
#include <iterator>
#include <map>
#include <set>
#include <mutex>
#include <new>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rt::d3d12_xs {
namespace {

// The retail title calls device slots well past ID3D12Device14 (slot 133 at least); every slot past the PC device's
// vtable goes through the generic thunk and is refused with E_NOTIMPL, except the console extensions the proxy models
// itself (55, 76, 78-80, 127, 133), so the table must outlast the highest one.
constexpr unsigned kMaxSlots = 192;
static_assert(kMaxSlots > 133, "the proxy table must cover the slots the retail title calls (up to 133)");
static_assert(kMaxSlots > 83, "the proxy table must cover ID3D12Device14 (83 slots)");
static_assert(sizeof(void*) == 8, "the Xbox-ABI proxy is x64-only");
static_assert(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) == 8,
              "the Xbox handle ABI is an 8-byte value in RAX; the heap thunk converts the PC hidden-pointer form");
static_assert(sizeof(D3D12_RESOURCE_DESC) == 56, "the measured Xbox description prefix is 56 bytes");

struct DeviceProxy {
    void** vtable;          // offset 0: the caller's ID3D12Device* points here
    ID3D12Device* real;     // offset 8: the PC device every thunk forwards to
    void** real_vtable;
    unsigned slot_count;    // number of valid entries in real_vtable
    std::atomic<unsigned long> refs;
    // The device's IDXGIDevice1 (see "IDXGIDevice1 on the device" below): a title's IDXGIDevice1* points here.
    void** dxgi_vtable;
    DxgiAdapterProvider adapter_provider;
    std::atomic<int> gpu_thread_priority;
    std::atomic<unsigned> max_frame_latency;
    // The first-frame subset of SetFrameIntervalX/ScheduleFrameEventX/WaitFrameEventX. Tokens were never measured;
    // they start at 1 and only have to be fresh and non-zero for the boot sequence. Waiting returns at once: there is
    // no vblank source until a presenter exists, and a bounded zero wait cannot stall the first frame.
    std::atomic<std::uint32_t> frame_interval_us{0};
    std::atomic<std::uint32_t> frame_latency{1};
    std::atomic<bool> frame_event_scheduled{false};
    std::atomic<std::uint64_t> frame_next_token{1};
};

constexpr unsigned kHeapSlots = 11;  // IUnknown + ID3D12Object + DeviceChild + Pageable + GetDesc + 2 handles

struct HeapProxy {
    void** vtable;          // offset 0: the caller's ID3D12DescriptorHeap* points here
    ID3D12DescriptorHeap* real;
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    std::atomic<unsigned long> refs;
};

std::atomic<std::uint64_t> g_hits[kMaxSlots];
std::atomic<bool> g_first[kMaxSlots];
std::atomic<bool> g_beyond_warned[kMaxSlots];
std::atomic<std::uint64_t> g_heap_hits[kHeapSlots];
std::atomic<bool> g_heap_first[kHeapSlots];
// The hit counters are for tests. They count from the first call of a *_slot_hits reader or of
// reset_device_proxy_stats, so the calls of a title, which reads no counter, write no shared counter.
std::atomic<bool> g_count_hits{false};

// For the readers and the reset. The flag is written one time only.
void start_hit_counts() {
    if (!g_count_hits.load(std::memory_order_relaxed)) g_count_hits.store(true, std::memory_order_relaxed);
}

void count_hit(std::atomic<std::uint64_t>& hits) {
    if (g_count_hits.load(std::memory_order_relaxed)) hits.fetch_add(1, std::memory_order_relaxed);
}

// True for the first call of a slot only. The flag is read first: a compare-exchange takes the cache line for
// writing also when it fails, and the flags of all slots share one or two lines.
bool first_call(std::atomic<bool>& seen) {
    if (seen.load(std::memory_order_relaxed)) return false;
    bool expected = false;
    return seen.compare_exchange_strong(expected, true, std::memory_order_relaxed);
}

// ---- The PC root signatures this device handed out -----------------------------------------------------------------
//
// A slot-57 graphics record names its root signature at +0x00 (docs/graphics_record.md section 1). The proxy does not
// wrap root signatures, so the pointer the title holds is the PC object itself - but the record is deserialized data,
// and passing an unvalidated pointer to CreateGraphicsPipelineState would make the layer call through it
// (PipelineState::create_graphics keys the pipeline on pRootSignature). This table is the proof: slot 16 records every
// root signature the PC device creates, and slot 57 passes the record's +0x00 on only when it is one of these objects
// (directly, or held in a title-side slot). Anything else is refused as pRootSignature, never guessed.
constexpr unsigned kRootSignaturesTracked = 256;
std::atomic<unsigned> g_root_signature_count{0};
std::mutex g_root_signature_mutex;
void* g_root_signatures[kRootSignaturesTracked] = {};

void note_root_signature(void* signature) {
    if (!signature) return;
    const unsigned index = g_root_signature_count.fetch_add(1, std::memory_order_relaxed);
    if (index < kRootSignaturesTracked) {
        const std::lock_guard lock(g_root_signature_mutex);
        g_root_signatures[index] = signature;
        return;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: more than %u root signatures created; a slot-57 record naming a later one is "
                     "refused as pRootSignature rather than passed on\n",
                     kRootSignaturesTracked);
}

bool is_tracked_root_signature(const void* signature) {
    if (!signature) return false;
    const std::lock_guard lock(g_root_signature_mutex);
    for (unsigned i = 0; i < kRootSignaturesTracked; ++i)
        if (g_root_signatures[i] == signature) return true;
    return false;
}

bool trace_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA("XS_TRACE", value, sizeof(value));
        return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

// PALMETTO_XS_CALLERS=1 (measurement aid): name the title code that drives a proxy slot. The trace and the first-call
// lines name the method, not the caller, and a slot the title reaches from two places (startup and its frame loop) is
// only diagnosable with the call site. For every device/queue/fence/allocator/list slot this logs the return address
// of the first four distinct call sites that reach it, raw, with the four argument words of that call. A return
// address on the stack is the title's own instruction after the call; the boot's JSONL map event names the image
// base to convert it to game+RVA. Nothing here changes an answer.
// PALMETTO_XS_CALLERS_PROFILE=1 also keeps a hit count per site and prints the hottest ones every 20 s, because a boot
// that runs to its timeout has no shutdown summary: the per-frame call sites are the ones whose counts keep growing.
enum class CallerKind : unsigned { device, queue, fence, allocator, list };
constexpr unsigned kCallerKinds = static_cast<unsigned>(CallerKind::list) + 1;
constexpr unsigned kCallerSites = 4;
constexpr const char* kCallerKindNames[kCallerKinds] = {"device", "queue", "fence", "allocator", "list"};
struct CallerSite {
    std::atomic<std::uint64_t> address{0};
    std::atomic<std::uint64_t> hits{0};
};
CallerSite g_caller_sites[kCallerKinds][kMaxSlots][kCallerSites];

void print_caller_profile() {
    struct Row {
        std::uint64_t hits;
        unsigned kind, slot, site;
    };
    std::vector<Row> rows;
    for (unsigned k = 0; k < kCallerKinds; ++k)
        for (unsigned s = 0; s < kMaxSlots; ++s)
            for (unsigned i = 0; i < kCallerSites; ++i) {
                const std::uint64_t hits = g_caller_sites[k][s][i].hits.load(std::memory_order_relaxed);
                if (hits) rows.push_back({hits, k, s, i});
            }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.hits > b.hits; });
    std::fprintf(stderr, "[rt] d3d12_xs: caller profile, hottest %zu site(s):\n", std::min<size_t>(rows.size(), 40));
    for (size_t n = 0; n < rows.size() && n < 40; ++n) {
        const Row& r = rows[n];
        const std::uint64_t address = g_caller_sites[r.kind][r.slot][r.site].address.load(std::memory_order_relaxed);
        std::fprintf(stderr, "[rt] d3d12_xs:   %10llu  %s slot %u caller 0x%llx\n",
                     static_cast<unsigned long long>(r.hits), kCallerKindNames[r.kind], r.slot,
                     static_cast<unsigned long long>(address));
    }
}

void start_caller_profile() {
    static std::atomic<bool> started{false};
    if (started.exchange(true, std::memory_order_relaxed)) return;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(20));
            print_caller_profile();
        }
    }).detach();
}

// One relaxed flag read on the hot path; the probe itself only runs when the switch is on.
bool callers_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA("PALMETTO_XS_CALLERS", value, sizeof(value));
        return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

__declspec(noinline) void note_caller(CallerKind kind, unsigned slot, const void* return_address, std::uint64_t a1,
                                      std::uint64_t a2, std::uint64_t a3, std::uint64_t a4) {
    if (!callers_enabled() || !return_address || slot >= kMaxSlots) return;
    static const bool profile = [] {
        char value[8] = {};
        return GetEnvironmentVariableA("PALMETTO_XS_CALLERS_PROFILE", value, sizeof(value)) > 0 && value[0] != '\0' &&
               value[0] != '0';
    }();
    if (profile) start_caller_profile();
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(return_address));
    auto& sites = g_caller_sites[static_cast<unsigned>(kind)][slot];
    for (unsigned i = 0; i < kCallerSites; ++i) {
        const std::uint64_t site = sites[i].address.load(std::memory_order_relaxed);
        if (site == address) {
            sites[i].hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (site != 0) continue;
        std::uint64_t expected = 0;
        if (!sites[i].address.compare_exchange_strong(expected, address, std::memory_order_relaxed)) return;
        sites[i].hits.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u caller 0x%llx (args 0x%llx 0x%llx 0x%llx 0x%llx)\n",
                     kCallerKindNames[static_cast<unsigned>(kind)], slot,
                     static_cast<unsigned long long>(address), static_cast<unsigned long long>(a1),
                     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
                     static_cast<unsigned long long>(a4));
        return;
    }
}

const char* slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 8: return "CreateCommandQueue -> queue proxy";
        case 9: return "CreateCommandAllocator (PC slot 9)";
        case 10: return "CreateGraphicsPipelineState (PC slot 10)";
        case 11: return "CreateComputePipelineState (PC slot 11)";
        case 14: return "CreateDescriptorHeap";
        case 16: return "CreateRootSignature";
        case 25: return "GetResourceAllocationInfo";
        case 26: return "GetCustomHeapProperties";
        case 27: return "CreateCommittedResource (PC slot 27)";
        case 31: return "CreateSharedHandle (proxies unwrapped)";
        case 32: return "OpenSharedHandle (a fence comes back wrapped)";
        case 34: return "MakeResident (proxies unwrapped)";
        case 35: return "Evict (proxies unwrapped)";
        case 36: return "CreateFence -> fence proxy";
        case 43: return "GetAdapterLuid";
        case 45: return "SetEventOnMultipleFenceCompletion (fence proxies unwrapped)";
        case 46: return "SetResidencyPriority (proxies unwrapped)";
        case 48: return "Xbox CreateCommandQueueX -> queue proxy";
        case 50: return "EnqueueMakeResident (proxies unwrapped)";
        case 55: return "Xbox CreateDerivedPipelineStateX (not PC CreateReservedResource1)";
        case 57: return "Xbox CreateGraphicsPipelineState (probed)";
        case 62: return "Xbox CreateComputePipelineState";
        case 67: return "Xbox DeserializeComputePipelineStateX";
        case 73: return "Xbox CreateCommandListX -> PC CreateCommandList";
        case 75: return "CreateCommandQueue1 -> queue proxy";
        case 76: return "Xbox resource creation";
        case 78: return "Xbox SetFrameIntervalX";
        case 79: return "Xbox ScheduleFrameEventX";
        case 80: return "Xbox WaitFrameEventX";
        case 127: return "Xbox CreateCommittedResource with a barrier layout (display surfaces)";
        case 133: return "Xbox CreatePlacedResourceX (a buffer on the title's pages)";
        default: return "forwarded";
    }
}

void note_slot(unsigned slot) {
    if (slot >= kMaxSlots) return;
    count_hit(g_hits[slot]);
    const bool first = first_call(g_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) first call\n", slot, slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) call\n", slot, slot_name(slot));
    }
}

// Xbox device slots (public HLE Freminet/ConsoleX, an observation, not Microsoft documentation). The defaults are
// the HLE's; an environment variable that names another slot in 3..127 replaces one (a whole-table move for a trace
// that disagrees). A value outside that range is logged once and ignored. The table is read once, on the first
// generic slot call, so a test that wants other slots sets the variables before it creates a device.
struct XboxDeviceSlotRoutes {
    unsigned frame_interval = 78;  // SetFrameIntervalX(output, interval_us, latency, flags)
    unsigned schedule_event = 79;  // ScheduleFrameEventX(event, offset_us, reserved, flags)
    unsigned wait_event = 80;      // WaitFrameEventX(event, timeout_ms, reserved, flags, &token)
    unsigned queue_x = 48;         // CreateCommandQueueX(desc, riid, out)
    unsigned list_x = 73;          // CreateCommandListX(desc, allocator, initial_state, riid, out)
};

unsigned env_device_slot(const char* name, unsigned fallback) {
    char value[32] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0) return fallback;
    if (length >= sizeof(value)) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s is too long for a device slot; the default is kept\n", name);
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long slot = std::strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || slot < 3 || slot > 127) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s=%s is not a device slot in 3..127; the default is kept\n", name,
                     value);
        return fallback;
    }
    return static_cast<unsigned>(slot);
}

const XboxDeviceSlotRoutes& xbox_device_slot_routes() {
    static const XboxDeviceSlotRoutes routes = [] {
        XboxDeviceSlotRoutes r;
        const unsigned frame_interval = env_device_slot("PALMETTO_XS_FRAME_INTERVAL_SLOT", r.frame_interval);
        const unsigned schedule_event = env_device_slot("PALMETTO_XS_SCHEDULE_EVENT_SLOT", r.schedule_event);
        const unsigned wait_event = env_device_slot("PALMETTO_XS_WAIT_EVENT_SLOT", r.wait_event);
        const unsigned queue_x = env_device_slot("PALMETTO_XS_QUEUE_X_SLOT", r.queue_x);
        const unsigned list_x = env_device_slot("PALMETTO_XS_LIST_X_SLOT", r.list_x);
        const bool moved = frame_interval != r.frame_interval || schedule_event != r.schedule_event ||
                           wait_event != r.wait_event || queue_x != r.queue_x || list_x != r.list_x;
        r.frame_interval = frame_interval;
        r.schedule_event = schedule_event;
        r.wait_event = wait_event;
        r.queue_x = queue_x;
        r.list_x = list_x;
        if (moved)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: Xbox device slots: SetFrameIntervalX=%u ScheduleFrameEventX=%u "
                         "WaitFrameEventX=%u CreateCommandQueueX=%u CreateCommandListX=%u\n",
                         r.frame_interval, r.schedule_event, r.wait_event, r.queue_x, r.list_x);
        return r;
    }();
    return routes;
}

// The public HLE call shapes. SetFrameIntervalX's output and flags are unused while there is no output or pacing;
// ScheduleFrameEventX's reserved event pointer is a3 in the console call and a4 is its flags. The logged words are the
// caller's, so a trace names the slot even when an override moved it.
void log_xbox_route_once(unsigned slot, const char* name, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                         std::uint64_t a4, std::uint64_t a5) {
    static std::atomic<bool> logged[kMaxSlots];
    if (slot >= kMaxSlots) return;
    bool expected = false;
    if (!logged[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: device slot %u routed to %s (args 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                 name, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4),
                 static_cast<unsigned long long>(a5));
}

const char* heap_slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 7: return "GetDevice";
        case 8: return "GetDesc";
        case 9: return "GetCPUDescriptorHandleForHeapStart";
        case 10: return "GetGPUDescriptorHandleForHeapStart";
        default: return "forwarded";
    }
}

void note_heap_slot(unsigned slot) {
    if (slot >= kHeapSlots) return;
    count_hit(g_heap_hits[slot]);
    const bool first = first_call(g_heap_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) first call\n", slot, heap_slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) call\n", slot, heap_slot_name(slot));
    }
}

std::uint64_t hr_to_u64(HRESULT hr) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(hr));
}

unsigned long proxy_add_ref(DeviceProxy* self) {
    note_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long proxy_release(DeviceProxy* self) {
    note_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        ID3D12Device* real = self->real;
        delete self;
        if (real) real->Release();
    }
    return refs;
}

bool is_device_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device14))) return true;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device13))) return true;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device12))) return true;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device11))) return true;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device10))) return true;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device9))) return true;
#endif
    if (IsEqualIID(iid, __uuidof(ID3D12Device8))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device7))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device6))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device5))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device4))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device3))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device2))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device1))) return true;
    return false;
}

HRESULT proxy_query_interface(DeviceProxy* self, const IID* riid, void** out) {
    note_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (IsEqualIID(*riid, __uuidof(IUnknown)) || IsEqualIID(*riid, __uuidof(ID3D12Object))) {
        *out = self;
        proxy_add_ref(self);
        return S_OK;
    }
    if (IsEqualIID(*riid, __uuidof(IDXGIObject)) || IsEqualIID(*riid, __uuidof(IDXGIDevice)) ||
        IsEqualIID(*riid, __uuidof(IDXGIDevice1))) {
        *out = &self->dxgi_vtable;
        proxy_add_ref(self);
        return S_OK;
    }
    if (!is_device_iid(*riid)) return E_NOINTERFACE;

    // Only claim an interface the PC device really implements; the proxy vtable covers its slots.
    void* supported = nullptr;
    HRESULT hr = self->real->QueryInterface(*riid, &supported);
    if (FAILED(hr)) return hr;
    if (!supported) return E_NOINTERFACE;
    static_cast<IUnknown*>(supported)->Release();
    *out = self;
    proxy_add_ref(self);
    return S_OK;
}

// ---- IDXGIDevice1 on the device ---------------------------------------------------------------------------------
//
// A GDKX title finds its output through the device: QueryInterface for IDXGIDevice1, GetAdapter, EnumOutputs(0),
// and hands that output to SetFrameIntervalX (docs/d3d12_vulkan.md section 4.5, from the public samples). A PC
// ID3D12Device has no IDXGIDevice, so the proxy answers it itself: a second vtable pointer inside the proxy object,
// with the proxy's reference count and QueryInterface, so the two pointers are one COM object. Only IDXGIDevice and
// IDXGIDevice1 are answered (what the samples ask for). The 14 slots are the public dxgi.h order; that the console
// keeps it is the same assumption as for the queue and the fence.
//
// GetAdapter and GetParent hand out the adapter of the device behind the proxy. The provider says how: the default
// asks the host's DXGI for the adapter with the device's LUID (the host backend); set_device_adapter_provider puts
// the Vulkan layer's own adapter there, so that backend never loads the host's dxgi.dll for it.
constexpr unsigned kDxgiDeviceSlots = 14;  // IUnknown 3, IDXGIObject 4, IDXGIDevice 5, IDXGIDevice1 2

DeviceProxy* dxgi_owner(void* self) {
    return reinterpret_cast<DeviceProxy*>(static_cast<char*>(self) - offsetof(DeviceProxy, dxgi_vtable));
}

HRESULT host_adapter(ID3D12Device* real, REFIID riid, void** out) {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    static const CreateFactoryFn create_factory = [] {
        const HMODULE dxgi = LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dxgi ? reinterpret_cast<CreateFactoryFn>(
                          reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                    : nullptr;
    }();
    if (!create_factory) return DXGI_ERROR_NOT_FOUND;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = create_factory(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND;
    hr = factory->EnumAdapterByLuid(real->GetAdapterLuid(), riid, out);
    factory->Release();
    return hr;
}

HRESULT dxgi_adapter(DeviceProxy* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const DxgiAdapterProvider provider = self->adapter_provider ? self->adapter_provider : &host_adapter;
    const HRESULT hr = provider(self->real, riid, out);
    if (FAILED(hr)) *out = nullptr;
    return hr;
}

void note_dxgi(const char* method) {
    d3d12_trace::record(method);
}

HRESULT WINAPI dxgi_query_interface(void* self, const IID* riid, void** out) {
    return proxy_query_interface(dxgi_owner(self), riid, out);
}
ULONG WINAPI dxgi_add_ref(void* self) { return proxy_add_ref(dxgi_owner(self)); }
ULONG WINAPI dxgi_release(void* self) { return proxy_release(dxgi_owner(self)); }

// IDXGIObject's private data is the device's: ID3D12Object has the same three methods with the same arguments.
HRESULT WINAPI dxgi_set_private_data(void* self, const GUID* name, UINT size, const void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_set_private_data_interface(void* self, const GUID* name, const IUnknown* object) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateDataInterface(*name, object);
}
HRESULT WINAPI dxgi_get_private_data(void* self, const GUID* name, UINT* size, void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->GetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_get_parent(void* self, const IID* riid, void** out) {
    note_dxgi("XboxIDXGIDevice::GetParent");
    if (!riid) return E_POINTER;
    return dxgi_adapter(dxgi_owner(self), *riid, out);
}
HRESULT WINAPI dxgi_get_adapter(void* self, IDXGIAdapter** out) {
    note_dxgi("XboxIDXGIDevice::GetAdapter");
    return dxgi_adapter(dxgi_owner(self), __uuidof(IDXGIAdapter), reinterpret_cast<void**>(out));
}
// CreateSurface and QueryResourceResidency are D3D10/11-era methods no D3D12 title has a use for; what the console
// answers is not measured, so they are refused and say so once.
HRESULT WINAPI dxgi_create_surface(void*, const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*,
                                   IDXGISurface** out) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: IDXGIDevice::CreateSurface is not implemented; returning E_NOTIMPL\n");
    if (out) *out = nullptr;
    return E_NOTIMPL;
}
HRESULT WINAPI dxgi_query_resource_residency(void*, IUnknown* const*, DXGI_RESIDENCY*, UINT) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: IDXGIDevice::QueryResourceResidency is not implemented; returning E_NOTIMPL\n");
    return E_NOTIMPL;
}
// The priority and the frame latency are kept and read back; nothing acts on them (the Xbox presenter's latency is
// SetFrameIntervalX's). The ranges are the documented ones: -7..7, and 1..16 with 0 for the default of 3.
HRESULT WINAPI dxgi_set_gpu_thread_priority(void* self, INT priority) {
    if (priority < -7 || priority > 7) return E_INVALIDARG;
    dxgi_owner(self)->gpu_thread_priority.store(priority, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_gpu_thread_priority(void* self, INT* priority) {
    if (!priority) return E_POINTER;
    *priority = dxgi_owner(self)->gpu_thread_priority.load(std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_set_maximum_frame_latency(void* self, UINT latency) {
    if (latency > 16) return DXGI_ERROR_INVALID_CALL;
    dxgi_owner(self)->max_frame_latency.store(latency ? latency : 3, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_maximum_frame_latency(void* self, UINT* latency) {
    if (!latency) return E_POINTER;
    *latency = dxgi_owner(self)->max_frame_latency.load(std::memory_order_relaxed);
    return S_OK;
}

void** get_dxgi_vtable() {
    static void* table[kDxgiDeviceSlots] = {
        reinterpret_cast<void*>(&dxgi_query_interface),          // 0
        reinterpret_cast<void*>(&dxgi_add_ref),                  // 1
        reinterpret_cast<void*>(&dxgi_release),                  // 2
        reinterpret_cast<void*>(&dxgi_set_private_data),         // 3  IDXGIObject
        reinterpret_cast<void*>(&dxgi_set_private_data_interface),
        reinterpret_cast<void*>(&dxgi_get_private_data),
        reinterpret_cast<void*>(&dxgi_get_parent),
        reinterpret_cast<void*>(&dxgi_get_adapter),              // 7  IDXGIDevice
        reinterpret_cast<void*>(&dxgi_create_surface),
        reinterpret_cast<void*>(&dxgi_query_resource_residency),
        reinterpret_cast<void*>(&dxgi_set_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_get_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_set_maximum_frame_latency),  // 12 IDXGIDevice1
        reinterpret_cast<void*>(&dxgi_get_maximum_frame_latency),
    };
    return table;
}

unsigned long heap_add_ref(HeapProxy* self) {
    note_heap_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long heap_release(HeapProxy* self) {
    note_heap_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        ID3D12DescriptorHeap* real = self->real;
        DeviceProxy* device = self->device;
        delete self;
        if (real) real->Release();
        if (device) proxy_release(device);
    }
    return refs;
}

bool is_heap_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(IUnknown));
}

HRESULT heap_query_interface(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!is_heap_iid(*riid)) return E_NOINTERFACE;
    *out = self;
    heap_add_ref(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice on the heap must hand back the Xbox-ABI device, not the raw PC device.
HRESULT heap_get_device(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(7);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

// The PC methods write through the hidden return pointer (RDX) on the measured runtime; the Xbox ABI wants the
// 8-byte value in RAX. Call with both conventions and keep whichever produced a value, so a runtime that really
// returns in RAX also works. The extra register is harmless when the callee ignores it.
D3D12_CPU_DESCRIPTOR_HANDLE heap_get_cpu_handle(HeapProxy* self) {
    note_heap_slot(9);
    constexpr SIZE_T kUnset = ~static_cast<SIZE_T>(0);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
    const D3D12_CPU_DESCRIPTOR_HANDLE returned =
        reinterpret_cast<Fn>(self->real_vtable[9])(self->real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

D3D12_GPU_DESCRIPTOR_HANDLE heap_get_gpu_handle(HeapProxy* self) {
    note_heap_slot(10);
    constexpr UINT64 kUnset = ~static_cast<UINT64>(0);
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_GPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
    const D3D12_GPU_DESCRIPTOR_HANDLE returned =
        reinterpret_cast<Fn>(self->real_vtable[10])(self->real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

template <unsigned Slot>
std::uint64_t WINAPI heap_slot(HeapProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(heap_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return heap_add_ref(self);
    } else if constexpr (Slot == 2) {
        return heap_release(self);
    } else if constexpr (Slot == 7) {
        return hr_to_u64(heap_get_device(self, reinterpret_cast<const IID*>(a1),
                                         reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 9) {
        return static_cast<std::uint64_t>(heap_get_cpu_handle(self).ptr);
    } else if constexpr (Slot == 10) {
        return static_cast<std::uint64_t>(heap_get_gpu_handle(self).ptr);
    } else {
        note_heap_slot(Slot);
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <std::size_t... I>
void fill_heap_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&heap_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_heap_vtable() {
    static void** vtable = [] {
        static void* storage[kHeapSlots];
        fill_heap_vtable(storage, std::make_index_sequence<kHeapSlots>{});
        return storage;
    }();
    return vtable;
}

HRESULT create_heap_proxy(ID3D12DescriptorHeap* real, DeviceProxy* device, REFIID riid, void** out) {
    if (!real || !device) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    HeapProxy* proxy = new (std::nothrow) HeapProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_heap_vtable();
    proxy->real = real;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy_add_ref(device);  // the heap keeps its Xbox-ABI device alive for GetDevice

    const HRESULT hr = heap_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its references to `real` and `device`.
        delete proxy;
        proxy_release(device);
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: heap proxy %p wraps PC heap %p (device proxy %p)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), static_cast<void*>(device));
    heap_release(proxy);
    return S_OK;
}

// ---- Command queue and fence proxies --------------------------------------------------------------------------
//
// Device slots 8 (CreateCommandQueue), 75 (CreateCommandQueue1), 36 (CreateFence) and 32 (OpenSharedHandle, for a
// fence) wrap what they create, so a title never holds a PC or Vulkan-layer queue or fence. The proxies forward
// the public chain in PC order, exactly like the device proxy, and a slot past the PC vtable is logged and refused
// with E_NOTIMPL. PresentX, SuspendX and ResumeX are queue methods past the public chain
// (docs/d3d12_command_objects.md §5.3). Their slots are not known and nothing is guessed: the first run of the
// executable names them in the log.
//
// Slot numbers are those of the Windows SDK 10.0.26100 d3d12.h: ID3D12CommandQueue has 19, ID3D12Fence 11 and
// ID3D12Fence1 12. That the console keeps these public slots, and keeps device slots 8/32/36/75, is an assumption
// the shipped binaries cannot test (§3.5: the package has no queue or fence client). Every queue and fence method
// takes integer and pointer arguments only; GetDesc returns its struct through the hidden pointer in RDX, which is
// forwarded unchanged (no Xbox variant is measured, §6).
//
// The refused range is the proxy's own table: 128 queue slots and 64 fence slots, every one a thunk. The table sits
// at the end of the first page of a 64 KiB reservation whose other pages are never committed, so a slot index past
// the table makes the vtable read itself fault, at an address that names the slot (a vectored handler logs it and
// lets the fault continue), instead of jumping through whatever memory follows. A title whose vtable read lands
// further away than that reservation is not covered.
//
// Objects cross back into the real runtime wherever a device or queue method takes them, and the real object is
// passed there: queue Signal/Wait (14/15) take a fence; device CreateSharedHandle (31) a device child,
// MakeResident/Evict (34/35), SetResidencyPriority (46) and EnqueueMakeResident (50) pageables, and
// SetEventOnMultipleFenceCompletion (45) and EnqueueMakeResident fences. The heap proxies of slot 14 are pageables
// and device children too and are unwrapped in the same places. Command lists are not wrapped yet, so
// ExecuteCommandLists passes them through, and a heap proxy still reaches the real runtime through the list's
// SetDescriptorHeaps. A command-list proxy (docs/d3d12_command_objects.md §3.5a) adds its vtable to
// proxy_real_object, which every unwrap goes through, and a queue slot 10 branch in child_slot that unwraps the list
// array (UnwrappedArray) before forwarding, and unwraps heap proxies in its own SetDescriptorHeaps.
//
// QueryInterface answers only the interfaces the proxy models (the base chain and its own interface, Fence1 when
// the real fence has it), like the device and heap proxies, so the COM identity holds and no real object leaks to
// the title. Palmetto's own code reaches the real object through queue_proxy_real_queue / fence_proxy_real_fence /
// allocator_proxy_real_allocator.
//
// Command allocators (device slot 9) are the third kind: their public chain is 9 slots (Reset at 8). Slot 9 wraps
// only an allocator that answers kPalmettoCommandAllocatorTag, because a list takes the title's allocator in
// CreateCommandList and Reset and only the Vulkan layer's lists resolve a proxy there (CommandAllocator::from);
// a PC runtime allocator is handed out unchanged until a command-list proxy unwraps it at Reset.

enum class ChildKind { queue, fence, allocator };

constexpr unsigned kQueueSlots = 128;        // the table: 19 public slots plus a refused tail
constexpr unsigned kFenceSlots = 64;         // the table: 12 public slots plus a refused tail
constexpr unsigned kQueuePublicSlots = 19;   // ID3D12CommandQueue
constexpr unsigned kQueueGetDesc = 18;           // the one queue method that returns a struct by value
constexpr unsigned kFencePublicSlots = 11;   // ID3D12Fence
constexpr unsigned kFence1PublicSlots = 12;  // ID3D12Fence1
constexpr unsigned kAllocatorSlots = 64;     // the table: 9 public slots plus a refused tail
constexpr unsigned kAllocatorPublicSlots = 9;  // ID3D12CommandAllocator: the base chain and Reset (slot 8)
constexpr SIZE_T kTableRegion = 64 * 1024;   // one allocation granule: the table page plus 15 never-committed pages
static_assert(kQueueSlots > kQueuePublicSlots && kFenceSlots > kFence1PublicSlots &&
                  kAllocatorSlots > kAllocatorPublicSlots,
              "the tables need a tail");
static_assert(kQueueSlots * sizeof(void*) <= 4096 && kFenceSlots * sizeof(void*) <= 4096 &&
                  kAllocatorSlots * sizeof(void*) <= 4096,
              "a table must fit in the first page of its reservation");

constexpr unsigned kQueueSignal = 14, kQueueWait = 15;  // (ID3D12Fence*, UINT64)
// The console queue inserts two methods before Signal (docs/d3d12_xs_surface.md records the public PresentX slot-30/32
// split "with two methods inserted before Signal"). Measured on this title: its Signal is console slot 16 and its GetDesc
// is console slot 20, so console slots 16..20 are PC slots 14..18. Slots 14 and 15 are the inserted methods, shapes
// not yet identified.
constexpr unsigned kConsoleQueueShift = 2;                 // console slot = PC slot + 2 from Signal on
constexpr unsigned kConsoleQueueSignal = 16;               // (ID3D12Fence*, UINT64)
constexpr unsigned kConsoleQueueWait = 17;                 // (ID3D12Fence*, UINT64)
constexpr unsigned kConsoleQueueTimestampFrequency = 18;   // (UINT64*)
constexpr unsigned kConsoleQueueClockCalibration = 19;     // (UINT64*, UINT64*)
constexpr unsigned kConsoleQueueGetDesc = 20;              // returns D3D12_COMMAND_QUEUE_DESC by value

struct ChildProxy {
    void** vtable;          // offset 0: the caller's ID3D12CommandQueue* / ID3D12Fence* points here
    IUnknown* real;         // offset 8: the PC or Vulkan-layer object every thunk forwards to
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    unsigned slot_count;    // public slots the real object implements; the rest of the table is refused
    std::atomic<unsigned long> refs;
    // Console fence slot 12 (docs/fence_slot12.md): the cell the proxy answers with. Shared because the watcher that
    // moves it can be dispatched after the proxy is released; empty on a queue or allocator proxy.
    std::shared_ptr<std::atomic<std::uint64_t>> value_cell;
    std::atomic<std::uint64_t> watched_target{0};  // highest value a watcher was asked for
};

// ---- Console fence slot 12 (docs/fence_slot12.md) ----
//
// A title's wait loop calls slot 12 on a fence proxy, keeps the answer and polls [answer] with monitorx/mwaitx until
// the word there reaches the value it wants. The word is the fence's completed value; the console keeps it in
// CPU-visible memory and the method hands out its address. The proxy keeps a cell at that value, set from the real
// fence when the method is called (and when the fence is advanced through a proxy) and from the Vulkan layer's fence
// dispatcher for a target the GPU has not reached yet. The title's comparison is 56-bit: (value - target) has bit 55
// clear once the value has reached the target, and UINT64_MAX is handled before it as "device removed".
constexpr std::uint64_t kFenceValueSignBit = 0x0080000000000000ull;

bool fence_value_reached(std::uint64_t value, std::uint64_t target) {
    return ((value - target) & kFenceValueSignBit) == 0;
}

// The cell only goes up: an Xbox fence value is monotonic and UINT64_MAX (a removed device) is its top.
void publish_fence_value(const std::shared_ptr<std::atomic<std::uint64_t>>& cell, std::uint64_t value) {
    if (!cell) return;
    std::uint64_t current = cell->load(std::memory_order_relaxed);
    while (current < value &&
           !cell->compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

// The real fence's current value (GetCompletedValue, PC slot 8, read through the real vtable).
void refresh_fence_value(ChildProxy* self) {
    if (!self->value_cell) return;
    using GetCompletedValueFn = std::uint64_t(WINAPI*)(void*);
    publish_fence_value(self->value_cell,
                        reinterpret_cast<GetCompletedValueFn>(self->real_vtable[8])(self->real));
}

// Registers a watch for a target the fence has not reached yet; the dispatcher writes the target into the cell once
// the GPU reaches it. Harmless when the title only asks for the address and no wait follows.
void watch_fence_value(ChildProxy* self, std::uint64_t target, std::uint64_t completed) {
    if (!self->value_cell || completed == UINT64_MAX) return;
    if (fence_value_reached(completed, target)) return;
    const FenceValueWatcherFn watch = fence_value_watcher();
    if (!watch) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: no fence value watcher is registered; a fence slot-12 cell only moves when "
                         "the fence is advanced through its proxy\n");
        return;
    }
    std::uint64_t watched = self->watched_target.load(std::memory_order_relaxed);
    while (watched < target && !self->watched_target.compare_exchange_weak(watched, target)) {
    }
    if (watched >= target) return;  // an earlier wait already asked for this value or a higher one
    watch(self->real, self->value_cell, target);
}

template <ChildKind K>
struct ChildStats {
    static constexpr unsigned kSlots = K == ChildKind::queue   ? kQueueSlots
                                       : K == ChildKind::fence ? kFenceSlots
                                                               : kAllocatorSlots;
    static inline std::atomic<std::uint64_t> hits[kSlots];
    static inline std::atomic<bool> first[kSlots];
    static inline std::atomic<bool> beyond_warned[kSlots];
    static inline std::atomic<std::uint64_t> overruns{0};
    // Where the table and its reservation are: written once, before the table is handed out.
    static inline std::atomic<std::uintptr_t> table_begin{0}, table_end{0}, region_end{0};
};

template <ChildKind K>
constexpr const char* child_kind_name() {
    return K == ChildKind::queue ? "queue" : K == ChildKind::fence ? "fence" : "allocator";
}

template <ChildKind K>
const char* child_public_name(unsigned slot) {
    static constexpr const char* object[] = {"QueryInterface", "AddRef", "Release", "GetPrivateData",
                                             "SetPrivateData", "SetPrivateDataInterface", "SetName", "GetDevice"};
    // Console order: PC's Signal/Wait positions hold the console's two inserted methods, and the rest are shifted two.
    static constexpr const char* queue[] = {"UpdateTileMappings", "CopyTileMappings", "ExecuteCommandLists",
                                            "SetMarker", "BeginEvent", "EndEvent", "console method 14",
                                            "console method 15", "Signal", "Wait", "GetTimestampFrequency",
                                            "GetClockCalibration", "GetDesc"};
    static constexpr const char* fence[] = {"GetCompletedValue", "SetEventOnCompletion", "Signal",
                                            "GetCreationFlags"};
    if (slot < 8) return object[slot];
    if constexpr (K == ChildKind::queue) {
        if (slot <= kConsoleQueueGetDesc) return queue[slot - 8];
    } else if constexpr (K == ChildKind::fence) {
        if (slot < kFence1PublicSlots) return fence[slot - 8];
    } else {
        if (slot == 8) return "Reset";
    }
    return nullptr;
}

// Fixed names for the trace channel (d3d12_trace::record takes fixed strings): one per table slot, built once, in
// the trace grammar of runtime/README.md and check_d3d12_trace.py (an identifier with `::`, no spaces):
// XboxID3D12CommandQueue::Signal, and XboxID3D12CommandQueue::Slot40 for a slot past the public chain.
template <ChildKind K>
const char* child_trace_name(unsigned slot) {
    static const auto names = [] {
        static char storage[ChildStats<K>::kSlots][64];
        const char* iface = K == ChildKind::queue   ? "XboxID3D12CommandQueue"
                            : K == ChildKind::fence ? "XboxID3D12Fence"
                                                    : "XboxID3D12CommandAllocator";
        for (unsigned s = 0; s < ChildStats<K>::kSlots; ++s) {
            const char* method = child_public_name<K>(s);
            if (method)
                std::snprintf(storage[s], sizeof(storage[s]), "%s::%s", iface, method);
            else
                std::snprintf(storage[s], sizeof(storage[s]), "%s::Slot%u", iface, s);
        }
        return &storage;
    }();
    return (*names)[slot < ChildStats<K>::kSlots ? slot : 0];
}

template <ChildKind K>
void note_child_slot(unsigned slot) {
    using S = ChildStats<K>;
    if (slot >= S::kSlots) return;
    count_hit(S::hits[slot]);
    const bool first = first_call(S::first[slot]);
    const char* method = child_public_name<K>(slot);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) first call\n", child_kind_name<K>(), slot,
                     method ? method : "past the public chain");
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) call\n", child_kind_name<K>(), slot,
                     method ? method : "past the public chain");
    }
}

// The counters reset_device_proxy_stats clears for one kind: per-slot hits, first-call and beyond-table flags, and
// the faulted reads past the table.
template <ChildKind K>
void reset_child_stats() {
    using S = ChildStats<K>;
    for (unsigned slot = 0; slot < S::kSlots; ++slot) {
        S::hits[slot].store(0, std::memory_order_relaxed);
        S::first[slot].store(false, std::memory_order_relaxed);
        S::beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    S::overruns.store(0, std::memory_order_relaxed);
}

// Frees a proxy whose last reference is gone, with the real object and the device reference it held.
void destroy_child(ChildProxy* self) {
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    delete self;
    if (real) real->Release();
    if (device) proxy_release(device);
}

template <ChildKind K>
unsigned long child_add_ref(ChildProxy* self) {
    note_child_slot<K>(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <ChildKind K>
unsigned long child_release(ChildProxy* self) {
    note_child_slot<K>(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_child(self);
    return refs;
}

// The interfaces the proxy answers: the base chain and the object's own interface.
template <ChildKind K>
bool is_child_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
        IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)))
        return true;
    if constexpr (K == ChildKind::queue) {
        return IsEqualIID(iid, __uuidof(ID3D12CommandQueue));
    } else if constexpr (K == ChildKind::fence) {
        return IsEqualIID(iid, __uuidof(ID3D12Fence)) || IsEqualIID(iid, __uuidof(ID3D12Fence1));
    } else {
        return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator));
    }
}

bool supports_iid(IUnknown* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// An interface the real object implements but the proxy does not model is refused where the PC runtime would answer:
// logged once per interface (the first 32 of them), so a title that stops after such a refusal shows what it asked
// for. Also reached from wrap_child, for the interface a title creates a queue or fence as.
void log_refused_iid(const char* kind, const IID& iid) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static IID seen[32];
    static unsigned count = 0;
    AcquireSRWLockExclusive(&lock);
    bool known = false;
    for (unsigned i = 0; i < count && !known; ++i) known = IsEqualIID(seen[i], iid) != 0;
    const bool log = !known && count < std::size(seen);
    if (log) seen[count++] = iid;
    ReleaseSRWLockExclusive(&lock);
    if (!log) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: %s proxy refuses {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}, which the real "
                 "object implements; E_NOINTERFACE\n",
                 kind, iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
                 iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
}

template <ChildKind K>
HRESULT child_query_interface(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    // Only what the proxy models, and of that only what the real object implements (ID3D12Fence1 is optional).
    if (!is_child_iid<K>(*riid)) {
        if (supports_iid(self->real, *riid)) log_refused_iid(child_kind_name<K>(), *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, *riid)) return E_NOINTERFACE;
    *out = self;
    child_add_ref<K>(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice hands back the Xbox-ABI device, not the real one. Without an output it is the real
// object's own answer (what the PC runtime returns for a null pointer).
template <ChildKind K>
HRESULT child_get_device(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(7);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure (bc59ef46)
    if (!riid) return E_POINTER;
    if (!out) {
        using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
        return reinterpret_cast<Fn>(self->real_vtable[7])(self->real, riid, nullptr);
    }
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

void** get_heap_vtable();
void** get_list_vtable();

template <ChildKind K>
void** get_child_vtable();

template <ChildKind K>
bool is_child_proxy(const void* object) {
    return object && *reinterpret_cast<void* const*>(object) == reinterpret_cast<void*>(get_child_vtable<K>());
}

constexpr unsigned kListSlots = 160;
constexpr unsigned kConsoleSetDescriptorHeaps = 32;
constexpr unsigned kConsoleExecuteBundle = 31;
constexpr unsigned kConsoleBarrier = 146;

struct ListProxy {
    void** vtable;                      // +0x00
    ID3D12GraphicsCommandList* real;    // +0x08
    DeviceProxy* device;                // +0x10: keeps the Xbox-ABI device alive; GetDevice returns it
    // +0x18..: the console object body a title's inline writers read. +0x18 holds the write cursor, +0x28 and
    // +0x30 the record limits; the rest is room for the measured flags, header table and pipeline record.
    alignas(8) std::uint8_t console_fields[0x600];
    std::atomic<unsigned long> refs;    // past the console body
    unsigned pc_slot_count;             // slots the real list implements; mapped slots past it are refused
    // The console command-list lifecycle (docs/list_lifecycle.md): a console list's recording memory belongs to its
    // allocator, so the title creates and closes its lists during setup and then records into them again every frame
    // without a Reset. The PC runtime instead enforces the PC rule: a closed list accepts no recording and Close then
    // fails. The proxy keeps the allocator the list was created with and, on a forwarded recording call that reaches
    // the real list closed, resets it against that allocator first - what the console's allocator-owned recording
    // memory does implicitly - so the frame's recording is real rather than dropped.
    ID3D12CommandAllocator* allocator;  // AddRef'd while the proxy lives
    std::atomic<int> recording_state;   // 1: the real list is recording, 0: its last Close succeeded

    void pin_cursor_at_limit() {
        const std::uint64_t at_limit = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(this) + 0x18 + 0x500);
        for (const size_t offset : {0x18, 0x28, 0x30})
            std::memcpy(reinterpret_cast<std::uint8_t*>(this) + offset, &at_limit, sizeof(at_limit));
    }
};
static_assert(offsetof(ListProxy, console_fields) == 0x18, "the console fields start at +0x18");

// The lifecycle emulation's logging: the first few reopens and their result, then silence (a reset per frame is normal
// and must not fill the log). A failed Reset is reported once with the console call that needed it, and that call is
// refused instead of being forwarded into a list the PC runtime would drop it from. Returns true when the reset
// succeeded (the caller forwards the call), false when it did not (the caller refuses it with that HRESULT).
bool note_list_reopen(ListProxy* self, unsigned console_slot, HRESULT reset) {
    if (SUCCEEDED(reset)) {
        static std::atomic<unsigned> reopened{0};
        if (reopened.fetch_add(1, std::memory_order_relaxed) < 4)
            std::fprintf(stderr, "[rt] d3d12_xs: reopened list %p for recording on console slot %u (allocator %p)\n",
                         static_cast<void*>(self), console_slot, static_cast<void*>(self->allocator));
        return true;
    }
    static std::atomic<bool> failed{false};
    if (!failed.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: could not reopen list %p for console slot %u: Reset 0x%08x (allocator %p); "
                     "refusing the call\n",
                     static_cast<void*>(self), console_slot, static_cast<unsigned>(reset),
                     static_cast<void*>(self->allocator));
    return false;
}

// The real object behind a heap, queue, fence or list proxy, or null when `object` is none of them.
IUnknown* proxy_real_object(const void* object) {
    if (!object) return nullptr;
    void* const vtable = *reinterpret_cast<void* const*>(object);
    if (vtable == reinterpret_cast<void*>(get_heap_vtable())) return static_cast<const HeapProxy*>(object)->real;
    if (vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::queue>()) ||
        vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::fence>()) ||
        vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::allocator>()))
        return static_cast<const ChildProxy*>(object)->real;
    if (vtable == reinterpret_cast<void*>(get_list_vtable()))
        return static_cast<const ListProxy*>(object)->real;
    return nullptr;
}

// `object` with a proxy replaced by its real object; anything else (a real object, null) unchanged.
inline std::uint64_t unwrap_object(std::uint64_t object) {
    IUnknown* real = proxy_real_object(reinterpret_cast<const void*>(object));
    return real ? reinterpret_cast<std::uint64_t>(real) : object;
}

// A copy of an array of interface pointers with every proxy replaced by its real object. Nothing here throws, since
// an exception must not leave a thunk for the title's code: up to 16 entries are kept inline (no allocation, which
// also suits a per-frame ExecuteCommandLists), a longer array is allocated with new (std::nothrow).
class UnwrappedArray {
public:
    // Copies `count` entries (a UINT in every PC prototype) of `objects`. False on allocation failure.
    bool fill(std::uint64_t objects, std::uint64_t count) {
        const auto n = static_cast<std::uint32_t>(count);
        if (!objects || !n) return true;
        std::uint64_t* copy = inline_;
        if (n > std::size(inline_)) {
            heap_.reset(new (std::nothrow) std::uint64_t[n]);
            if (!heap_) return false;
            copy = heap_.get();
        }
        const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
        for (std::uint32_t i = 0; i < n; ++i) copy[i] = unwrap_object(in[i]);
        data_ = copy;
        return true;
    }
    // The copy to pass on, or 0 when there was no array (the caller's argument then goes on unchanged).
    std::uint64_t data() const { return reinterpret_cast<std::uint64_t>(data_); }

private:
    std::uint64_t inline_[16];
    std::unique_ptr<std::uint64_t[]> heap_;
    std::uint64_t* data_ = nullptr;
};

struct ListStats {
    static inline std::atomic<std::uint64_t> hits[kListSlots];
    static inline std::atomic<bool> first[kListSlots];
};

inline void note_list_slot(unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0, std::uint64_t a4 = 0) {
    if (slot >= kListSlots) return;
    ListStats::hits[slot].fetch_add(1, std::memory_order_relaxed);
    bool expected = false;
    if (ListStats::first[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: list slot %u first call (args 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                     static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                     static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
}

// ---- Console recording-call probe (measurement only; PALMETTO_XS_CLEAR_BARRIER_PROBE=1) --------------------------
//
// Two console recording calls still invalidate a reopened PC list (docs/list_lifecycle.md section 5): console slot 52
// (ClearRenderTargetView -> PC slot 48) and console slot 146 (the enhanced-barrier group -> PC GCL7 slot 80). Both need
// their measured arguments before either can be translated, and this proxy is where a title's console-ABI call is seen
// before the PC layer does. The probe answers, capped and off by default:
//   * the clear's four arguments and the 24-byte target-view capture the Vulkan layer stores at the CPU handle;
//   * whether that handle was the destination of a recent CreateRenderTargetView (a ring of the last destinations and
//     the resource each was made from);
//   * every console descriptor heap's requested address and size with the CPU/GPU handles the layer returned (the
//     create_console_descriptor_heap console-address convention, so a handle can be placed in its heap);
//   * the barrier group's type, count and every entry's sync/access/layout/resource words.
// No call is changed: each hook prints and returns, and no control flow depends on the probe.
static bool readable_range(const void* pointer, std::size_t bytes);  // defined below, for the dumps

bool clear_barrier_probe() {
    static const bool on = [] {
        char value[8] = {};
        return GetEnvironmentVariableA("PALMETTO_XS_CLEAR_BARRIER_PROBE", value, sizeof(value)) > 0 &&
               value[0] != '\0' && value[0] != '0';
    }();
    return on;
}

// A CPU descriptor handle a CreateRenderTargetView wrote a capture at, with the resource it was made from. Recorded
// unconditionally (an RTV is created rarely and the recording is one atomic add): the console ClearRenderTargetView
// conversion asks whether the handle it was given is a slot this proxy itself filled, and with what resource.
struct RtvDestination {
    std::uint64_t handle;
    std::uint64_t resource;
};
constexpr unsigned kRtvDestinationRing = 8192;
RtvDestination g_rtv_destinations[kRtvDestinationRing];
std::atomic<unsigned> g_rtv_destination_count{0};

void note_rtv_destination(std::uint64_t handle, std::uint64_t resource) {
    if (!handle) return;
    RtvDestination& entry =
        g_rtv_destinations[g_rtv_destination_count.fetch_add(1, std::memory_order_relaxed) % kRtvDestinationRing];
    entry.resource = resource;
    entry.handle = handle;
}

// The resource the most recent CreateRenderTargetView made the view at `handle` from; false when no such destination
// is in the ring (a handle never written, or one older than the ring).
bool find_rtv_destination(std::uint64_t handle, std::uint64_t& resource) {
    const unsigned count = std::min(g_rtv_destination_count.load(std::memory_order_relaxed), kRtvDestinationRing);
    bool found = false;
    for (unsigned i = 0; i < count; ++i) {
        if (g_rtv_destinations[i].handle != handle) continue;
        resource = g_rtv_destinations[i].resource;
        found = true;
    }
    return found;
}

// The Vulkan layer's CPU descriptor slot for an RTV or DSV holds a palmetto::TargetView
// (runtime/src/command_list.h): resource id, mip, first array slice, layer count and view format. The proxy reads the
// words; xs_proxy_mock_test checks this mirror against that header's layout.
struct TargetViewWords {
    std::uint64_t resource;
    std::uint32_t mip;
    std::uint32_t first_layer;
    std::uint32_t layer_count;
    std::uint32_t format;
};
static_assert(sizeof(TargetViewWords) == 24, "a target-view capture is five words");

void print_target_view(const char* what, unsigned slot, std::uint64_t handle, std::uint64_t resource) {
    if (!handle || !readable_range(reinterpret_cast<const void*>(handle), sizeof(TargetViewWords))) {
        std::fprintf(stderr, "[rt] d3d12_xs: probe slot %u %s handle 0x%llx resource 0x%llx: handle not readable\n",
                     slot, what, static_cast<unsigned long long>(handle), static_cast<unsigned long long>(resource));
        return;
    }
    TargetViewWords view{};
    std::memcpy(&view, reinterpret_cast<const void*>(handle), sizeof(view));
    std::uint64_t created_with = 0;
    const bool created = find_rtv_destination(handle, created_with);
    std::fprintf(stderr,
                 "[rt] d3d12_xs: probe slot %u %s handle 0x%llx resource 0x%llx capture r=0x%llx mip=%u layer0=%u "
                 "layers=%u format=%u created=%d created_with=0x%llx\n",
                 slot, what, static_cast<unsigned long long>(handle), static_cast<unsigned long long>(resource),
                 static_cast<unsigned long long>(view.resource), view.mip, view.first_layer, view.layer_count,
                 view.format, created ? 1 : 0, static_cast<unsigned long long>(created_with));
}

// Console slot 52 (ClearRenderTargetView): the CPU handle the title passes, its colour/count/rects and the capture at
// the handle. Eight calls are enough to see the frame's clear (the first one is what poisons the list).
void probe_console_clear(unsigned slot, std::uint64_t handle, std::uint64_t colour, std::uint64_t count,
                         std::uint64_t rects) {
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    std::fprintf(stderr, "[rt] d3d12_xs: probe slot %u clear args handle=0x%llx colour=0x%llx count=0x%llx rects=0x%llx\n",
                 slot, static_cast<unsigned long long>(handle), static_cast<unsigned long long>(colour),
                 static_cast<unsigned long long>(count), static_cast<unsigned long long>(rects));
    if (colour && readable_range(reinterpret_cast<const void*>(colour), 4 * sizeof(float))) {
        const auto* c = reinterpret_cast<const float*>(colour);
        std::fprintf(stderr, "[rt] d3d12_xs: probe slot %u clear colour %g %g %g %g\n", slot,
                     static_cast<double>(c[0]), static_cast<double>(c[1]), static_cast<double>(c[2]),
                     static_cast<double>(c[3]));
    }
    print_target_view("clear", slot, handle, 0);
}

// D3D12_BARRIER_GROUP as PC D3D12 declares it: {Type, NumBarriers, union {pGlobalBarriers, pTextureBarriers,
// pBufferBarriers}}.
struct BarrierGroupWords {
    std::uint32_t type;
    std::uint32_t num_barriers;
    std::uint64_t barriers;
};
static_assert(sizeof(BarrierGroupWords) == 16, "a barrier-group header is 16 bytes");

// Console slot 146 (GCL7::Barrier): every group and every entry's words. Entries are printed as raw qwords so the dump
// assumes nothing about the console's enhanced-barrier record: the PC shape is 16 bytes for a global barrier, 32 for a
// buffer one and 64 for a texture one, so that many words are printed per entry.
void probe_console_barrier(unsigned slot, std::uint64_t groups, std::uint64_t group_pointer) {
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 4) return;
    std::fprintf(stderr, "[rt] d3d12_xs: probe slot %u barrier groups=%llu groups_ptr=0x%llx\n", slot,
                 static_cast<unsigned long long>(groups), static_cast<unsigned long long>(group_pointer));
    if (!group_pointer || !readable_range(reinterpret_cast<const void*>(group_pointer), sizeof(BarrierGroupWords))) {
        std::fprintf(stderr, "[rt] d3d12_xs: probe slot %u barrier groups are not readable\n", slot);
        return;
    }
    const auto* group = reinterpret_cast<const BarrierGroupWords*>(group_pointer);
    for (std::uint64_t g = 0; g < groups && g < 8; ++g) {
        const unsigned words = group[g].type == 0 ? 2 : group[g].type == 1 ? 8 : group[g].type == 2 ? 4 : 0;
        std::fprintf(stderr, "[rt] d3d12_xs: probe slot %u barrier group %llu type=%u num=%u entries=0x%llx\n", slot,
                     static_cast<unsigned long long>(g), group[g].type, group[g].num_barriers,
                     static_cast<unsigned long long>(group[g].barriers));
        if (!words || !group[g].barriers || !group[g].num_barriers) continue;
        if (!readable_range(reinterpret_cast<const void*>(group[g].barriers),
                            static_cast<std::size_t>(group[g].num_barriers) * words * sizeof(std::uint64_t))) {
            std::fprintf(stderr, "[rt] d3d12_xs: probe slot %u barrier group %llu entries are not readable\n", slot,
                         static_cast<unsigned long long>(g));
            continue;
        }
        const auto* entry = reinterpret_cast<const std::uint64_t*>(group[g].barriers);
        for (std::uint32_t i = 0; i < group[g].num_barriers && i < 8; ++i) {
            std::fprintf(stderr, "[rt] d3d12_xs: probe slot %u barrier group %llu entry %u:", slot,
                         static_cast<unsigned long long>(g), i);
            for (unsigned w = 0; w < words; ++w)
                std::fprintf(stderr, " %016llx", static_cast<unsigned long long>(entry[i * words + w]));
            std::fprintf(stderr, "\n");
        }
    }
}

// A console descriptor heap: its description, the requested console address/size and the CPU/GPU handles the layer
// returned, so a clear handle can be placed in its heap.
void probe_console_heap(unsigned slot, const D3D12_DESCRIPTOR_HEAP_DESC& desc, std::uint64_t address,
                        std::uint64_t size, std::uint64_t cpu_handle, std::uint64_t gpu_handle) {
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 16) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: probe device slot %u heap type=%u num=%u flags=0x%x address=0x%llx size=0x%llx "
                 "cpu=0x%llx gpu=0x%llx\n",
                 slot, desc.Type, desc.NumDescriptors, static_cast<unsigned>(desc.Flags),
                 static_cast<unsigned long long>(address), static_cast<unsigned long long>(size),
                 static_cast<unsigned long long>(cpu_handle), static_cast<unsigned long long>(gpu_handle));
}

// ---- Console slot 146 (enhanced barriers): a resource-less texture barrier becomes a global barrier ---------------
//
// Measured (docs/list_lifecycle.md section 5, docs/handles_barriers.md): the title's frame records
// `Barrier(1, &group)` where the group is a PC-shaped D3D12_BARRIER_GROUP of type TEXTURE and each entry a 64-byte PC
// D3D12_TEXTURE_BARRIER. The entries that invalidate the PC list are the ones with pResource == 0 - measured values
// are SyncBefore 0x1000 (ALL_SHADING), SyncAfter 0x40 (RENDER_TARGET), AccessBefore 0x10 (UNORDERED_ACCESS),
// AccessAfter 0x8 (RENDER_TARGET), LayoutBefore 3 (UNORDERED_ACCESS), LayoutAfter 2 (RENDER_TARGET), the
// all-subresources range and no flags. The console accepts a texture barrier without a resource and treats it as
// "every texture"; PC D3D12 refuses it (CommandList::barrier returns false on a null resource) and marks the whole
// list invalid, which is what made every reopened frame list report E_INVALIDARG at Close.
//
// The provable translation is a GLOBAL barrier carrying the same SyncBefore/SyncAfter/AccessBefore/AccessAfter: a
// global barrier orders exactly those stages and accesses for every resource, which is a superset of "every texture".
// The layout numbers have no encoding without a resource (a global barrier has none), so they are dropped and named in
// the log. A texture entry with a real resource is passed through unchanged (it is the PC shape the layer already
// records). A group of type GLOBAL or BUFFER is PC-shaped too and is passed through; a buffer barrier with no resource
// has not been measured and is refused rather than translated by analogy.
//
// The buffers are thread_local: the proxy is called from the title's recording threads, one translated call at a time
// on each, and the arguments are consumed by the forwarded call before the next translation on that thread.
struct ConsoleBarrierTranslation {
    D3D12_BARRIER_GROUP groups[3];  // one GLOBAL, one TEXTURE and one BUFFER group at most
    D3D12_GLOBAL_BARRIER globals[32];
    D3D12_BUFFER_BARRIER buffers[32];
    D3D12_TEXTURE_BARRIER textures[32];
};
thread_local ConsoleBarrierTranslation g_console_barrier_translation;

// Translates a console slot-146 barrier call's arguments in place. Returns false when the call must be refused (the
// caller answers 0 without forwarding, so the PC list cannot be invalidated); true when the call may be forwarded,
// with `groups`/`group_pointer` possibly rewritten to the translation buffers.
bool translate_console_barrier_groups(std::uint64_t& groups, std::uint64_t& group_pointer) {
    if (!groups) return true;  // nothing to translate: the call goes on unchanged
    if (!group_pointer ||
        !readable_range(reinterpret_cast<const void*>(group_pointer), sizeof(D3D12_BARRIER_GROUP))) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: console barrier (list slot 146) at an unreadable group pointer 0x%llx: "
                         "refusing it instead of forwarding a barrier PC D3D12 would reject\n",
                         static_cast<unsigned long long>(group_pointer));
        return false;
    }
    const auto* input = reinterpret_cast<const D3D12_BARRIER_GROUP*>(group_pointer);
    // The measured console call passes one group; a count this large means the pointer is not the group list the
    // console call describes. Refusing it (rather than walking it) cannot lose a valid barrier and cannot fault.
    constexpr std::uint64_t kMostBarrierGroups = 64;
    if (groups > kMostBarrierGroups) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: console barrier (list slot 146) asks for %llu groups (the measured call "
                         "passes one); refusing it\n",
                         static_cast<unsigned long long>(groups));
        return false;
    }
    ConsoleBarrierTranslation& out = g_console_barrier_translation;
    unsigned global_count = 0;
    unsigned buffer_count = 0;
    unsigned texture_count = 0;
    unsigned translated = 0;
    for (std::uint64_t g = 0; g < groups; ++g) {
        const D3D12_BARRIER_GROUP& group = input[g];
        if (group.Type == D3D12_BARRIER_TYPE_GLOBAL) {
            if (!group.NumBarriers) continue;
            if (!group.pGlobalBarriers ||
                !readable_range(group.pGlobalBarriers, group.NumBarriers * sizeof(D3D12_GLOBAL_BARRIER)) ||
                global_count + group.NumBarriers > std::size(out.globals)) {
                static std::atomic<bool> bad{false};
                if (!bad.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: console barrier (list slot 146): group %llu is a global-barrier group "
                                 "whose %u entries are unreadable or do not fit; refusing the call\n",
                                 static_cast<unsigned long long>(g), group.NumBarriers);
                return false;
            }
            std::memcpy(&out.globals[global_count], group.pGlobalBarriers,
                        group.NumBarriers * sizeof(D3D12_GLOBAL_BARRIER));
            global_count += group.NumBarriers;
        } else if (group.Type == D3D12_BARRIER_TYPE_BUFFER) {
            if (!group.NumBarriers) continue;
            if (!group.pBufferBarriers ||
                !readable_range(group.pBufferBarriers, group.NumBarriers * sizeof(D3D12_BUFFER_BARRIER)) ||
                buffer_count + group.NumBarriers > std::size(out.buffers)) {
                static std::atomic<bool> bad{false};
                if (!bad.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: console barrier (list slot 146): group %llu is a buffer-barrier group "
                                 "whose %u entries are unreadable or do not fit; refusing the call\n",
                                 static_cast<unsigned long long>(g), group.NumBarriers);
                return false;
            }
            for (std::uint32_t i = 0; i < group.NumBarriers; ++i) {
                if (group.pBufferBarriers[i].pResource) continue;
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: console barrier (list slot 146): buffer barrier without a resource "
                                 "(sync 0x%x->0x%x, access 0x%x->0x%x, offset 0x%llx size 0x%llx) is not a shape this "
                                 "proxy has measured; refusing the call rather than guessing\n",
                                 group.pBufferBarriers[i].SyncBefore, group.pBufferBarriers[i].SyncAfter,
                                 group.pBufferBarriers[i].AccessBefore, group.pBufferBarriers[i].AccessAfter,
                                 static_cast<unsigned long long>(group.pBufferBarriers[i].Offset),
                                 static_cast<unsigned long long>(group.pBufferBarriers[i].Size));
                return false;
            }
            std::memcpy(&out.buffers[buffer_count], group.pBufferBarriers,
                        group.NumBarriers * sizeof(D3D12_BUFFER_BARRIER));
            buffer_count += group.NumBarriers;
        } else if (group.Type == D3D12_BARRIER_TYPE_TEXTURE) {
            if (!group.NumBarriers) continue;
            if (!group.pTextureBarriers ||
                !readable_range(group.pTextureBarriers, group.NumBarriers * sizeof(D3D12_TEXTURE_BARRIER))) {
                static std::atomic<bool> bad{false};
                if (!bad.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: console barrier (list slot 146): group %llu is a texture-barrier group "
                                 "whose %u entries are unreadable; refusing the call\n",
                                 static_cast<unsigned long long>(g), group.NumBarriers);
                return false;
            }
            for (std::uint32_t i = 0; i < group.NumBarriers; ++i) {
                const D3D12_TEXTURE_BARRIER& barrier = group.pTextureBarriers[i];
                if (barrier.pResource) {
                    if (texture_count >= std::size(out.textures)) {
                        static std::atomic<bool> bad{false};
                        if (!bad.exchange(true, std::memory_order_relaxed))
                            std::fprintf(stderr,
                                         "[rt] d3d12_xs: console barrier (list slot 146): more than %zu texture "
                                         "barriers in one call; refusing it\n",
                                         std::size(out.textures));
                        return false;
                    }
                    out.textures[texture_count++] = barrier;
                    continue;
                }
                if (global_count >= std::size(out.globals)) {
                    static std::atomic<bool> bad{false};
                    if (!bad.exchange(true, std::memory_order_relaxed))
                        std::fprintf(stderr,
                                     "[rt] d3d12_xs: console barrier (list slot 146): more than %zu translated "
                                     "barriers in one call; refusing it\n",
                                     std::size(out.globals));
                    return false;
                }
                // A resource-less texture barrier: the console's "every texture". A global barrier with the same
                // sync/access words is the PC encoding; the layouts cannot be carried and are logged.
                D3D12_GLOBAL_BARRIER& global = out.globals[global_count++];
                global.SyncBefore = barrier.SyncBefore;
                global.SyncAfter = barrier.SyncAfter;
                global.AccessBefore = barrier.AccessBefore;
                global.AccessAfter = barrier.AccessAfter;
                ++translated;
                static std::atomic<unsigned> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: console barrier (list slot 146): texture barrier without a resource "
                                 "(sync 0x%x->0x%x, access 0x%x->0x%x, layout %u->%u, subresource 0x%x) translated to "
                                 "a global barrier; the console's layouts have no PC encoding without a resource and "
                                 "are dropped\n",
                                 barrier.SyncBefore, barrier.SyncAfter, barrier.AccessBefore, barrier.AccessAfter,
                                 barrier.LayoutBefore, barrier.LayoutAfter,
                                 barrier.Subresources.IndexOrFirstMipLevel);
            }
        } else {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: console barrier (list slot 146): barrier group %llu has type %u, not one "
                             "of the three PC types; refusing the call\n",
                             static_cast<unsigned long long>(g), group.Type);
            return false;
        }
    }
    if (!translated) return true;  // every group is already PC-shaped: forward the call untouched
    unsigned group_count = 0;
    if (global_count) {
        out.groups[group_count].Type = D3D12_BARRIER_TYPE_GLOBAL;
        out.groups[group_count].NumBarriers = global_count;
        out.groups[group_count].pGlobalBarriers = out.globals;
        ++group_count;
    }
    if (texture_count) {
        out.groups[group_count].Type = D3D12_BARRIER_TYPE_TEXTURE;
        out.groups[group_count].NumBarriers = texture_count;
        out.groups[group_count].pTextureBarriers = out.textures;
        ++group_count;
    }
    if (buffer_count) {
        out.groups[group_count].Type = D3D12_BARRIER_TYPE_BUFFER;
        out.groups[group_count].NumBarriers = buffer_count;
        out.groups[group_count].pBufferBarriers = out.buffers;
        ++group_count;
    }
    groups = group_count;
    group_pointer = reinterpret_cast<std::uint64_t>(&out.groups[0]);
    return true;
}

// ---- Console slot 52 (ClearRenderTargetView): an empty target view is not forwarded -------------------------------
//
// Measured (docs/handles_barriers.md): the frame's clear passes a CPU descriptor handle the proxy itself filled. The
// layer stores its 24-byte target-view capture at that handle, and the measured capture is empty (resource 0, layer
// count 0) because the title created that descriptor with CreateRenderTargetView(pResource = 0) during setup - a null
// render-target view. The console accepts clearing a null view (it clears nothing); PC D3D12 refuses it
// (CommandList::clear_render_target returns false for a view without a resource), which marked the reopened list
// invalid and failed the frame's Close.
//
// The handle convention is therefore not the problem: the handle is exactly the slot the proxy's own
// CreateRenderTargetView wrote, so there is nothing to remap (a console address that needed the
// create_console_descriptor_heap mapping would not be in the destination ring). A clear whose capture is a real view
// is forwarded unchanged. One whose capture is empty cannot be forwarded: the honest answer is to refuse it, and for
// a slot this proxy created from a null resource that refusal is the exact semantics - clearing nothing - so the
// frame's recording stays valid and its Close is kept. Returns true when the call may be forwarded.
bool translate_console_clear(std::uint64_t handle, std::uint64_t colour) {
    TargetViewWords view{};
    if (!handle || !readable_range(reinterpret_cast<const void*>(handle), sizeof(view))) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: console ClearRenderTargetView (list slot 52) at an unreadable handle 0x%llx: "
                         "refusing it instead of forwarding a view PC D3D12 would reject\n",
                         static_cast<unsigned long long>(handle));
        return false;
    }
    std::memcpy(&view, reinterpret_cast<const void*>(handle), sizeof(view));
    if (view.resource && view.layer_count) return true;  // a real view: the PC clear records it as usual
    std::uint64_t created_with = 0;
    const bool created = find_rtv_destination(handle, created_with);
    static std::atomic<unsigned> logged{0};
    if (created && created_with == 0) {
        if (logged.fetch_add(1, std::memory_order_relaxed) < 2)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: console ClearRenderTargetView (list slot 52) on a null view: handle 0x%llx was "
                         "created by CreateRenderTargetView with pResource=0, so clearing it clears nothing; PC D3D12 "
                         "refuses it. Refused as the no-op it is (the list stays valid and its Close is kept)\n",
                         static_cast<unsigned long long>(handle));
        return false;
    }
    if (logged.fetch_add(1, std::memory_order_relaxed) < 2) {
        if (created)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: console ClearRenderTargetView (list slot 52): handle 0x%llx was created from "
                         "resource 0x%llx but its capture is empty now; refusing it rather than forwarding a view PC "
                         "D3D12 would reject\n",
                         static_cast<unsigned long long>(handle), static_cast<unsigned long long>(created_with));
        else
            std::fprintf(stderr,
                         "[rt] d3d12_xs: console ClearRenderTargetView (list slot 52): handle 0x%llx (colour %llx) was "
                         "not made by CreateRenderTargetView and its capture is empty; refusing it rather than "
                         "forwarding a view PC D3D12 would reject\n",
                         static_cast<unsigned long long>(handle), static_cast<unsigned long long>(colour));
    }
    return false;
}

// The retail 1.0.2.1 executable asks device slot 73 for this console command-list IID. The PC SDK does not declare
// it, so without this the PC CreateCommandList returns E_NOINTERFACE and the title asserts on a null list.
// {c3827890-4024-474c-98cf-645b51dc4874}
inline constexpr GUID kConsoleGraphicsCommandListIid{
    0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};

inline bool is_list_iid(const IID& iid) {
    return IsEqualIID(iid, kConsoleGraphicsCommandListIid) ||
           IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList1)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList2)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList3)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList4)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList5)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList6)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList7)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList8));
}

inline unsigned long list_add_ref(ListProxy* self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

inline void destroy_list(ListProxy* self) {
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    ID3D12CommandAllocator* allocator = self->allocator;
    delete self;
    if (allocator) allocator->Release();
    if (real) real->Release();
    if (device) proxy_release(device);
}

inline unsigned long list_release(ListProxy* self) {
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_list(self);
    return refs;
}

inline HRESULT list_query_interface(ListProxy* self, const IID* riid, void** out) {
    note_list_slot(0, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out), 0, 0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    // The console command-list IID has no PC counterpart, so the wrapped PC list never answers it: this proxy does.
    if (IsEqualIID(*riid, kConsoleGraphicsCommandListIid)) {
        *out = self;
        list_add_ref(self);
        return S_OK;
    }
    if (!is_list_iid(*riid)) {
        if (supports_iid(self->real, *riid)) log_refused_iid("list", *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, *riid)) return E_NOINTERFACE;
    *out = self;
    list_add_ref(self);
    return S_OK;
}

inline unsigned detect_list_slot_count(IUnknown* real) {
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList8))) return 82;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList7))) return 81;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList6))) return 80;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList5))) return 79;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList4))) return 77;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList3))) return 68;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList2))) return 67;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList1))) return 66;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList))) return 60;
    return 9;
}

template <unsigned Slot>
std::uint64_t WINAPI list_slot(ListProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if (callers_enabled()) note_caller(CallerKind::list, Slot, _ReturnAddress(), a1, a2, a3, a4);
    if constexpr (Slot == 0) {
        return hr_to_u64(list_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return list_add_ref(self);
    } else if constexpr (Slot == 2) {
        return list_release(self);
    } else if constexpr (Slot == 7) {  // GetDevice
        note_list_slot(7, a1, a2, 0, 0);
        if (a2) *reinterpret_cast<void**>(a2) = nullptr;
        if (!a1) return hr_to_u64(E_POINTER);
        if (!a2) {
            using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
            return hr_to_u64(reinterpret_cast<Fn>((*reinterpret_cast<void***>(self->real))[7])(
                self->real, reinterpret_cast<const IID*>(a1), nullptr));
        }
        return hr_to_u64(proxy_query_interface(self->device, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else {
        note_list_slot(Slot, a1, a2, a3, a4);
        // PALMETTO_XS_LIST_MUTE=32[,34...] (measurement): answer those console slots with 0 instead of forwarding
        // them, so a boot can bisect which recording call makes the PC list report its Close as E_INVALIDARG.
        static const auto muted = [] {
            std::array<bool, kListSlots> set{};
            char value[256] = {};
            if (GetEnvironmentVariableA("PALMETTO_XS_LIST_MUTE", value, sizeof(value)) > 0) {
                char* at = value;
                while (*at) {
                    char* end = nullptr;
                    const unsigned long slot = std::strtoul(at, &end, 0);
                    if (slot < kListSlots) set[slot] = true;
                    if (end == at) break;
                    at = *end == ',' ? end + 1 : end;
                }
            }
            return set;
        }();
        if (muted[Slot]) return 0;
        // Measurement probe for the two console recording calls that still invalidate a reopened list
        // (docs/list_lifecycle.md section 5). It prints and returns; the call below is unchanged.
        if constexpr (Slot == 52) {
            if (clear_barrier_probe()) probe_console_clear(Slot, a1, a2, a3, a4);
        } else if constexpr (Slot == 146) {
            if (clear_barrier_probe()) probe_console_barrier(Slot, a1, a2);
        }
        // PALMETTO_XS_LIST_DUMP=146[,..] (measurement): the arguments and the pointed-to words of a console list
        // slot, for the first four calls, to check a console structure against its PC shape.
        static const auto dumped = [] {
            std::array<unsigned, 8> set{};
            char value[64] = {};
            if (GetEnvironmentVariableA("PALMETTO_XS_LIST_DUMP", value, sizeof(value)) > 0) {
                char* at = value;
                unsigned n = 0;
                while (*at && n < set.size()) {
                    char* end = nullptr;
                    set[n++] = static_cast<unsigned>(std::strtoul(at, &end, 0));
                    if (end == at) break;
                    at = *end == ',' ? end + 1 : end;
                }
            }
            return set;
        }();
        for (unsigned dump_index = 0; dump_index < dumped.size() && dumped[dump_index] != 0; ++dump_index) {
            const unsigned want = dumped[dump_index];
            if (want == Slot) {
                static std::atomic<unsigned> dump_count{0};
                if (dump_count.fetch_add(1, std::memory_order_relaxed) < 4) {
                    std::fprintf(stderr, "[rt] d3d12_xs: dump slot %u a1=0x%llx a2=0x%llx a3=0x%llx a4=0x%llx\n",
                                 Slot, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                                 static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (a2 && VirtualQuery(reinterpret_cast<const void*>(a2), &mbi, sizeof(mbi)) == sizeof(mbi) &&
                        mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                                                   PAGE_EXECUTE_WRITECOPY))) {
                        const auto* words = reinterpret_cast<const std::uint64_t*>(a2);
                        const auto* inner = reinterpret_cast<const std::uint64_t*>(words[1]);
                        MEMORY_BASIC_INFORMATION imbi{};
                        if (words[1] &&
                            VirtualQuery(inner, &imbi, sizeof(imbi)) == sizeof(imbi) && imbi.State == MEM_COMMIT) {
                            std::fprintf(stderr, "[rt] d3d12_xs: dump slot %u [a2[1]]: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\\n",
                                         Slot, static_cast<unsigned long long>(inner[0]),
                                         static_cast<unsigned long long>(inner[1]),
                                         static_cast<unsigned long long>(inner[2]),
                                         static_cast<unsigned long long>(inner[3]),
                                         static_cast<unsigned long long>(inner[4]),
                                         static_cast<unsigned long long>(inner[5]),
                                         static_cast<unsigned long long>(inner[6]),
                                         static_cast<unsigned long long>(inner[7]));
                        }
                        std::fprintf(stderr, "[rt] d3d12_xs: dump slot %u [a2]: %016llx %016llx %016llx %016llx "
                                             "%016llx %016llx %016llx %016llx\n",
                                     Slot, static_cast<unsigned long long>(words[0]),
                                     static_cast<unsigned long long>(words[1]),
                                     static_cast<unsigned long long>(words[2]),
                                     static_cast<unsigned long long>(words[3]),
                                     static_cast<unsigned long long>(words[4]),
                                     static_cast<unsigned long long>(words[5]),
                                     static_cast<unsigned long long>(words[6]),
                                     static_cast<unsigned long long>(words[7]));
                    }
                }
            }
        }
        constexpr unsigned pc = console_to_pc_list_slot(Slot);
        if constexpr (pc == kListNotForwarded) {
            if constexpr (Slot == 148) {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: console list slot 148 AllocateUploadMemory stub called (size %llu, align %llu)\n",
                                 static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2));
            }
            return 0;
        } else {
            if (pc >= self->pc_slot_count) {
                static std::atomic<bool> beyond_warned[kListSlots];
                bool expected = false;
                if (beyond_warned[Slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: list slot %u maps to PC slot %u, beyond the real list's %u "
                                 "slots; returning E_NOTIMPL\n",
                                 Slot, pc, self->pc_slot_count);
                return hr_to_u64(E_NOTIMPL);
            }
            using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t);
            auto** const real_vtable = *reinterpret_cast<void***>(self->real);
            // PALMETTO_XS_LIST_ID=1 (measurement): which list proxy a Close or a recording call lands on, capped.
            static const bool list_id = [] {
                char value[8] = {};
                return GetEnvironmentVariableA("PALMETTO_XS_LIST_ID", value, sizeof(value)) > 0 &&
                       value[0] != '\0' && value[0] != '0';
            }();
            if (list_id && (Slot == 9 || Slot == 10 || Slot == 32 || Slot == 146 || Slot == 34)) {
                static std::atomic<unsigned> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 12000)
                    std::fprintf(stderr, "[rt] d3d12_xs: listid slot %u list %p state %d\n", Slot,
                                 static_cast<void*>(self),
                                 self->recording_state.load(std::memory_order_relaxed));
            }
            // Console command-list lifecycle (docs/list_lifecycle.md): the title records every frame into lists it
            // created and closed during setup. A forwarded recording call that reaches the real list closed resets it
            // against the allocator it was created with first, which is what the console's allocator-owned recording
            // memory does implicitly. PC recording starts at ClearState (slot 11): the slots before it (GetType,
            // Close, Reset) append no commands, never reopen anything, and keep their exact previous behaviour.
            if constexpr (pc >= 11) {
                if (self->allocator && self->recording_state.load(std::memory_order_relaxed) == 0) {
                    using ResetFn = HRESULT(WINAPI*)(void*, ID3D12CommandAllocator*, ID3D12PipelineState*);
                    const HRESULT reset = reinterpret_cast<ResetFn>(real_vtable[10])(self->real, self->allocator,
                                                                                    nullptr);
                    if (!note_list_reopen(self, Slot, reset)) return hr_to_u64(reset);
                    self->recording_state.store(1, std::memory_order_relaxed);
                }
            }
            UnwrappedArray heaps;
            // Console slot 146 (enhanced barriers): the console records texture barriers with no resource, which the
            // console reads as "every texture" and the PC runtime refuses and records as an invalid list. Translate
            // them to global barriers (translate_console_barrier_groups); a refused call answers 0 (the method is
            // void) and is never forwarded, so it cannot invalidate the list.
            if constexpr (Slot == kConsoleBarrier) {
                if (!translate_console_barrier_groups(a1, a2)) return 0;
            }
            // Console slot 52 (ClearRenderTargetView): a clear of an empty target view is refused rather than forwarded
            // (translate_console_clear); the measured case is a null view the proxy itself created, whose clear is a
            // no-op on the console. A real view is forwarded unchanged with the same handle.
            if constexpr (Slot == 52) {
                if (!translate_console_clear(a1, a2)) return 0;
            }
            if constexpr (Slot == 10) {
                a1 = unwrap_object(a1);
                self->pin_cursor_at_limit();
            } else if constexpr (Slot == kConsoleExecuteBundle) {
                a1 = unwrap_object(a1);
            } else if constexpr (Slot == kConsoleSetDescriptorHeaps) {
                if (!heaps.fill(a2, a1)) return 0;
                if (heaps.data()) a2 = heaps.data();
            }
            const std::uint64_t result = reinterpret_cast<ForwardFn>(real_vtable[pc])(
                self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            if constexpr (Slot == 9) {  // Close: a S_OK leaves the real list closed
                if (list_id) {
                    static std::atomic<unsigned> close_logged{0};
                    if (close_logged.fetch_add(1, std::memory_order_relaxed) < 4000)
                        std::fprintf(stderr, "[rt] d3d12_xs: closeid list %p code 0x%08x\n",
                                     static_cast<void*>(self),
                                     static_cast<unsigned>(static_cast<std::uint32_t>(result)));
                }
                if (SUCCEEDED(static_cast<HRESULT>(static_cast<std::uint32_t>(result))))
                    self->recording_state.store(0, std::memory_order_relaxed);
            } else if constexpr (Slot == 10) {  // Reset: a S_OK starts a recording
                if (SUCCEEDED(static_cast<HRESULT>(static_cast<std::uint32_t>(result))))
                    self->recording_state.store(1, std::memory_order_relaxed);
            }
            return result;
        }
    }
}

template <std::size_t... I>
void fill_list_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&list_slot<static_cast<unsigned>(I)>)), ...);
}

void** get_list_vtable() {
    static void** vtable = [] {
        static void* storage[kListSlots];
        fill_list_vtable(storage, std::make_index_sequence<kListSlots>{});
        return storage;
    }();
    return vtable;
}

HRESULT wrap_list(IUnknown* created, DeviceProxy* device, ID3D12CommandAllocator* allocator, const IID* riid,
                  void** out) {
    if (out) *out = nullptr;
    if (!created) return E_FAIL;
    auto* proxy = new (std::nothrow) ListProxy();
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_list_vtable();
    proxy->real = static_cast<ID3D12GraphicsCommandList*>(created);
    proxy->device = device;
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->pc_slot_count = detect_list_slot_count(created);
    proxy->allocator = allocator;
    proxy->recording_state.store(1, std::memory_order_relaxed);  // CreateCommandList creates it recording
    if (allocator) allocator->AddRef();
    proxy->pin_cursor_at_limit();
    proxy_add_ref(device);

    const HRESULT hr = list_query_interface(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_list(proxy);
        return hr;
    }
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: list proxy %p wraps %p (%u PC vtable slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(created), proxy->pc_slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);
    return S_OK;
}

// The vtable read for a slot past a table faults in the reservation behind it; name the slot. Only reads the
// exception record and writes with WriteFile, so it is safe whatever the faulting thread holds.
template <ChildKind K>
void report_table_overrun(std::uintptr_t address) {
    using S = ChildStats<K>;
    const std::uintptr_t begin = S::table_begin.load(std::memory_order_acquire);
    const std::uintptr_t end = S::table_end.load(std::memory_order_acquire);
    const std::uintptr_t limit = S::region_end.load(std::memory_order_acquire);
    if (!end || address < end || address >= limit) return;
    S::overruns.fetch_add(1, std::memory_order_relaxed);
    char line[160];
    unsigned long long slot = (address - begin) / sizeof(void*);
    char digits[24];
    int d = 0;
    do {
        digits[d++] = static_cast<char>('0' + slot % 10);
        slot /= 10;
    } while (slot && d < 23);
    int n = 0;
    const auto put = [&](const char* text) {
        while (*text && n < static_cast<int>(sizeof(line)) - 1) line[n++] = *text++;
    };
    put("[rt] d3d12_xs: ");
    put(child_kind_name<K>());
    put(" slot ");
    while (d > 0 && n < static_cast<int>(sizeof(line)) - 1) line[n++] = digits[--d];
    put(" is past the proxy table; the vtable read faults\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
}

LONG CALLBACK table_overrun_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
        report_table_overrun<ChildKind::queue>(address);
        report_table_overrun<ChildKind::fence>(address);
        report_table_overrun<ChildKind::allocator>(address);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // diagnostics only: the fault goes on to the title's handlers or the crash
}

template <ChildKind K, unsigned Slot>
std::uint64_t WINAPI child_slot(ChildProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    constexpr CallerKind caller_kind = K == ChildKind::queue   ? CallerKind::queue
                                       : K == ChildKind::fence ? CallerKind::fence
                                                               : CallerKind::allocator;
    if (callers_enabled()) note_caller(caller_kind, Slot, _ReturnAddress(), a1, a2, a3, a4);
    if constexpr (Slot == 0) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_query_interface<K>(self, reinterpret_cast<const IID*>(a1),
                                                  reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return child_add_ref<K>(self);
    } else if constexpr (Slot == 2) {
        return child_release<K>(self);
    } else if constexpr (Slot == 7) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_get_device<K>(self, reinterpret_cast<const IID*>(a1),
                                             reinterpret_cast<void**>(a2)));
    } else if constexpr (K == ChildKind::queue && Slot == kConsoleQueueGetDesc) {
        // The title calls this as (this, buffer): the queue object in RCX and the D3D12_COMMAND_QUEUE_DESC it wants in
        // RDX. Our queue is a C++ object whose GetDesc returns that struct by value, so MSVC wants the hidden return
        // pointer in RCX and the object in RDX - the opposite order. Forwarded through the generic path, the descriptor
        // was written into the front of the real queue object and the title's buffer kept whatever it held, which the
        // title then read as live values (measured: a clobbered callee-saved register and a fault on 0x100000000).
        note_child_slot<K>(Slot);
        d3d12_trace::record(child_trace_name<K>(Slot));
        using GetDescFn = void(WINAPI*)(void*, void*);
        auto* const method = reinterpret_cast<GetDescFn>(self->real_vtable[kQueueGetDesc]);
        method(reinterpret_cast<void*>(a1), self->real);
        return 0;
    } else if constexpr (K == ChildKind::queue && (Slot == 14 || Slot == 15)) {
        // The console's two methods inserted before Signal; their shapes are not identified, and forwarding them by
        // number reached PC Signal/Wait, which is wrong by construction. Refusing keeps the call honest and logged.
        note_child_slot<K>(Slot);
        static std::atomic<bool> inserted_logged[2] = {};
        if (!inserted_logged[Slot - 14].exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: queue slot %u is one of the console's two methods before Signal; refusing "
                         "(args 0x%llx 0x%llx 0x%llx 0x%llx)\n",
                         Slot, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                         static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
        d3d12_trace::record(child_trace_name<K>(Slot), "unsupported");
        return hr_to_u64(E_NOTIMPL);
} else if constexpr (K == ChildKind::queue && Slot == 32) {
        // Console PresentX, past the PC queue table (docs/d3d12_xs_surface.md 7.1: (plane count, planes, params), plane
        // entries 0x48 bytes with Token @0x00, ResourceCount @0x08, ppResources @0x10, ...). The first calls are dumped
        // once so the real shape decides the wiring instead of the public layouts alone. PALMETTO_PRESENT_STUB=1 answers
        // S_OK without presenting - a measurement of whether present success changes the title's loop, never a fix.
        note_child_slot<K>(Slot);
        static std::atomic<std::uint64_t> presents{0};
        const std::uint64_t present_number = presents.fetch_add(1, std::memory_order_relaxed) + 1;
        // Sampled, not one-shot: whether the plane ever carries a real resource decides the next thread.
        const bool sampled = present_number <= 2 || present_number == 1000 || present_number == 10000 ||
                             present_number == 100000 || present_number == 1000000;
        if (sampled) {
            std::fprintf(stderr,
                         "[rt] d3d12_xs: PresentX call %llu: planes %llu at %p, params %p\n",
                         static_cast<unsigned long long>(present_number), static_cast<unsigned long long>(a1),
                         reinterpret_cast<void*>(a2), reinterpret_cast<void*>(a3));
            MEMORY_BASIC_INFORMATION mbi{};
            const bool readable = a2 && VirtualQuery(reinterpret_cast<const void*>(a2), &mbi, sizeof(mbi)) ==
                                               sizeof(mbi) &&
                                  mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (readable) {
                const auto* plane_words = reinterpret_cast<const std::uint64_t*>(a2);
                const auto* plane_bytes = reinterpret_cast<const std::uint8_t*>(a2);
                std::uint32_t resource_count = 0;
                std::uint64_t resources = 0;
                std::memcpy(&resource_count, plane_bytes + 0x08, sizeof(resource_count));
                std::memcpy(&resources, plane_bytes + 0x10, sizeof(resources));
                std::fprintf(stderr,
                             "[rt]   plane 0 (call %llu): token 0x%llx resources %u at %p, colour space %d scale "
                             "filter %d flags 0x%x\n",
                             static_cast<unsigned long long>(present_number),
                             static_cast<unsigned long long>(plane_words[0]), resource_count,
                             reinterpret_cast<void*>(resources),
                             *reinterpret_cast<const int*>(plane_bytes + 0x28),
                             *reinterpret_cast<const int*>(plane_bytes + 0x2C),
                             *reinterpret_cast<const unsigned*>(plane_bytes + 0x40));
                MEMORY_BASIC_INFORMATION list_mbi{};
                const bool list_readable =
                    resources && VirtualQuery(reinterpret_cast<const void*>(resources), &list_mbi, sizeof(list_mbi)) ==
                                     sizeof(list_mbi) &&
                    list_mbi.State == MEM_COMMIT && !(list_mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
                if (list_readable && resource_count)
                    std::fprintf(stderr, "[rt]   plane 0 resource 0 (call %llu): %p\n",
                                 static_cast<unsigned long long>(present_number),
                                 reinterpret_cast<void*>(reinterpret_cast<const std::uint64_t*>(resources)[0]));
            }
        }
        static const bool stub = GetEnvironmentVariableA("PALMETTO_PRESENT_STUB", nullptr, 0) != 0;
        if (stub) {
            static std::atomic<std::uint64_t> stubbed{0};
            const std::uint64_t n = stubbed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 3)
                std::fprintf(stderr,
                             "[rt] d3d12_xs: PresentX call %llu answered S_OK by PALMETTO_PRESENT_STUB (nothing "
                             "presented)\n",
                             static_cast<unsigned long long>(n));
            d3d12_trace::record(child_trace_name<K>(Slot));
            return hr_to_u64(S_OK);
        }
        // docs/present_wiring.md: console PresentX through the presentation the Vulkan backend registered (the
        // palmetto_register_* hook above; the registration carries the present-to-queue call and the frame-token bridge).
        // With nothing registered - a GPU-free test binary, or a boot before the backend registered - the hook answers
        // HRESULT_FROM_WIN32(ERROR_INVALID_STATE) and the call stays an honest refusal.
        const HRESULT present_result = rt::d3d12::xbox_present_queue_present(
            self->real, static_cast<std::uint32_t>(a1),
            reinterpret_cast<const rt::d3d12::XboxPresentPlaneParameters*>(a2),
            reinterpret_cast<const rt::d3d12::XboxPresentParameters*>(a3));
        d3d12_trace::record(child_trace_name<K>(Slot), SUCCEEDED(present_result) ? "ok" : "failed");
        return hr_to_u64(present_result);
    } else if constexpr (K == ChildKind::queue && Slot == 35) {
        // The console's submit. Measured (docs/render_loop.md): the title's frame function calls it as
        // (count, command-list array, frame token, reserved), with the array being the same storage the startup filled
        // with command lists; there is no ExecuteCommandLists call anywhere in the boot. The earlier answer here - a
        // "clock calibration pair" written through a2 - was wrong and overwrote the title's own list pointers with two
        // QPC words, which is why the frame loop never submitted anything.
        note_child_slot<K>(Slot);
        UnwrappedArray lists;
        if (!lists.fill(a2, a1)) return hr_to_u64(E_OUTOFMEMORY);
        const std::uint64_t array = lists.data() ? lists.data() : a2;
        using ExecuteFn = void(WINAPI*)(void*, std::uint64_t, std::uint64_t);
        reinterpret_cast<ExecuteFn>(self->real_vtable[10])(self->real, a1, array);  // PC ExecuteCommandLists
        static std::atomic<std::uint64_t> calls{0};
        const std::uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call <= 3)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: queue slot 35 submitted %llu command list(s) from %p (frame token 0x%llx)\n",
                         static_cast<unsigned long long>(a1), reinterpret_cast<void*>(array),
                         static_cast<unsigned long long>(a3));
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(S_OK);
    } else if constexpr (K == ChildKind::fence && Slot == kConsoleFenceValueAddress) {
        // Console fence slot 12. The measured caller is the title's wait loop (docs/fence_slot12.md): it calls
        // (fence, target, &continuation, flags), keeps the answer and polls [answer] with monitorx/mwaitx until the
        // word there reaches the target, treating UINT64_MAX as a removed device. The console keeps a fence's
        // completed value in CPU-visible memory and this is its address, so the answer is the proxy's value cell.
        note_child_slot<K>(Slot);
        d3d12_trace::record(child_trace_name<K>(Slot));
        if (!self->value_cell) return hr_to_u64(E_OUTOFMEMORY);  // the cell is made with the proxy
        refresh_fence_value(self);
        const std::uint64_t completed = self->value_cell->load(std::memory_order_acquire);
        watch_fence_value(self, a1, completed);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: fence slot %u answered as the address of the completed-value cell "
                         "(target 0x%llx, completed 0x%llx, flags 0x%llx)\n",
                         Slot, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(completed),
                         static_cast<unsigned long long>(a3 & 0xff));
        return reinterpret_cast<std::uint64_t>(self->value_cell.get());
    } else {
        note_child_slot<K>(Slot);
        // Console order: slots from Signal on name the PC method two below them (heap and allocator proxies keep their
        // own numbering, so only the queue maps).
        constexpr unsigned real_slot =
            (K == ChildKind::queue && Slot >= kConsoleQueueSignal && Slot <= kConsoleQueueGetDesc)
                ? Slot - kConsoleQueueShift
                : Slot;
        if (real_slot >= self->slot_count) {
            bool expected = false;
            if (ChildStats<K>::beyond_warned[Slot].compare_exchange_strong(expected, true,
                                                                            std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "[rt] d3d12_xs: %s slot %u is beyond the PC %s vtable (%u slots); returning E_NOTIMPL "
                             "(args 0x%llx 0x%llx 0x%llx 0x%llx)\n",
                             child_kind_name<K>(), Slot, child_kind_name<K>(), self->slot_count,
                             static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                             static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
            }
            d3d12_trace::record(child_trace_name<K>(Slot), "unsupported");
            return hr_to_u64(E_NOTIMPL);
        }
        d3d12_trace::record(child_trace_name<K>(Slot));  // "called": the real object's result is not inspected
        [[maybe_unused]] const std::uint64_t fence_argument = a1;  // still the title's proxy when a fence is handed on
        if constexpr (K == ChildKind::queue && (real_slot == kQueueSignal || real_slot == kQueueWait))
            a1 = unwrap_object(a1);
        UnwrappedArray unwrapped_lists;
        if constexpr (K == ChildKind::queue && Slot == 10) {
            if (!unwrapped_lists.fill(a2, a1)) return hr_to_u64(E_OUTOFMEMORY);
            if (unwrapped_lists.data()) a2 = unwrapped_lists.data();
        }

        // The PC vtable entry with the real object as `this`; integer/pointer arguments, the hidden struct-return
        // pointer and stack arguments keep their positions, as in the device proxy.
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        // The console slot names this PC method, not the one at the same index.
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[real_slot]);
        const std::uint64_t result = fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
        // Keep a fence's slot-12 cell in step with value changes the title causes through the proxies: Signal moves
        // the value, GetCompletedValue is the value, and a queue Signal on a fence proxy is the console path that
        // signals it (docs/fence_slot12.md).
        if constexpr (K == ChildKind::queue) {
            if (Slot == kConsoleQueueSignal) {
                const auto* fence_proxy =
                    reinterpret_cast<const ChildProxy*>(static_cast<std::uintptr_t>(fence_argument));
                if (is_fence_proxy(fence_proxy)) refresh_fence_value(const_cast<ChildProxy*>(fence_proxy));
            }
        } else if constexpr (K == ChildKind::fence) {
            if (real_slot == 8) publish_fence_value(self->value_cell, result);
            else if (real_slot == 10) refresh_fence_value(self);
        }
        return result;
    }
}

template <ChildKind K, std::size_t... I>
void fill_child_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&child_slot<K, static_cast<unsigned>(I)>)), ...);
}

// True when the fault logger is installed (once per process; a failure is logged once).
bool install_table_overrun_handler() {
    static const bool installed = [] {
        if (AddVectoredExceptionHandler(0, &table_overrun_handler)) return true;
        std::fprintf(stderr, "[rt] d3d12_xs: AddVectoredExceptionHandler failed (error %lu); a slot past a queue or "
                     "fence proxy table faults without being named\n", GetLastError());
        return false;
    }();
    return installed;
}

template <ChildKind K>
void** get_child_vtable() {
    static void** vtable = [] {
        using S = ChildStats<K>;
        constexpr std::size_t bytes = S::kSlots * sizeof(void*);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const SIZE_T page = system.dwPageSize;
        if (void* base = page >= bytes ? VirtualAlloc(nullptr, kTableRegion, MEM_RESERVE, PAGE_NOACCESS) : nullptr) {
            if (VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE)) {
                auto* table = reinterpret_cast<void**>(static_cast<char*>(base) + page - bytes);
                fill_child_vtable<K>(table, std::make_index_sequence<S::kSlots>{});
                DWORD old = 0;
                if (!VirtualProtect(base, page, PAGE_READONLY, &old))  // the table works, but stays writable
                    std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table stays writable (VirtualProtect error %lu)\n",
                                 child_kind_name<K>(), GetLastError());
                S::table_begin.store(reinterpret_cast<std::uintptr_t>(table), std::memory_order_release);
                S::region_end.store(reinterpret_cast<std::uintptr_t>(base) + kTableRegion, std::memory_order_release);
                // table_end is what "guarded" means (command_object_tables_guarded, the fault logger): set only when
                // a fault past the table will be named. Without the logger the region still faults, unnamed.
                if (install_table_overrun_handler())
                    S::table_end.store(reinterpret_cast<std::uintptr_t>(base) + page, std::memory_order_release);
                return table;
            }
            VirtualFree(base, 0, MEM_RELEASE);
        }
        // No reservation: the table still works, but a slot past it is not caught.
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table without a guard region\n", child_kind_name<K>());
        static void* storage[S::kSlots];
        fill_child_vtable<K>(storage, std::make_index_sequence<S::kSlots>{});
        return storage;
    }();
    return vtable;
}

// Wraps `created` (one reference, owned by the proxy on success, released on failure) and returns `riid` from the
// proxy through `out`.
template <ChildKind K>
HRESULT wrap_child(IUnknown* created, DeviceProxy* device, const IID* riid, void** out) {
    if (!created) return E_FAIL;
    ChildProxy* proxy = new (std::nothrow) ChildProxy;
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_child_vtable<K>();
    proxy->real = created;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(created);
    if constexpr (K == ChildKind::queue) {
        proxy->slot_count = kQueuePublicSlots;
    } else if constexpr (K == ChildKind::fence) {
        proxy->slot_count = supports_iid(created, __uuidof(ID3D12Fence1)) ? kFence1PublicSlots : kFencePublicSlots;
        // The cell console fence slot 12 answers with (docs/fence_slot12.md); its address is fixed for the proxy.
        try {
            proxy->value_cell = std::make_shared<std::atomic<std::uint64_t>>(std::uint64_t{0});
        } catch (...) {
            created->Release();
            delete proxy;
            return E_OUTOFMEMORY;
        }
    } else {
        proxy->slot_count = kAllocatorPublicSlots;
    }
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy_add_ref(device);  // the object keeps its Xbox-ABI device alive for GetDevice
    if constexpr (K == ChildKind::fence) refresh_fence_value(proxy);  // the cell starts at the initial value

    const HRESULT hr = child_query_interface<K>(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_child(proxy);  // no reference escaped
        return hr;
    }
    // Titles create fences at run time: the first proxy of each kind is logged, every one only with XS_TRACE.
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy %p wraps %p (device proxy %p, %u public slots)\n",
                     child_kind_name<K>(), static_cast<void*>(proxy), static_cast<void*>(created),
                     static_cast<void*>(device), proxy->slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);  // drop the creation reference; `out` holds the caller's
    return S_OK;
}

// Calls the real device's slot `Slot` with these five words in the positions the thunk received them (see "A thunk
// declares 14 argument words" at the top: no word may move to another stack position on the way).
template <unsigned Slot>
std::uint64_t forward_device(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                             std::uint64_t a4, std::uint64_t a5) {
    using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t);
    return reinterpret_cast<Fn>(self->real_vtable[Slot])(self->real, a1, a2, a3, a4, a5);
}

// A device slot past the PC device's vtable (the real device predates the interface): E_NOTIMPL, logged once per
// slot. The generic thunk and the slots the proxy handles itself (31/34/35/45/46/50, 75) log the same line.
HRESULT refuse_beyond_device(unsigned slot, unsigned slot_count) {
    // The console device vtable is longer than any PC one (the retail title reaches slots 81, 86 and 127; 127 is a
    // resource creation the proxy now models, 81 and 86 are answered here). Those slots are console extensions with
    // no PC counterpart, and refusing them aborts the title.
    // Log the slot and answer success with a zero result, the same policy the frame-event slots already use: a boot
    // must not abort on an extension the runtime does not model, and the log names every one that was stubbed.
    bool expected = false;
    if (slot < kMaxSlots && g_beyond_warned[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot %u (%s) is beyond the PC device vtable (%u slots); stubbed with "
                     "S_OK / 0\n",
                     slot, slot_name(slot), slot_count);
    return S_OK;
}

// Device slot 8: CreateCommandQueue(desc, riid, out). Without an output it is the device's own capability answer
// (S_FALSE for a valid description on the PC), as #558 does for slot 14.
HRESULT proxy_create_command_queue(DeviceProxy* self, const D3D12_COMMAND_QUEUE_DESC* desc, const IID* riid,
                                   void** out) {
    note_slot(8);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as slots 14/62/76
    if (!riid) return E_POINTER;
    // The caller's null output goes to the real device on purpose: its answer (S_FALSE, E_UNEXPECTED, ...) is the
    // one to give, although the SDK annotates the parameter as required.
#pragma warning(suppress : 6387)
    if (!out) return self->real->CreateCommandQueue(desc, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandQueue(desc, __uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out);
}

// Xbox device slot 78 (public HLE): SetFrameIntervalX(output, interval_us, latency, flags). The output and flags are
// not used before a presenter exists. The period is a vblank period in microseconds in the public samples (16667 for
// 60 Hz), and latency is the allowed frames in flight. S_OK even for 0: a boot must not abort here, and nothing paces.
HRESULT proxy_set_frame_interval_x(DeviceProxy* self, std::uint64_t output, std::uint64_t interval_us,
                                   std::uint64_t latency, std::uint64_t flags) {
    (void)output;
    (void)flags;
    self->frame_interval_us.store(static_cast<std::uint32_t>(interval_us), std::memory_order_relaxed);
    self->frame_latency.store(static_cast<std::uint32_t>(latency ? latency : 1), std::memory_order_relaxed);
    return S_OK;
}

// Xbox device slot 79 (public HLE): ScheduleFrameEventX(event, offset_us, pEvent, flags); our pacer has only the origin.
HRESULT proxy_schedule_frame_event_x(DeviceProxy* self, std::uint64_t event, std::uint64_t offset_us,
                                     std::uint64_t flags) {
    (void)event;
    (void)offset_us;
    (void)flags;
    self->frame_event_scheduled.store(true, std::memory_order_relaxed);
    return S_OK;
}

// Xbox device slot 80 (public HLE): WaitFrameEventX(event, timeout_ms, pEvent, flags, &token). It returns at once with
// a fresh non-zero token (the first-frame choice): there is no vblank source behind the device before a presenter, and
// a zero wait cannot stall the title. A null token pointer is E_POINTER, as a _Out_ parameter.
HRESULT proxy_wait_frame_event_x(DeviceProxy* self, std::uint64_t event, std::uint64_t timeout_ms,
                                 std::uint64_t flags, std::uint64_t token_word) {
    (void)event;
    (void)timeout_ms;
    (void)flags;
    auto* token = reinterpret_cast<std::uint64_t*>(token_word);
    if (!token) return E_POINTER;
    std::uint64_t value = self->frame_next_token.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) value = self->frame_next_token.fetch_add(1, std::memory_order_relaxed);  // skip the null token
    *token = value;
    // docs/present_wiring.md: the frame-token bridge's report side. The presentation's pacer has not issued this token,
    // so it would refuse it as "never handed out"; reporting it before the title can present it makes it presentable.
    // A no-op until a presentation is registered.
    rt::d3d12::xbox_present_note_issued_token(value);
    return S_OK;
}

// Xbox device slot 73 (public HLE): CreateCommandListX(desc, allocator, initial_state, riid, out). The console call is
// not the PC six-argument form: the type is the first word of D3D12XBOX_COMMAND_LIST_DESC and there is no node mask
// (the HLE passes 0). The second desc word is logged, not interpreted: its public layout is not established.
HRESULT proxy_create_command_list_x(DeviceProxy* self, std::uint64_t desc_word, std::uint64_t allocator_word,
                                    std::uint64_t initial_state_word, const IID* riid, void** out) {
    if (out) *out = nullptr;
    if (!desc_word || !allocator_word) return E_INVALIDARG;
    if (!riid) return E_POINTER;
    if (!out) return E_POINTER;

    std::uint32_t words[2] = {};
    std::memcpy(words, reinterpret_cast<const void*>(desc_word), sizeof(words));
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY)
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;  // as the public HLE does

    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 73 CreateCommandListX desc words 0x%08x 0x%08x -> node mask 0, type %d\n",
                     words[0], words[1], static_cast<int>(type));

    // Palmetto wraps its Vulkan allocators; a PC allocator comes back unwrapped and is passed as it is. Pipeline states
    // are not wrapped by this proxy, so initial_state is already the real object (a proxy there is passed through too).
    auto* allocator = allocator_proxy_real_allocator(reinterpret_cast<void*>(allocator_word));
    if (!allocator) allocator = reinterpret_cast<ID3D12CommandAllocator*>(allocator_word);
    auto* initial_state = reinterpret_cast<ID3D12PipelineState*>(initial_state_word);
    void* created = nullptr;
    // Ask the PC runtime for the base list interface, which it always implements, and let the console-ABI proxy answer
    // the IID the title asked for. A PC IID goes through unchanged.
    const IID pc_iid = IsEqualIID(*riid, kConsoleGraphicsCommandListIid) ? __uuidof(ID3D12GraphicsCommandList) : *riid;
    const HRESULT hr = self->real->CreateCommandList(0, type, allocator, initial_state, pc_iid, &created);
    if (FAILED(hr)) return hr;
    return wrap_list(static_cast<IUnknown*>(created), self, allocator, riid, out);
}

// Device slot 75: ID3D12Device9::CreateCommandQueue1(desc, creator_id, riid, out), called by slot index.
HRESULT proxy_create_command_queue1(DeviceProxy* self, std::uint64_t desc, std::uint64_t creator_id,
                                    const IID* riid, void** out) {
    note_slot(75);
    if (out) *out = nullptr;  // as slot 8
    if (self->slot_count <= 75) return refuse_beyond_device(75, self->slot_count);  // predates ID3D12Device9
    if (!riid) return E_POINTER;
    using Fn = HRESULT(WINAPI*)(ID3D12Device*, std::uint64_t, std::uint64_t, const IID*, void**);
    const auto create = reinterpret_cast<Fn>(self->real_vtable[75]);
    if (!out) return create(self->real, desc, creator_id, riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = create(self->real, desc, creator_id, &__uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out);
}

// Device slot 36: CreateFence(initial_value, flags, riid, out).
HRESULT proxy_create_fence(DeviceProxy* self, std::uint64_t initial_value, std::uint64_t flags, const IID* riid,
                           void** out) {
    note_slot(36);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto fence_flags = static_cast<D3D12_FENCE_FLAGS>(static_cast<std::uint32_t>(flags));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateFence(initial_value, fence_flags, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateFence(initial_value, fence_flags, __uuidof(ID3D12Fence), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(created), self, riid, out);
}

// True when `allocator` is one whose consumers resolve allocator proxies: Palmetto's own Vulkan-layer allocator,
// whose lists (CreateCommandList, Reset) find the real allocator through allocator_proxy_real_allocator. A PC
// runtime allocator does not answer the tag, and its lists would take a proxy for their own allocator.
bool resolves_allocator_proxies(IUnknown* allocator) {
    void* found = nullptr;
    if (FAILED(allocator->QueryInterface(kPalmettoCommandAllocatorTag, &found)) || !found) return false;
    static_cast<IUnknown*>(found)->Release();  // the implementation pointer starts with its IUnknown chain
    return true;
}

// Device slot 9: CreateCommandAllocator(type, riid, out). An allocator proxy where the list side resolves it (see
// resolves_allocator_proxies); any other allocator goes back unchanged, logged once, so the host runtime's lists
// never see a proxy. Null output and null IID as slot 8.
HRESULT proxy_create_command_allocator(DeviceProxy* self, std::uint64_t type, const IID* riid, void** out) {
    note_slot(9);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto list_type = static_cast<D3D12_COMMAND_LIST_TYPE>(static_cast<std::uint32_t>(type));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateCommandAllocator(list_type, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandAllocator(list_type, __uuidof(ID3D12CommandAllocator), &created);
    if (FAILED(hr)) return hr;
    if (!created) return E_FAIL;
    auto* allocator = static_cast<IUnknown*>(created);
    if (!resolves_allocator_proxies(allocator)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: allocators of this device are not wrapped: its command lists "
                                 "cannot resolve an allocator proxy\n");
        const HRESULT asked = allocator->QueryInterface(*riid, out);
        allocator->Release();
        return asked;
    }
    return wrap_child<ChildKind::allocator>(allocator, self, riid, out);
}

// Device slot 32: OpenSharedHandle(handle, riid, out). The object is opened as asked; when it is a fence (whatever
// interface was asked for, IUnknown included) the title gets a fence proxy, anything else (heaps, resources) is
// returned unchanged.
HRESULT proxy_open_shared_handle(DeviceProxy* self, std::uint64_t handle, const IID* riid, void** out) {
    note_slot(32);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto shared = reinterpret_cast<HANDLE>(handle);
    if (!out) return self->real->OpenSharedHandle(shared, *riid, nullptr);
    void* opened = nullptr;
    HRESULT hr = self->real->OpenSharedHandle(shared, *riid, &opened);
    if (FAILED(hr) || !opened) return FAILED(hr) ? hr : E_FAIL;
    void* fence = nullptr;
    if (FAILED(static_cast<IUnknown*>(opened)->QueryInterface(__uuidof(ID3D12Fence), &fence)) || !fence) {
        *out = opened;  // not a fence
        return hr;
    }
    static_cast<IUnknown*>(opened)->Release();
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(fence), self, riid, out);
}

// Device slots that take heap, queue or fence objects: the real runtime gets the real objects.
//   31 CreateSharedHandle(object, attributes, access, name, handle)
//   34 MakeResident(count, objects)      35 Evict(count, objects)
//   45 SetEventOnMultipleFenceCompletion(fences, values, count, flags, event)            (ID3D12Device1)
//   46 SetResidencyPriority(count, objects, priorities)                                  (ID3D12Device1)
//   50 EnqueueMakeResident(flags, count, objects, fence, value)                          (ID3D12Device3)
template <unsigned Slot>
HRESULT proxy_unwrapping_call(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                              std::uint64_t a4, std::uint64_t a5) {
    note_slot(Slot);
    if (Slot >= self->slot_count) return refuse_beyond_device(Slot, self->slot_count);  // predates the interface
    UnwrappedArray objects;
    std::uint64_t r1 = a1, r2 = a2, r3 = a3, r4 = a4;
    if constexpr (Slot == 31) {
        r1 = unwrap_object(a1);
    } else if constexpr (Slot == 34 || Slot == 35 || Slot == 46) {
        if (!objects.fill(a2, a1)) return E_OUTOFMEMORY;
        if (objects.data()) r2 = objects.data();
    } else if constexpr (Slot == 45) {
        if (!objects.fill(a1, a3)) return E_OUTOFMEMORY;
        if (objects.data()) r1 = objects.data();
    } else if constexpr (Slot == 50) {
        if (!objects.fill(a3, a2)) return E_OUTOFMEMORY;
        if (objects.data()) r3 = objects.data();
        r4 = unwrap_object(a4);
    }
    // r1-r4 differ from a1-a4 only in a position the method takes, so the title's frame has that word.
    return static_cast<HRESULT>(static_cast<std::uint32_t>(forward_device<Slot>(self, r1, r2, r3, r4, a5)));
}

static bool readable_range(const void* ptr, std::size_t bytes) {
    if (!ptr || !bytes) return false;
    const auto* p = static_cast<const std::uint8_t*>(ptr);
    std::size_t remaining = bytes;
    while (remaining) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        const DWORD protect = mbi.Protect & 0xffu;
    if (mbi.Protect & PAGE_GUARD) return false;  // a guard page faults on touch; masking it away hid that
        if (protect == PAGE_NOACCESS || protect == PAGE_GUARD || protect == PAGE_EXECUTE) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto addr = reinterpret_cast<std::uintptr_t>(p);
        const std::size_t avail = static_cast<std::size_t>(base + mbi.RegionSize - addr);
        if (avail >= remaining) return true;
        remaining -= avail;
        p += avail;
    }
    return true;
}

// ---- Console device slot 55: CreateDerivedPipelineStateX ------------------------------------------------------------
//
// The console device vtable is the PC one only up to slot 43; from 44 on it is the console's own extension table, so a
// console slot number there is not a PC method number. Slot 55 is one of the console's pipeline-state extensions:
// CreateDerivedPipelineStateX(base, count, patches, riid, out), "the base pipeline with these state parts replaced".
// PC slot 55 is ID3D12Device4::CreateReservedResource1(desc, state, clear, session, riid, out), which reads the same
// five words in unrelated positions. The measured call is the console shape and not the PC one:
// game+0x247A5C2 passes five arguments, IID_ID3D12PipelineState as the fourth and the out pointer as the fifth
// (docs/display_path.md section 4; docs/device_slot55.md). Its neighbours in the same console table are the measured
// #57 (a deserialized graphics record, docs/slot57_wiring.md), #62 (create compute) and #67 (deserialize a compute
// record); the public HLE's device table numbers the same group the same way and gives the patch record the console's
// layout: a u32 part at +0 and the part's payload at +4, one record of 0x14C bytes. The payloads are the PC structures
// themselves - part 0 a D3D12_DEPTH_STENCIL_DESC (0x34 B), 1 a D3D12_RASTERIZER_DESC (0x2C), 2 a D3D12_BLEND_DESC
// (0x148), 3 a UINT sample mask - and those three sizes are the PC structures' own: the second, independent
// confirmation of the shape.
//
// Until this branch existed, console #55 fell through to the generic forward: the PC device read the console's base
// pointer as a resource description and the riid as a protected session, refused every call (41,266 times in a 90 s
// boot, docs/display_path.md), and the title - which does not test that HRESULT - kept a null pipeline state.
//
// A PC device has no "derive a pipeline from another pipeline", so the proxy rebuilds it: the description of every
// graphics pipeline this device creates is remembered (slot 57's converted record), a derivation copies that
// description, replaces the parts the call patches and creates the result with the PC device's own
// CreateGraphicsPipelineState, the title's riid and out pointer, and the real HRESULT back. A base this device did not
// create has no description to rebuild from, so it is refused with a named log (E_NOTIMPL) rather than guessed from a
// pointer - the same proof policy slot 57 applies to the root signatures its records name.

// The bytecode, input elements and semantic names a remembered description points at, owned here so a later derivation
// never reads title memory that may be gone. One storage is shared by the base pipeline and every pipeline derived
// from it.
struct TrackedPipelineStorage {
    std::vector<std::uint8_t> ps, vs, ds, hs, gs;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string> semantics;
};

// A graphics pipeline state this device created, and what it was built from.
struct TrackedPipeline {
    bool graphics = false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::shared_ptr<TrackedPipelineStorage> storage;
};

// The count a boot needs is slot 57's distinct records plus one entry per derivation. A table that overflows refuses a
// later base as untracked (logged once and counted) instead of evicting a description a live derivation may name.
constexpr std::size_t kTrackedPipelines = 65536;
std::mutex g_tracked_pipeline_mutex;
std::map<void*, TrackedPipeline> g_tracked_pipelines;
std::atomic<bool> g_tracked_pipeline_overflow{false};

// Deep copy of one stage bytecode into storage owned by the table.
void own_stage_bytes(std::vector<std::uint8_t>& storage, D3D12_SHADER_BYTECODE& bytes, bool* readable) {
    if (!bytes.pShaderBytecode || !bytes.BytecodeLength) {
        bytes.pShaderBytecode = nullptr;
        bytes.BytecodeLength = 0;
        return;
    }
    const auto* source = static_cast<const std::uint8_t*>(bytes.pShaderBytecode);
    if (!readable_range(source, bytes.BytecodeLength)) {
        *readable = false;
        return;
    }
    storage.assign(source, source + bytes.BytecodeLength);
    bytes.pShaderBytecode = storage.data();
}

// Remembers what a graphics pipeline this device created was built from, so console slot 55 can derive from it. The
// description is copied field by field's worth of storage: every pointer it carries into title memory (stage bytecode,
// the input-element array and its semantic names) is replaced by a copy owned here. An unreadable pointer leaves the
// pipeline untracked - a derivation from it is then refused by name, never built from a stale pointer.
void track_graphics_pipeline_state(ID3D12PipelineState* pipeline, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    if (!pipeline) return;
    auto storage = std::make_shared<TrackedPipelineStorage>();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC copy = desc;
    bool readable = true;
    own_stage_bytes(storage->ps, copy.PS, &readable);
    own_stage_bytes(storage->vs, copy.VS, &readable);
    own_stage_bytes(storage->ds, copy.DS, &readable);
    own_stage_bytes(storage->hs, copy.HS, &readable);
    own_stage_bytes(storage->gs, copy.GS, &readable);
    if (copy.InputLayout.NumElements && copy.InputLayout.pInputElementDescs) {
        const std::size_t bytes =
            static_cast<std::size_t>(copy.InputLayout.NumElements) * sizeof(D3D12_INPUT_ELEMENT_DESC);
        if (!readable_range(copy.InputLayout.pInputElementDescs, bytes)) {
            readable = false;
        } else {
            storage->elements.assign(copy.InputLayout.pInputElementDescs,
                                     copy.InputLayout.pInputElementDescs + copy.InputLayout.NumElements);
            // Reserve first: a reallocation would move the std::string objects and invalidate the c_str() pointers
            // just stored in the element array.
            storage->semantics.reserve(storage->elements.size());
            for (auto& element : storage->elements) {
                if (element.SemanticName && readable_range(element.SemanticName, 1)) {
                    std::size_t length = 0;
                    while (length < 256 && readable_range(element.SemanticName + length, 1) &&
                           element.SemanticName[length] != '\0')
                        ++length;
                    storage->semantics.emplace_back(element.SemanticName, length);
                    element.SemanticName = storage->semantics.back().c_str();
                } else {
                    element.SemanticName = "";
                }
            }
            copy.InputLayout.pInputElementDescs = storage->elements.data();
            copy.InputLayout.NumElements = static_cast<UINT>(storage->elements.size());
        }
    } else {
        copy.InputLayout.pInputElementDescs = nullptr;
        copy.InputLayout.NumElements = 0;
    }
    // build() never fills the stream-output declaration (runtime/src/graphics_pso.h), and a description copied with it
    // would carry pointers into title memory; there is nothing to copy, so it stays empty.
    copy.StreamOutput = D3D12_STREAM_OUTPUT_DESC{};
    if (!readable) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: a created graphics pipeline's description pointed at unreadable bytes; it is "
                         "not tracked, so a console slot 55 derivation from it is refused as an untracked base\n");
        return;
    }
    const std::lock_guard lock(g_tracked_pipeline_mutex);
    if (g_tracked_pipelines.size() >= kTrackedPipelines) {
        if (!g_tracked_pipeline_overflow.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: %zu graphics pipeline descriptions tracked; a later console slot 55 base is "
                         "refused as untracked\n",
                         kTrackedPipelines);
        return;
    }
    TrackedPipeline entry;
    entry.graphics = true;
    entry.desc = copy;
    entry.storage = std::move(storage);
    // A pipeline pointer that comes back for a released object replaces the stale entry; a derived pipeline keeps its
    // own reference to the storage it points at, so replacing this entry never invalidates another description.
    g_tracked_pipelines[static_cast<void*>(pipeline)] = std::move(entry);
}

// The description a pipeline state this device created was built from. False for anything the device did not hand out.
bool tracked_pipeline_state(const void* pipeline, TrackedPipeline* out) {
    if (!pipeline) return false;
    const std::lock_guard lock(g_tracked_pipeline_mutex);
    const auto it = g_tracked_pipelines.find(const_cast<void*>(pipeline));
    if (it == g_tracked_pipelines.end()) return false;
    *out = it->second;
    return true;
}

// The console patch record: the part word, then the part's payload, one 0x14C-byte record per part (the largest
// payload, D3D12_BLEND_DESC, is 0x148 bytes).
constexpr std::size_t kXboxPatchStride = 0x14C;
constexpr std::uint64_t kMaxXboxPatches = 64;
constexpr std::uint64_t kSlot55SummaryInterval = 5000;

struct Slot55Stats {
    std::mutex mutex;
    std::uint64_t calls = 0;
    std::uint64_t base_tracked = 0;
    std::uint64_t base_untracked = 0;
    std::uint64_t base_compute = 0;
    std::uint64_t base_null = 0;
    std::uint64_t reached_create = 0;
    std::uint64_t created = 0;
    std::map<std::uint32_t, std::uint64_t> parts;    // part word -> calls that carried it
    std::map<std::uint32_t, std::uint64_t> results;  // PC HRESULT -> calls
    std::set<const void*> bases;                     // the distinct base pipeline states the boot names
    std::set<std::string> reasons_logged;
    std::set<std::uint32_t> results_logged;
    unsigned summaries = 0;
};

Slot55Stats& slot55_stats() {
    static Slot55Stats stats;
    return stats;
}

void print_slot55_summary_locked(Slot55Stats& stats) {
    ++stats.summaries;
    const std::uint64_t refused_before = stats.calls - stats.reached_create;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: slot 55 histogram #%u at %llu call(s), %zu distinct base(s): base this device created "
                 "%llu, untracked %llu, a compute pipeline %llu, null %llu; reached PC CreateGraphicsPipelineState %llu, "
                 "created %llu, refused before the call %llu\n",
                 stats.summaries, static_cast<unsigned long long>(stats.calls), stats.bases.size(),
                 static_cast<unsigned long long>(stats.base_tracked),
                 static_cast<unsigned long long>(stats.base_untracked),
                 static_cast<unsigned long long>(stats.base_compute),
                 static_cast<unsigned long long>(stats.base_null),
                 static_cast<unsigned long long>(stats.reached_create),
                 static_cast<unsigned long long>(stats.created),
                 static_cast<unsigned long long>(refused_before));
    std::string parts;
    for (const auto& entry : stats.parts) {
        if (!parts.empty()) parts += ", ";
        parts += std::to_string(entry.first) + " " + std::to_string(entry.second);
    }
    std::fprintf(stderr, "[rt] d3d12_xs: slot 55   patch parts (0 depth-stencil, 1 rasterizer, 2 blend, 3 sample mask): "
                         "%s\n",
                 parts.empty() ? "none" : parts.c_str());
    if (stats.results.empty()) {
        std::fprintf(stderr, "[rt] d3d12_xs: slot 55   PC CreateGraphicsPipelineState results: none reached\n");
        return;
    }
    std::string results;
    for (const auto& entry : stats.results) {
        if (!results.empty()) results += ", ";
        if (entry.first == 0)
            results += "S_OK " + std::to_string(entry.second);
        else {
            char code[16] = {};
            std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(entry.first));
            results += std::string(code) + " " + std::to_string(entry.second);
        }
    }
    std::fprintf(stderr, "[rt] d3d12_xs: slot 55   PC CreateGraphicsPipelineState results: %s\n", results.c_str());
}

// What one slot-55 call measured, filled before the single lock that updates the histogram.
struct Slot55Call {
    const void* base = nullptr;
    bool base_tracked = false;
    bool base_graphics = false;
    bool base_null = false;
    std::uint64_t count = 0;
    std::string parts;   // the part words the call carried, in order
    std::vector<std::uint32_t> parts_list;  // the same, for the per-part histogram
    std::string reason;  // the field that refused the call; empty when it reached the PC device
    std::string why;     // the full refusal sentence, for the first occurrence of each reason
    bool reached_create = false;
    HRESULT result = E_FAIL;
};

void note_slot55(Slot55Call& call) {
    Slot55Stats& stats = slot55_stats();
    const std::lock_guard lock(stats.mutex);
    ++stats.calls;
    if (call.base) stats.bases.insert(call.base);
    if (call.base_null) ++stats.base_null;
    else if (!call.base_tracked) ++stats.base_untracked;
    else if (!call.base_graphics) ++stats.base_compute;
    else ++stats.base_tracked;
    if (call.reached_create) ++stats.reached_create;
    if (!call.reason.empty() && stats.reasons_logged.insert(call.reason).second)
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 55 refused (%s): base %p, %llu patch record(s) [%s]; %s\n",
                     call.reason.c_str(), call.base, static_cast<unsigned long long>(call.count), call.parts.c_str(),
                     call.why.c_str());
    if (call.reached_create) {
        const auto code = static_cast<std::uint32_t>(call.result);
        ++stats.results[code];
        if (SUCCEEDED(call.result)) ++stats.created;
        if (stats.results_logged.insert(code).second)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 55 base %p + %llu patch record(s) [%s] -> PC CreateGraphicsPipelineState "
                         "0x%08lx\n",
                         call.base, static_cast<unsigned long long>(call.count), call.parts.c_str(),
                         static_cast<unsigned long>(call.result));
    }
    for (const auto& part : call.parts_list) ++stats.parts[part];
    if (stats.calls % kSlot55SummaryInterval == 0) print_slot55_summary_locked(stats);
}

// Console device slot 55: CreateDerivedPipelineStateX(base, count, patches, riid, out). The title's riid and out
// pointer reach the PC device's own CreateGraphicsPipelineState for the rebuilt description, and its HRESULT comes back
// unchanged; a base the proxy cannot rebuild is refused by name instead of forwarded to an unrelated PC method.
HRESULT proxy_create_derived_pipeline_state(DeviceProxy* self, std::uint64_t base_word, std::uint64_t count,
                                            std::uint64_t patches_word, const IID* riid, void** out) {
    note_slot(55);
    // The trace names the console method once per call, so a boot's traced-call count stays comparable with the trees
    // that forwarded this slot to the PC method it is not (the same reason slot 57 records a line).
    d3d12_trace::record("XboxID3D12Device::Slot55");
    if (out) *out = nullptr;
    if (!out) return E_POINTER;
    if (!riid) return E_POINTER;

    static std::atomic<bool> explained{false};
    if (!explained.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot 55 is the console's CreateDerivedPipelineStateX(base, count, patches, "
                     "riid, out), not PC slot 55 CreateReservedResource1: a derivation copies the description this device "
                     "built for the base pipeline (slot 57), replaces the patched fixed-function parts and creates a new "
                     "PC pipeline state. An untracked base is refused by name.\n");

    Slot55Call call;
    call.base = reinterpret_cast<const void*>(base_word);
    call.count = count;
    if (!base_word) {
        call.base_null = true;
        call.reason = "base";
        call.why = "the base pipeline state is null";
        note_slot55(call);
        return E_INVALIDARG;
    }
    TrackedPipeline base;
    call.base_tracked = tracked_pipeline_state(call.base, &base);
    call.base_graphics = call.base_tracked && base.graphics;
    if (!call.base_tracked || !call.base_graphics) {
        call.reason = call.base_tracked ? "base.compute" : "base";
        call.why = call.base_tracked
                       ? "the base pipeline state is a compute pipeline, and the console's patched parts are graphics "
                         "fixed-function state"
                       : "the base pipeline state is not one this device created, so there is no description to derive "
                         "from";
        note_slot55(call);
        return E_NOTIMPL;
    }
    if (count > kMaxXboxPatches) {
        call.reason = "patches.count";
        call.why = "the patch count " + std::to_string(count) + " is outside the console's measured shape";
        note_slot55(call);
        return E_INVALIDARG;
    }
    const auto* patches = reinterpret_cast<const std::uint8_t*>(patches_word);
    if (count && !patches) {
        call.reason = "patches";
        call.why = "the patch array is null";
        note_slot55(call);
        return E_INVALIDARG;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC derived = base.desc;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint8_t* record = patches + static_cast<std::size_t>(i) * kXboxPatchStride;
        if (!readable_range(record, sizeof(std::uint32_t))) {
            call.reason = "patches";
            call.why = "patch record " + std::to_string(i) + " is not readable";
            note_slot55(call);
            return E_INVALIDARG;
        }
        std::uint32_t part = 0;
        std::memcpy(&part, record, sizeof(part));
        call.parts_list.push_back(part);
        if (!call.parts.empty()) call.parts += ",";
        call.parts += std::to_string(part);
        const std::uint8_t* payload = record + sizeof(std::uint32_t);
        switch (part) {
            case 0:  // D3D12_DEPTH_STENCIL_DESC
                if (!readable_range(payload, sizeof(D3D12_DEPTH_STENCIL_DESC))) {
                    call.reason = "patches[].payload";
                    call.why = "the depth-stencil payload of patch record " + std::to_string(i) + " is not readable";
                    note_slot55(call);
                    return E_INVALIDARG;
                }
                std::memcpy(&derived.DepthStencilState, payload, sizeof(derived.DepthStencilState));
                break;
            case 1:  // D3D12_RASTERIZER_DESC
                if (!readable_range(payload, sizeof(D3D12_RASTERIZER_DESC))) {
                    call.reason = "patches[].payload";
                    call.why = "the rasterizer payload of patch record " + std::to_string(i) + " is not readable";
                    note_slot55(call);
                    return E_INVALIDARG;
                }
                std::memcpy(&derived.RasterizerState, payload, sizeof(derived.RasterizerState));
                break;
            case 2:  // D3D12_BLEND_DESC
                if (!readable_range(payload, sizeof(D3D12_BLEND_DESC))) {
                    call.reason = "patches[].payload";
                    call.why = "the blend payload of patch record " + std::to_string(i) + " is not readable";
                    note_slot55(call);
                    return E_INVALIDARG;
                }
                std::memcpy(&derived.BlendState, payload, sizeof(derived.BlendState));
                break;
            case 3: {  // the UINT sample mask
                std::uint32_t mask = 0;
                if (!readable_range(payload, sizeof(mask))) {
                    call.reason = "patches[].payload";
                    call.why = "the sample-mask payload of patch record " + std::to_string(i) + " is not readable";
                    note_slot55(call);
                    return E_INVALIDARG;
                }
                std::memcpy(&mask, payload, sizeof(mask));
                derived.SampleMask = mask;
                break;
            }
            default:
                call.reason = "patches.part";
                call.why = "patch part " + std::to_string(part) +
                           " has no measured PC field in a graphics pipeline description, so the derived pipeline "
                           "would be missing unproven state";
                note_slot55(call);
                return E_NOTIMPL;
        }
    }
    call.reached_create = true;
    const HRESULT hr = self->real->CreateGraphicsPipelineState(&derived, *riid, out);
    if (SUCCEEDED(hr) && out && *out)
        track_graphics_pipeline_state(static_cast<ID3D12PipelineState*>(*out), derived);
    call.result = hr;
    note_slot55(call);
    return hr;
}

// ---- Slot 57: the record converted through rt::graphics_pso --------------------------------------------------------
//
// The slot-57 argument is a deserialized pipeline-library graphics record: a root signature, the fixed-function state
// blob, the input-layout blob and (native program, kind-2 stage blob) pairs in record-slot order
// (docs/graphics_record.md). rt::graphics_pso::build turns the decoded record plus the caller's inputs into a PC
// D3D12_GRAPHICS_PIPELINE_STATE_DESC and refuses by field what it cannot prove; this branch supplies the two console
// inputs the call does not carry - an explicitly logged attachment stand-in and the documented default fixed-function
// state - and sends the description to the PC device's own CreateGraphicsPipelineState, exactly as slot 67 forwards a
// compute description.
//
// It is also the measurement. One boot asks for ~10,000 distinct records and which field refuses the most decides the
// next decode (the input-layout element table and its format codes, the state blob, the stage slots or the attachment
// stand-in). So every refusal is counted by its `why` field and by record stage shape, the first occurrence of each
// reason is logged with the record's fields, the input-layout blob of each size/count shape is described once (its
// printable runs and their offsets), a compact histogram is printed every kSlot57SummaryInterval calls and every
// kSlot57SummarySeconds seconds, and a record is never built from a guessed input layout or a silent state default.
constexpr std::uint64_t kSlot57SummaryInterval = 2000;

// The attachment stand-in. The slot-57 call carries no render-target or depth-stencil formats, and the sample
// description lives in the undecoded fixed-function state blob (docs/graphics_pso.md section 5): the whole attachment
// list is an explicit, logged stand-in - one R8G8B8A8_UNORM target, no depth-stencil, sample count 1 - the same
// measure family as the documented default state. A wrong format here is a wrong pipeline; the replay creates the
// draw's image views with the PSO's format, so the console's real formats are still a decode to do.
constexpr DXGI_FORMAT kSlot57StandinRtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kSlot57StandinRenderTargetCount = 1;

// Where the record's +0x00 root signature pointer lands: the PC object itself (the proxy does not wrap root
// signatures), a title-side slot that holds one (accepted only when the value inside is a tracked PC object, so the
// mapping is proved), an untracked pointer (refused as pRootSignature), or null.
enum class Slot57Root : int { kNull = 0, kDirect, kIndirect, kUntracked, kCount };

const char* slot57_root_name(Slot57Root root) {
    switch (root) {
        case Slot57Root::kNull: return "null";
        case Slot57Root::kDirect: return "PC object";
        case Slot57Root::kIndirect: return "PC object via a title slot";
        case Slot57Root::kUntracked: return "untracked";
        default: return "unknown";
    }
}

// The record's stage shape in slot order: "ps", "vs+ps", "ps+ms", "ms".
std::string slot57_shape(const rt::graphics_record::Record& record) {
    std::string shape;
    for (std::size_t i = 0; i < record.stage_count; ++i) {
        if (!shape.empty()) shape += "+";
        shape += rt::graphics_record::stage_slot_name(record.stages[i].slot);
    }
    return shape;
}

// One distinct record, identified by the bytes the decoder validates (the same identity the old probe printed as an
// FNV over the first 0x68 bytes; this one covers the whole validated record).
std::uint64_t slot57_digest(const void* descriptor) {
    const auto* bytes = static_cast<const std::uint8_t*>(descriptor);
    std::uint64_t digest = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < rt::graphics_record::kDescriptorValidatedBytes; ++i)
        digest = (digest ^ bytes[i]) * 0x100000001b3ull;
    return digest;
}

// The builder's `why` field, e.g. "input_layout.elements[0].format" from "input_layout.elements[0].format: the ...".
std::string slot57_reason_field(const std::string& why) {
    const std::size_t colon = why.find(':');
    return colon == std::string::npos ? why : why.substr(0, colon);
}

// The two `why` fields that name a conversion path this tree has not built yet - not a malformed record - answer
// E_NOTIMPL; every other refusal is an invalid record for this converter.
HRESULT slot57_refusal_hresult(const std::string& why) {
    if (why.rfind("record.stages[].slot: a mesh stage", 0) == 0) return E_NOTIMPL;
    if (why.rfind("input_layout.elements[0].format", 0) == 0) return E_NOTIMPL;
    return E_INVALIDARG;
}

// The input-layout blob's printable runs with their offsets, for the first occurrence of each (size, count) shape.
// The committed decoder accepts only "all names first, then count*28 attribute bytes"; a boot's blobs that do not
// decode are exactly the ones this line explains (whether the names are contiguous, and where they start). Bounded:
// at most 8 runs of 4..23 printable bytes, one short line.
std::string slot57_layout_runs(const rt::graphics_record::Span& layout) {
    std::string runs;
    unsigned emitted = 0;
    for (std::size_t at = 0; at < layout.size && emitted < 8 && runs.size() < 96;) {
        const std::uint8_t byte = layout.data[at];
        if (byte < 0x20 || byte > 0x7e) {
            ++at;
            continue;
        }
        std::size_t end = at;
        while (end < layout.size && layout.data[end] >= 0x20 && layout.data[end] <= 0x7e && end - at < 23) ++end;
        if (end - at < 4) {
            at = end;
            continue;
        }
        char where[24] = {};
        std::snprintf(where, sizeof(where), "%s+0x%zx:", runs.empty() ? "" : ",", at);
        runs += where;
        runs.append(reinterpret_cast<const char*>(layout.data + at), end - at);
        ++emitted;
        at = end;
    }
    return runs;
}

// PALMETTO_XS_SLOT57_LAYOUT_DUMP=1 (measurement only, off by default): print the first bytes of one input-layout blob
// per (size, count@+0x4C) shape. The boot refuses on that blob, and the probe's printable-run offsets only locate the
// semantic strings; the element table's encoding (where each name, each attribute block and the size relation sit) is
// the next decode, and one dump of each shape is what measures it. Nothing here changes an answer.
bool slot57_layout_dump() {
    static const bool on = [] {
        char value[8] = {};
        return GetEnvironmentVariableA("PALMETTO_XS_SLOT57_LAYOUT_DUMP", value, sizeof(value)) > 0 && value[0] != '\0' &&
               value[0] != '0';
    }();
    return on;
}

struct Slot57ReasonStats {
    std::uint64_t calls = 0;
    std::set<std::uint64_t> records;                 // distinct records that refused this way
    std::map<std::string, std::uint64_t> shapes;     // calls per record stage shape
};

// One boot's refusal histogram. Bounded: the number of distinct `why` fields and stage shapes is a handful, and the
// per-reason record sets together hold one digest per distinct record.
struct Slot57Stats {
    std::mutex mutex;
    std::uint64_t calls = 0;
    std::set<std::uint64_t> records;
    std::map<std::string, Slot57ReasonStats> reasons;
    std::set<std::string> reasons_logged;
    std::uint64_t decoded = 0;
    std::uint64_t decode_refused = 0;
    std::uint64_t layout_decoded = 0;
    std::uint64_t layout_empty = 0;
    std::uint64_t layout_with_elements = 0;
    std::uint64_t layout_refused = 0;
    std::uint64_t stage_bytes_unreadable = 0;
    std::uint64_t roots[static_cast<int>(Slot57Root::kCount)] = {};
    std::uint64_t built = 0;        // reached CreateGraphicsPipelineState
    std::uint64_t pso_created = 0;  // ...and answered S_OK
    std::map<std::uint32_t, std::uint64_t> results;  // CreateGraphicsPipelineState HRESULTs (S_OK is 0)
    std::set<std::uint32_t> results_logged;
    std::map<std::string, std::uint64_t> shapes;
    // The input-layout blobs the boot carries, by size and by the word the committed decoder reads as the element
    // count at +0x4C: this is what names the next decode when blobs do not decode (a size/count pair plus the
    // printable runs' offsets says whether the names are contiguous or interleaved with the attribute bytes).
    std::map<std::string, std::uint64_t> layout_shapes;
    std::set<std::string> layout_shapes_logged;
    // The state record's measured container (docs/state_blob.md): how many calls carried a container this tree's
    // decoder proves, by family/item count, how many it refused, and how many fixed-function fields it yielded.
    std::uint64_t state_decoded = 0;
    std::uint64_t state_refused = 0;
    std::uint64_t state_fields_pinned = 0;
    std::map<std::string, std::uint64_t> state_shapes;
    std::set<std::string> state_shapes_logged;
    std::map<std::string, std::uint64_t> state_reasons;  // "state.fixed_function" today: no field is pinned
    std::set<std::string> state_reasons_logged;
    unsigned summaries = 0;
};

Slot57Stats& slot57_stats() {
    static Slot57Stats stats;
    return stats;
}

// What one slot-57 call measured, filled before the single lock that updates the histogram.
struct Slot57Call {
    std::uint64_t digest = 0;
    std::string shape;
    std::string reason;      // `why` field that refused it; empty when it reached the PC device
    std::string why;         // the builder's full refusal, for the first-occurrence line
    bool decoded = false;
    bool layout_decoded = false;
    bool stage_bytes_readable = true;
    std::size_t elements = 0;
    std::size_t state_bytes = 0;
    std::size_t layout_bytes = 0;
    std::string stages;      // "ps 448 B, vs 384 B"
    std::uint32_t layout_count = 0;  // the word at +0x4C that the committed decoder reads as the element count
    bool layout_count_read = false;
    std::string layout_runs;         // printable runs with offsets, for the first occurrence of each layout shape
    const std::uint8_t* layout_data = nullptr;  // the blob itself, kept for PALMETTO_XS_SLOT57_LAYOUT_DUMP
    bool state_decoded = false;      // the state record's container decoded (docs/state_blob.md)
    bool state_fields_decoded = false;  // ...and its fixed-function fields were yielded (none is pinned today)
    std::string state_shape;         // "vertex k=1" / "mesh k=0": the container's family and variable item count
    std::string state_reason;        // the fixed-function refusal's field ("state.fixed_function")
    std::string state_why;           // its full sentence, for the first-occurrence line
    Slot57Root root = Slot57Root::kNull;
    bool reached_create = false;
    HRESULT result = E_FAIL;
};

void print_slot57_summary_locked(Slot57Stats& stats) {
    ++stats.summaries;
    std::vector<std::pair<std::string, const Slot57ReasonStats*>> reasons;
    reasons.reserve(stats.reasons.size());
    for (const auto& entry : stats.reasons) reasons.emplace_back(entry.first, &entry.second);
    std::sort(reasons.begin(), reasons.end(),
              [](const auto& a, const auto& b) { return a.second->calls > b.second->calls; });
    const std::uint64_t refused_before_call = stats.calls - stats.built;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: slot 57 histogram #%u at %llu call(s), %zu distinct record(s): decoded %llu, "
                 "decode-refused %llu, built %llu, PSOs created %llu, refused before the call %llu\n",
                 stats.summaries, static_cast<unsigned long long>(stats.calls), stats.records.size(),
                 static_cast<unsigned long long>(stats.decoded), static_cast<unsigned long long>(stats.decode_refused),
                 static_cast<unsigned long long>(stats.built), static_cast<unsigned long long>(stats.pso_created),
                 static_cast<unsigned long long>(refused_before_call));
    for (std::size_t i = 0; i < reasons.size() && i < 8; ++i) {
        std::string shapes;
        for (const auto& entry : reasons[i].second->shapes) {
            if (!shapes.empty()) shapes += ", ";
            shapes += entry.first + " " + std::to_string(entry.second);
        }
        std::fprintf(stderr, "[rt] d3d12_xs: slot 57   why %s: %llu call(s), %zu distinct record(s) [%s]\n",
                     reasons[i].first.c_str(), static_cast<unsigned long long>(reasons[i].second->calls),
                     reasons[i].second->records.size(), shapes.c_str());
    }
    std::string shapes;
    for (const auto& entry : stats.shapes) {
        if (!shapes.empty()) shapes += ", ";
        shapes += entry.first + " " + std::to_string(entry.second);
    }
    std::fprintf(stderr, "[rt] d3d12_xs: slot 57   stage shapes: %s\n", shapes.empty() ? "none" : shapes.c_str());
    std::string layout_shapes;
    for (const auto& entry : stats.layout_shapes) {
        if (!layout_shapes.empty()) layout_shapes += ", ";
        layout_shapes += entry.first + " " + std::to_string(entry.second);
    }
    std::fprintf(stderr, "[rt] d3d12_xs: slot 57   input-layout blobs by size/count@+0x4C: %s\n",
                 layout_shapes.empty() ? "none" : layout_shapes.c_str());
    std::string state_shapes;
    for (const auto& entry : stats.state_shapes) {
        if (!state_shapes.empty()) state_shapes += ", ";
        state_shapes += entry.first + " " + std::to_string(entry.second);
    }
    std::uint64_t state_reason_rows = 0;
    for (const auto& entry : stats.state_reasons) state_reason_rows += entry.second;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: slot 57   state records by family/item count: %s; containers decoded %llu, refused %llu; "
                 "fixed-function fields pinned %llu (state.fixed_function rows %llu)\n",
                 state_shapes.empty() ? "none" : state_shapes.c_str(),
                 static_cast<unsigned long long>(stats.state_decoded),
                 static_cast<unsigned long long>(stats.state_refused),
                 static_cast<unsigned long long>(stats.state_fields_pinned),
                 static_cast<unsigned long long>(state_reason_rows));
    const std::uint64_t untracked = stats.roots[static_cast<int>(Slot57Root::kUntracked)];
    std::fprintf(stderr,
                 "[rt] d3d12_xs: slot 57   gates: input layouts empty %llu, with elements %llu, undecodable %llu; "
                 "root signatures PC object %llu, via a title slot %llu, untracked %llu, null %llu; unreadable stage "
                 "bytes %llu; attachments stand-in 1xR8G8B8A8_UNORM (no depth-stencil, sample count 1); fixed-function "
                 "state = documented_default_state() while no field of the decoded state record is pinned\n",
                 static_cast<unsigned long long>(stats.layout_empty),
                 static_cast<unsigned long long>(stats.layout_with_elements),
                 static_cast<unsigned long long>(stats.layout_refused),
                 static_cast<unsigned long long>(stats.roots[static_cast<int>(Slot57Root::kDirect)]),
                 static_cast<unsigned long long>(stats.roots[static_cast<int>(Slot57Root::kIndirect)]),
                 static_cast<unsigned long long>(untracked),
                 static_cast<unsigned long long>(stats.roots[static_cast<int>(Slot57Root::kNull)]),
                 static_cast<unsigned long long>(stats.stage_bytes_unreadable));
    if (stats.results.empty()) {
        std::fprintf(stderr, "[rt] d3d12_xs: slot 57   PC CreateGraphicsPipelineState results: none reached\n");
        return;
    }
    std::string results;
    for (const auto& entry : stats.results) {
        if (!results.empty()) results += ", ";
        if (entry.first == 0)
            results += "S_OK " + std::to_string(entry.second);
        else {
            char code[16] = {};
            std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(entry.first));
            results += std::string(code) + " " + std::to_string(entry.second);
        }
    }
    std::fprintf(stderr, "[rt] d3d12_xs: slot 57   PC CreateGraphicsPipelineState results: %s\n", results.c_str());
}

// Counts one call, logs the first occurrence of a reason / a result, and prints the interval summary.
void note_slot57(Slot57Call& call) {
    Slot57Stats& stats = slot57_stats();
    const std::lock_guard lock(stats.mutex);
    ++stats.calls;
    if (call.digest) stats.records.insert(call.digest);
    if (!call.shape.empty()) ++stats.shapes[call.shape];
    ++stats.roots[static_cast<int>(call.root)];
    if (call.decoded) {
        ++stats.decoded;
        if (call.layout_count_read) {
            const std::string layout_shape =
                std::to_string(call.layout_bytes) + "B/count" + std::to_string(call.layout_count);
            ++stats.layout_shapes[layout_shape];
            if (stats.layout_shapes_logged.insert(layout_shape).second) {
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 57 input-layout blob %zu B (first seen): element count word at "
                             "+0x4C = %u, printable runs [%s]\n",
                             call.layout_bytes, call.layout_count, call.layout_runs.c_str());
                if (slot57_layout_dump() && call.layout_data) {
                    const std::size_t shown =
                        std::min<std::size_t>(call.layout_bytes, rt::graphics_record::kInputLayoutMaxBytes);
                    std::fprintf(stderr, "[rt] d3d12_xs: slot 57 layout dump (%zu B, count %u):", call.layout_bytes,
                                 call.layout_count);
                    for (std::size_t i = 0; i < shown; ++i) {
                        if (i % 32 == 0) std::fprintf(stderr, "\n[rt]   +0x%02zx:", i);
                        std::fprintf(stderr, " %02x", int(call.layout_data[i]));
                    }
                    std::fprintf(stderr, "\n");
                }
            }
        }
        if (call.layout_decoded) {
            ++stats.layout_decoded;
            if (call.elements)
                ++stats.layout_with_elements;
            else
                ++stats.layout_empty;
        } else {
            ++stats.layout_refused;
        }
        if (!call.stage_bytes_readable) ++stats.stage_bytes_unreadable;
    } else {
        ++stats.decode_refused;
    }
    if (call.state_decoded) {
        ++stats.state_decoded;
        ++stats.state_shapes[call.state_shape];
        if (stats.state_shapes_logged.insert(call.state_shape).second)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 57 state record container (first of %s): the measured container decodes; "
                         "no fixed-function field of it is pinned by a measurement of this title's data "
                         "(docs/state_blob.md)\n",
                         call.state_shape.c_str());
    } else {
        ++stats.state_refused;
    }
    if (call.state_fields_decoded) ++stats.state_fields_pinned;
    if (!call.state_reason.empty()) {
        ++stats.state_reasons[call.state_reason];
        if (stats.state_reasons_logged.insert(call.state_reason).second)
            std::fprintf(stderr, "[rt] d3d12_xs: slot 57 fixed-function state refused (first of \"%s\"): %s\n",
                         call.state_reason.c_str(), call.state_why.c_str());
    }
    if (!call.reason.empty()) {
        Slot57ReasonStats& reason = stats.reasons[call.reason];
        ++reason.calls;
        if (call.digest) reason.records.insert(call.digest);
        ++reason.shapes[call.shape];
        if (stats.reasons_logged.insert(call.reason).second) {
            std::fprintf(stderr, "[rt] d3d12_xs: slot 57 refused (first of \"%s\"): %s\n", call.reason.c_str(),
                         call.why.c_str());
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 57   record %016llx shape %s: state %zu B, input layout %zu B/%zu "
                         "element(s), root signature %s, stages [%s]\n",
                         static_cast<unsigned long long>(call.digest), call.shape.c_str(), call.state_bytes,
                         call.layout_bytes, call.elements, slot57_root_name(call.root), call.stages.c_str());
        }
    }
    if (call.reached_create) {
        ++stats.built;
        ++stats.results[static_cast<std::uint32_t>(call.result)];
        if (call.result == S_OK) {
            ++stats.pso_created;
            static bool created_logged = false;
            if (!created_logged) {
                created_logged = true;
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 57 created the first PC graphics pipeline (record %016llx shape %s, "
                             "root signature %s, stages [%s]); the attachment list and the fixed-function state are "
                             "the logged stand-ins, not decoded console values\n",
                             static_cast<unsigned long long>(call.digest), call.shape.c_str(),
                             slot57_root_name(call.root), call.stages.c_str());
            }
        } else if (stats.results_logged.insert(static_cast<std::uint32_t>(call.result)).second) {
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 57 PC CreateGraphicsPipelineState refused the first description with "
                         "0x%08lx (record %016llx shape %s, root signature %s, stages [%s])\n",
                         static_cast<unsigned long>(call.result), static_cast<unsigned long long>(call.digest),
                         call.shape.c_str(), slot57_root_name(call.root), call.stages.c_str());
        }
    }
    if (stats.calls % kSlot57SummaryInterval == 0) print_slot57_summary_locked(stats);
}

// The record's +0x00 mapped to the PC root signature it names, or null with the class naming why not. A title-side
// slot holding a PC object is accepted only when the value inside is one this device handed out: the mapping is
// proved against the root-signature table, never guessed from the pointer's shape.
Slot57Root classify_slot57_root(void* root, ID3D12RootSignature** pc_root_signature) {
    *pc_root_signature = nullptr;
    if (!root) return Slot57Root::kNull;
    if (is_tracked_root_signature(root)) {
        *pc_root_signature = static_cast<ID3D12RootSignature*>(root);
        return Slot57Root::kDirect;
    }
    if (readable_range(root, sizeof(void*))) {
        void* inner = nullptr;
        std::memcpy(&inner, root, sizeof(inner));
        if (inner && is_tracked_root_signature(inner)) {
            *pc_root_signature = static_cast<ID3D12RootSignature*>(inner);
            return Slot57Root::kIndirect;
        }
    }
    return Slot57Root::kUntracked;
}

// One slot-57 call: decode, fill the caller's inputs, build, and forward the description. Returns the HRESULT the
// title gets: S_OK when the PC device created the pipeline, the named refusal otherwise (never a silent success).
HRESULT convert_slot57_record(DeviceProxy* self, const void* desc, const IID* riid, void** out) {
    *out = nullptr;  // a refusal never leaves a stale PSO; the PC device nulls it too
    Slot57Call call;

    // The stand-in declaration, once: a reader of the histogram has to know the attachment list and the fixed-function
    // state it measured were substitutes. Both are required to be explicit (docs/graphics_pso.md sections 4 and 5);
    // the state record's container is decoded (docs/state_blob.md) but no fixed-function field of it is pinned.
    static std::atomic<bool> standins_logged{false};
    if (!standins_logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 57 stand-ins (logged once): attachments = 1x R8G8B8A8_UNORM, no "
                     "depth-stencil, sample count 1 (the slot-57 call carries no formats; the console's are in the "
                     "state record and are not decoded); fixed-function state = "
                     "rt::graphics_pso::documented_default_state() while every field of the state record is refused by "
                     "name (docs/state_blob.md; counted as state.fixed_function every %llu calls). Refusals are counted "
                     "by field; a record that cannot be built returns its named refusal\n",
                     static_cast<unsigned long long>(kSlot57SummaryInterval));

    // 1. The record's own bytes. decode() validates the shape and never follows the record's pointers, so the spans it
    //    returns are range-checked here before anything dereferences them.
    if (!desc || !readable_range(desc, rt::graphics_record::kDescriptorValidatedBytes)) {
        call.reason = "record";
        call.why = "the slot-57 descriptor is not readable for the 0x108 bytes the decoder validates";
        note_slot57(call);
        return E_INVALIDARG;
    }
    call.digest = slot57_digest(desc);
    rt::graphics_record::Record record;
    if (!rt::graphics_record::decode(desc, rt::graphics_record::kDescriptorValidatedBytes, &record)) {
        call.reason = "decode";
        call.why = "the record does not decode as a deserialized graphics pipeline record (docs/graphics_record.md)";
        note_slot57(call);
        return E_INVALIDARG;
    }
    call.decoded = true;
    call.shape = slot57_shape(record);
    call.state_bytes = record.state.size;
    call.layout_bytes = record.input_layout.size;
    for (std::size_t i = 0; i < record.stage_count; ++i) {
        if (!call.stages.empty()) call.stages += ", ";
        call.stages += rt::graphics_record::stage_slot_name(record.stages[i].slot);
        call.stages += " " + std::to_string(record.stages[i].program.size) + " B";
        if (!readable_range(record.stages[i].program.data, record.stages[i].program.size))
            call.stage_bytes_readable = false;
    }

    // 2. The two blobs the builder reads are checked for readability first: the decoder copies only pointers.
    if (!readable_range(record.state.data, record.state.size)) {
        call.reason = "record.state";
        call.why = "the fixed-function state blob the record points at is not readable";
        note_slot57(call);
        return E_INVALIDARG;
    }
    if (!readable_range(record.input_layout.data, record.input_layout.size)) {
        call.reason = "record.input_layout";
        call.why = "the input-layout blob the record points at is not readable";
        note_slot57(call);
        return E_INVALIDARG;
    }

    // 2b. The state record's measured container (docs/state_blob.md): its family and variable item count are decoded,
    //     and its fixed-function fields are requested by name. The container is the same shape build() proves, so a
    //     container that does not decode still refuses later as record.state; the fixed-function fields refuse with
    //     their own histogram row and the documented default set below stays the explicit stand-in. Nothing is guessed.
    rt::graphics_record::State state_record;
    call.state_decoded = rt::graphics_record::decode_state(record.state, &state_record);
    rt::graphics_pso::FixedFunctionState measured_state{};
    if (call.state_decoded) {
        call.state_shape = std::string(rt::graphics_record::state_family_name(state_record.family)) + " k=" +
                           std::to_string(state_record.variable_item_count);
        rt::graphics_pso::FixedFunctionFieldStatus measured_fields;
        std::string state_why;
        call.state_fields_decoded =
            rt::graphics_pso::decode_fixed_function_state(state_record, &measured_state, &measured_fields, &state_why);
        if (!call.state_fields_decoded) {
            call.state_reason = slot57_reason_field(state_why);
            call.state_why = state_why;
        }
    }
    std::vector<rt::graphics_record::Element> elements;
    call.layout_decoded = rt::graphics_record::decode_input_layout(record.input_layout, &elements);
    call.elements = elements.size();
    // The word the committed decoder reads as the element count, and the blob's printable runs with their offsets:
    // when a layout does not decode, this pair is what says whether the names are contiguous (all names, then the
    // attribute blocks) or interleaved with them - the boundary the next decode has to establish.
    if (record.input_layout.size >= rt::graphics_record::kInputLayoutCountOffset + sizeof(std::uint32_t)) {
        std::memcpy(&call.layout_count, record.input_layout.data + rt::graphics_record::kInputLayoutCountOffset,
                    sizeof(call.layout_count));
        call.layout_count_read = true;
    }
    call.layout_runs = slot57_layout_runs(record.input_layout);
    call.layout_data = record.input_layout.data;
    if (!call.stage_bytes_readable) {
        call.reason = "record.stages[].program";
        call.why = "a stage's native program bytes the record points at are not readable";
        note_slot57(call);
        return E_INVALIDARG;
    }

    // 3. The caller's inputs: the root signature the record names (proved against the table above), the explicitly
    //    logged attachment stand-in and the documented default fixed-function state. Nothing here is a silent default.
    ID3D12RootSignature* pc_root_signature = nullptr;
    call.root = classify_slot57_root(record.root_signature, &pc_root_signature);
    rt::graphics_pso::GraphicsPsoInputs inputs{};
    inputs.root_signature = pc_root_signature;  // null is refused by name as pRootSignature, never replaced silently
    inputs.render_target_count = kSlot57StandinRenderTargetCount;
    inputs.rtv_formats[0] = kSlot57StandinRtvFormat;
    inputs.dsv_format = DXGI_FORMAT_UNKNOWN;
    // The state the record actually yielded, when a measurement pins it; the documented default set is the explicit
    // stand-in while decode_fixed_function_state refuses (today every field of every state record is unpinned).
    inputs.defaults = call.state_fields_decoded ? measured_state : rt::graphics_pso::documented_default_state();
    for (std::size_t i = 0; i < record.stage_count; ++i)
        inputs.stages[record.stages[i].slot] = {record.stages[i].program.data, record.stages[i].program.size};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pc{};
    std::string why;
    if (!rt::graphics_pso::build(record, inputs, &pc, &why)) {
        call.reason = slot57_reason_field(why);
        call.why = why;
        note_slot57(call);
        return slot57_refusal_hresult(why);
    }

    // 4. The PC device's own CreateGraphicsPipelineState, with the same forwarding shape slot 67 uses for compute: the
    //    description, the title's riid and out pointer, and the real HRESULT back. The native stages resolve there
    //    (docs/graphics_pso.md section 2), so an unresolvable program is its E_INVALIDARG/E_NOTIMPL, not ours.
    call.reached_create = true;
    call.result = self->real->CreateGraphicsPipelineState(&pc, *riid, out);
    // The description of every graphics pipeline this device creates is remembered, so console slot 55
    // (CreateDerivedPipelineStateX) can rebuild it with its patched fixed-function parts.
    if (SUCCEEDED(call.result) && out && *out)
        track_graphics_pipeline_state(static_cast<ID3D12PipelineState*>(*out), pc);
    note_slot57(call);
    return call.result;
}

HRESULT proxy_create_descriptor_heap(DeviceProxy* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, const IID* riid,
                                     void** out) {
    note_slot(14);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as the PC device does
    if (!riid) return E_POINTER;
    // No output: a capability check, answered by the device (S_FALSE, as on the PC).
    if (!out) return self->real->CreateDescriptorHeap(desc, *riid, nullptr);

    void* created = nullptr;
    HRESULT hr = E_INVALIDARG;
    bool console_extended = false;
    std::uint64_t console_address = 0;
    std::uint64_t console_size = 0;
    if (desc && desc->Type <= D3D12_DESCRIPTOR_HEAP_TYPE_DSV &&
        (desc->Flags & ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(desc);
        const void* tail = bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC);
        if (readable_range(tail, sizeof(std::uint64_t) * 2)) {
            std::uint64_t address = 0;
            std::uint64_t size = 0;
            std::memcpy(&address, tail, sizeof(address));
            std::memcpy(&size, bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC) + sizeof(address), sizeof(size));
            // Console-extended slot 14 (issue #313): address/size follow the PC desc. Only take this path when a
            // non-zero console VA is present; normal PC 16-byte descs never enter it.
            if (address != 0) {
                console_extended = true;
                console_address = address;
                console_size = size;
                if (const auto fn = rt::d3d12::console_heap_slot()) {
                    hr = fn(self->real, desc, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(address), size,
                            __uuidof(ID3D12DescriptorHeap), &created);
                } else {
                    hr = self->real->CreateDescriptorHeap(desc, __uuidof(ID3D12DescriptorHeap), &created);
                }
            }
        }
    }
    if (!console_extended)
        hr = self->real->CreateDescriptorHeap(desc, __uuidof(ID3D12DescriptorHeap), &created);
    if (FAILED(hr) || !created) return FAILED(hr) ? hr : E_FAIL;

    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(created);
    hr = create_heap_proxy(heap, self, *riid, out);
    if (FAILED(hr)) {
        heap->Release();
    } else if (clear_barrier_probe() && console_extended && desc && out && *out) {
        // Measurement: the requested console range and the handles the layer gave the title, so a console CPU
        // descriptor handle can be placed in its heap.
        auto* proxy = static_cast<HeapProxy*>(*out);
        probe_console_heap(14, *desc, console_address, console_size, heap_get_cpu_handle(proxy).ptr,
                           heap_get_gpu_handle(proxy).ptr);
    }
    return hr;
}

// The Xbox slot 62 takes two more words between the description and the IID: (desc, w2, w3, riid, out). Measured
// on the package's XboxBvhBuilder.dll (its constructor passes w2 = w3 = 0 and IID_ID3D12PipelineState in the fourth
// argument), as the prior art had recorded. Their meaning is unknown; non-zero values are logged once.
HRESULT proxy_create_compute_pipeline_state(DeviceProxy* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                            std::uint64_t w2, std::uint64_t w3, const IID* riid, void** out) {
    note_slot(62);
    if (w2 || w3) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: slot 62 extra words 0x%llx 0x%llx (ignored)\n",
                         static_cast<unsigned long long>(w2), static_cast<unsigned long long>(w3));
    }
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    return self->real->CreateComputePipelineState(desc, *riid, out);
}

// The Xbox slot 57 is the console's precompiled graphics pipeline creation: the title passes the description in a1,
// IID_ID3D12PipelineState in a2 and the out pointer in a3. The description is not a PC D3D12_GRAPHICS_PIPELINE_STATE_DESC
// (the console's graphics description is not in the public GDK reference, docs/xbox_d3d12_xs.md 5.3): it is a root
// signature followed by six pointer/size pairs, and the first call measures what those pairs are rather than assuming.
// Measured: pairs 2 and 4 carry the FSPX kind-6 program prologue (docs/fspx.md, "Kind 6"), pairs 3 and 5 start with the
// word 2 that docs/shader_library.md section 3 gives a graphics stage blob, and pairs 0 and 1 are the 388-byte state and
// 188-byte input-layout blobs of a pipeline-library graphics record. So this is the graphics analogue of the compute
// slot 67's DeserializeComputePipelineStateX: it creates a pipeline state from a precompiled record, with two
// (program, stage blob) pairs in slot order (slot 0 the pixel shader, slot 1 the vertex shader). The probe logs per pair
// the size, the prefix, whether it is a native kind-6 program whose SHA-256 the shader index keys it by, and for a blob
// its tag offsets, digest and printable names, then refuses: no resolver for those records exists yet. With
// PALMETTO_STANDIN_PIPELINES on it hands over a real stand-in graphics pipeline instead (see the end of this function).
// The trace line per call is not part of the conversion: this slot used to be forwarded to a PC method that recorded
// one, and the boot's D3D12 call count is compared across trees, so the probe must record the call too.
HRESULT proxy_create_graphics_pipeline_probe(DeviceProxy* self, const void* desc, const IID* riid, void** out) {
    note_slot(57);
    // Past the public device chain the slot has no PC name, so it is named by index, as a child slot past its chain is
    // ("XboxID3D12CommandQueue::Slot40"); d3d12_trace::record takes a fixed string (runtime/README.md, [d3d12-trace]).
    d3d12_trace::record("XboxID3D12Device::Slot57");
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!riid) return E_POINTER;
    // How many distinct records a boot asks for, and of which shape: each descriptor's six pointer/size pairs and the
    // first words of its state and input-layout blobs, once per distinct descriptor. This sizes the work that has to be
    // published and translated before a resolver can find anything: it is the number of graphics pipelines the title
    // needs at this point in the boot, not the number of calls.
    {
        const auto* bytes = static_cast<const std::uint8_t*>(desc);
        std::uint64_t digest = 0xcbf29ce484222325ull;
        for (int i = 0; i < 0x68; ++i) digest = (digest ^ bytes[i]) * 0x100000001b3ull;
        static std::mutex record_mutex;
        static std::set<std::uint64_t> seen_records;
        bool first_time = false;
        {
            const std::lock_guard lock(record_mutex);
            first_time = seen_records.insert(digest).second;
        }
        if (first_time) {
            const auto* words = reinterpret_cast<const std::uint64_t*>(desc);
            std::fprintf(stderr, "[rt] d3d12_xs: slot 57 record %016llx: root %p, pairs",
                         static_cast<unsigned long long>(digest), reinterpret_cast<void*>(words[0]));
            const std::uint64_t sizes[6] = {words[2], words[4], words[6], words[8], words[10], words[12]};
            for (int i = 0; i < 6; ++i) std::fprintf(stderr, " %llu", static_cast<unsigned long long>(sizes[i]));
            std::uint32_t state_word = 0, layout_word = 0;
            if (readable_range(reinterpret_cast<const void*>(words[1]), 4))
                state_word = *reinterpret_cast<const std::uint32_t*>(words[1]);
            if (readable_range(reinterpret_cast<const void*>(words[3]), 8))
                layout_word = *reinterpret_cast<const std::uint32_t*>(words[3]);
            std::fprintf(stderr, "; state[0] %08x layout[0] %08x\n", state_word, layout_word);
            // The records differ mostly in state and input layout, so the translation job is sized by distinct programs,
            // not by records. Pairs 2 and 4 carry the program bytes (a size and an FNV over the first 512 bytes identify
            // one well enough to count; SHA-256 is the key the index actually uses).
            for (int slot = 0; slot < 2; ++slot) {
                const int pair = 2 + 2 * slot;
                const std::uint64_t address = words[2 * pair + 1];
                const std::uint64_t size = words[2 * pair + 2];
                if (!address || !size || !readable_range(reinterpret_cast<const void*>(address), 1)) continue;
                const auto* code = reinterpret_cast<const std::uint8_t*>(address);
                if (size > (1ull << 20) || !readable_range(code, static_cast<std::size_t>(size))) continue;
                // SHA-256 of the program bytes as passed is exactly the index's fspx_code_sha256, the key
                // create_compute already resolves with - so this list is directly the build's to-translate list.
                const std::string sha = palmetto::sha256_hex(
                    std::span<const std::uint8_t>(code, static_cast<std::size_t>(size)));
                static std::mutex program_mutex;
                static std::set<std::string> seen_programs[2];
                bool first_program = false;
                {
                    const std::lock_guard lock(program_mutex);
                    first_program = seen_programs[slot].insert(sha).second;
                }
                if (first_program)
                    std::fprintf(stderr, "[rt] d3d12_xs: slot 57 program %d: %llu B sha256 %s\n", slot,
                                 static_cast<unsigned long long>(size), sha.c_str());
            }
        }
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        const auto* words = static_cast<const std::uint64_t*>(desc);
        std::fprintf(stderr, "[rt] d3d12_xs: slot 57 probe: riid %08lx-%04x-%04x, %u words:\n",
                     static_cast<unsigned long>(riid->Data1), riid->Data2, riid->Data3, 40u);
        for (int i = 0; i < 40; i += 4) {
            std::fprintf(stderr, "[rt]   +%02x: %016llx %016llx %016llx %016llx\n", 8 * i,
                         static_cast<unsigned long long>(words[i]), static_cast<unsigned long long>(words[i + 1]),
                         static_cast<unsigned long long>(words[i + 2]), static_cast<unsigned long long>(words[i + 3]));
        }
        const auto* bytes = static_cast<const std::uint8_t*>(desc);
        for (int pair = 0; pair < 6; ++pair) {
            const std::size_t at = 0x08 + 0x10 * static_cast<std::size_t>(pair);
            std::uint64_t address = 0;
            std::uint64_t size = 0;
            if (!readable_range(bytes + at, sizeof(address) + sizeof(size))) {
                std::fprintf(stderr, "[rt]   pair %d at +%02x: description unreadable\n", pair, unsigned(at));
                continue;
            }
            std::memcpy(&address, bytes + at, sizeof(address));
            std::memcpy(&size, bytes + at + sizeof(address), sizeof(size));
            const auto* code = reinterpret_cast<const std::uint8_t*>(address);
            const bool readable = address && readable_range(code, 16);
            char head[33] = {0};
            for (int b = 0; readable && b < 16; ++b) std::snprintf(head + 2 * b, 3, "%02x", int(code[b]));
            const char* tag = "none";
            if (readable) {
                if (std::memcmp(code, "FSSM", 4) == 0) tag = "FSSM";
                else if (std::memcmp(code, "DXBC", 4) == 0) tag = "DXBC";
                else if (std::memcmp(code, "FSPX", 4) == 0) tag = "FSPX";
            }
            bool prologue = false;
            if (address && size >= 36 && readable_range(code, 36)) {
                std::uint8_t first[36];
                std::memcpy(first, code, sizeof(first));
                prologue = palmetto::fspx_bare_program({first, sizeof(first)});
            }
            const std::string sha =
                address && size && size <= (16u << 20) && readable_range(code, static_cast<std::size_t>(size))
                    ? palmetto::sha256_hex({code, static_cast<std::size_t>(size)})
                    : std::string();
            std::fprintf(stderr,
                         "[rt]   pair %d at +%02x: code %p size %llu first16 %s (tag %s, FSPX kind-6 prologue %s, "
                         "sha256 %s)\n",
                         pair, unsigned(at), reinterpret_cast<const void*>(address),
                         static_cast<unsigned long long>(size), head, tag, prologue ? "yes" : "no",
                         sha.empty() ? "unreadable" : sha.c_str());
            if (!address || !size || size > 8192 || !readable_range(code, static_cast<std::size_t>(size))) continue;
            const std::size_t length = static_cast<std::size_t>(size);
            const std::size_t prefix_length = length < 64 ? length : 64;
            char prefix[2 * 64 + 1] = {0};
            for (std::size_t i = 0; i < prefix_length; ++i) std::snprintf(prefix + 2 * i, 3, "%02x", int(code[i]));
            std::fprintf(stderr, "[rt]     pair %d first%zu: %s\n", pair, prefix_length, prefix);
            // The 0x40/0x41/0x42/0x43 tags and the 16 bytes after the 0x43 word are what the stage and layout blobs of a
            // pipeline-library record are made of (docs/shader_library.md section 3): logging their offsets and the
            // digest is enough to write a reader without dumping the blob.
            std::string tags;
            char entry[32] = {0};
            for (std::size_t where = 0; where + 4 <= length && tags.size() < 480; where += 4) {
                std::uint32_t word = 0;
                std::memcpy(&word, code + where, 4);
                if (word != 0x40 && word != 0x41 && word != 0x42 && word != 0x43 && word != 0x403) continue;
                std::snprintf(entry, sizeof(entry), "%s0x%x@+0x%zx", tags.empty() ? "" : ", ", unsigned(word), where);
                tags += entry;
            }
            std::string digest;
            for (std::size_t where = 0; where + 20 <= length; where += 4) {
                std::uint32_t word = 0;
                std::memcpy(&word, code + where, 4);
                if (word != 0x43) continue;
                char hex[33] = {0};
                for (int b = 0; b < 16; ++b) std::snprintf(hex + 2 * b, 3, "%02x", int(code[where + 4 + b]));
                digest = hex;
                break;
            }
            std::size_t zero_tail = 0;
            while (zero_tail < length && code[length - 1 - zero_tail] == 0) ++zero_tail;
            // An input layout names its semantics, so any printable run of four or more bytes is worth reporting.
            std::string names;
            for (std::size_t where = 0; where + 4 <= length && names.size() < 240; ++where) {
                if (code[where] < 0x20 || code[where] > 0x7e) continue;
                std::size_t end = where;
                while (end < length && code[end] >= 0x20 && code[end] <= 0x7e && end - where < 32) ++end;
                if (end - where < 4) continue;
                names.append(names.empty() ? "" : ",");
                names.append(reinterpret_cast<const char*>(code + where), end - where);
                where = end;
            }
            std::fprintf(stderr, "[rt]     pair %d structure: tags [%s] digest16 %s trailing_zeros %zu ascii [%s]\n", pair,
                         tags.empty() ? "none" : tags.c_str(), digest.empty() ? "none" : digest.c_str(), zero_tail,
                         names.empty() ? "none" : names.c_str());
        }
    }
    // The record is converted through rt::graphics_pso (docs/graphics_pso.md section 5): a built description goes to
    // the PC device's own CreateGraphicsPipelineState, a refused one returns its named field and the title gets a null
    // PSO exactly as the probe answered before. Which field refuses, counted over the boot, is the measurement.
    const HRESULT converted = convert_slot57_record(self, desc, riid, out);
    if (converted == S_OK) return S_OK;
    // With PALMETTO_STANDIN_PIPELINES on, the Vulkan device hands the title a real graphics pipeline built from this
    // project's own VS+PS pair instead (d3d12_vulkan_device.cpp, create_standin_graphics_pipeline), so a boot measures
    // whether the refusal is what the title is waiting for without changing what the default boot answers. Diagnostic
    // only: the stand-in's draws do not do the record's work.
    if (const auto create_standin = rt::d3d12::graphics_standin_slot()) {
        // The record's stage count (the measured slots are 0 PS, 1 VS, 2 MS: docs/graphics_record.md section 1),
        // reported on the stand-in lines so a reader can tell the shape that was stood in for.
        unsigned stage_count = 0;
        const auto* record = static_cast<const std::uint8_t*>(desc);
        for (int slot = 0; slot < 3; ++slot) {
            const std::size_t at = 0x28 + 0x20 * std::size_t(slot);
            std::uint64_t address = 0, size = 0;
            if (!readable_range(record + at, sizeof(address) + sizeof(size))) continue;
            std::memcpy(&address, record + at, sizeof(address));
            std::memcpy(&size, record + at + sizeof(address), sizeof(size));
            if (address && size) ++stage_count;
        }
        const HRESULT standin = create_standin(self->real, *riid, out);
        if (SUCCEEDED(standin) && *out) {
            static std::atomic<std::uint64_t> standin_count{0};
            const std::uint64_t count = standin_count.fetch_add(1, std::memory_order_relaxed) + 1;
            // The explanation is capped like the compute stand-in's; the counter line is not, so the last line before
            // the next stop gives the exact number of stand-ins the boot took.
            if (count <= 3)
                std::fprintf(stderr,
                             "[rt] d3d12_xs: STAND-IN graphics pipeline #%llu (record stage count %u): the slot-57 "
                             "record was refused by the converter, so a pipeline built from this project's own VS+PS "
                             "pair is handed over; its draws will not do the record's work\n",
                             static_cast<unsigned long long>(count), stage_count);
            else
                std::fprintf(stderr, "[rt] d3d12_xs: STAND-IN graphics pipeline #%llu (record stage count %u)\n",
                             static_cast<unsigned long long>(count), stage_count);
            return standin;
        }
        static std::atomic<int> standin_failures{0};
        if (standin_failures.fetch_add(1, std::memory_order_relaxed) < 3)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 57 graphics stand-in not created (0x%08lx, record stage count %u); the "
                         "converter's refusal is returned\n",
                         static_cast<unsigned long>(standin), stage_count);
        *out = nullptr;
    }
    return converted;
}

// The Xbox slot 67 creates a compute pipeline from native GPU code: (desc, riid, out), named
// DeserializeComputePipelineStateX in amd_fidelityfx_upscaler_xbox.dll's messages (call at RVA 0x56d9). The desc
// passes the root signature at +0x00, followed by pointer/size pairs for the shader's FSPX entries (docs/fspx.md):
// kind 0 (dispatch state) at +0x08/+0x10, kind 1 at +0x18/+0x20, kind 6 (the RDNA2 program) at +0x28/+0x30, and
// kind 7 (records) at +0x38/+0x40. Translates the console description to a standard D3D12_COMPUTE_PIPELINE_STATE_DESC
// with CS bytecode pointing at kind 6 and routes it to CreateComputePipelineState on the PC device (slot 11), which
// on the Vulkan device resolves through the translator. The PC slot 67 is CreateProtectedResourceSession1.
HRESULT proxy_create_compute_pipeline_from_native_code(DeviceProxy* self, const void* desc,
                                                       const IID* riid, void** out) {
    note_slot(67);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!riid) return E_POINTER;

    // FSPX description layout (docs/fspx.md §How the DLL reads it):
    struct XboxNativeComputePipelineStateDesc {
        ID3D12RootSignature* root_signature;  // +0x00
        const void* kind0;                    // +0x08: dispatch state
        std::uint64_t kind0_size;             // +0x10: 28 bytes
        const void* kind1;                    // +0x18: unknown
        std::uint64_t kind1_size;             // +0x20: 20 bytes
        const void* kind6;                    // +0x28: RDNA2 program code
        std::uint64_t kind6_size;             // +0x30: program size in bytes
        const void* kind7;                    // +0x38: resource/hash records
        std::uint64_t kind7_size;             // +0x40: records size in bytes
    };
    const auto* xbox_desc = static_cast<const XboxNativeComputePipelineStateDesc*>(desc);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pc_desc{};
    pc_desc.pRootSignature = xbox_desc->root_signature;
    pc_desc.CS.pShaderBytecode = xbox_desc->kind6;
    pc_desc.CS.BytecodeLength = static_cast<SIZE_T>(xbox_desc->kind6_size);
    // With kind 0 (the DLL always passes it: its reader refuses a payload without one), the four pairs go on as one
    // FSPX payload, so the device matches the program by its kind-6 and kind-0 keys (docs/fspx.md, "Build and run-time
    // lookup"): two programs with the same code and different dispatch state stay apart. Every pair is checked before
    // it is read (palmetto::fspx_payload_from_parts); an inconsistent one refuses the call. Without kind 0 the kind-6
    // bytes go on alone, as before.
    std::vector<std::uint8_t> payload;
    if (xbox_desc->kind0 || xbox_desc->kind0_size) {
        const palmetto::FspxPart parts[] = {{0, xbox_desc->kind0, xbox_desc->kind0_size},
                                           {1, xbox_desc->kind1, xbox_desc->kind1_size},
                                           {6, xbox_desc->kind6, xbox_desc->kind6_size},
                                           {7, xbox_desc->kind7, xbox_desc->kind7_size}};
        payload = palmetto::fspx_payload_from_parts(parts);
        if (payload.empty()) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 67 refused: a pointer/size pair is inconsistent or too large "
                             "(kind0 %llu B, kind1 %llu B, kind6 %llu B, kind7 %llu B)\n",
                             static_cast<unsigned long long>(xbox_desc->kind0_size),
                             static_cast<unsigned long long>(xbox_desc->kind1_size),
                             static_cast<unsigned long long>(xbox_desc->kind6_size),
                             static_cast<unsigned long long>(xbox_desc->kind7_size));
            return E_INVALIDARG;
        }
        pc_desc.CS.pShaderBytecode = payload.data();
        pc_desc.CS.BytecodeLength = payload.size();
    }

    pc_desc.NodeMask = 0;
    pc_desc.CachedPSO = {nullptr, 0};
    pc_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // Every failed call is logged with its number: the DLL creates its pipelines one after the other, and only the
    // first call of a slot is announced, so a miss on a later program would otherwise read as the first call's.
    static std::atomic<std::uint64_t> calls{0};
    const std::uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const HRESULT hr = self->real->CreateComputePipelineState(&pc_desc, *riid, out);
    if (FAILED(hr))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 67 call %llu failed with 0x%08lx (kind 0 %llu B, kind 1 %llu B, kind 6 %llu B, "
                     "kind 7 %llu B)\n",
                     static_cast<unsigned long long>(call), static_cast<unsigned long>(hr),
                     static_cast<unsigned long long>(xbox_desc->kind0_size),
                     static_cast<unsigned long long>(xbox_desc->kind1_size),
                     static_cast<unsigned long long>(xbox_desc->kind6_size),
                     static_cast<unsigned long long>(xbox_desc->kind7_size));
    return hr;
}

HRESULT proxy_create_resource(DeviceProxy* self, const D3D12_HEAP_PROPERTIES* heap_properties,
                              D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* xbox_desc,
                              D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE* clear_value,
                              const IID* riid, void** out) {
    note_slot(76);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!xbox_desc) return E_POINTER;
    if (!riid) return E_POINTER;

    // The measured description is 56 bytes plus Xbox extension words; D3D12_RESOURCE_DESC is exactly 56 bytes.
    // The Xbox layout word is 0x101 (tiled) or 0x1 (row-major buffer), and is not a PC layout value. Committed
    // resources take ROW_MAJOR for buffers and UNKNOWN for everything else, so the word is ignored and replaced.
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_layout = static_cast<std::uint32_t>(pc_desc.Layout);
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 76 resource: dimension=%d xbox layout=0x%x -> PC layout=0x%x\n",
                     static_cast<int>(pc_desc.Dimension), xbox_layout, static_cast<unsigned>(pc_desc.Layout));
    }
    return self->real->CreateCommittedResource(heap_properties, heap_flags, &pc_desc, initial_state, clear_value,
                                               *riid, out);
}

// The PC resource state a console D3D12_BARRIER_LAYOUT names. The console's resource creations carry a layout in the
// initial-state's place (docs/surface_population.md section 2); 0 is COMMON/PRESENT and 0xFFFFFFFF is "undefined",
// which is the PC's COMMON (contents undefined, any use) for a resource it makes now.
D3D12_RESOURCE_STATES state_from_barrier_layout(std::uint32_t layout) {
    switch (layout) {
        case 1: return D3D12_RESOURCE_STATE_GENERIC_READ;
        case 2: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case 3: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case 4: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case 5: return D3D12_RESOURCE_STATE_DEPTH_READ;
        case 6: return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        case 7: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case 8: return D3D12_RESOURCE_STATE_COPY_DEST;
        case 9: return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        case 10: return D3D12_RESOURCE_STATE_RESOLVE_DEST;
        default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

// Device slot 127 is the console's CreateCommittedResource with a D3D12_BARRIER_LAYOUT in the initial state's place:
// (heap properties, heap flags, desc, layout, clear value, protected session, castable-format count, castable formats,
// riid, out), measured on the retail executable (docs/surface_population.md section 2). The title creates its two
// display surfaces through it with heap flags 0x8 (D3D12_HEAP_FLAG_ALLOW_DISPLAY), and those surfaces are the ones
// the present names and two of the render-target views come from. The slot used to fall past the PC vtable into the
// beyond-vtable stub, which answers S_OK without writing the out pointer: the title then kept the null the stub never
// wrote in its own surface record and presented it. The PC resource is made from the console description the way slots
// 76/133 translate theirs, and the layout is mapped to the PC state it names; a host refusal is retried once with the
// state COMMON (what a PC may always start in), and then once without the console heap flags, each retry logged.
HRESULT proxy_create_committed_resource_layout(DeviceProxy* self, const D3D12_HEAP_PROPERTIES* heap_properties,
                                               D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* xbox_desc,
                                               std::uint32_t layout, const D3D12_CLEAR_VALUE* clear_value,
                                               const IID* riid, void** out) {
    note_slot(127);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!xbox_desc || !riid) return E_POINTER;
    // An integer mask: a D3D12_RESOURCE_FLAGS constant past the SDK's last enumerator is not a constant expression.
    constexpr std::uint32_t kPcResourceFlags = 0x5ff;
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_flags = static_cast<std::uint32_t>(pc_desc.Flags);
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags & kPcResourceFlags);
    const auto xbox_layout = static_cast<std::uint32_t>(pc_desc.Layout);
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // A console alignment is not one of the PC's two placement alignments: the driver picks its own.
    if (pc_desc.Alignment != 0 && pc_desc.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
        pc_desc.Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
        pc_desc.Alignment = 0;
    const D3D12_RESOURCE_STATES state = state_from_barrier_layout(layout);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 127 CreateCommittedResource with a barrier layout: dimension=%d width=%llu "
                     "format=%u xbox flags=0x%x -> PC flags=0x%x, layout word 0x%x, barrier layout %u -> state 0x%x, "
                     "heap flags 0x%x\n",
                     static_cast<int>(pc_desc.Dimension), static_cast<unsigned long long>(pc_desc.Width),
                     static_cast<unsigned>(pc_desc.Format), xbox_flags,
                     static_cast<unsigned>(static_cast<std::uint32_t>(pc_desc.Flags)), xbox_layout, layout,
                     static_cast<unsigned>(state), static_cast<unsigned>(static_cast<std::uint32_t>(heap_flags)));
    }
    HRESULT hr = self->real->CreateCommittedResource(heap_properties, heap_flags, &pc_desc, state, clear_value, *riid,
                                                     out);
    if (FAILED(hr) && state != D3D12_RESOURCE_STATE_COMMON) {
        const HRESULT retry = self->real->CreateCommittedResource(
            heap_properties, heap_flags, &pc_desc, D3D12_RESOURCE_STATE_COMMON, clear_value, *riid, out);
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 127: state 0x%x for barrier layout %u failed with 0x%08lx; as COMMON the "
                     "host answered 0x%08lx\n",
                     static_cast<unsigned>(state), layout, static_cast<unsigned long>(hr),
                     static_cast<unsigned long>(retry));
        hr = retry;
    }
    if (FAILED(hr) && heap_flags != D3D12_HEAP_FLAG_NONE) {
        const HRESULT retry =
            self->real->CreateCommittedResource(heap_properties, D3D12_HEAP_FLAG_NONE, &pc_desc, state, clear_value,
                                                *riid, out);
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 127: heap flags 0x%x failed with 0x%08lx; without them the host answered "
                     "0x%08lx\n",
                     static_cast<unsigned>(static_cast<std::uint32_t>(heap_flags)), static_cast<unsigned long>(hr),
                     static_cast<unsigned long>(retry));
        hr = retry;
    }
    return hr;
}

// Device slot 133 is the console's CreatePlacedResourceX: (va, desc, state, clear, riid, out), measured on the retail
// executable (docs/retail_boot_chain.md, section 3.4). `va` is the start of a physical-page range the title has just
// mapped, `desc` a 56-byte description with Xbox bits above the PC resource flags and an Xbox layout word, and
// `state` is 0xFFFFFFFF, which is no PC state. The resource is a buffer on those pages, made by the handler the
// device behind the proxy registered (set_placed_at_va_handler), with the PC flags kept and the layout made
// ROW_MAJOR, as slot 76 does.
std::atomic<PlacedAtVaHandler> g_placed_at_va{nullptr};
std::atomic<PlacedAtVaPagesHook> g_placed_at_va_pages{nullptr};

HRESULT proxy_create_placed_resource_x(DeviceProxy* self, void* va, const D3D12_RESOURCE_DESC* xbox_desc,
                                       std::uint32_t state, const IID* riid, void** out) {
    note_slot(133);
    if (out) *out = nullptr;
    // Without a handler (a PC device behind the proxy, or none registered) the slot refuses like any slot past the
    // PC vtable, whatever its arguments.
    const PlacedAtVaHandler handler = g_placed_at_va.load(std::memory_order_acquire);
    if (!handler) return E_NOTIMPL;
    if (!out || !xbox_desc || !riid) return E_POINTER;
    // An integer mask: a D3D12_RESOURCE_FLAGS constant past the SDK's last enumerator is not a constant expression
    // for clang-cl with SDK 10.0.26100 (found in review). The cast is applied to the masked value at run time.
    // The PC flags up to RAYTRACING_ACCELERATION_STRUCTURE (0x100) and USE_TIGHT_ALIGNMENT (0x400); 0x200 is
    // dropped, as committed_resource_refusal says the host refuses it (found in review).
    constexpr std::uint32_t kPcResourceFlags = 0x5ff;
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_flags = static_cast<std::uint32_t>(pc_desc.Flags);
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags & kPcResourceFlags);
    // The Xbox layout word (0x101 tiled, 0x1 row-major buffer) is no PC layout: ROW_MAJOR for a buffer and UNKNOWN
    // for everything else, as slot 76 does.
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 133 CreatePlacedResourceX: va=%p dimension=%d width=%llu xbox flags=0x%x "
                     "state=0x%x -> a buffer on the title's pages\n",
                     va, static_cast<int>(pc_desc.Dimension), static_cast<unsigned long long>(pc_desc.Width),
                     xbox_flags, state);
    }
    // The buffer's pages are committed before the handler walks them (set_placed_at_va_pages_hook).
    const PlacedAtVaPagesHook pages = g_placed_at_va_pages.load(std::memory_order_acquire);
    if (pages && pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && pc_desc.Width &&
        !pages(va, static_cast<std::size_t>(pc_desc.Width))) {
        static std::atomic<bool> refused{false};
        if (!refused.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[rt] d3d12_xs: slot 133: committing the pages of va=%p width=%llu failed (error %lu)\n",
                         va, static_cast<unsigned long long>(pc_desc.Width), GetLastError());
        }
    }
    return handler(self->real, va, pc_desc, state, *riid, out);
}

template <unsigned Slot>
std::uint64_t WINAPI proxy_slot(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if (callers_enabled()) note_caller(CallerKind::device, Slot, _ReturnAddress(), a1, a2, a3, a4);
    if constexpr (Slot == 0) {
        return hr_to_u64(proxy_query_interface(self, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return proxy_add_ref(self);
    } else if constexpr (Slot == 2) {
        return proxy_release(self);
    } else if constexpr (Slot == 8) {
        return hr_to_u64(proxy_create_command_queue(self, reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1),
                                                    reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 9) {
        return hr_to_u64(proxy_create_command_allocator(self, a1, reinterpret_cast<const IID*>(a2),
                                                        reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 12) {
        note_slot(12);
        a3 = unwrap_object(a3);
        if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(12, self->slot_count));
        using CreateListFn = HRESULT(WINAPI*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                              ID3D12PipelineState*, const IID*, void**);
        auto fn = reinterpret_cast<CreateListFn>(self->real_vtable[12]);
        return hr_to_u64(fn(self->real, static_cast<UINT>(a1), static_cast<D3D12_COMMAND_LIST_TYPE>(a2),
                            reinterpret_cast<ID3D12CommandAllocator*>(a3),
                            reinterpret_cast<ID3D12PipelineState*>(a4),
                            reinterpret_cast<const IID*>(a5),
                            reinterpret_cast<void**>(a6)));
    } else if constexpr (Slot == 36) {
        return hr_to_u64(proxy_create_fence(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                            reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 32) {
        return hr_to_u64(proxy_open_shared_handle(self, a1, reinterpret_cast<const IID*>(a2),
                                                  reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 31 || Slot == 34 || Slot == 35 || Slot == 45 || Slot == 46 || Slot == 50) {
        return hr_to_u64(proxy_unwrapping_call<Slot>(self, a1, a2, a3, a4, a5));
    } else if constexpr (Slot == 75) {
        return hr_to_u64(proxy_create_command_queue1(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                                     reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 14) {
        return hr_to_u64(proxy_create_descriptor_heap(
            self, reinterpret_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(a1),
            reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 16) {
        // CreateRootSignature, forwarded unchanged. The created PC object is remembered so slot 57 can prove that the
        // root signature a record names at +0x00 is one this device handed out (the root-signature table above); with
        // no proof the builder refuses it as pRootSignature instead of the layer calling through an unchecked pointer.
        note_slot(16);
        if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(16, self->slot_count));
        if (!a4) return hr_to_u64(E_POINTER);
        auto* const out = reinterpret_cast<void**>(a5);
        const HRESULT hr = self->real->CreateRootSignature(
            static_cast<UINT>(a1), reinterpret_cast<const void*>(a2), static_cast<SIZE_T>(a3),
            *reinterpret_cast<const IID*>(a4), out);
        if (SUCCEEDED(hr) && out && *out) note_root_signature(*out);
        return hr_to_u64(hr);
    } else if constexpr (Slot == 61) {
        // The console calls this slot with two arguments from 192 sites during pipeline setup, while the PC interface's
        // slot 61 is CreateMetaCommand (three arguments): forwarding by number makes the layer read -1. Identify what
        // the first argument points at before deciding what this slot is.
        note_slot(61);
        {
            static std::atomic<bool> logged{false};
            if (trace_enabled() && !logged.exchange(true, std::memory_order_relaxed)) {
                std::fprintf(stderr, "[rt] slot 61 probe: a1=%p a2=0x%llx a3=%p a4=0x%llx\n",
                             reinterpret_cast<void*>(a1), static_cast<unsigned long long>(a2),
                             reinterpret_cast<void*>(a3), static_cast<unsigned long long>(a4));
                if (a1 && readable_range(reinterpret_cast<const void*>(a1), 32)) {
                    const auto* d = reinterpret_cast<const std::uint32_t*>(a1);
                    std::fprintf(stderr, "[rt] slot 61 first words: %08x %08x %08x %08x | %08x %08x %08x %08x\n",
                                 d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
                } else {
                    std::fprintf(stderr, "[rt] slot 61: a1 is not readable\n");
                }
            }
        }
        // The first argument holds small integers rather than a graphics pipeline description, and the second is a
        // title address, so this is not the PC CreateMetaCommand the slot number would forward to. Refuse instead of
        // calling a method with the wrong arity; the probe above already names the arguments.
        return hr_to_u64(E_NOTIMPL);
    } else if constexpr (Slot == 55) {
        // Console #55 is CreateDerivedPipelineStateX, not PC #55 CreateReservedResource1: (base, count, patches, riid,
        // out), with the out pointer in the fifth position (docs/device_slot55.md).
        return hr_to_u64(proxy_create_derived_pipeline_state(
            self, a1, a2, a3, reinterpret_cast<const IID*>(a4), reinterpret_cast<void**>(a5)));
    } else if constexpr (Slot == 57) {
        return hr_to_u64(proxy_create_graphics_pipeline_probe(
            self, reinterpret_cast<const void*>(a1), reinterpret_cast<const IID*>(a2),
            reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 62) {
        return hr_to_u64(proxy_create_compute_pipeline_state(
            self, reinterpret_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(a1), a2, a3,
            reinterpret_cast<const IID*>(a4), reinterpret_cast<void**>(a5)));
    } else if constexpr (Slot == 67) {
        return hr_to_u64(proxy_create_compute_pipeline_from_native_code(
            self, reinterpret_cast<const void*>(a1), reinterpret_cast<const IID*>(a2),
            reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 133) {
        return hr_to_u64(proxy_create_placed_resource_x(self, reinterpret_cast<void*>(a1),
                                                        reinterpret_cast<const D3D12_RESOURCE_DESC*>(a2),
                                                        static_cast<std::uint32_t>(a3), reinterpret_cast<const IID*>(a5),
                                                        reinterpret_cast<void**>(a6)));
    } else if constexpr (Slot == 25) {
        // GetResourceAllocationInfo returns D3D12_RESOURCE_ALLOCATION_INFO (16 bytes) by value: the same class as
        // slot 26 (docs/boot_audit.md A1). The console order puts the hidden return buffer in RCX, which this thunk
        // receives as `self`, and the device proxy in RDX (`a1`).
        note_slot(25);
        if (is_device_proxy(reinterpret_cast<const void*>(a1))) {
            auto* device = reinterpret_cast<DeviceProxy*>(a1);
            if (device->real && device->real_vtable) {
                const D3D12_RESOURCE_ALLOCATION_INFO info = device->real->GetResourceAllocationInfo(
                    static_cast<UINT>(a2), static_cast<UINT>(a3),
                    reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4));
                std::memcpy(reinterpret_cast<void*>(self), &info, sizeof(info));
                return reinterpret_cast<std::uint64_t>(self);
            }
        }
        // PC order: the device is `self`, the hidden return buffer is a1.
        if (a1) {
            const D3D12_RESOURCE_ALLOCATION_INFO info = self->real->GetResourceAllocationInfo(
                static_cast<UINT>(a2), static_cast<UINT>(a3),
                reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4));
            std::memcpy(reinterpret_cast<void*>(a1), &info, sizeof(info));
            return a1;
        }
        return 0;
    } else if constexpr (Slot == 26) {
        note_slot(26);
        // GetCustomHeapProperties returns D3D12_HEAP_PROPERTIES by value. The retail executable passes the hidden
        // return buffer in RCX, which this thunk receives as `self`, and the device proxy in RDX (`a1`); a PC member
        // takes the device in RCX and the buffer in RDX. docs/retail_boot_chain.md section 3.1.
        //
        // The PC method must be called as a member: calling its vtable entry directly with (buffer, real, ...) hands
        // it the buffer as `this` and the device object as the return buffer, so it writes the structure over the
        // device's first words and every later virtual call on that device faults. Copy its result instead.
        if (is_device_proxy(reinterpret_cast<const void*>(a1))) {
            auto* device = reinterpret_cast<DeviceProxy*>(a1);
            if (device->real && device->real_vtable) {
                if (trace_enabled())
                    std::fprintf(stderr, "[rt] d3d12_xs: device slot 26 (GetCustomHeapProperties): console order\n");
                const D3D12_HEAP_PROPERTIES properties = device->real->GetCustomHeapProperties(
                    static_cast<UINT>(a2), static_cast<D3D12_HEAP_TYPE>(static_cast<std::uint32_t>(a3)));
                std::memcpy(reinterpret_cast<void*>(self), &properties, sizeof(properties));
                // The console convention returns the buffer's address in RAX; the caller reads its result there.
                return reinterpret_cast<std::uint64_t>(self);
            }
        }
        // PC order: the device is `self` and the hidden return buffer is `a1`. Fill it and return its address, so this
        // branch can never fall off the end of the function (the regression the audit found, and the shape the
        // GPU-free tests call).
        if (a1) {
            const D3D12_HEAP_PROPERTIES properties = self->real->GetCustomHeapProperties(
                static_cast<UINT>(a2), static_cast<D3D12_HEAP_TYPE>(static_cast<std::uint32_t>(a3)));
            std::memcpy(reinterpret_cast<void*>(a1), &properties, sizeof(properties));
            return a1;
        }
        return 0;
    } else if constexpr (Slot == 70) {
        note_slot(70);
        // Measured from the title's own call site (a small thunk that loads the device from a global and then does
        // `mov rdx, rcx; mov r8, rdx; mov r9, r8; call [rax+0x230]`): the console calls this slot with THREE arguments,
        // not the seven PC CreatePlacedResource1 takes. Forwarding it by number therefore handed that method the
        // leftovers of four unrelated registers as its description, state, clear, riid and out pointers, and its first
        // write went to an address inside the title's own image. Until the three-argument shape is identified, refuse
        // this slot instead of calling a method with the wrong arity.
        if (trace_enabled())
            std::fprintf(stderr,
                         "[rt] d3d12_xs: device slot 70 called with 3 arguments (0x%llx, 0x%llx, 0x%llx); the console "
                         "shape is not the PC CreatePlacedResource1 one, refusing\n",
                         static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                         static_cast<unsigned long long>(a3));
        return hr_to_u64(E_NOTIMPL);
    } else if constexpr (Slot == 108) {
        // Console extension: GetResourceAllocationInfo in the console's struct-return order. The hidden 16-byte return
        // buffer arrives in RCX (this thunk's `self`), the device proxy in RDX (`a1`), the two counts in R8/R9 and the
        // description array on the stack (`a4`). Measured at the title's call sites: it reads the 16 bytes back and
        // compares the alignment with 0x1000, then treats the size's high dword as an assert. Answering success without
        // writing the buffer left a garbage size, the title's allocator asserted and the resource factory returned null,
        // which is what the boot then dereferenced.
        note_slot(Slot);
        if (is_device_proxy(reinterpret_cast<const void*>(a1))) {
            auto* device = reinterpret_cast<DeviceProxy*>(a1);
            if (device->real && device->real_vtable) {
                const auto* descs = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4);
                D3D12_RESOURCE_ALLOCATION_INFO info{};
                bool answered = false;
                if (descs && readable_range(descs, sizeof(D3D12_RESOURCE_DESC)) &&
                    descs->Dimension <= D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                    info = device->real->GetResourceAllocationInfo(static_cast<UINT>(a2), static_cast<UINT>(a3), descs);
                    // D3D12 reports an unsupported size as UINT64_MAX, not 0; treating it as an answer published an
                    // allocation the title's allocator then asserted on, so it counts as unanswered here.
                    answered = info.SizeInBytes != 0 && info.SizeInBytes != UINT64_MAX;
                }
                if (!answered) {
                    // A description the host cannot size: report one 64 KiB-aligned allocation, which is the console
                    // alignment this caller expects and never over-commits.
                    static std::atomic<bool> logged{false};
                    if (!logged.exchange(true, std::memory_order_relaxed))
                        std::fprintf(stderr,
                                     "[rt] d3d12_xs: slot 108 could not size the description, reporting 64 KiB / "
                                     "64 KiB alignment\n");
                    info.Alignment = 0x10000;
                    info.SizeInBytes = 0x10000;
                }
                std::memcpy(reinterpret_cast<void*>(self), &info, sizeof(info));
                // The fifth stack argument is the per-description record array (offset, alignment, size per desc).
                auto* records = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO1*>(a5);
                if (records && readable_range(records, sizeof(D3D12_RESOURCE_ALLOCATION_INFO1))) {
                    records[0].Offset = 0;
                    records[0].Alignment = info.Alignment;
                    records[0].SizeInBytes = info.SizeInBytes;
                }
                return reinterpret_cast<std::uint64_t>(self);
            }
        }
        return hr_to_u64(E_NOTIMPL);
    } else if constexpr (Slot == 109) {
        // Console extension measured like 108 but not on this path yet: publish zero into a caller stack slot only, which
        // is all its measured call sites expect.
        note_slot(Slot);
        const auto here109 = static_cast<std::uintptr_t>(static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(&a1)));
        const auto target109 = static_cast<std::uintptr_t>(a1);
        const auto distance109 = target109 > here109 ? target109 - here109 : here109 - target109;
        if (a1 && distance109 < (16u << 20) &&
            readable_range(reinterpret_cast<const void*>(a1), sizeof(std::uint32_t)))
            *reinterpret_cast<std::uint32_t*>(a1) = 0;
        return hr_to_u64(S_OK);
    } else if constexpr (Slot == 76) {
        return hr_to_u64(proxy_create_resource(
            self, reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(a1),
            static_cast<D3D12_HEAP_FLAGS>(static_cast<std::uint32_t>(a2)),
            reinterpret_cast<const D3D12_RESOURCE_DESC*>(a3),
            static_cast<D3D12_RESOURCE_STATES>(static_cast<std::uint32_t>(a4)),
            reinterpret_cast<const D3D12_CLEAR_VALUE*>(a5), reinterpret_cast<const IID*>(a6),
            reinterpret_cast<void**>(a7)));
    } else if constexpr (Slot == 127) {
        // The title's display-surface creation (D3D12_HEAP_FLAG_ALLOW_DISPLAY) is this slot; it used to be answered
        // by the beyond-vtable stub with S_OK and no out write (docs/surface_population.md).
        return hr_to_u64(proxy_create_committed_resource_layout(
            self, reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(a1),
            static_cast<D3D12_HEAP_FLAGS>(static_cast<std::uint32_t>(a2)),
            reinterpret_cast<const D3D12_RESOURCE_DESC*>(a3), static_cast<std::uint32_t>(a4),
            reinterpret_cast<const D3D12_CLEAR_VALUE*>(a5), reinterpret_cast<const IID*>(a9),
            reinterpret_cast<void**>(a10)));
    } else if constexpr (Slot == 20) {
        // ID3D12Device::CreateRenderTargetView, forwarded unchanged. This branch exists only for the measurement probe:
        // it records the destination handle and the resource, then prints the capture the layer writes there, so a
        // console ClearRenderTargetView handle can be compared with the view that was created for it
        // (PALMETTO_XS_CLEAR_BARRIER_PROBE=1).
        note_slot(Slot);
        if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(Slot, self->slot_count));
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t);
        const std::uint64_t result = reinterpret_cast<ForwardFn>(self->real_vtable[Slot])(
            self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
        if (a3) {
            // Record what this destination was created from (used by translate_console_clear to tell a null view the
            // proxy made from a handle it never saw) and, with the measurement switch on, print the capture.
            if (clear_barrier_probe()) {
                static std::atomic<unsigned> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 32) print_target_view("create", Slot, a3, a1);
            }
            note_rtv_destination(a3, a1);
        }
        return result;
    } else {
        note_slot(Slot);
        // Xbox device slots, before the PC vtable is indexed: the public HLE's 78/79/80 would otherwise reach
        // CreateReservedResource2/CreateSampler2 or fall past an 80-slot table, and 48/73 would reach unrelated PC
        // methods. The check is before the slot-count refusal because slot 80 is outside a PC ID3D12Device11 table.
        const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
        if (Slot == routes.frame_interval) {
            log_xbox_route_once(Slot, "SetFrameIntervalX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_set_frame_interval_x(self, a1, a2, a3, a4));
        }
        if (Slot == routes.schedule_event) {
            log_xbox_route_once(Slot, "ScheduleFrameEventX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_schedule_frame_event_x(self, a1, a2, a4));
        }
        if (Slot == routes.wait_event) {
            log_xbox_route_once(Slot, "WaitFrameEventX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_wait_frame_event_x(self, a1, a2, a4, a5));
        }
        if (Slot == routes.queue_x) {
            log_xbox_route_once(Slot, "CreateCommandQueueX (PC desc from the 16-byte prefix, Flags masked)", a1, a2,
                                a3, a4, a5);
            // The console description starts with a PC D3D12_COMMAND_QUEUE_DESC and continues with Xbox words
            // (docs/retail_boot_chain.md section 3.2). The PC method reads exactly those 16 bytes, so copy the prefix
            // and mask Flags to the one bit the host accepts: the console carries bits the Vulkan layer refuses,
            // which is what "CreateCommandQueue refused (type, priority, flags or node mask), type 2" reports.
            D3D12_COMMAND_QUEUE_DESC pc_desc{};
            if (a1) std::memcpy(&pc_desc, reinterpret_cast<const void*>(a1), sizeof(pc_desc));
            pc_desc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(
                static_cast<std::uint32_t>(pc_desc.Flags) &
                static_cast<std::uint32_t>(D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT));
            return hr_to_u64(proxy_create_command_queue(self, a1 ? &pc_desc : nullptr,
                                                        reinterpret_cast<const IID*>(a2),
                                                        reinterpret_cast<void**>(a3)));
        }
        if (Slot == routes.list_x) {
            log_xbox_route_once(Slot, "CreateCommandListX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_create_command_list_x(self, a1, a2, a3, reinterpret_cast<const IID*>(a4),
                                                         reinterpret_cast<void**>(a5)));
        }
        if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(Slot, self->slot_count));

        // The PC vtable entry, called with the PC device as `this`. Integer/pointer arguments, the hidden
        // struct-return pointer and any stack arguments keep their positions; XMM registers are untouched.
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <std::size_t... I>
void fill_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&proxy_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_vtable() {
    static void** vtable = [] {
        static void* storage[kMaxSlots];
        fill_vtable(storage, std::make_index_sequence<kMaxSlots>{});
        return storage;
    }();
    return vtable;
}

bool supports(ID3D12Device* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// Highest device interface the PC runtime exposes; its cumulative slot count (from d3d12.h) bounds forwarding.
// Windows SDK 10.0.26100 defines ID3D12Device through ID3D12Device14 only; there is no Device15/Device16 header.
unsigned detect_slot_count(ID3D12Device* real) {
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device14))) return 83;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device13))) return 82;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device12))) return 81;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device11))) return 80;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device10))) return 79;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device9))) return 76;
#endif
    if (supports(real, __uuidof(ID3D12Device8))) return 73;
    if (supports(real, __uuidof(ID3D12Device7))) return 68;
    if (supports(real, __uuidof(ID3D12Device6))) return 66;
    if (supports(real, __uuidof(ID3D12Device5))) return 65;
    if (supports(real, __uuidof(ID3D12Device4))) return 57;
    if (supports(real, __uuidof(ID3D12Device3))) return 51;
    if (supports(real, __uuidof(ID3D12Device2))) return 48;
    if (supports(real, __uuidof(ID3D12Device1))) return 47;
    return 44;
}

}  // namespace

HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out) {
    if (!real) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    DeviceProxy* proxy = new (std::nothrow) DeviceProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_vtable();
    proxy->real = real;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->slot_count = detect_slot_count(real);
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->dxgi_vtable = get_dxgi_vtable();
    proxy->adapter_provider = nullptr;
    proxy->gpu_thread_priority.store(0, std::memory_order_relaxed);
    proxy->max_frame_latency.store(3, std::memory_order_relaxed);

    const unsigned slot_count = proxy->slot_count;
    HRESULT hr = proxy_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its reference to `real`.
        delete proxy;
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: device proxy %p wraps PC device %p (%u PC vtable slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), slot_count);
    proxy_release(proxy);  // drop the creation reference; `out` holds the caller's reference
    return S_OK;
}

bool is_device_proxy(const void* device) {
    return device && *reinterpret_cast<void* const*>(device) == reinterpret_cast<void*>(get_vtable());
}

void* device_proxy_from_dxgi_device(void* dxgi_device) {
    if (!dxgi_device || *static_cast<void***>(dxgi_device) != get_dxgi_vtable()) return nullptr;
    return dxgi_owner(dxgi_device);
}

void* device_proxy_primary(void* device) {
    if (is_device_proxy(device)) return device;
    return device_proxy_from_dxgi_device(device);
}

bool set_device_adapter_provider(void* device, DxgiAdapterProvider provider) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) {
        // Not installing one silently would leave the host's adapter in its place.
        std::fprintf(stderr, "[rt] d3d12_xs: set_device_adapter_provider: %p is not a device proxy; no provider set\n",
                     device);
        return false;
    }
    proxy->adapter_provider = provider;
    return true;
}

bool is_heap_proxy(const void* heap) {
    return heap && *reinterpret_cast<void* const*>(heap) == reinterpret_cast<void*>(get_heap_vtable());
}

ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap) {
    if (!is_heap_proxy(heap)) return nullptr;
    return static_cast<const HeapProxy*>(heap)->real;
}

unsigned device_proxy_slot_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->slot_count;
}

void set_placed_at_va_handler(PlacedAtVaHandler handler) {
    g_placed_at_va.store(handler, std::memory_order_release);
}

PlacedAtVaHandler placed_at_va_handler() { return g_placed_at_va.load(); }
PlacedAtVaPagesHook placed_at_va_pages_hook() { return g_placed_at_va_pages.load(); }

void set_placed_at_va_pages_hook(PlacedAtVaPagesHook hook) {
    g_placed_at_va_pages.store(hook, std::memory_order_release);
}

ID3D12Device* device_proxy_real_device(const void* device) {
    if (!is_device_proxy(device)) return nullptr;
    return static_cast<const DeviceProxy*>(device)->real;
}

bool device_slot_overridden(unsigned slot) {
    if (slot <= 2 || slot == 25 || slot == 26 || slot == 8 || slot == 9 || slot == 14 || slot == 31 || slot == 32 || slot == 34 || slot == 35 ||
        slot == 36 || slot == 45 || slot == 46 || slot == 50 || slot == 55 || slot == 62 || slot == 67 ||
        slot == 75 || slot == 76 || slot == 127 || slot == 133)
        return true;
    const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
    return slot == routes.frame_interval || slot == routes.schedule_event || slot == routes.wait_event ||
           slot == routes.queue_x || slot == routes.list_x;
}

bool is_queue_proxy(const void* queue) { return is_child_proxy<ChildKind::queue>(queue); }
bool is_fence_proxy(const void* fence) { return is_child_proxy<ChildKind::fence>(fence); }
bool is_allocator_proxy(const void* allocator) { return is_child_proxy<ChildKind::allocator>(allocator); }

ID3D12CommandAllocator* allocator_proxy_real_allocator(const void* allocator) {
    if (!is_allocator_proxy(allocator)) return nullptr;
    return reinterpret_cast<ID3D12CommandAllocator*>(static_cast<const ChildProxy*>(allocator)->real);
}

ID3D12CommandQueue* queue_proxy_real_queue(const void* queue) {
    if (!is_queue_proxy(queue)) return nullptr;
    return reinterpret_cast<ID3D12CommandQueue*>(static_cast<const ChildProxy*>(queue)->real);
}

ID3D12Fence* fence_proxy_real_fence(const void* fence) {
    if (!is_fence_proxy(fence)) return nullptr;
    return reinterpret_cast<ID3D12Fence*>(static_cast<const ChildProxy*>(fence)->real);
}

unsigned queue_proxy_slot_count(const void* queue) {
    return is_queue_proxy(queue) ? static_cast<const ChildProxy*>(queue)->slot_count : 0;
}

unsigned fence_proxy_slot_count(const void* fence) {
    return is_fence_proxy(fence) ? static_cast<const ChildProxy*>(fence)->slot_count : 0;
}

unsigned queue_proxy_table_size() { return kQueueSlots; }
unsigned fence_proxy_table_size() { return kFenceSlots; }
unsigned allocator_proxy_table_size() { return kAllocatorSlots; }
unsigned allocator_proxy_slot_count(const void* allocator) {
    return is_allocator_proxy(allocator) ? static_cast<const ChildProxy*>(allocator)->slot_count : 0;
}

unsigned long command_object_proxy_ref_count(const void* object) {
    if (!is_queue_proxy(object) && !is_fence_proxy(object) && !is_allocator_proxy(object)) return 0;
    return static_cast<const ChildProxy*>(object)->refs.load(std::memory_order_relaxed);
}

std::uint64_t queue_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool queue_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t fence_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool fence_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t queue_table_overruns() {
    return ChildStats<ChildKind::queue>::overruns.load(std::memory_order_relaxed);
}

std::uint64_t fence_table_overruns() {
    return ChildStats<ChildKind::fence>::overruns.load(std::memory_order_relaxed);
}

bool command_object_tables_guarded() {
    get_child_vtable<ChildKind::queue>();
    get_child_vtable<ChildKind::fence>();
    get_child_vtable<ChildKind::allocator>();
    return ChildStats<ChildKind::queue>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::fence>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::allocator>::table_end.load(std::memory_order_acquire);
}

std::uint64_t allocator_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool allocator_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t allocator_table_overruns() {
    return ChildStats<ChildKind::allocator>::overruns.load(std::memory_order_relaxed);
}

bool heap_slot_overridden(unsigned slot) {
    return slot <= 2 || slot == 7 || slot == 9 || slot == 10;
}

std::uint64_t device_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kMaxSlots ? g_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool device_slot_first_call_seen(unsigned slot) {
    return slot < kMaxSlots && g_first[slot].load(std::memory_order_relaxed);
}

unsigned long device_proxy_ref_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->refs.load(std::memory_order_relaxed);
}

std::uint64_t heap_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kHeapSlots ? g_heap_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool heap_slot_first_call_seen(unsigned slot) {
    return slot < kHeapSlots && g_heap_first[slot].load(std::memory_order_relaxed);
}

bool is_command_list_proxy(const void* list) {
    return list && *reinterpret_cast<void* const*>(list) == reinterpret_cast<void*>(get_list_vtable());
}

ID3D12GraphicsCommandList* command_list_proxy_real_list(const void* list) {
    if (!is_command_list_proxy(list)) return nullptr;
    return static_cast<const ListProxy*>(list)->real;
}

unsigned command_list_proxy_table_size() { return kListSlots; }

std::uint64_t list_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kListSlots ? ListStats::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool list_slot_first_call_seen(unsigned slot) {
    return slot < kListSlots && ListStats::first[slot].load(std::memory_order_relaxed);
}

void reset_device_proxy_stats() {
    start_hit_counts();
    for (unsigned slot = 0; slot < kMaxSlots; ++slot) {
        g_hits[slot].store(0, std::memory_order_relaxed);
        g_first[slot].store(false, std::memory_order_relaxed);
        g_beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kHeapSlots; ++slot) {
        g_heap_hits[slot].store(0, std::memory_order_relaxed);
        g_heap_first[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kListSlots; ++slot) {
        ListStats::hits[slot].store(0, std::memory_order_relaxed);
        ListStats::first[slot].store(false, std::memory_order_relaxed);
    }
    reset_child_stats<ChildKind::queue>();
    reset_child_stats<ChildKind::fence>();
    reset_child_stats<ChildKind::allocator>();
}

}  // namespace rt::d3d12_xs
