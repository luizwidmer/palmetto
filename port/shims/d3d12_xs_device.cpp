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
#include "startup_trace.h"  // rt::startup_trace::detail::crash_image_base: the mapped title's base for the slot-127 hook
#include "xmem.h"  // rt::xmem::copy_page_mappings_fn, the mapping primitive of queue slot 31
#include <tlhelp32.h>
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
#include <cstdarg>
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

// ---- Streamed DirectStorage block ranges (measurement only) -------------------------------------------------------
//
// docs/streamed_consumer.md. dstorage.cpp (the same executable) reports every block it has just decompressed into a
// title address through palmetto_stream_block_completed. The Xbox-ABI device proxy below is the only layer between the
// title and the PC device, so a consumer's GPU-side step would show up here first: a heap created from the block's
// address, a placed resource at it, or any other device call whose argument is a pointer into it. With the shim's
// PALMETTO_STREAM_CONSUMER unset the registry stays empty and every check is two relaxed atomic loads; with it set, a
// match prints (bounded) and each instrumented slot prints its first call. Nothing here changes behaviour.
//
// The title image base is published once by dstorage.cpp (from its enqueue caller) so a caller's return address can be
// printed as game+RVA without a module lookup - this file is also linked into GPU-free test binaries that have no shim
// registry.
namespace streamed_blocks {

constexpr unsigned kRanges = 8192;

struct Range {
    std::atomic<std::uint64_t> va{0};
    std::atomic<std::uint64_t> bytes{0};
};

Range g_ranges[kRanges];
std::atomic<unsigned> g_count{0};
std::atomic<std::uint64_t> g_min{~std::uint64_t(0)};
std::atomic<std::uint64_t> g_max{0};
std::atomic<std::uint64_t> g_title_base{0};
std::atomic<unsigned> g_match_lines{0};

bool active() { return g_count.load(std::memory_order_relaxed) != 0; }

void set_title_base(std::uint64_t base) { g_title_base.store(base, std::memory_order_relaxed); }

void add(void* va, std::size_t bytes) {
    if (!va || bytes == 0) return;
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(va));
    const unsigned index = g_count.fetch_add(1, std::memory_order_relaxed);
    if (index < kRanges) {
        g_ranges[index].va.store(address, std::memory_order_relaxed);
        g_ranges[index].bytes.store(bytes, std::memory_order_relaxed);
    }
    std::uint64_t previous_min = g_min.load(std::memory_order_relaxed);
    while (address < previous_min &&
           !g_min.compare_exchange_weak(previous_min, address, std::memory_order_relaxed)) {
    }
    const std::uint64_t end = address + bytes;
    std::uint64_t previous_max = g_max.load(std::memory_order_relaxed);
    while (end > previous_max &&
           !g_max.compare_exchange_weak(previous_max, end, std::memory_order_relaxed)) {
    }
}

// Index of the first recorded block that intersects [address, address+bytes), or -1. The min/max prefilter rejects every
// ordinary pointer (and null) before the table is walked, so a boot with the switch on pays two atomic loads per call.
int find(std::uint64_t address, std::uint64_t bytes) {
    if (address == 0 || bytes == 0) return -1;
    const std::uint64_t min = g_min.load(std::memory_order_relaxed);
    const std::uint64_t max = g_max.load(std::memory_order_relaxed);
    if (max <= min || address >= max || address + bytes <= min) return -1;
    const unsigned count = std::min(g_count.load(std::memory_order_relaxed), kRanges);
    for (unsigned i = 0; i < count; ++i) {
        const std::uint64_t va = g_ranges[i].va.load(std::memory_order_relaxed);
        const std::uint64_t length = g_ranges[i].bytes.load(std::memory_order_relaxed);
        if (length == 0 || va == 0) continue;
        if (address < va + length && va < address + bytes) return static_cast<int>(i);
    }
    return -1;
}

// A caller's return address as "game+0x..." when it lies in the title image's 256 MiB window, else the raw address.
void caller_text(std::uint64_t caller, char* out, std::size_t capacity) {
    const std::uint64_t base = g_title_base.load(std::memory_order_relaxed);
    if (caller && base && caller >= base && caller - base < 0x10000000ull)
        std::snprintf(out, capacity, "game+0x%llx", static_cast<unsigned long long>(caller - base));
    else
        std::snprintf(out, capacity, "0x%llx (not the title image)", static_cast<unsigned long long>(caller));
}

// The RVA of a caller when it lies in the title image, false otherwise.
bool caller_rva(std::uint64_t caller, std::uint64_t& rva) {
    const std::uint64_t base = g_title_base.load(std::memory_order_relaxed);
    if (!caller || !base || caller < base || caller - base >= 0x10000000ull) return false;
    rva = caller - base;
    return true;
}

}  // namespace streamed_blocks

extern "C" void palmetto_stream_block_completed(void* va, std::size_t bytes) {
    streamed_blocks::add(va, bytes);
}

extern "C" void palmetto_stream_set_title_base(std::uint64_t base) { streamed_blocks::set_title_base(base); }

// For the next increment (docs/streamed_consumer.md): a caller outside this file - the XG texture-computer shim - can ask
// whether an address range is one of the recorded DirectStorage blocks without reading the registry. -1 when none. Only
// meaningful while dstorage.cpp's PALMETTO_STREAM_CONSUMER filled the registry; both calls are cheap otherwise.
extern "C" int palmetto_streamed_block_index(const void* address, std::size_t bytes) {
    return streamed_blocks::find(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address)), bytes);
}

extern "C" unsigned palmetto_streamed_block_count() {
    return streamed_blocks::g_count.load(std::memory_order_relaxed);
}

// Loader thread ids, reported by the DirectStorage shim (dstorage.cpp) and by the streamed-block guard. The load-window
// trace logs every device call from these threads (the loading threads), while the frame loop's device calls are
// filtered out; queue/fence calls are logged for the first four per slot so the frame loop's call shapes stay visible
// once. Plain atomics: the guard's fault handler registers a tid too, so nothing here may lock or allocate.
namespace load_tids {
constexpr unsigned kMax = 64;
std::atomic<unsigned long> g_tids[kMax];
std::atomic<unsigned> g_count{0};

bool contains(unsigned long tid) {
    if (tid == 0) return false;
    const unsigned count = g_count.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < count && i < kMax; ++i)
        if (g_tids[i].load(std::memory_order_relaxed) == tid) return true;
    return false;
}

void note(unsigned long tid) {
    if (tid == 0 || contains(tid)) return;
    const unsigned index = g_count.fetch_add(1, std::memory_order_relaxed);
    if (index < kMax) g_tids[index].store(tid, std::memory_order_relaxed);
}
}  // namespace load_tids

extern "C" void palmetto_load_tid_note(unsigned long tid) { load_tids::note(tid); }

// ---- PALMETTO_PIPELINE_TIMELINE: the pipeline-conversion traffic as a time series ----------------------------------
//
// docs/streamed_consumer.md section 6e. One line every N seconds (PALMETTO_PIPELINE_TIMELINE=<seconds>, off by default)
// with wall time and cumulative counters: slot-57 record calls / converted / refused, slot-55 derived calls, graphics
// and compute pipelines created, submissions, presents, the console-call total, the process thread count and the last
// slot-57 caller. Counters only (no game data); bounded to 240 lines; changes no call.
std::atomic<std::uint64_t> g_pl_slot57_calls{0};
std::atomic<std::uint64_t> g_pl_slot57_converted{0};
std::atomic<std::uint64_t> g_pl_slot57_refused{0};
std::atomic<std::uint64_t> g_pl_slot55_calls{0};
std::atomic<std::uint64_t> g_pl_graphics_created{0};
std::atomic<std::uint64_t> g_pl_compute_calls{0};
std::atomic<std::uint64_t> g_pl_compute_created{0};
std::atomic<std::uint64_t> g_pl_submits{0};
std::atomic<std::uint64_t> g_pl_presents{0};
std::atomic<std::uint64_t> g_pl_console_calls{0};
std::atomic<std::uint64_t> g_pl_last_slot57_caller{0};
std::atomic<std::uint64_t> g_pl_last_event_us{0};

double pipeline_timeline_period() {
    static const double seconds = [] {
        char value[32] = {};
        if (GetEnvironmentVariableA("PALMETTO_PIPELINE_TIMELINE", value, sizeof value) == 0 || value[0] == '\0')
            return 0.0;
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        return end != value && parsed > 0.0 ? parsed : 0.0;
    }();
    return seconds;
}

std::uint64_t pipeline_timeline_us() {
    static const auto origin = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - origin).count());
}

void pipeline_timeline_note_event() { g_pl_last_event_us.store(pipeline_timeline_us(), std::memory_order_relaxed); }

unsigned process_thread_count() {
    unsigned count = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    const DWORD self = GetCurrentProcessId();
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == self) ++count;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

void pipeline_timeline_start() {
    static std::atomic<bool> started{false};
    const double every = pipeline_timeline_period();
    if (every <= 0.0 || started.exchange(true, std::memory_order_relaxed)) return;
    std::thread([every] {
        const auto began = std::chrono::steady_clock::now();
        for (unsigned line = 1; line <= 240; ++line) {
            const auto due = began + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                         std::chrono::duration<double>(every * line));
            std::this_thread::sleep_until(due);
            const std::uint64_t caller = g_pl_last_slot57_caller.load(std::memory_order_relaxed);
            char caller_text[96] = "(none)";
            if (caller) {
                streamed_blocks::caller_text(caller, caller_text, sizeof caller_text);
            }
            std::fprintf(stderr,
                         "[rt] pipeline timeline +%.1fs: slot57 calls %llu converted %llu refused %llu; slot55 calls "
                         "%llu; graphics created %llu; compute calls %llu created %llu; submits %llu; presents %llu; "
                         "console calls %llu; threads %u; last slot57 caller %s\n",
                         every * line, static_cast<unsigned long long>(g_pl_slot57_calls.load()),
                         static_cast<unsigned long long>(g_pl_slot57_converted.load()),
                         static_cast<unsigned long long>(g_pl_slot57_refused.load()),
                         static_cast<unsigned long long>(g_pl_slot55_calls.load()),
                         static_cast<unsigned long long>(g_pl_graphics_created.load()),
                         static_cast<unsigned long long>(g_pl_compute_calls.load()),
                         static_cast<unsigned long long>(g_pl_compute_created.load()),
                         static_cast<unsigned long long>(g_pl_submits.load()),
                         static_cast<unsigned long long>(g_pl_presents.load()),
                         static_cast<unsigned long long>(g_pl_console_calls.load()), process_thread_count(),
                         caller_text);
        }
    }).detach();
}

// A guarded read of any committed, readable address in this process. Exported for the DirectStorage shim's loader-state
// snapshot (docs/streamed_consumer.md section 6c): the loader object is a host-heap object whose fields the title's load
// spinner polls, and the shim reads them to report the armed/in-flight ordering. Read-only; returns the byte count copied
// (0 when the address is not readable), never faults.
extern "C" unsigned palmetto_mem_read(unsigned long long address, void* out, unsigned size) {
    if (!out || size == 0) return 0;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info) == 0) return 0;
    if (info.State != MEM_COMMIT) return 0;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    const std::uint64_t region_end = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    if (address + size > region_end) return 0;
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return size;
}

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
        case 41: return "CreateCommandSignature (console argument mapping)";
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
        case 74: return "Xbox three-word call -> S_OK";
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

// ---- Console command signatures (docs/command_signature.md) ---------------------------------------------------------
//
// Device slot 41 is CreateCommandSignature at its PC position: (desc, root signature, riid, out). The retail title's
// renderer device setup game+0x2478440 creates five one-argument descriptions there, every one with NodeMask 0 and no
// root signature (caller RVAs game+0x247954e, 0x247958a, 0x24795c6, 0x2479605, 0x2479646). Measured argument type and
// ByteStride, in call order: 0 DRAW / 16, 1 DRAW_INDEXED / 20, 105 / 12, 2 DISPATCH / 12, 10 DISPATCH_MESH / 12. Four
// of those are PC D3D12_INDIRECT_ARGUMENT_TYPE values and pass the layer's PC validation unchanged. 105 is outside the
// PC enum; its record is 12 bytes, the size of D3D12_DISPATCH_ARGUMENTS, and its measured ExecuteIndirect use (console
// list slot 61, caller game+0x94445) walks one 12-byte record after binding the compute state, the shape the DISPATCH
// signature has. It is created as DISPATCH. HYPOTHESIS: what the console does differently for it is not identified; the
// behaviour is cross-checked against the public fork, whose measured row reads the same type the same way.
//
// The fifth description is DISPATCH_MESH, a PC type this layer cannot carry: a CommandSignature here has a draw,
// draw_indexed or dispatch kind only, and the replay has no mesh indirect path. The proxy answers that description with
// a named dispatch-shaped stand-in so the title's setup can finish, and the list proxy refuses an ExecuteIndirect that
// names a stand-in by name instead of recording a dispatch the console would have run as mesh work. Removing the
// stand-in is the next slice's job: IndirectKind::dispatch_mesh plus vkCmdDrawMeshTasksIndirectEXT.
constexpr std::uint32_t kConsoleIndirectArgumentDispatch = 105;  // the console-only argument type measured at slot 41
constexpr unsigned kCommandSignatureArguments = 8;               // the bounded description copy
constexpr unsigned kCommandSignatureStandins = 4;                // console DISPATCH_MESH stand-ins kept for the check

std::atomic<void*> g_command_signature_standins[kCommandSignatureStandins];
std::atomic<unsigned> g_command_signature_standin_count{0};

// One relaxed flag read on the hot path; the probe prints only when PALMETTO_XS_SIG41 is set.
bool command_signature_probe() {
    static const bool enabled = [] {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA("PALMETTO_XS_SIG41", value, sizeof(value));
        return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

std::uint32_t indirect_argument_type(const D3D12_INDIRECT_ARGUMENT_DESC& arg) {
    std::uint32_t word = 0;
    std::memcpy(&word, &arg.Type, sizeof(word));
    return word;
}

void set_indirect_argument_type(D3D12_INDIRECT_ARGUMENT_DESC& arg, std::uint32_t word) {
    std::memcpy(&arg.Type, &word, sizeof(word));
}

// The stand-in registry is only read by an ExecuteIndirect that names one of these signature objects. The creation
// runs on the title's setup thread before the frame lists that could name it, so the release-store is enough.
void note_command_signature_standin(void* signature) {
    if (!signature) return;
    const unsigned index = g_command_signature_standin_count.fetch_add(1, std::memory_order_relaxed);
    if (index < kCommandSignatureStandins)
        g_command_signature_standins[index].store(signature, std::memory_order_release);
}

bool is_command_signature_standin(const void* signature) {
    if (!signature) return false;
    const unsigned count = std::min(g_command_signature_standin_count.load(std::memory_order_acquire),
                                    kCommandSignatureStandins);
    for (unsigned i = 0; i < count; ++i)
        if (g_command_signature_standins[i].load(std::memory_order_acquire) == signature) return true;
    return false;
}

// Console slot 25 (GetResourceAllocationInfo) with the console's depth-stencil + unordered-access pair sized as a plain
// depth-stencil texture. The console allows that flag pair on a texture; the PC runtime refuses it and answers D3D12's
// invalid allocation (SizeInBytes = UINT64_MAX). The title asserts on the high word of that size at game+0x344196
// before its allocation can fail, so the query is re-issued once with ALLOW_UNORDERED_ACCESS cleared and that size is
// returned when it is valid. Only a refused first answer pays for the retry; a description the retry still refuses is
// logged (bounded: one line for the measured pair, four for later refusals) and the host's answer is returned unchanged,
// so this stand-in cannot hide a genuine refusal. The count cap keeps the stack copy bounded.
inline D3D12_RESOURCE_ALLOCATION_INFO slot25_allocation_info(ID3D12Device* real, UINT mask, UINT count,
                                                             const D3D12_RESOURCE_DESC* descs) {
    D3D12_RESOURCE_ALLOCATION_INFO info = real->GetResourceAllocationInfo(mask, count, descs);
    if (info.SizeInBytes != UINT64_MAX || !descs || !count || count > 64) return info;
    D3D12_RESOURCE_DESC clean[64];
    bool changed = false;
    for (UINT i = 0; i < count; ++i) {
        clean[i] = descs[i];
        if (clean[i].Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            (clean[i].Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
            (clean[i].Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
            clean[i].Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            changed = true;
        }
    }
    if (changed) {
        const D3D12_RESOURCE_ALLOCATION_INFO retry = real->GetResourceAllocationInfo(mask, count, clean);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 25: depth-stencil texture (format %d flags 0x%x) sized without unordered "
                         "access, which the PC refuses with it: %s\n",
                         static_cast<int>(descs[0].Format), static_cast<unsigned>(descs[0].Flags),
                         retry.SizeInBytes != UINT64_MAX ? "sized" : "still refused");
        if (retry.SizeInBytes != UINT64_MAX) return retry;
    }
    static std::atomic<unsigned> refusals{0};
    if (refusals.fetch_add(1, std::memory_order_relaxed) < 4)
        for (UINT i = 0; i < count; ++i)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 25 refused description %u/%u: dimension %d format %d flags 0x%x layout "
                         "0x%x %llux%ux%u mips %u samples %u\n",
                         i, count, static_cast<int>(descs[i].Dimension), static_cast<int>(descs[i].Format),
                         static_cast<unsigned>(descs[i].Flags), static_cast<unsigned>(descs[i].Layout),
                         static_cast<unsigned long long>(descs[i].Width), descs[i].Height, descs[i].DepthOrArraySize,
                         descs[i].MipLevels, descs[i].SampleDesc.Count);
    return info;
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
// Command allocators (device slot 9) are the third kind: their public PC chain is 8 slots (the base chain and Reset at
// 8), and console slot 9 is the allocator-callback registration (docs/allocator_slot9.md). Device slot 9 wraps only an
// allocator that answers kPalmettoCommandAllocatorTag, because a list takes the title's allocator in CreateCommandList
// and Reset and only the Vulkan layer's lists resolve a proxy there (CommandAllocator::from); a PC runtime allocator is
// handed out unchanged until a command-list proxy unwraps it at Reset.

enum class ChildKind { queue, fence, allocator };

constexpr unsigned kQueueSlots = 128;        // the table: 19 public slots plus a refused tail
constexpr unsigned kFenceSlots = 64;         // the table: 12 public slots plus a refused tail
constexpr unsigned kQueuePublicSlots = 19;   // ID3D12CommandQueue
constexpr unsigned kQueueGetDesc = 18;           // the one queue method that returns a struct by value
constexpr unsigned kFencePublicSlots = 11;   // ID3D12Fence
constexpr unsigned kFence1PublicSlots = 12;  // ID3D12Fence1
constexpr unsigned kAllocatorSlots = 64;     // the table: 9 public slots, console slot 9, plus a refused tail
constexpr unsigned kAllocatorPublicSlots = 9;  // ID3D12CommandAllocator: the base chain and Reset (slot 8), a count
constexpr unsigned kConsoleAllocatorCallbackSlot = 9;  // past the PC chain: the callback registration (callback, context)
constexpr SIZE_T kTableRegion = 64 * 1024;   // one allocation granule: the table page plus 15 never-committed pages
static_assert(kQueueSlots > kQueuePublicSlots && kFenceSlots > kFence1PublicSlots &&
                  kAllocatorSlots > kConsoleAllocatorCallbackSlot,
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
constexpr unsigned kQueueCopyPageMappingsBatchX = 31;      // CopyPageMappingsBatchX, past the public chain

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
    // Allocators only: the pair console slot 9 registered (docs/allocator_slot9.md). The context is written before the
    // callback, so a reader that sees a callback sees the context registered with it; on replacement a reader may pair
    // the new callback with the previous context, which is documented and read by nothing in the runtime.
    std::atomic<std::uintptr_t> console_callback{0};
    std::atomic<std::uintptr_t> console_callback_context{0};
    // Allocators only: the once-per-proxy lifecycle the invocation model uses. The title re-registers on its reused
    // allocators once per presented frame, so the first registration on a proxy is the point the runtime would set the
    // allocator's recording memory up; later registrations only replace the pair (docs/allocator_slot9.md).
    std::atomic<bool> console_callback_claimed{false};
    std::atomic<bool> console_callback_invoked{false};
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
        if (slot == kConsoleAllocatorCallbackSlot) return "SetAllocatorCallbackX";
    }
    return nullptr;
}

// Fixed names for the trace channel (d3d12_trace::record takes fixed strings): one per table slot, built once, in
// the trace grammar of port/README.md and check_d3d12_trace.py (an identifier with `::`, no spaces):
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

// ---- Allocator slot 9: the recorded callback's invocation (docs/allocator_slot9.md) ------------------------------
//
// The measured callback game+0x555DF10 reads three of its seven arguments: arg3, the recording pool's memory class
// (passed on as edx to game+0x4D9F450 through game+0x4D9F400), arg4, the size (2^arg4 when arg4 < 16, else the explicit
// byte count), and arg6, the recording context (the context the registration named). It allocates a block from the
// pool at context+0x12780, frees the block at context+0x12798 if one is there and stores the new block at
// context+0x12798. The title itself takes 8 bytes of class 0x18 from that pool (game+0x18845E) immediately before it
// registers, so class 0x18 is a measured class for this pool; the size is the only unmeasured input, and the model
// asks for one console recording chunk. Off by default (PALMETTO_XS_ALLOCATOR_CALLBACK=1 turns it on) because the
// console's invocation condition is not measured and the title never reads the block back; the probe
// (PALMETTO_XS_ALLOCATOR_CALLBACK_PROBE=<seconds>) invokes the pair once after that second so one boot can show what
// the callback does with measured arguments.
namespace allocator_callback_invocation {

struct Counts {
    std::atomic<std::uint64_t> registrations{0}, first_registrations{0}, invocations{0}, refusals{0};
};

Counts& counts() {
    static Counts counts;
    return counts;
}

std::atomic<int> g_mode{-2};  // -2: the environment switch; 0/1: a test's explicit choice

bool env_flag(const char* name) {
    char value[8] = {};
    return GetEnvironmentVariableA(name, value, sizeof(value)) > 0 && value[0] != '\0' && value[0] != '0';
}

bool callback_env_enabled() {
    static const bool on = env_flag("PALMETTO_XS_ALLOCATOR_CALLBACK");
    return on;
}

bool callback_trace_enabled() {
    static const bool on = env_flag("PALMETTO_XS_ALLOCATOR_CALLBACK_TRACE");
    return on;
}

// The model's arguments: class 0x18 is the class the title's own recording-pool allocation uses (game+0x18845E);
// the size is one console recording chunk, and the two environment variables exist so a probe can move them without
// a rebuild (they are measurement inputs, not a contract).
std::uint64_t invocation_size() {
    static const std::uint64_t size = [] {
        char value[32] = {};
        if (GetEnvironmentVariableA("PALMETTO_XS_ALLOCATOR_CALLBACK_SIZE", value, sizeof(value)) > 0 && value[0]) {
            const std::uint64_t parsed = std::strtoull(value, nullptr, 0);
            if (parsed) return parsed;
        }
        return std::uint64_t{0x10000};
    }();
    return size;
}

std::uint64_t invocation_class() {
    static const std::uint64_t value = [] {
        char text[32] = {};
        if (GetEnvironmentVariableA("PALMETTO_XS_ALLOCATOR_CALLBACK_CLASS", text, sizeof(text)) > 0 && text[0])
            return std::strtoull(text, nullptr, 0);
        return std::uint64_t{0x18};
    }();
    return value;
}

// The probe's one shot: seconds after the first registration call, or 0 when it is off.
std::uint64_t probe_after_seconds() {
    static const std::uint64_t seconds = [] {
        char text[32] = {};
        if (GetEnvironmentVariableA("PALMETTO_XS_ALLOCATOR_CALLBACK_PROBE", text, sizeof(text)) > 0 && text[0])
            return std::strtoull(text, nullptr, 0);
        return std::uint64_t{0};
    }();
    return seconds;
}

std::atomic<std::uint64_t> probe_fired{0};

bool read_own(std::uint64_t address, void* out, std::size_t bytes) {
    if (!address) return false;
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, bytes, &done) == TRUE &&
           done == bytes;
}

bool callback_active() {
    if (callback_trace_enabled() || probe_after_seconds()) return true;
    const int mode = g_mode.load(std::memory_order_relaxed);
    return mode == 1 || (mode == -2 && callback_env_enabled());
}

void log_line(std::atomic<unsigned>& lines, const char* format, ...) {
    if (lines.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

// Calls the registered pair once and reports what it did to the context. The function reads only integers, so the
// same layout is used for the call on every architecture the title image runs on (Win64 x64).
void invoke(std::uint64_t callback, std::uint64_t context, const char* why) {
    Counts& c = counts();
    if (!callback || !context) {
        c.refusals.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<unsigned> lines{0};
        log_line(lines,
                 "[rt] d3d12_xs: allocator callback invocation (%s) refused: callback 0x%llx context 0x%llx; a pair "
                 "without both words is not a registration\n",
                 why, static_cast<unsigned long long>(callback), static_cast<unsigned long long>(context));
        return;
    }
    std::uint64_t before = 0;
    const bool context_readable = read_own(context + 0x12798, &before, sizeof(before));
    if (!context_readable) {
        c.refusals.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<unsigned> lines{0};
        log_line(lines,
                 "[rt] d3d12_xs: allocator callback invocation (%s) refused: the context 0x%llx is not readable at "
                 "+0x12798; the model will not call title code on an unverified context\n",
                 why, static_cast<unsigned long long>(context));
        return;
    }
    using Callback = std::uint64_t(WINAPI*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                            std::uint64_t, std::uint64_t, std::uint64_t);
    const auto fn = reinterpret_cast<Callback>(static_cast<std::uintptr_t>(callback));
    const std::uint64_t block = fn(0, 0, 0, invocation_class(), invocation_size(), 0, context);
    std::uint64_t after = 0;
    const bool after_readable = read_own(context + 0x12798, &after, sizeof(after));
    c.invocations.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<unsigned> lines{0};
    log_line(lines,
             "[rt] d3d12_xs: allocator callback invoked (%s): block 0x%llx, context+0x12798 0x%llx -> 0x%llx (class "
             "0x%llx size 0x%llx)\n",
             why, static_cast<unsigned long long>(block), static_cast<unsigned long long>(before),
             static_cast<unsigned long long>(after_readable ? after : 0),
             static_cast<unsigned long long>(invocation_class()),
             static_cast<unsigned long long>(invocation_size()));
    if (!block) {
        c.refusals.fetch_add(1, std::memory_order_relaxed);
        log_line(lines,
                 "[rt] d3d12_xs: allocator callback invocation (%s) returned no memory: refused by name, the "
                 "recording context gets nothing\n",
                 why);
    }
}

}  // namespace allocator_callback_invocation

// Console allocator slot 9 (docs/allocator_slot9.md): record the pair the title just registered. This title registers on
// the allocator it is about to use, once per presented frame (tens of thousands over a long boot), so only the first call
// is logged; with XS_TRACE every call gets the usual per-slot line from note_child_slot. The first registration on a
// proxy is the model's invocation point (docs/allocator_slot9.md's addendum): the pair is called once there, behind the
// opt-in switch (or once at the probe's second), and later registrations only replace the pair.
void record_console_allocator_callback(ChildProxy* self, std::uint64_t callback, std::uint64_t context) {
    using namespace allocator_callback_invocation;
    self->console_callback_context.store(static_cast<std::uintptr_t>(context), std::memory_order_relaxed);
    self->console_callback.store(static_cast<std::uintptr_t>(callback), std::memory_order_release);
    Counts& c = counts();
    c.registrations.fetch_add(1, std::memory_order_relaxed);
    const bool active_model = callback_active();
    if (active_model) {
        bool expected = false;
        if (self->console_callback_claimed.compare_exchange_strong(expected, true, std::memory_order_relaxed))
            c.first_registrations.fetch_add(1, std::memory_order_relaxed);
        if (callback_trace_enabled()) {
            static std::atomic<unsigned> lines{0};
            log_line(lines,
                     "[rt] d3d12_xs: allocator slot 9 registration: callback 0x%llx context 0x%llx (registrations "
                     "%llu)\n",
                     static_cast<unsigned long long>(callback), static_cast<unsigned long long>(context),
                     static_cast<unsigned long long>(c.registrations.load(std::memory_order_relaxed)));
        }
        const std::uint64_t probe_seconds = probe_after_seconds();
        if (probe_seconds) {
            static const std::uint64_t started = GetTickCount64();
            if (!probe_fired.load(std::memory_order_relaxed) &&
                GetTickCount64() - started >= probe_seconds * 1000 &&
                !probe_fired.exchange(1, std::memory_order_relaxed))
                invoke(callback, context, "probe");
        } else if (allocator_callback_invocation_enabled()) {
            bool first_invocation = false;
            if (self->console_callback_invoked.compare_exchange_strong(first_invocation, true,
                                                                      std::memory_order_relaxed))
                invoke(callback, context, "first registration on the allocator");
        }
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: allocator slot %u (SetAllocatorCallbackX) recorded callback 0x%llx context "
                     "0x%llx%s\n",
                     kConsoleAllocatorCallbackSlot, static_cast<unsigned long long>(callback),
                     static_cast<unsigned long long>(context),
                     active_model || callback_env_enabled() ? " (invocation model on: see PALMETTO_XS_ALLOCATOR_CALLBACK)"
                                                   : " (recorded on the proxy; never invoked)");
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
    // Diagnostic (docs/frame_capture.md, the root-argument fallback): the console list slots a title inline writer falls
    // back to at the user-data limit are 20/24/35/43/45/47. Classify their arguments (shape only, bounded per slot,
    // opt-in PALMETTO_LIST_SLOT_TRACE), so a root-32-bit-constant-like payload - an (index, count, values pointer) triple
    // - is recognised if the load window carries one. No values are printed.
    static const bool trace = [] {
        char value[8] = {};
        return GetEnvironmentVariableA("PALMETTO_LIST_SLOT_TRACE", value, sizeof(value)) > 0 && value[0] != '\0' &&
               value[0] != '0';
    }();
    if (!trace) return;
    if (slot != 20 && slot != 24 && slot != 35 && slot != 43 && slot != 45 && slot != 47) return;
    static std::atomic<unsigned> logged[kListSlots];
    const unsigned shown = logged[slot].fetch_add(1, std::memory_order_relaxed);
    if (shown >= 8) return;
    const bool constant_like = a1 < 64 && a2 >= 1 && a2 <= 32;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: list slot %u fallback call %u: args 0x%llx 0x%llx 0x%llx 0x%llx; %s\n", slot, shown,
                 static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4),
                 constant_like ? "index/count shape (possible root 32-bit constants)" : "other shape");
}

// ---- Console list slots 70/71/72/74 (docs/bottom_of_pipe.md): counters and the measured model -------------------
//
// The measured shapes and their call sites are in d3d12_xs_device.h. Two switches, both off by default:
//   PALMETTO_XS_CONSOLE_LIST_EXTRAS=1  the model: slot 70 forwards the depth bounds, slots 71/72 write the measured
//                                      value into the mapped title destination at record time, slot 74 is evaluated.
//   PALMETTO_XS_CONSOLE_LIST_TRACE=1   measurement only: the decoded shapes are logged (bounded) and no call changes.
// The default path still answers slots 71/72/74 with 0 and slot 70 through the float entry (which, with the switch
// off, counts and returns without forwarding). A boot that runs to its timeout has no shutdown summary, so the
// counters are also printed every 16384 extra calls: that is the boot's reported table.
namespace console_list_extras {

struct Counts {
    std::atomic<std::uint64_t> slot70_calls{0}, slot70_forwarded{0};
    std::atomic<std::uint64_t> slot71_calls{0}, slot71_writes{0}, slot71_refused{0};
    std::atomic<std::uint64_t> slot72_calls{0}, slot72_writes{0}, slot72_timestamps{0}, slot72_refused{0};
    std::atomic<std::uint64_t> slot74_calls{0}, slot74_met{0}, slot74_unmet{0}, slot74_unreadable{0},
        slot74_refused{0};
};

Counts& counts() {
    static Counts counts;
    return counts;
}

bool env_flag(const char* name) {
    char value[8] = {};
    return GetEnvironmentVariableA(name, value, sizeof(value)) > 0 && value[0] != '\0' && value[0] != '0';
}

// -2: not resolved yet, -1: the environment switch, 0/1: a test's explicit choice.
std::atomic<int> g_mode{-2};

bool env_enabled() {
    static const bool on = env_flag("PALMETTO_XS_CONSOLE_LIST_EXTRAS");
    return on;
}

bool trace_enabled() {
    static const bool on = env_flag("PALMETTO_XS_CONSOLE_LIST_TRACE");
    return on;
}

void print_summary();

// True while a shape is decoded or written: the counters and the periodic summary are only paid then.
bool active() {
    const int mode = g_mode.load(std::memory_order_relaxed);
    const bool on = mode == 1 || (mode == -2 && env_enabled());
    if (!on && !trace_enabled()) return false;
    static const bool summary_registered = [] {
        // The boot is killed by the harness's timeout, so atexit is a best-effort extra; the periodic line is the
        // measurement.
        std::atexit(&print_summary);
        return true;
    }();
    (void)summary_registered;
    return true;
}

void maybe_print_summary() {
    static std::atomic<std::uint64_t> events{0};
    const std::uint64_t n = events.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 16384 == 0) print_summary();
}

void print_summary() {
    const Counts& c = counts();
    std::fprintf(stderr,
                 "[rt] d3d12_xs: console list extras: slot70 calls %llu forwarded %llu | slot71 calls %llu writes "
                 "%llu refused %llu | slot72 calls %llu writes %llu timestamps %llu refused %llu | slot74 calls %llu "
                 "met %llu unmet %llu unreadable %llu refused %llu\n",
                 static_cast<unsigned long long>(c.slot70_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot70_forwarded.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot71_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot71_writes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot71_refused.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot72_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot72_writes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot72_timestamps.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot72_refused.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot74_calls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot74_met.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot74_unmet.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot74_unreadable.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slot74_refused.load(std::memory_order_relaxed)));
}

// A bounded diagnostic line per shape: the first eight calls and then one in every 4096, which is how a steady
// state after the loading window is seen without filling the log.
void trace_shape(const char* what, std::atomic<std::uint64_t>& call, const char* format, ...) {
    if (!trace_enabled()) return;
    const std::uint64_t n = call.load(std::memory_order_relaxed);
    if (n > 8 && n % 4096 != 0) return;
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fprintf(stderr, "[rt] d3d12_xs: console list %s call %llu: %s\n", what,
                 static_cast<unsigned long long>(n), line);
}

// The mapped title address is CPU-visible process memory in this layer, so the model reads and writes it through the
// kernel: a page the title unmapped or reserved reports failure instead of faulting the recording thread.
bool read_own(std::uint64_t address, void* out, std::size_t bytes) {
    if (!address) return false;
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, bytes, &done) == TRUE &&
           done == bytes;
}

bool write_own(std::uint64_t address, const void* data, std::size_t bytes) {
    if (!address) return false;
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), data, bytes, &done) == TRUE &&
           done == bytes;
}

void refuse(const char* slot, const char* why, std::uint64_t address, std::uint64_t a, std::uint64_t b,
            std::uint64_t c) {
    static std::atomic<unsigned> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: console list %s refused: %s (address 0x%llx, args 0x%llx 0x%llx 0x%llx)\n", slot, why,
                 static_cast<unsigned long long>(address), static_cast<unsigned long long>(a),
                 static_cast<unsigned long long>(b), static_cast<unsigned long long>(c));
}

// The console's 100 MHz clock from the CPU's at record time (the measured alternative the console offers for a
// bottom-of-pipe write's value: flags 1 or 2 ask for a timestamp, docs/bottom-of-pipe in the page's references).
std::uint64_t console_clock_now() {
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? static_cast<std::uint64_t>(value.QuadPart) : 1;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart) * 100'000'000ull / frequency;
}

// List slot 71, Write32BitValueBottomOfPipeX(address, dword, flush, flags), measured (address, dword, 0, 0).
void write32_bottom_of_pipe(std::uint64_t address, std::uint64_t value, std::uint64_t flush, std::uint64_t flags,
                            std::uint64_t caller) {
    if (!active()) return;
    Counts& c = counts();
    c.slot71_calls.fetch_add(1, std::memory_order_relaxed);
    if (trace_enabled()) {
        char where[64];
        streamed_blocks::caller_text(caller, where, sizeof(where));
        trace_shape("slot 71 (Write32BitValueBottomOfPipeX)", c.slot71_calls,
                    "address 0x%llx value 0x%llx flush 0x%llx flags 0x%llx (caller %s)",
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(flush), static_cast<unsigned long long>(flags), where);
    }
    if (!console_list_extras_enabled()) return;
    // The console arguments are dwords: the title writes only the low half of the stack slot, so the upper 32 bits of
    // a4 are caller residue (measured 0xaf00000000 at the slot-71 site). Compare the argument, not the residue.
    const std::uint32_t console_flags = static_cast<std::uint32_t>(flags);
    if (!address || (address & 3) || flush != 0 || console_flags != 0) {
        c.slot71_refused.fetch_add(1, std::memory_order_relaxed);
        refuse("slot 71", "shape not decoded (needs a 4-byte-aligned address, flush 0, flags 0)", address, value,
               flush, flags);
        return;
    }
    const auto word = static_cast<std::uint32_t>(value);
    if (!write_own(address, &word, sizeof(word))) {
        c.slot71_refused.fetch_add(1, std::memory_order_relaxed);
        refuse("slot 71", "the destination is not writable title memory", address, value, flush, flags);
        return;
    }
    c.slot71_writes.fetch_add(1, std::memory_order_relaxed);
    maybe_print_summary();
}

// List slot 72, Write64BitValueBottomOfPipeX(address, qword, flush, flags), measured (address, 0, 0, 1): flags 1/2
// ask for a timestamp instead of the value.
void write64_bottom_of_pipe(std::uint64_t address, std::uint64_t value, std::uint64_t flush, std::uint64_t flags,
                            std::uint64_t caller) {
    if (!active()) return;
    Counts& c = counts();
    c.slot72_calls.fetch_add(1, std::memory_order_relaxed);
    if (trace_enabled()) {
        char where[64];
        streamed_blocks::caller_text(caller, where, sizeof(where));
        trace_shape("slot 72 (Write64BitValueBottomOfPipeX)", c.slot72_calls,
                    "address 0x%llx value 0x%llx flush 0x%llx flags 0x%llx (caller %s)",
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(flush), static_cast<unsigned long long>(flags), where);
    }
    if (!console_list_extras_enabled()) return;
    const std::uint32_t console_flags = static_cast<std::uint32_t>(flags);  // the caller's upper half is residue
    if (!address || (address & 7) || flush != 0 || (console_flags & ~std::uint32_t{3})) {
        c.slot72_refused.fetch_add(1, std::memory_order_relaxed);
        refuse("slot 72", "shape not decoded (needs an 8-byte-aligned address, flush 0, flags 0/1/2)", address, value,
               flush, flags);
        return;
    }
    std::uint64_t written = value;
    if (console_flags & 3) {
        written = console_clock_now();
        c.slot72_timestamps.fetch_add(1, std::memory_order_relaxed);
    }
    if (!write_own(address, &written, sizeof(written))) {
        c.slot72_refused.fetch_add(1, std::memory_order_relaxed);
        refuse("slot 72", "the destination is not writable title memory", address, value, flush, flags);
        return;
    }
    c.slot72_writes.fetch_add(1, std::memory_order_relaxed);
    maybe_print_summary();
}

// List slot 74, Wait64BitValueX(address, comparison, qword, flags), measured (address, 7, qword, 1): comparison 7 is
// GREATER_EQUAL and all 64 bits are compared. The model reads the cell at record time and counts whether it already
// holds: when a bottom-of-pipe write of this list named the address, that write has been made (the model writes at
// record time), so the recorded order meets it; a cell our memory map cannot read is refused by name; a cell that does
// not hold yet is the measured unmet case, whose consumer would be the queue hold the replay layer does not carry.
void memory_wait64(std::uint64_t address, std::uint64_t comparison, std::uint64_t value, std::uint64_t flags,
                   std::uint64_t caller) {
    if (!active()) return;
    Counts& c = counts();
    c.slot74_calls.fetch_add(1, std::memory_order_relaxed);
    if (trace_enabled()) {
        char where[64];
        streamed_blocks::caller_text(caller, where, sizeof(where));
        trace_shape("slot 74 (Wait64BitValueX)", c.slot74_calls,
                    "address 0x%llx comparison 0x%llx value 0x%llx flags 0x%llx (caller %s)",
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(comparison),
                    static_cast<unsigned long long>(value), static_cast<unsigned long long>(flags), where);
    }
    if (!console_list_extras_enabled()) return;
    const std::uint32_t console_flags = static_cast<std::uint32_t>(flags);  // the caller's upper half is residue
    if (!address || comparison != 7 || console_flags != 1) {
        c.slot74_refused.fetch_add(1, std::memory_order_relaxed);
        refuse("slot 74", "shape not decoded (needs comparison 7 GREATER_EQUAL and flags 1)", address, comparison,
               value, flags);
        return;
    }
    std::uint64_t current = 0;
    if (!read_own(address, &current, sizeof(current))) {
        c.slot74_unreadable.fetch_add(1, std::memory_order_relaxed);
        refuse("slot 74", "the waited cell is not readable title memory", address, comparison, value, flags);
        return;
    }
    if (current >= value) {
        c.slot74_met.fetch_add(1, std::memory_order_relaxed);
    } else {
        const std::uint64_t unmet = c.slot74_unmet.fetch_add(1, std::memory_order_relaxed) + 1;
        if (unmet <= 8)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: console list slot 74 memory wait not met at record time: cell 0x%llx holds "
                         "0x%llx, wants >= 0x%llx (unmet %llu); the queue hold for a cross-list producer is not "
                         "carried in this shim (docs/bottom_of_pipe.md)\n",
                         static_cast<unsigned long long>(address), static_cast<unsigned long long>(current),
                         static_cast<unsigned long long>(value), static_cast<unsigned long long>(unmet));
    }
    maybe_print_summary();
}

}  // namespace console_list_extras


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

// ---- Streamed-block correlation on the device and command-list proxies (measurement only) -------------------------
//
// Filled by dstorage.cpp (the streamed_blocks registry above). Two questions are answered here, one line each and no
// behaviour change:
//   * does a title device or command-list call ever name a streamed block? Every first six argument words are tested
//     against the recorded ranges; a match names the slot, the argument position, the block and the caller (game+RVA).
//     This covers a heap created from the block's address, a placed resource at it, and any entry point whose number this
//     proxy has not identified yet (an unknown console slot still matches if it passes the address).
//   * do the entry points a consumer's GPU-side step would use get called at all? The first call of each relevant device
//     slot and copy-family command-list slot prints its arguments and the title caller, so a negative answer ("no copy
//     call, no heap/placed resource over the range") is measured on this side too, not inferred from the replay's count.
const char* consumer_slot_name(unsigned slot) {
    switch (slot) {
        case 8: return "CreateCommandQueue";
        case 12: return "CreateCommandList";
        case 27: return "CreateCommittedResource (PC slot)";
        case 28: return "CreateHeap (PC slot)";
        case 29: return "CreatePlacedResource (PC slot)";
        case 48: return "console CreateCommandQueueX (PC OpenExistingHeapFromAddress is shadowed here)";
        case 49: return "OpenExistingHeapFromFileMapping (PC slot)";
        case 53: return "CreateCommittedResource1 (PC slot)";
        case 54: return "CreateHeap1 (PC slot)";
        case 69: return "CreateCommittedResource2 (PC slot)";
        case 70: return "console slot 70 (PC CreatePlacedResource1 shape does not fit)";
        case 73: return "console CreateCommandListX";
        case 75: return "CreateCommandQueue1";
        case 76: return "console resource creation";
        case 77: return "CreatePlacedResource2 (PC slot)";
        case 127: return "console committed resource with a barrier layout";
        case 133: return "console CreatePlacedResourceX";
        default: return "device";
    }
}

void stream_note_match(const char* kind, unsigned slot, unsigned argument, std::uint64_t word, std::uint64_t caller) {
    const int block = streamed_blocks::find(word, 1);
    if (block < 0) return;
    if (streamed_blocks::g_match_lines.fetch_add(1, std::memory_order_relaxed) >= 32) return;
    char where[64];
    streamed_blocks::caller_text(caller, where, sizeof where);
    std::fprintf(stderr,
                 "[rt] d3d12_xs: STREAM CONSUMER: %s slot %u argument %u = 0x%llx names streamed block #%d (caller "
                 "%s)\n",
                 kind, slot, argument, static_cast<unsigned long long>(word), block, where);
}

void stream_note_device_call(unsigned slot, const void* return_address, std::uint64_t a1, std::uint64_t a2,
                             std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6) {
    if (!streamed_blocks::active() || slot >= kMaxSlots) return;
    const std::uint64_t caller = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(return_address));
    const std::uint64_t words[6] = {a1, a2, a3, a4, a5, a6};
    for (unsigned i = 0; i < 6; ++i) stream_note_match("device", slot, i + 1, words[i], caller);
    switch (slot) {
        case 8: case 12: case 27: case 28: case 29: case 48: case 49: case 53: case 54: case 69: case 70: case 73:
        case 75: case 76: case 77: case 127: case 133: break;
        default: return;
    }
    static std::atomic<bool> seen[kMaxSlots];
    bool expected = false;
    if (!seen[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    char where[64];
    streamed_blocks::caller_text(caller, where, sizeof where);
    char type[48] = "";
    if ((slot == 8 || slot == 48 || slot == 75) && readable_range(reinterpret_cast<const void*>(a1), 4)) {
        const unsigned command_type = *reinterpret_cast<const std::uint32_t*>(a1);
        std::snprintf(type, sizeof type, " command type %u%s", command_type, command_type == 3 ? " (COPY)" : "");
    } else if (slot == 12) {
        const auto command_type = static_cast<unsigned>(a2);
        std::snprintf(type, sizeof type, " command type %u%s", command_type, command_type == 3 ? " (COPY)" : "");
    } else if (slot == 73 && readable_range(reinterpret_cast<const void*>(a1), 4)) {
        const unsigned command_type = *reinterpret_cast<const std::uint32_t*>(a1);
        std::snprintf(type, sizeof type, " command type %u%s", command_type, command_type == 3 ? " (COPY)" : "");
    }
    std::fprintf(stderr,
                 "[rt] d3d12_xs: stream consumer: device slot %u (%s) first call: args 0x%llx 0x%llx 0x%llx 0x%llx%s "
                 "(caller %s)\n",
                 slot, consumer_slot_name(slot), static_cast<unsigned long long>(a1),
                 static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
                 static_cast<unsigned long long>(a4), type, where);
}

void stream_note_list_call(unsigned console_slot, unsigned pc_slot, const void* return_address, std::uint64_t a1,
                           std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6) {
    if (!streamed_blocks::active() || console_slot >= kListSlots) return;
    // Copy-family console slots (the PC order the console keeps before its extras): 15 CopyBufferRegion, 16
    // CopyTextureRegion, 17 CopyResource, 18 CopyTiles, 19 ResolveSubresource; the console's CopyTextureRegionX at 85 and
    // the enhanced-barrier group at 146 (a barrier that names resources). First call per slot, then the argument match.
    const bool interesting = (console_slot >= 15 && console_slot <= 19) || console_slot == 85 || console_slot == 146;
    if (!interesting) return;
    const std::uint64_t caller = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(return_address));
    const std::uint64_t words[6] = {a1, a2, a3, a4, a5, a6};
    for (unsigned i = 0; i < 6; ++i) stream_note_match("list", console_slot, i + 1, words[i], caller);
    static std::atomic<bool> seen[kListSlots];
    bool expected = false;
    if (!seen[console_slot].compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    char where[64];
    streamed_blocks::caller_text(caller, where, sizeof where);
    std::fprintf(stderr,
                 "[rt] d3d12_xs: stream consumer: console list slot %u -> PC slot %u first call: args 0x%llx 0x%llx "
                 "0x%llx 0x%llx 0x%llx 0x%llx (caller %s)\n",
                 console_slot, pc_slot, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4),
                 static_cast<unsigned long long>(a5), static_cast<unsigned long long>(a6), where);
}

// ---- Xbox-ABI call shape of the console view-creation sites (measurement only; PALMETTO_XBOX_VIEW_SHAPE=1) ---------
//
// docs/streamed_consumer.md: the linear-layout A/B saw streamed-block addresses in the *higher* words of the console
// CreateShaderResourceView (device slot 18), CreateUnorderedAccessView (slot 19) and slot 70 calls. The question is
// whether such a word is a real Xbox-ABI argument or a saved register / caller-frame word that happens to lie in a
// streamed block. This probe answers it without changing a call:
//   * once per call site (up to four per slot) it prints the caller as game+RVA and all fourteen words the thunk
//     receives, each labelled ARG (the forwarded PC method reads it) or BEYOND (the caller's stack after the callee's
//     argument vector: a saved register, a return-address slot or the caller's own frame - not an argument);
//   * for every word that is a readable pointer it scans the first 256 bytes of the pointed-to structure for values that
//     fall in a recorded streamed block, and prints each hit with its byte offset, so an address carried indirectly (in
//     a descriptor) is not missed;
//   * the first call per site also prints the first 8 dwords of each readable pointer argument, so the structure can be
//     identified offline;
//   * across the boot it counts, per slot and per word position, how many calls had a word inside a streamed range, and
//     prints that histogram on the first monitored call, whenever a position matches for the first time, and every 256
//     monitored calls. A match at an ARG position is the finding this probe exists to rule in or out.
// Off by default; bounded (4 sites x 3 slots, 14-word lines, 24 histogram lines); no argument is changed.
constexpr unsigned kViewShapeWords = 14;
constexpr unsigned kViewShapeSites = 4;
constexpr unsigned kViewShapeSlotCount = 3;
constexpr unsigned kViewShapeSlots[kViewShapeSlotCount] = {18, 19, 70};

bool view_shape_enabled() {
    static const bool on = [] {
        char value[8] = {};
        return GetEnvironmentVariableA("PALMETTO_XBOX_VIEW_SHAPE", value, sizeof(value)) > 0 && value[0] != '\0' &&
               value[0] != '0';
    }();
    return on;
}

// Argument positions the forwarded PC method really reads after `this` (slot 70 is refused: its console call is measured
// as three arguments, docs/device_slot55.md and the slot-70 refusal in proxy_slot). Everything past this is the caller's
// stack, not the callee's argument vector.
unsigned view_shape_argument_count(unsigned slot) { return slot == 19 ? 4u : 3u; }

unsigned view_shape_index(unsigned slot) {
    for (unsigned i = 0; i < kViewShapeSlotCount; ++i)
        if (kViewShapeSlots[i] == slot) return i;
    return kViewShapeSlotCount;
}

struct ViewShapeStats {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> hits[kViewShapeWords + 1];
    std::atomic<std::uint64_t> structure_hits{0};
    std::atomic<bool> position_seen[kViewShapeWords + 1];
};
ViewShapeStats g_view_shape[kViewShapeSlotCount];
struct ViewShapeSite {
    std::atomic<std::uint64_t> caller{0};
    std::atomic<bool> logged{false};
};
ViewShapeSite g_view_shape_sites[kViewShapeSlotCount][kViewShapeSites];
std::atomic<std::uint64_t> g_view_shape_calls{0};

void view_shape_histogram(unsigned slot) {
    const unsigned index = view_shape_index(slot);
    if (index >= kViewShapeSlotCount) return;
    const ViewShapeStats& stats = g_view_shape[index];
    const unsigned arguments = view_shape_argument_count(slot);
    char line[512];
    int at = std::snprintf(line, sizeof line,
                           "[rt] d3d12_xs: view shape histogram: slot %u calls=%llu (words 1-%u = the Xbox-ABI "
                           "arguments, the rest are caller stack):",
                           slot, static_cast<unsigned long long>(stats.calls.load(std::memory_order_relaxed)), arguments);
    bool any = false;
    for (unsigned word = 1; word <= kViewShapeWords && at > 0 && static_cast<size_t>(at) < sizeof line - 48; ++word) {
        const std::uint64_t hits = stats.hits[word].load(std::memory_order_relaxed);
        if (!hits) continue;
        any = true;
        at += std::snprintf(line + at, sizeof line - static_cast<size_t>(at), " w%u=%llu%s", word,
                            static_cast<unsigned long long>(hits), word <= arguments ? "(ARG)" : "(beyond)");
    }
    if (!any) at += std::snprintf(line + at, sizeof line - static_cast<size_t>(at), " no streamed-range hit in any word");
    if (at > 0 && static_cast<size_t>(at) < sizeof line - 48)
        std::snprintf(line + at, sizeof line - static_cast<size_t>(at), "; pointed-to structures: %llu block hit(s)",
                      static_cast<unsigned long long>(stats.structure_hits.load(std::memory_order_relaxed)));
    std::fprintf(stderr, "%s\n", line);
}

// Scan the first 256 bytes of a pointer argument for streamed-block addresses: a descriptor that carries the block's
// address indirectly is the case a word scan alone would miss. Returns the number of hits (each also printed, bounded).
unsigned view_shape_scan_pointer(unsigned slot, unsigned word, std::uint64_t pointer, std::uint64_t caller,
                                 const char* origin) {
    if (!pointer) return 0;
    unsigned hits = 0;
    for (unsigned i = 0; i < 32; ++i) {
        const std::uint64_t address = pointer + static_cast<std::uint64_t>(i) * 8;
        if (!readable_range(reinterpret_cast<const void*>(address), 8)) return hits;  // also refuses a guard page
        const std::uint64_t value = *reinterpret_cast<const std::uint64_t*>(address);
        const int block = streamed_blocks::find(value, 1);
        if (block < 0) continue;
        ++hits;
        if (streamed_blocks::g_match_lines.fetch_add(1, std::memory_order_relaxed) >= 64) return hits;
        char where[64];
        streamed_blocks::caller_text(caller, where, sizeof where);
        std::fprintf(stderr,
                     "[rt] d3d12_xs: view shape: slot %u word %u [%s] structure +0x%x = 0x%llx names streamed block #%d "
                     "(caller %s)\n",
                     slot, word, origin, i * 8u, static_cast<unsigned long long>(value), block, where);
    }
    return hits;
}

// Where a word of the thunk's fourteen sits relative to the callee's argument vector. The three measured sites are
// reached through two tiny wrappers whose prologues were disassembled offline (RVAs only):
//   0x164EE0 (slot-18 site, returns at 0x164F01) and 0x4ECC800 (slot-70 site, returns at 0x4ECC821):
//       push rsi; sub rsp,0x20; rsi=r8; r8=rdx; rdx=rcx; rcx=<device global>; r9=rsi; call [rax+slot]; then
//       tail-call the cache-line writeback 0x4ECC830 with (rsi=r8, 0).  Callee words: 1-3 = the forwarded arguments,
//       4 = the caller's rsi saved by `push rsi` (a register save, not an argument), 5 = the wrapper's return address,
//       6+ = the wrapper caller's stack.
//   0x16475F0 (slot-19 site, returns at 0x1647616):
//       push rsi; sub rsp,0x30; r9=rdx; rdx=rcx; rcx=<device global>; rsi=r8; [rsp+0x20]=r8; r8d=0; call [rax+slot];
//       then tail-call the writeback with (rsi=r8, 0).  Callee words: 1-3 = the forwarded arguments, 4 = the stored
//       fourth argument, 5 = an unwritten local slot, 6 = the caller's rsi saved by `push rsi`, 7 = the wrapper's return
//       address, 8+ = the wrapper caller's stack.
// A word 4-5 match in the A/B boot is therefore a register save or an unwritten local - not a call argument.
const char* view_shape_origin(unsigned slot, std::uint64_t caller, unsigned word) {
    constexpr std::uint64_t kSrvSite = 0x164F01;
    constexpr std::uint64_t kUavSite = 0x1647616;
    constexpr std::uint64_t kSlot70Site = 0x4ECC821;
    std::uint64_t rva = 0;
    const bool known = streamed_blocks::caller_rva(caller, rva);
    if (known && (rva == kSrvSite || rva == kSlot70Site)) {
        if (word <= 3) return "arg";
        if (word == 4) return "saved-rsi";
        if (word == 5) return "retaddr";
        return "caller-stack";
    }
    if (known && rva == kUavSite) {
        if (word <= 3) return "arg";
        if (word == 4) return "arg4";
        if (word == 5) return "local";
        if (word == 6) return "saved-rsi";
        if (word == 7) return "retaddr";
        return "caller-stack";
    }
    return word <= view_shape_argument_count(slot) ? "arg" : "caller-stack?";
}

void view_shape_log_site(unsigned slot, const std::uint64_t* words, std::uint64_t caller) {
    char where[64];
    streamed_blocks::caller_text(caller, where, sizeof where);
    char line[1024];
    int at = std::snprintf(line, sizeof line, "[rt] d3d12_xs: view shape: slot %u site %s:", slot, where);
    for (unsigned word = 1; word <= kViewShapeWords && at > 0 && static_cast<size_t>(at) < sizeof line - 48; ++word) {
        const int block = streamed_blocks::find(words[word - 1], 1);
        const char* origin = view_shape_origin(slot, caller, word);
        if (std::strcmp(origin, "retaddr") == 0) {
            char code[64];
            streamed_blocks::caller_text(words[word - 1], code, sizeof code);
            at += std::snprintf(line + at, sizeof line - static_cast<size_t>(at), " w%u[%s]=%s%s", word, origin, code,
                                block >= 0 ? " (streamed)" : "");
        } else {
            at += std::snprintf(line + at, sizeof line - static_cast<size_t>(at), " w%u[%s]=0x%llx%s", word, origin,
                                static_cast<unsigned long long>(words[word - 1]), block >= 0 ? " (streamed)" : "");
        }
    }
    std::fprintf(stderr, "%s\n", line);
    // The first 8 dwords of each readable pointer word, so the structure can be identified offline. Bounded: one site
    // line per slot, at most 14 pointer dumps of 8 dwords.
    for (unsigned word = 1; word <= kViewShapeWords; ++word) {
        const std::uint64_t pointer = words[word - 1];
        if (!readable_range(reinterpret_cast<const void*>(pointer), 32)) continue;
        const auto* dwords = reinterpret_cast<const std::uint32_t*>(pointer);
        std::fprintf(stderr,
                     "[rt] d3d12_xs: view shape: slot %u w%u first dwords: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                     slot, word, dwords[0], dwords[1], dwords[2], dwords[3], dwords[4], dwords[5], dwords[6],
                     dwords[7]);
    }
}

// Called at the top of every device thunk when the switch is on. `words` holds the fourteen words the thunk receives
// (words 1-4 in the argument registers, words 5-14 the caller's stack).
void view_shape_probe(unsigned slot, const void* return_address, const std::uint64_t* words) {
    const unsigned index = view_shape_index(slot);
    if (index >= kViewShapeSlotCount) return;
    ViewShapeStats& stats = g_view_shape[index];
    const std::uint64_t caller = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(return_address));
    const bool baseline = g_view_shape_calls.fetch_add(1, std::memory_order_relaxed) == 0;
    stats.calls.fetch_add(1, std::memory_order_relaxed);
    bool new_position = false;
    for (unsigned word = 1; word <= kViewShapeWords; ++word) {
        if (streamed_blocks::find(words[word - 1], 1) < 0) continue;
        stats.hits[word].fetch_add(1, std::memory_order_relaxed);
        if (!stats.position_seen[word].exchange(true, std::memory_order_relaxed)) new_position = true;
    }
    // The pointed-to structures, across the boot: every call for slots 18/19 (a few tens of calls each), every 1024th
    // call for slot 70 (13,824 calls), so an indirect block address inside a descriptor is measured beyond the first
    // call of a site.
    const bool scan_structures = slot != 70 || stats.calls.load(std::memory_order_relaxed) % 1024 == 0;
    if (scan_structures) {
        for (unsigned word = 1; word <= kViewShapeWords; ++word) {
            const std::uint64_t pointer = words[word - 1];
            if (!readable_range(reinterpret_cast<const void*>(pointer), 32)) continue;
            const unsigned hits = view_shape_scan_pointer(slot, word, pointer, caller,
                                                          view_shape_origin(slot, caller, word));
            if (!hits) continue;
            stats.structure_hits.fetch_add(hits, std::memory_order_relaxed);
            new_position = true;
        }
    }
    // The first call per call site: the full vector, the pointer structures and the argument/stack split.
    for (unsigned site = 0; site < kViewShapeSites; ++site) {
        ViewShapeSite& entry = g_view_shape_sites[index][site];
        if (entry.caller.load(std::memory_order_relaxed) == caller) break;
        if (entry.caller.load(std::memory_order_relaxed) != 0) continue;
        std::uint64_t expected = 0;
        if (!entry.caller.compare_exchange_strong(expected, caller, std::memory_order_relaxed)) break;
        view_shape_log_site(slot, words, caller);
        break;
    }
    const std::uint64_t calls = stats.calls.load(std::memory_order_relaxed);
    if (baseline || new_position || calls % 256 == 0) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: view shape: slot %u has %llu call(s), %llu monitored call(s) in total\n", slot,
                     static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(g_view_shape_calls.load(std::memory_order_relaxed)));
        view_shape_histogram(slot);
    }
}

// ---- Load-window trace (measurement only; PALMETTO_LOAD_TRACE=<from_s>,<to_s>) -------------------------------------
//
// docs/streamed_consumer.md: the title's CPU deserializer reads the first sixteen streamed blocks at ~2.3 s and then
// stops while the frame loop keeps presenting a black blit. This trace names what the device and the queue/fence proxies
// are asked inside the loading window, relative to the first traced call: one line per call with its thread, caller and
// arguments, plus the fence value a GetCompletedValue returns, so the primitive the loading code waits on is measured
// rather than inferred. `PALMETTO_LOAD_TRACE=1` means 0..15 s; "from,to" narrows it. Bounded (20,000 lines), off by
// default, changes no call.
struct LoadTraceWindow {
    bool on = false;
    double from = 0.0;
    double to = 15.0;
};

const LoadTraceWindow& load_trace_window() {
    static const LoadTraceWindow window = [] {
        LoadTraceWindow w;
        char value[64] = {};
        if (GetEnvironmentVariableA("PALMETTO_LOAD_TRACE", value, sizeof(value)) == 0 || value[0] == '\0') return w;
        w.on = true;
        char* end = nullptr;
        w.from = std::strtod(value, &end);
        if (end == value) return w;
        if (*end == ',') {
            const double to = std::strtod(end + 1, nullptr);
            if (to > w.from) w.to = to;
        }
        return w;
    }();
    return window;
}

// Seconds since the first traced call: taken on first use, so it starts with the device proxy's first forwarded call.
double load_trace_seconds() {
    static const auto started = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

std::atomic<std::uint64_t> g_load_trace_lines{0};

void load_trace_note(const char* kind, unsigned slot, const void* caller, const std::uint64_t* words, unsigned count,
                     std::uint64_t result, bool has_result) {
    if (!load_trace_window().on) return;
    const double at = load_trace_seconds();
    const LoadTraceWindow& window = load_trace_window();
    if (at < window.from || at >= window.to) return;
    if (g_load_trace_lines.fetch_add(1, std::memory_order_relaxed) >= 60000) return;
    char where[64];
    streamed_blocks::caller_text(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller)), where,
                                 sizeof where);
    char args[256];
    int used = 0;
    for (unsigned i = 0; i < count && static_cast<size_t>(used) < sizeof args - 32; ++i)
        used += std::snprintf(args + used, sizeof args - static_cast<size_t>(used), " 0x%llx",
                              static_cast<unsigned long long>(words[i]));
    if (used < 0) used = 0;
    if (has_result && static_cast<size_t>(used) < sizeof args - 40)
        std::snprintf(args + used, sizeof args - static_cast<size_t>(used), " result 0x%llx",
                      static_cast<unsigned long long>(result));
    std::fprintf(stderr, "[rt] load trace +%.3fs tid %lu %s slot %u: args%s%s caller %s\n", at,
                 static_cast<unsigned long>(GetCurrentThreadId()), kind, slot, count ? "" : " (none)", args, where);
}

// ---- Title memory watch (measurement only; PALMETTO_TITLE_WATCH=<hex_rva>[,...]) ------------------------------------
//
// docs/streamed_consumer.md section 9: once the loader gate's state word is named (from the polling-loop disassembly),
// this reads it - and up to three more words - from the title image once a second and prints the values, so the
// transition that never happens is a measured sequence instead of an inference. RVAs only (no title content is printed),
// bounded (4 words x 121 lines), off by default, reads title memory and changes no call.
constexpr unsigned kTitleWatchMax = 4;
std::atomic<unsigned> g_title_watch_count{0};
std::uint64_t g_title_watch_rva[kTitleWatchMax];

bool title_watch_enabled() {
    static const bool on = [] {
        char value[128] = {};
        return GetEnvironmentVariableA("PALMETTO_TITLE_WATCH", value, sizeof(value)) > 0;
    }();
    return on;
}

void title_watch_start() {
    static std::atomic<bool> started{false};
    if (!title_watch_enabled() || started.exchange(true, std::memory_order_relaxed)) return;
    // Parse the RVA list (comma separated, hex with or without 0x) once.
    char value[128] = {};
    if (GetEnvironmentVariableA("PALMETTO_TITLE_WATCH", value, sizeof(value)) > 0) {
        char* at = value;
        while (*at && g_title_watch_count.load(std::memory_order_relaxed) < kTitleWatchMax) {
            char* end = nullptr;
            const unsigned long long rva = std::strtoull(at, &end, 16);
            if (end == at) break;
            const unsigned index = g_title_watch_count.fetch_add(1, std::memory_order_relaxed);
            if (index < kTitleWatchMax) g_title_watch_rva[index] = rva;
            at = end;
            while (*at == ',' || *at == ' ') ++at;
        }
    }
    if (g_title_watch_count.load(std::memory_order_relaxed) == 0) {
        std::fprintf(stderr, "[rt] title watch: PALMETTO_TITLE_WATCH has no RVA; nothing is watched\n");
        return;
    }
    std::thread([] {
        std::uint64_t title_base = 0;
        for (unsigned i = 0; i < 600 && title_base == 0; ++i) {  // the title base is published by the first DStorage call
            title_base = streamed_blocks::g_title_base.load(std::memory_order_relaxed);
            if (title_base == 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (title_base == 0) {
            std::fprintf(stderr, "[rt] title watch: no title image base was published; nothing is watched\n");
            return;
        }
        for (unsigned second = 0; second <= 120; ++second) {
            const unsigned count = std::min(g_title_watch_count.load(std::memory_order_relaxed), kTitleWatchMax);
            for (unsigned w = 0; w < count; ++w) {
                const std::uint64_t address = title_base + g_title_watch_rva[w];
                char text[96];
                if (!readable_range(reinterpret_cast<const void*>(address), 8)) {
                    std::snprintf(text, sizeof text, "unreadable");
                } else {
                    const std::uint64_t qword = *reinterpret_cast<const std::uint64_t*>(address);
                    std::snprintf(text, sizeof text, "qword 0x%016llx dword 0x%08x",
                                  static_cast<unsigned long long>(qword),
                                  *reinterpret_cast<const std::uint32_t*>(address));
                }
                std::fprintf(stderr, "[rt] title watch +%us rva 0x%llx: %s\n", second,
                             static_cast<unsigned long long>(g_title_watch_rva[w]), text);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }).detach();
}

// ---- Title pointer peek (measurement only; PALMETTO_TITLE_PEEK=<slot_rva>,<deref_off>,<field_off>,...) -------------
//
// docs/streamed_consumer.md section 9: the loading job's state fields are reached through two pointers in title memory
// (a global holds the manager; manager+deref holds the job object whose +0x78/+0x64/+0x6C fields the loading spinner
// polls). Once those RVAs are known this reads the pointer chain, then the listed job fields, once a second for 120 s, so
// the state transitions are measured values. Bounded (121 lines x fields), off by default, reads title memory, changes
// no call.
constexpr unsigned kTitlePeekMax = 10;

bool title_peek_enabled() {
    static const bool on = [] {
        char value[192] = {};
        return GetEnvironmentVariableA("PALMETTO_TITLE_PEEK", value, sizeof(value)) > 0;
    }();
    return on;
}

void title_peek_start() {
    static std::atomic<bool> started{false};
    if (!title_peek_enabled() || started.exchange(true, std::memory_order_relaxed)) return;
    char value[192] = {};
    if (GetEnvironmentVariableA("PALMETTO_TITLE_PEEK", value, sizeof(value)) == 0) return;
    std::uint64_t slot_rva = 0;
    std::uint64_t deref_offset = ~std::uint64_t{0};  // "!" after the slot means: the slot itself is the object
    std::uint64_t fields[kTitlePeekMax] = {};
    unsigned count = 0;
    char* at = value;
    slot_rva = std::strtoull(at, &at, 16);
    if (*at == '!') {
        ++at;
    } else if (*at == ',') {
        ++at;
        deref_offset = std::strtoull(at, &at, 16);
    }
    while (*at && count < kTitlePeekMax) {
        while (*at == ',' || *at == ' ') ++at;
        const std::uint64_t offset = std::strtoull(at, &at, 16);
        fields[count++] = offset;
    }
    std::thread([slot_rva, deref_offset, fields, count] {
        std::uint64_t title_base = 0;
        for (unsigned i = 0; i < 600 && title_base == 0; ++i) {
            title_base = streamed_blocks::g_title_base.load(std::memory_order_relaxed);
            if (title_base == 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (title_base == 0) return;
        // Dense sampling for the first 20 s (the loading burst and the job state transitions happen there), then 1 Hz.
        double elapsed_seconds = 0.0;
        for (unsigned sample = 0; sample < 180; ++sample) {
            const unsigned long milliseconds = sample < 80 ? 250 : 1000;
            const std::uint64_t slot = title_base + slot_rva;
            std::uint64_t manager = 0;
            if (readable_range(reinterpret_cast<const void*>(slot), 8))
                manager = *reinterpret_cast<const std::uint64_t*>(slot);
            std::uint64_t job = 0;
            if (manager && deref_offset == ~std::uint64_t{0}) {
                job = manager;
            } else if (manager && readable_range(reinterpret_cast<const void*>(manager + deref_offset), 8)) {
                job = *reinterpret_cast<const std::uint64_t*>(manager + deref_offset);
            }
            if (job == 0 || !readable_range(reinterpret_cast<const void*>(job), 0x88)) {
                std::fprintf(stderr, "[rt] title peek +%.2fs slot 0x%llx manager 0x%llx job 0x%llx: not set\n",
                             elapsed_seconds, static_cast<unsigned long long>(slot_rva),
                             static_cast<unsigned long long>(manager), static_cast<unsigned long long>(job));
            } else {
                for (unsigned w = 0; w < count; ++w) {
                    const std::uint64_t address = job + fields[w];
                    if (!readable_range(reinterpret_cast<const void*>(address), 8)) continue;
                    std::fprintf(stderr, "[rt] title peek +%.2fs job 0x%llx off 0x%llx: qword 0x%016llx\n",
                                 elapsed_seconds, static_cast<unsigned long long>(job),
                                 static_cast<unsigned long long>(fields[w]),
                                 static_cast<unsigned long long>(
                                     *reinterpret_cast<const std::uint64_t*>(address)));
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
            elapsed_seconds += static_cast<double>(milliseconds) / 1000.0;
        }
    }).detach();
}

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
// (port/src/command_list.h): resource id, mip, first array slice, layer count and view format. The proxy reads the
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
    if (streamed_blocks::active())
        stream_note_list_call(Slot, console_to_pc_list_slot(Slot), _ReturnAddress(), a1, a2, a3, a4, a5, a6);
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
    } else if constexpr (Slot == 71) {
        // Console Write32BitValueBottomOfPipeX (docs/bottom_of_pipe.md): (address, dword, flush, flags), measured
        // (address, dword, 0, 0) at game+0x4ED46AA.
        note_list_slot(Slot, a1, a2, a3, a4);
        console_list_extras::write32_bottom_of_pipe(a1, a2, a3, a4,
                                                    reinterpret_cast<std::uint64_t>(_ReturnAddress()));
        return 0;
    } else if constexpr (Slot == 72) {
        // Console Write64BitValueBottomOfPipeX: (address, qword, flush, flags), measured (address, 0, 0, 1) at
        // game+0x2482B37, flags 1/2 asking for a timestamp.
        note_list_slot(Slot, a1, a2, a3, a4);
        console_list_extras::write64_bottom_of_pipe(a1, a2, a3, a4,
                                                    reinterpret_cast<std::uint64_t>(_ReturnAddress()));
        return 0;
    } else if constexpr (Slot == 74) {
        // Console Wait64BitValueX: (address, comparison, qword, flags), measured (address, 7, qword, 1) at
        // game+0x2484B12; comparison 7 is GREATER_EQUAL.
        note_list_slot(Slot, a1, a2, a3, a4);
        console_list_extras::memory_wait64(a1, a2, a3, a4, reinterpret_cast<std::uint64_t>(_ReturnAddress()));
        return 0;
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
            // Console slot 61 (ExecuteIndirect, PC slot 59): a signature the proxy created as a stand-in for the
            // console's DISPATCH_MESH description has no replay in this layer (docs/command_signature.md). Refuse the
            // call by name and record no command, instead of running a dispatch the console would have run as mesh work.
            if constexpr (Slot == 61) {
                if (is_command_signature_standin(reinterpret_cast<const void*>(a1))) {
                    static std::atomic<unsigned> refused{0};
                    if (refused.fetch_add(1, std::memory_order_relaxed) < 4) {
                        char caller[64];
                        streamed_blocks::caller_text(reinterpret_cast<std::uint64_t>(_ReturnAddress()), caller,
                                                     sizeof(caller));
                        std::fprintf(stderr,
                                     "[rt] d3d12_xs: list slot 61 ExecuteIndirect with the console DISPATCH_MESH "
                                     "stand-in at %s: not recorded (mesh indirect replay is not implemented)\n",
                                     caller);
                    }
                    return 0;
                }
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
            // Measurement (docs/vertex_buffer_va.md): console slot 48 forwards to PC IASetVertexBuffers and console slot
            // 47 to PC IASetIndexBuffer, so the views the title binds are in the title's own memory. The probe prints
            // each view's VA, which is what the replay then has to resolve, and the title caller, so the source of a
            // title VA is measured at the moment the title names it. It runs when the existing PALMETTO_XS_LIST_DUMP
            // names slot 47 or 48 (docs/dev-tools.md); bounded, no call changes.
            if constexpr (Slot == 48 || Slot == 47) {
                static const bool vb_trace = [] {
                    char value[64] = {};
                    if (GetEnvironmentVariableA("PALMETTO_XS_LIST_DUMP", value, sizeof(value)) <= 0) return false;
                    char* at = value;
                    while (*at) {
                        char* end = nullptr;
                        const unsigned long dumped_slot = std::strtoul(at, &end, 0);
                        if (end == at) break;
                        if (dumped_slot == 47 || dumped_slot == 48) return true;
                        at = *end == ',' ? end + 1 : end;
                    }
                    return false;
                }();
                if (vb_trace) {
                    static std::atomic<unsigned> shown{0};
                    const unsigned n = shown.fetch_add(1, std::memory_order_relaxed);
                    if (n < 96) {
                        char caller[64];
                        streamed_blocks::caller_text(reinterpret_cast<std::uint64_t>(_ReturnAddress()), caller,
                                                     sizeof(caller));
                        if constexpr (Slot == 48) {
                            std::fprintf(stderr,
                                         "[rt] d3d12_xs: vb trace %u: start %llu count %llu views 0x%llx (caller %s)",
                                         n, static_cast<unsigned long long>(a1),
                                         static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
                                         caller);
                            const auto* views = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(a3);
                            const unsigned count = static_cast<unsigned>(std::min<std::uint64_t>(a2, 8));
                            if (a3 && count && readable_range(views, count * sizeof(*views))) {
                                for (unsigned i = 0; i < count; ++i)
                                    std::fprintf(stderr, " %u:va 0x%llx size %u stride %u", i,
                                                 static_cast<unsigned long long>(views[i].BufferLocation),
                                                 views[i].SizeInBytes, views[i].StrideInBytes);
                            } else if (a3 && a2) {
                                std::fprintf(stderr, " views at 0x%llx are not readable",
                                             static_cast<unsigned long long>(a3));
                            }
                        } else {
                            std::fprintf(stderr,
                                         "[rt] d3d12_xs: ib trace %u: view 0x%llx (caller %s)",
                                         n, static_cast<unsigned long long>(a1), caller);
                            const auto* view = reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(a1);
                            if (a1 && readable_range(view, sizeof(*view)))
                                std::fprintf(stderr, " va 0x%llx size %u format %u",
                                             static_cast<unsigned long long>(view->BufferLocation),
                                             view->SizeInBytes, view->Format);
                            else if (a1)
                                std::fprintf(stderr, " view at 0x%llx is not readable",
                                             static_cast<unsigned long long>(a1));
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
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

// Console list slot 70 is PC slot 62 OMSetDepthBounds (docs/bottom_of_pipe.md): the two floats are in XMM1/XMM2, which
// the integer thunk cannot carry, so this entry declares them and the compiler reads the same registers the title's
// call wrote. With the model on it forwards to the real list's PC slot 62; with it off it counts and returns, which is
// the proxy's previous visible behaviour for a void console slot.
std::uint64_t WINAPI list_slot_om_set_depth_bounds(ListProxy* self, float minimum, float maximum) {
    note_list_slot(kConsoleOmSetDepthBounds);
    if (callers_enabled()) note_caller(CallerKind::list, kConsoleOmSetDepthBounds, _ReturnAddress(), 0, 0, 0, 0);
    if (console_list_extras::active()) {
        auto& c = console_list_extras::counts();
        c.slot70_calls.fetch_add(1, std::memory_order_relaxed);
        if (console_list_extras::trace_enabled()) {
            static std::atomic<unsigned> lines{0};
            const unsigned n = lines.fetch_add(1, std::memory_order_relaxed);
            if (n < 8 || n % 4096 == 0) {
                char where[64];
                streamed_blocks::caller_text(reinterpret_cast<std::uint64_t>(_ReturnAddress()), where, sizeof(where));
                std::fprintf(stderr,
                             "[rt] d3d12_xs: console list slot 70 (OMSetDepthBounds) call %u: min %f max %f "
                             "(caller %s)\n",
                             n, static_cast<double>(minimum), static_cast<double>(maximum), where);
            }
        }
        if (console_list_extras_enabled()) {
            auto** const real_vtable = *reinterpret_cast<void***>(self->real);
            if (kPcOmSetDepthBounds < self->pc_slot_count) {
                using Fn = void(WINAPI*)(void*, float, float);
                reinterpret_cast<Fn>(real_vtable[kPcOmSetDepthBounds])(self->real, minimum, maximum);
                c.slot70_forwarded.fetch_add(1, std::memory_order_relaxed);
            } else {
                static std::atomic<unsigned> lines{0};
                if (lines.fetch_add(1, std::memory_order_relaxed) < 8)
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: console list slot 70 (OMSetDepthBounds) refused: the real list has "
                                 "only %u slots, PC slot %u is past it\n",
                                 self->pc_slot_count, kPcOmSetDepthBounds);
            }
        }
        console_list_extras::maybe_print_summary();
    }
    return 0;
}

void** get_list_vtable() {
    static void** vtable = [] {
        static void* storage[kListSlots];
        fill_list_vtable(storage, std::make_index_sequence<kListSlots>{});
        storage[kConsoleOmSetDepthBounds] = reinterpret_cast<void*>(&list_slot_om_set_depth_bounds);
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

// ---- Queue slot 31: CopyPageMappingsBatchX ---------------------------------------------------------------
//
// Re-derived from our own copy of the retail executable (static): the call goes through the queue's [+0xF8] entry at
// game+0xF2CC7F, the console queue's slot 31, past the PC chain that ends at console GetDesc 20. The argument setup is
// the console shape: EDX is the batch count, R8 the batch array, R9 the physical-page pool's base VA, [rsp+0x20] the
// pool's length in 64 KiB pages and [rsp+0x28] 0 (flags); eax is not tested after the call, so the title reads the
// destinations whatever the answer is. The builder (game+0xF2C460..0xF2CD18) stores a batch at a 24-byte stride
// (game+0xF2C709 advances to the next one) as {UINT64 destination; UINT32 range count; const range* ranges} and a range
// at a 12-byte stride (game+0xF2C6C4 advances to the next one) as {UINT32 kind, start, count}; the kinds the builder
// emits from the pool it created in game+0x1E6C110 are 1 (one pool page at the run's destination, repeated) and 2
// (unmap the destination pages). The console ABI name CopyPageMappingsBatchX comes from the public console layers that
// dispatch it; Microsoft's public Learn reference does not document the method, so the shape above (not the name) is
// what this branch implements.
//
// On the console the call installs GPU page-table mappings from the pool's physical pages at the destination pages. Our
// xmem owns those physical pages, so every range reaches rt::xmem::copy_page_mappings_fn, which gives each destination
// page a second view of its source page's physical page (both addresses are the one memory) or unmaps it; the pool base
// is a VA of the title's own mapped pages, and a source page with nothing mapped leaves its destination page unmapped.
// The destination window is the same title VA the D3D12 layer reads on the CPU: xmem maps host memory at the console VA
// and the layer commits those pages before reading them (docs/page_address_space.md), so unlike a layer that reads a
// paired CPU reservation there is no second address here to fill - making the destination readable IS making the
// title-visible memory readable.
//
// Without xmem (a GPU-free test binary) the hook is null and the call keeps the answer every queue slot past the PC
// chain gets below: the beyond-table warning once and E_NOTIMPL, never a guessed success. With it, a range outside the
// pool, an unknown kind, a null batch array or a range xmem refuses is logged once and the call returns E_INVALIDARG
// after applying the ranges it could (the title does not read the result, and a partly applied batch is closer to the
// console than dropping it).
constexpr std::uint64_t kConsolePageSize = 0x10000;

// One range with the batch cursor at its first destination page. The hook is called with the pool page(s) the range
// names: kind 0 maps pool pages start, start + 1, ... one after another; kind 1 maps the one pool page `start` at every
// destination page; kind 2 unmaps them; kind 3 leaves the destination pages as they are (no call, nothing changes).
bool copy_console_page_range(const rt::xmem::PageRange& range, std::uint64_t destination, std::uint64_t pool,
                             std::uint32_t pool_pages) {
    const rt::xmem::CopyPageMappingsFn remap = rt::xmem::copy_page_mappings_fn;
    if (!remap) return false;
    void* const at = reinterpret_cast<void*>(destination);
    const void* const from = reinterpret_cast<const void*>(pool + std::uint64_t{range.start} * kConsolePageSize);
    switch (range.kind) {
        case 0:
            return pool != 0 && std::uint64_t{range.start} + range.count <= pool_pages &&
                   remap(at, from, range.count, false);
        case 1:
            return pool != 0 && range.start < pool_pages && remap(at, from, range.count, true);
        case 2:
            return remap(at, nullptr, range.count, false);
        case 3:
            return true;
        default:
            return false;
    }
}

HRESULT queue_copy_page_mappings(std::uint64_t count_word, std::uint64_t batches_word, std::uint64_t pool,
                                 std::uint64_t pool_pages_word) {
    const auto count = static_cast<std::uint32_t>(count_word);
    const auto pool_pages = static_cast<std::uint32_t>(pool_pages_word);
    const auto* const batches = reinterpret_cast<const rt::xmem::PageBatch*>(batches_word);
    static std::atomic<bool> first_call{true}, first_refusal{true};
    if (first_call.exchange(false, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: queue slot 31 (CopyPageMappingsBatchX) first call: %u batch(es), pool 0x%llx "
                     "of %u 64 KiB pages\n",
                     count, static_cast<unsigned long long>(pool), pool_pages);
    const auto refuse = [](const char* why) {
        if (first_refusal.exchange(false, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: queue slot 31: %s; refusing that range with E_INVALIDARG (logged once)\n",
                         why);
    };
    if (count && !batches) {
        refuse("a batch count with a null batch array");
        return E_INVALIDARG;
    }
    bool all = true;
    for (std::uint32_t b = 0; b < count; ++b) {
        const rt::xmem::PageBatch& batch = batches[b];
        if (batch.count && !batch.ranges) {
            all = false;
            refuse("a range count with a null range array");
            continue;
        }
        std::uint64_t destination = batch.destination;
        for (std::uint32_t r = 0; r < batch.count; ++r) {
            const rt::xmem::PageRange& range = batch.ranges[r];
            if (!copy_console_page_range(range, destination, pool, pool_pages)) {
                all = false;
                char why[128];
                std::snprintf(why, sizeof(why),
                              "range kind %u, pool pages %u+%u at 0x%llx (pool of %u pages%s)", range.kind,
                              range.start, range.count, static_cast<unsigned long long>(destination), pool_pages,
                              rt::xmem::copy_page_mappings_fn ? "" : ", no mapper");
                refuse(why);
            }
            destination += std::uint64_t{range.count} * kConsolePageSize;
        }
    }
    return all ? S_OK : E_INVALIDARG;
}

template <ChildKind K, unsigned Slot>
std::uint64_t WINAPI child_slot(ChildProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    constexpr CallerKind caller_kind = K == ChildKind::queue   ? CallerKind::queue
                                       : K == ChildKind::fence ? CallerKind::fence
                                                               : CallerKind::allocator;
    g_pl_console_calls.fetch_add(1, std::memory_order_relaxed);
    if constexpr (K == ChildKind::queue && Slot == 32) g_pl_presents.fetch_add(1, std::memory_order_relaxed);
    if constexpr (K == ChildKind::queue && Slot == 35) g_pl_submits.fetch_add(1, std::memory_order_relaxed);
    if (callers_enabled()) note_caller(caller_kind, Slot, _ReturnAddress(), a1, a2, a3, a4);
    if constexpr (K == ChildKind::queue || K == ChildKind::fence) {
        if (load_trace_window().on) {
            // Queue/fence calls: the first four of each slot (so the frame loop's call shapes are visible) plus every
            // call from a loader thread.
            static std::atomic<unsigned> first_calls{0};
            const unsigned n = first_calls.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 4 || load_tids::contains(static_cast<unsigned long>(GetCurrentThreadId()))) {
                const std::uint64_t load_words[3] = {a1, a2, a3};
                load_trace_note(K == ChildKind::queue ? "queue" : "fence", Slot, _ReturnAddress(), load_words, 3, 0,
                                false);
            }
        }
    }
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
    } else if constexpr (K == ChildKind::allocator && Slot == kConsoleAllocatorCallbackSlot) {
        // Console allocator slot 9, the callback registration (docs/allocator_slot9.md). Measured on this title from
        // game+0x188501: the caller passes (callback in RDX, context in R8) on the allocator it is about to use - once
        // per presented frame on this title - and ignores the answer: the instructions after the return overwrite RAX
        // before anything tests it. The console runtime would call the callback when it needs the allocator's recording
        // memory; our command lists own that memory in the Vulkan layer, so there is nothing for a title callback to
        // provide. The pair is recorded on this proxy and the call answers S_OK; the callback is never invoked and the
        // real allocator is never addressed (its slot 9 is not this method). Slots 10+ keep the measured refusal below.
        note_child_slot<K>(Slot);
        d3d12_trace::record(child_trace_name<K>(Slot));
        record_console_allocator_callback(self, a1, a2);
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
        if (load_trace_window().on) {
            const std::uint64_t cell_words[2] = {a1, completed};
            load_trace_note("fence-cell", Slot, _ReturnAddress(), cell_words, 2,
                            reinterpret_cast<std::uint64_t>(self->value_cell.get()), true);
        }
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
        // Queue slot 31 (CopyPageMappingsBatchX) is answered through xmem's page mapping when this build has one. With
        // no hook (a GPU-free test binary) the call falls through to the beyond-table refusal below, which is the
        // answer every unmodelled queue slot past the PC chain keeps.
        if constexpr (K == ChildKind::queue) {
            if (Slot == kQueueCopyPageMappingsBatchX && rt::xmem::copy_page_mappings_fn) {
                const HRESULT mapped = queue_copy_page_mappings(a1, a2, a3, a4);
                return hr_to_u64(d3d12_trace::result(child_trace_name<K>(Slot), mapped));
            }
        }
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
            if (load_trace_window().on)
                load_trace_note("fence-result", Slot, _ReturnAddress(), nullptr, 0, result, true);
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

// ---- Console device 70: a descriptor publication into a descriptor slot, and the measurement of it ------------------
//
// The console device vtable is the PC one only up to slot 43; from 44 on it is the console's own extension table (the
// slot-55 comment below), so slot 70 is a console extension and not PC CreatePlacedResource1 - which is why forwarding it
// by number crashed (docs/boot_audit.md section 5). docs/device_slot70.md carries the full evidence; the measured facts:
//
//   * call shape. Wrapper game+0x4ECC800 (function 0x4ECC800..0x4ECC830): `push rsi; sub rsp,0x20; rsi=r8; r8=rdx;
//     rdx=rcx; rcx=<device global>; r9=rsi; call [rax+0x230]` - three arguments after the device - then
//     `mov rcx,rsi; xor edx,edx; jmp 0x4ECC830`, the cache-line writeback (`clwb` over
//     [align_down(arg3,64), arg3+global)), i.e. the title publishes its third argument and flushes its cache lines.
//     The dominant site game+0x4D9F363 (inside function game+0x4D9F200..0x4D9F3B2, return address game+0x4D9F369,
//     13,568 calls in a 130.2 s boot) calls the slot directly with the same three-argument shape. The fifth word the
//     thunk receives at the wrapper site is the wrapper's saved rsi (`push rsi`), i.e. not an argument: the call's
//     argument vector is three words.
//   * argument contents (measured in the boot, both sites): arg1 is a D3D12_RESOURCE_DESC of a BUFFER over the block
//     the caller just filled (Dimension 1, Width 0x80 at the dominant site and 0xAE00000 at the wrapper site);
//     arg3 is a 32-byte-aligned descriptor slot - a console descriptor heap (0x...220) at the wrapper site, a host
//     descriptor slot of an ordinary shader-visible heap at the dominant site - and it holds zeros before the call at
//     12,214 of the first 12,288 calls. arg2 is a 0x40-byte record whose measured fields at the dominant site are:
//     +0x00 the destination address (equal to arg3), +0x08 0x1688, +0x18 the element count (0x20 for the 128-byte
//     block), +0x28 the CPU block the caller filled, +0x30 the block's format word. At the wrapper site +0x00 is the
//     fixed word 0x100000027 instead, so the record's first word is not a constant view address.
//
// The naive conversion - the PC device's CreateConstantBufferView over (arg2, arg3) - was measured and is falsified: it
// writes a V# whose base is the record's first word, which at the dominant site is the destination descriptor itself,
// so the replay refuses the next submissions ("ExecuteCommandLists failed: unsupported; nothing was submitted"), present
// fails, and the title crashes 29.5 s into the boot (docs/device_slot70.md section 5). The conversion is therefore left
// behind the off-by-default switch as a measurement instrument, not as a mode this tree enables.
//
// The default answer is the measured refusal (E_NOTIMPL, which the title does not test), written inline by the caller so
// the default path is unchanged. PALMETTO_SLOT70_PROBE=1 dumps the two records, the destination before/after and the
// per-heap-page/offset statistics; PALMETTO_SLOT70_CBV=1 adds the falsified conversion; PALMETTO_SLOT70_PUBLISH=1 adds
// the matrix publication over the record's CPU block (section 6 of the page). All are read once and off by default; a
// call outside the measured shape is refused by name.
bool slot70_enabled(const char* name) {
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    return length > 0 && value[0] != '\0' && value[0] != '0';
}

// The measurement switches are read once, on the first slot-70 call. Two reasons: a boot sets them before the title
// starts, and the default path must not import a call on the slot-70 path (the tree measured its boot with this branch
// answering inline; an extra call there was measured to change the boot's timing, docs/device_slot70.md section 6), and
// the GPU-free test sets the conversion before its first slot-70 call. `measurement_enabled` is one cached boolean, so
// with neither switch set the default answer costs no call at all.
bool slot70_measurement_enabled() {
    static const bool on = [] {
        return slot70_enabled("PALMETTO_SLOT70_PROBE") || slot70_enabled("PALMETTO_SLOT70_CBV") ||
               slot70_enabled("PALMETTO_SLOT70_PUBLISH") || slot70_enabled("PALMETTO_SLOT70_CENSUS") ||
               slot70_enabled("PALMETTO_SLOT70_PAIR");
    }();
    return on;
}
bool slot70_probe_enabled() {
    static const bool on = slot70_enabled("PALMETTO_SLOT70_PROBE");
    return on;
}
bool slot70_cbv_enabled() {
    static const bool on = slot70_enabled("PALMETTO_SLOT70_CBV");
    return on;
}
// The measurement of the publication record (docs/device_slot70.md section 5): write a MATRIX V# over the record's CPU
// block into the destination slot. Off by default; the layout that proved wrong (a CBV over the record's first word) is
// the separate PALMETTO_SLOT70_CBV switch above.
bool slot70_publish_enabled() {
    static const bool on = slot70_enabled("PALMETTO_SLOT70_PUBLISH");
    return on;
}
// The shape census (docs/device_slot70.md section 6): is slot 70 also the producer of sampled T#s, or are all its
// records buffer publications? Bounded: the distinct shape is keyed by the fields the publication decision reads plus the
// resource description and the sort of its first word; one line per new shape (64 at most) and a summary per 4096 calls.
bool slot70_census_enabled() {
    static const bool on = slot70_enabled("PALMETTO_SLOT70_CENSUS");
    return on;
}
// Pairing the texture publications with the console resource creations (docs/device_slot70.md section 8).
bool slot70_pair_enabled() {
    static const bool on = slot70_enabled("PALMETTO_SLOT70_PAIR");
    return on;
}

constexpr unsigned kSlot70Sites = 4;
constexpr unsigned kSlot70DestPages = 8;
std::atomic<std::uint64_t> g_slot70_calls{0};
std::atomic<std::uint64_t> g_slot70_site_caller[kSlot70Sites];
std::atomic<std::uint64_t> g_slot70_site_hits[kSlot70Sites];
std::atomic<std::uint64_t> g_slot70_dest_page[kSlot70DestPages];
std::atomic<std::uint64_t> g_slot70_dest_page_hits[kSlot70DestPages];
std::atomic<std::uint64_t> g_slot70_dest_min{0};
std::atomic<std::uint64_t> g_slot70_dest_max{0};
std::atomic<std::uint64_t> g_slot70_dest_zero{0};   // destinations that held zeros before the call
std::atomic<std::uint64_t> g_slot70_published{0};   // conversions that reached the PC CreateConstantBufferView
std::atomic<bool> g_slot70_shape_logged{false};
// The matrix publication (PALMETTO_SLOT70_PUBLISH=1): publications written, and why the rest were not.
std::atomic<std::uint64_t> g_slot70_matrix_published{0};
std::atomic<std::uint64_t> g_slot70_matrix_empty{0};     // the record's block pages held zeros
std::atomic<std::uint64_t> g_slot70_matrix_unplaced{0};  // the block is not inside a buffer the title placed at slot 133
std::atomic<std::uint64_t> g_slot70_matrix_unreadable{0};
std::atomic<bool> g_slot70_matrix_logged{false};
// The census measured texture-shaped resource descriptions at this slot (60 calls, all at the startup wrapper site). They
// are not pinned to a console T# encoding, so the publication refuses them by name instead of writing a guess.
std::atomic<std::uint64_t> g_slot70_texture_refused{0};
std::atomic<bool> g_slot70_texture_logged{false};
// The pairing (PALMETTO_SLOT70_PAIR=1): texture publications that found a matching creation, those that did not, and the
// layer T#s written.
std::atomic<std::uint64_t> g_slot70_pairs_found{0};
std::atomic<std::uint64_t> g_slot70_pairs_published{0};
std::atomic<std::uint64_t> g_slot70_pairs_none{0};
std::atomic<bool> g_slot70_pair_logged{false};
// The creation lifetime registry (section 9): every console resource creation the proxy handles, bounded by count, with
// the description, the made resource, its GPU VA (a buffer VA or an image key) and the title VA. Declared here so the
// slot-70 summary can report it.
constexpr unsigned kSlot70Creations = 1024;
struct Slot70CreationRecord {
    std::uint64_t call_index = 0;  // device-call sequence when the creation was seen
    std::uint64_t title_va = 0;
    std::uint64_t gpu_va = 0;  // GetGPUVirtualAddress: a buffer VA or an image key for a texture
    std::uint64_t resource = 0;
    unsigned source = 0;  // 76, 127 or 133
    D3D12_RESOURCE_DESC desc{};
};
Slot70CreationRecord g_slot70_creations[kSlot70Creations];
std::uint64_t g_slot70_creation_total = 0;
std::uint64_t g_slot70_creation_dropped = 0;
std::mutex g_slot70_creation_mutex;
std::atomic<std::uint64_t> g_device_call_index{0};
std::atomic<std::uint64_t> g_slot70_identity_va{0};
std::atomic<bool> g_slot70_identity_done{false};


// The census' distinct shapes. A shape is what the publication decision reads plus the resource description and the kind
// of the record's first word, so two boots' logs can be compared as counts. The mutex is only taken on the census path
// (off by default) and the table is bounded, so a boot cannot be slowed or flooded by it.
struct Slot70ShapeKey {
    std::uint32_t dimension = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    std::uint32_t mip_levels = 0;
    std::uint32_t array_size = 0;
    std::uint32_t count = 0;
    std::uint32_t declared = 0;
    std::uint32_t format_word = 0;
    std::uint32_t first_word_kind = 0;  // 1 = the fixed 0x100000027, 2 = the destination address, 0 = other/absent
    std::uint32_t block_nonzero = 0;
    std::uint64_t width = 0;
    std::uint64_t calls = 0;
};
constexpr unsigned kSlot70Shapes = 64;
Slot70ShapeKey g_slot70_shapes[kSlot70Shapes];
unsigned g_slot70_shape_count = 0;
std::mutex g_slot70_census_mutex;
// The texture-shaped publication probe (docs/device_slot70.md section 7): the 60 startup wrapper-site calls carry
// texture resource descriptions, and the question is whether the title writes a console T# into the destination itself
// (then the mirror already copies it) or leaves it for the device to write. Bounded: the first 8 distinct destinations
// are dumped (32-byte descriptor, before) and re-read later in the boot (three times each) so a write that happens after
// the call is caught. Only descriptor bytes are printed - the record's block is counted, never printed (no game data).
constexpr unsigned kSlot70TextureProbes = 8;
struct Slot70TextureProbe {
    std::atomic<std::uint64_t> dest{0};
    std::atomic<unsigned> dumps{0};
    std::atomic<std::uint32_t> last[8];
};
Slot70TextureProbe g_slot70_texture_probes[kSlot70TextureProbes];
std::atomic<unsigned> g_slot70_texture_probe_count{0};
std::mutex g_slot70_texture_probe_mutex;

std::atomic<std::uint64_t> g_slot70_census_calls{0};
std::atomic<std::uint64_t> g_slot70_census_non_buffer{0};
std::atomic<std::uint64_t> g_slot70_census_dest32{0};
std::atomic<std::uint64_t> g_slot70_census_dest16{0};

// The buffer placements the proxy accepted at console slot 133 (CreatePlacedResourceX), recorded on success. The
// slot-70 publication only names a block inside one of them: only a title range the layer registered has an arena
// stand-in, so only its V# base is rebased into the shader-visible copy and only its bytes are pulled by the replay's
// page pull (docs/page_address_space.md, docs/page_content.md). A block outside them would produce a V# the shader adds
// heap_base to and nothing backs, so it is refused instead.
constexpr unsigned kSlot70PlacedRanges = 32;
std::atomic<std::uint64_t> g_placed_range_va[kSlot70PlacedRanges];
std::atomic<std::uint64_t> g_placed_range_width[kSlot70PlacedRanges];
std::atomic<unsigned> g_placed_range_count{0};
std::atomic<bool> g_placed_range_overflow{false};

void note_placed_buffer_range(std::uint64_t va, std::uint64_t width) {
    if (!va || !width) return;
    const unsigned slot = g_placed_range_count.load(std::memory_order_relaxed);
    if (slot >= kSlot70PlacedRanges) {
        if (!g_placed_range_overflow.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: %u placed buffer ranges recorded; slot 70 cannot name a later one\n",
                         kSlot70PlacedRanges);
        return;
    }
    g_placed_range_va[slot].store(va, std::memory_order_relaxed);
    g_placed_range_width[slot].store(width, std::memory_order_relaxed);
    g_placed_range_count.store(slot + 1, std::memory_order_release);
    // One bounded line per placement, so the boot log names the range a publication will name (ABI words only).
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
        std::fprintf(stderr, "[rt] d3d12_xs: slot 133 placed buffer range 0x%llx + 0x%llx\n",
                     static_cast<unsigned long long>(va), static_cast<unsigned long long>(width));
}

bool find_placed_buffer_range(std::uint64_t va, std::uint64_t& base, std::uint64_t& width) {
    const unsigned count = g_placed_range_count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count && i < kSlot70PlacedRanges; ++i) {
        const std::uint64_t start = g_placed_range_va[i].load(std::memory_order_relaxed);
        const std::uint64_t bytes = g_placed_range_width[i].load(std::memory_order_relaxed);
        if (bytes && va >= start && va - start < bytes) {
            base = start;
            width = bytes;
            return true;
        }
    }
    return false;
}

// A destination's heap page and offset within it: a console descriptor heap is a 64 KiB-aligned console address, so
// the page is what the heap was created at and the offset is descriptor_size * index.
void slot70_note_dest(std::uint64_t dest, bool zero_before) {
    const std::uint64_t page = dest & ~std::uint64_t(0xFFFF);
    const std::uint64_t offset = dest & 0xFFFF;
    for (unsigned i = 0; i < kSlot70DestPages; ++i) {
        const std::uint64_t known = g_slot70_dest_page[i].load(std::memory_order_relaxed);
        if (known == page) {
            g_slot70_dest_page_hits[i].fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (known != 0) continue;
        std::uint64_t expected = 0;
        if (g_slot70_dest_page[i].compare_exchange_strong(expected, page, std::memory_order_relaxed))
            g_slot70_dest_page_hits[i].fetch_add(1, std::memory_order_relaxed);
        break;
    }
    std::uint64_t low = g_slot70_dest_min.load(std::memory_order_relaxed);
    while (offset < low && !g_slot70_dest_min.compare_exchange_weak(low, offset, std::memory_order_relaxed)) {}
    std::uint64_t high = g_slot70_dest_max.load(std::memory_order_relaxed);
    while (offset > high && !g_slot70_dest_max.compare_exchange_weak(high, offset, std::memory_order_relaxed)) {}
    if (zero_before) g_slot70_dest_zero.fetch_add(1, std::memory_order_relaxed);
}

void slot70_dump(const char* what, std::uint64_t pointer, unsigned bytes) {
    if (!pointer || !bytes || !readable_range(reinterpret_cast<const void*>(pointer), bytes)) {
        std::fprintf(stderr, "[rt] d3d12_xs: slot 70 probe: %s at 0x%llx is not readable\n", what,
                     static_cast<unsigned long long>(pointer));
        return;
    }
    const auto* words = reinterpret_cast<const std::uint32_t*>(pointer);
    char line[512];
    int at = std::snprintf(line, sizeof line, "[rt] d3d12_xs: slot 70 probe: %s 0x%llx:", what,
                           static_cast<unsigned long long>(pointer));
    for (unsigned i = 0; i < bytes / 4 && at > 0 && static_cast<std::size_t>(at) < sizeof line - 16; ++i)
        at += std::snprintf(line + at, sizeof line - static_cast<std::size_t>(at), " %08x", words[i]);
    std::fprintf(stderr, "%s\n", line);
}

void slot70_summary(std::uint64_t calls) {
    char pages[512];
    int at = std::snprintf(pages, sizeof pages, "[rt] d3d12_xs: slot 70: %llu call(s); destination heap page(s):",
                           static_cast<unsigned long long>(calls));
    for (unsigned i = 0; i < kSlot70DestPages && at > 0 && static_cast<std::size_t>(at) < sizeof pages - 64; ++i) {
        const std::uint64_t page = g_slot70_dest_page[i].load(std::memory_order_relaxed);
        if (!page) continue;
        at += std::snprintf(pages + at, sizeof pages - static_cast<std::size_t>(at), " 0x%llx +%llu", 
                            static_cast<unsigned long long>(page),
                            static_cast<unsigned long long>(g_slot70_dest_page_hits[i].load(std::memory_order_relaxed)));
    }
    std::fprintf(stderr, "%s\n", pages);
    std::fprintf(stderr,
                 "[rt] d3d12_xs: slot 70: destination offsets 0x%llx..0x%llx, %llu call(s) left the destination zeroed, "
                 "%llu CBV conversion(s)\n",
                 static_cast<unsigned long long>(g_slot70_dest_min.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_dest_max.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_dest_zero.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_published.load(std::memory_order_relaxed)));
    // The matrix publication: what was written, and what was refused and why (counts only - no block bytes).
    std::fprintf(stderr,
                 "[rt] d3d12_xs: slot 70 publish: %llu matrix V# written, %llu refused (empty block pages), %llu "
                 "refused (block outside a slot-133 range), %llu refused (block unreadable), %llu refused "
                 "(texture-shaped description, no pinned T# encoding)\n",
                 static_cast<unsigned long long>(g_slot70_matrix_published.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_matrix_empty.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_matrix_unplaced.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_matrix_unreadable.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_texture_refused.load(std::memory_order_relaxed)));
    std::fprintf(stderr,
                 "[rt] d3d12_xs: slot 70 pair: %llu texture publication(s) paired, %llu with no creation in the window, "
                 "%llu layer T# published\n",
                 static_cast<unsigned long long>(g_slot70_pairs_found.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_pairs_none.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_pairs_published.load(std::memory_order_relaxed)));
    {
        const std::lock_guard lock(g_slot70_creation_mutex);
        const std::uint64_t total =
            g_slot70_creation_total < kSlot70Creations ? g_slot70_creation_total : kSlot70Creations;
        unsigned by_source[134] = {};
        unsigned with_gpu_va = 0;
        for (std::uint64_t i = 0; i < total; ++i) {
            if (g_slot70_creations[i].source < 134) ++by_source[g_slot70_creations[i].source];
            if (g_slot70_creations[i].gpu_va) ++with_gpu_va;
        }
        std::fprintf(stderr,
                     "[rt] d3d12_xs: creation registry: %llu creation(s) recorded (%llu dropped), %u with a GPU VA; "
                     "slot 76/127/133 = %u/%u/%u\n",
                     static_cast<unsigned long long>(total),
                     static_cast<unsigned long long>(g_slot70_creation_dropped), with_gpu_va, by_source[76],
                     by_source[127], by_source[133]);
    }
}

void slot70_census_note(const D3D12_RESOURCE_DESC* desc, bool desc_readable, const std::uint8_t* record,
                        bool record_readable, std::uint64_t dest, std::uint64_t call) {
    Slot70ShapeKey key;
    if (desc_readable && desc) {
        key.dimension = desc->Dimension;
        key.width = desc->Width;
        key.height = desc->Height;
        key.format = static_cast<std::uint32_t>(desc->Format);
        key.mip_levels = desc->MipLevels;
        key.array_size = desc->DepthOrArraySize;
    }
    std::uint64_t block = 0;
    if (record_readable && record) {
        const std::uint64_t first = *reinterpret_cast<const std::uint64_t*>(record);
        key.count = *reinterpret_cast<const std::uint32_t*>(record + 0x18);
        key.declared = *reinterpret_cast<const std::uint32_t*>(record + 0x08);
        key.format_word = *reinterpret_cast<const std::uint32_t*>(record + 0x30);
        key.first_word_kind = first == 0x100000027ull ? 1u : (first == dest ? 2u : 0u);
        block = *reinterpret_cast<const std::uint64_t*>(record + 0x28);
    }
    if (block && readable_range(reinterpret_cast<const void*>(block), 64)) {
        const auto* words = reinterpret_cast<const std::uint32_t*>(block);
        for (unsigned i = 0; i < 16; ++i) key.block_nonzero += words[i] != 0 ? 1u : 0u;
    }
    g_slot70_census_calls.fetch_add(1, std::memory_order_relaxed);
    if (key.dimension != 1) g_slot70_census_non_buffer.fetch_add(1, std::memory_order_relaxed);
    if ((dest & 31) == 0) g_slot70_census_dest32.fetch_add(1, std::memory_order_relaxed);
    else if ((dest & 15) == 0) g_slot70_census_dest16.fetch_add(1, std::memory_order_relaxed);
    const std::lock_guard lock(g_slot70_census_mutex);
    for (unsigned i = 0; i < g_slot70_shape_count; ++i) {
        Slot70ShapeKey& known = g_slot70_shapes[i];
        if (known.dimension == key.dimension && known.width == key.width && known.height == key.height &&
            known.format == key.format && known.mip_levels == key.mip_levels && known.array_size == key.array_size &&
            known.count == key.count && known.declared == key.declared && known.format_word == key.format_word &&
            known.first_word_kind == key.first_word_kind && known.block_nonzero == key.block_nonzero) {
            ++known.calls;
            return;
        }
    }
    if (g_slot70_shape_count >= kSlot70Shapes) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 70 census: %u distinct shapes, the table is full; later shapes are "
                         "folded into the call count\n",
                         kSlot70Shapes);
        return;
    }
    key.calls = 1;
    g_slot70_shapes[g_slot70_shape_count] = key;
    ++g_slot70_shape_count;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: slot 70 census: shape #%u: desc dim=%u width=0x%llx height=0x%x format=0x%x mips=0x%x "
                 "array=0x%x; record count=0x%x declared=0x%x format_word=0x%x first_word_kind=%u block_nonzero=%u "
                 "(call %llu)\n",
                 g_slot70_shape_count, key.dimension, static_cast<unsigned long long>(key.width), key.height, key.format,
                 key.mip_levels, key.array_size, key.count, key.declared, key.format_word, key.first_word_kind,
                 key.block_nonzero, static_cast<unsigned long long>(call));
}

void slot70_census_summary(std::uint64_t calls) {
    const std::lock_guard lock(g_slot70_census_mutex);
    std::fprintf(stderr,
                 "[rt] d3d12_xs: slot 70 census: %llu call(s), %u distinct shape(s), %llu non-buffer desc(s), "
                 "destinations %llu 32-byte-aligned / %llu 16-byte-aligned\n",
                 static_cast<unsigned long long>(calls), g_slot70_shape_count,
                 static_cast<unsigned long long>(g_slot70_census_non_buffer.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_census_dest32.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slot70_census_dest16.load(std::memory_order_relaxed)));
    // The distinct format words with how many calls and how many distinct shapes carry each: a T#-shaped console format
    // word would show here next to the buffer ones.
    struct FormatWordHistogram {
        std::uint32_t word = 0;
        std::uint64_t calls = 0;
        unsigned shapes = 0;
    } histogram[16] = {};
    unsigned used = 0;
    for (unsigned i = 0; i < g_slot70_shape_count; ++i) {
        unsigned at = used;
        for (unsigned h = 0; h < used; ++h)
            if (histogram[h].word == g_slot70_shapes[i].format_word) at = h;
        if (at == used) {
            if (used == std::size(histogram)) continue;
            histogram[used].word = g_slot70_shapes[i].format_word;
            ++used;
        }
        histogram[at].calls += g_slot70_shapes[i].calls;
        ++histogram[at].shapes;
    }
    char line[512];
    int at = std::snprintf(line, sizeof line, "[rt] d3d12_xs: slot 70 census: format words:");
    for (unsigned h = 0; h < used && at > 0 && static_cast<std::size_t>(at) < sizeof line - 64; ++h)
        at += std::snprintf(line + at, sizeof line - static_cast<std::size_t>(at), " 0x%x (%llu call(s), %u shape(s))",
                            histogram[h].word, static_cast<unsigned long long>(histogram[h].calls),
                            histogram[h].shapes);
    std::fprintf(stderr, "%s\n", line);
}

// ---- Pairing the texture publications with the resources the title creates, and the creation lifetime registry ------
//
// The texture publications carry a resource description but no resource handle, so the device behind the call can only
// resolve the resource from the creations around it. This is a bounded-by-count lifetime record (1024) of every console
// resource creation the proxy handles - device slot 76 (the console's committed-resource creation), slot 127
// (create-with-layout) and slot 133 (placed resources) - with the Xbox description, the real resource the layer made, its
// GPU VA (GetGPUVirtualAddress: a buffer VA or, for a texture, the layer's image key), the title VA (133) and the device
// call index. It answers two questions: which creation a texture publication views (pairing) and whether a given address
// was ever created (identity lookup, PALMETTO_SLOT70_IDENTITY_VA).


// The identity lookup's target (PALMETTO_SLOT70_IDENTITY_VA, hex). Read once; 0 means no lookup.
std::uint64_t slot70_identity_target() {
    static const std::uint64_t value = [] {
        char text[32] = {};
        if (GetEnvironmentVariableA("PALMETTO_SLOT70_IDENTITY_VA", text, sizeof(text)) == 0) return std::uint64_t(0);
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(text, &end, 16);
        return static_cast<std::uint64_t>(parsed);
    }();
    return value;
}

// A created resource's GPU VA: for a texture that is the layer's image key (GetGPUVirtualAddress), the same value a
// title uses to build a T#. Only safe on a real COM object (a test's synthetic pointer is not, and is skipped).
std::uint64_t slot70_resource_gpu_va(std::uint64_t resource) {
    if (!resource) return 0;
    if (!readable_range(reinterpret_cast<const void*>(resource), sizeof(void*))) return 0;
    const auto* vtable_pointer = *reinterpret_cast<void* const*>(reinterpret_cast<const void*>(resource));
    if (!readable_range(vtable_pointer, sizeof(void*))) return 0;
    return reinterpret_cast<ID3D12Resource*>(resource)->GetGPUVirtualAddress();
}

void note_console_resource_creation(unsigned slot, const D3D12_RESOURCE_DESC& desc, void* resource,
                                    std::uint64_t va) {
    const std::lock_guard lock(g_slot70_creation_mutex);
    if (g_slot70_creation_total >= kSlot70Creations) {
        ++g_slot70_creation_dropped;
        return;
    }
    Slot70CreationRecord& entry = g_slot70_creations[g_slot70_creation_total];
    entry.call_index = g_device_call_index.load(std::memory_order_relaxed);
    entry.title_va = va;
    entry.resource = reinterpret_cast<std::uint64_t>(resource);
    entry.gpu_va = slot70_resource_gpu_va(entry.resource);
    entry.source = slot;
    entry.desc = desc;
    ++g_slot70_creation_total;
    // A creation's GPU VA is the image key a title builds a T# from: report it for the first few, and answer the identity
    // lookup as soon as a creation matches the configured address.
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 24)
        std::fprintf(stderr,
                     "[rt] d3d12_xs: creation registry: #%llu slot %u dim=%u 0x%llx x 0x%x format=0x%x mips=0x%x "
                     "array=0x%x title VA=0x%llx gpu VA=0x%llx resource 0x%llx\n",
                     static_cast<unsigned long long>(g_slot70_creation_total), slot,
                     static_cast<unsigned>(desc.Dimension), static_cast<unsigned long long>(desc.Width),
                     static_cast<unsigned>(desc.Height), static_cast<unsigned>(desc.Format),
                     static_cast<unsigned>(desc.MipLevels), static_cast<unsigned>(desc.DepthOrArraySize),
                     static_cast<unsigned long long>(va), static_cast<unsigned long long>(entry.gpu_va),
                     static_cast<unsigned long long>(entry.resource));
}

void slot70_report_identity() {
    const std::uint64_t target = slot70_identity_target();
    if (!target || g_slot70_identity_done.exchange(true, std::memory_order_relaxed)) return;
    const std::lock_guard lock(g_slot70_creation_mutex);
    const std::uint64_t total = g_slot70_creation_total < kSlot70Creations ? g_slot70_creation_total : kSlot70Creations;
    const Slot70CreationRecord* by_title = nullptr;
    const Slot70CreationRecord* by_gpu = nullptr;
    const Slot70CreationRecord* contains = nullptr;
    unsigned matching_shape = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
        const Slot70CreationRecord& entry = g_slot70_creations[i];
        if (entry.title_va == target) by_title = &entry;
        if (entry.gpu_va == target) by_gpu = &entry;
        const std::uint64_t extent = entry.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? entry.desc.Width : 0;
        if (entry.title_va && target >= entry.title_va && target - entry.title_va < extent) contains = &entry;
        if (entry.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && entry.desc.Width == 3840 &&
            entry.desc.Height == 2160)
            ++matching_shape;
    }
    if (by_title)
        std::fprintf(stderr,
                     "[rt] d3d12_xs: identity 0x%llx: found as a creation's TITLE VA: slot %u dim=%u 0x%llx x 0x%x "
                     "format=0x%x mips=0x%x array=0x%x gpu VA=0x%llx resource 0x%llx (call %llu)\n",
                     static_cast<unsigned long long>(target), by_title->source,
                     static_cast<unsigned>(by_title->desc.Dimension),
                     static_cast<unsigned long long>(by_title->desc.Width), static_cast<unsigned>(by_title->desc.Height),
                     static_cast<unsigned>(by_title->desc.Format), static_cast<unsigned>(by_title->desc.MipLevels),
                     static_cast<unsigned>(by_title->desc.DepthOrArraySize),
                     static_cast<unsigned long long>(by_title->gpu_va),
                     static_cast<unsigned long long>(by_title->resource),
                     static_cast<unsigned long long>(by_title->call_index));
    else if (by_gpu)
        std::fprintf(stderr,
                     "[rt] d3d12_xs: identity 0x%llx: found as a creation's GPU VA (a buffer VA or an IMAGE KEY): slot %u "
                     "dim=%u 0x%llx x 0x%x format=0x%x mips=0x%x array=0x%x title VA=0x%llx resource 0x%llx (call %llu); "
                     "%u creation(s) are 3840x2160 2D textures\n",
                     static_cast<unsigned long long>(target), by_gpu->source,
                     static_cast<unsigned>(by_gpu->desc.Dimension),
                     static_cast<unsigned long long>(by_gpu->desc.Width), static_cast<unsigned>(by_gpu->desc.Height),
                     static_cast<unsigned>(by_gpu->desc.Format), static_cast<unsigned>(by_gpu->desc.MipLevels),
                     static_cast<unsigned>(by_gpu->desc.DepthOrArraySize),
                     static_cast<unsigned long long>(by_gpu->title_va),
                     static_cast<unsigned long long>(by_gpu->resource),
                     static_cast<unsigned long long>(by_gpu->call_index), matching_shape);
    else if (contains)
        std::fprintf(stderr,
                     "[rt] d3d12_xs: identity 0x%llx: not a creation VA or GPU VA, but INSIDE creation #%llu's title range "
                     "(title VA 0x%llx width 0x%llx, offset 0x%llx, dim=%u format=0x%x)\n",
                     static_cast<unsigned long long>(target),
                     static_cast<unsigned long long>(contains - g_slot70_creations),
                     static_cast<unsigned long long>(contains->title_va),
                     static_cast<unsigned long long>(contains->desc.Width),
                     static_cast<unsigned long long>(target - contains->title_va),
                     static_cast<unsigned>(contains->desc.Dimension), static_cast<unsigned>(contains->desc.Format));
    else
        std::fprintf(stderr,
                     "[rt] d3d12_xs: identity 0x%llx: NOT the title VA, GPU VA (image key) or range of any of the %llu "
                     "creation(s) recorded%s; %u of them are 3840x2160 2D textures\n",
                     static_cast<unsigned long long>(target), static_cast<unsigned long long>(total),
                     g_slot70_creation_dropped ? " (registry full; dropped creations)" : "", matching_shape);
}

// The newest creation before `call` whose description matches the publication on dimension/width/height/format/mips/array.
bool slot70_find_pairing(const D3D12_RESOURCE_DESC& publication, std::uint64_t call, std::uint64_t& resource,
                         std::uint64_t& title_va, std::uint64_t& gpu_va, unsigned& source, std::uint64_t& distance,
                         unsigned& candidates, unsigned& matched) {
    const std::lock_guard lock(g_slot70_creation_mutex);
    const std::uint64_t total = g_slot70_creation_total < kSlot70Creations ? g_slot70_creation_total : kSlot70Creations;
    bool found = false;
    std::uint64_t best = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
        const Slot70CreationRecord& entry = g_slot70_creations[i];
        if (entry.call_index > call) continue;
        const std::uint64_t d = call - entry.call_index;
        ++candidates;
        const D3D12_RESOURCE_DESC& c = entry.desc;
        if (c.Dimension != publication.Dimension || c.Width != publication.Width || c.Height != publication.Height ||
            c.Format != publication.Format || c.MipLevels != publication.MipLevels ||
            c.DepthOrArraySize != publication.DepthOrArraySize)
            continue;
        ++matched;
        if (!found || d <= best) {
            found = true;
            best = d;
            resource = entry.resource;
            title_va = entry.title_va;
            gpu_va = entry.gpu_va;
            source = entry.source;
            distance = d;
        }
    }
    return found && resource != 0;
}

void slot70_publish_texture_view(DeviceProxy* self, const D3D12_RESOURCE_DESC& publication, std::uint64_t resource,
                                 std::uint64_t destination, std::uint64_t call, unsigned source, std::uint64_t distance,
                                 unsigned candidates, unsigned matched) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = publication.Format;
    bool supported = true;
    switch (publication.Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            if (publication.DepthOrArraySize > 1) {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srv.Texture1DArray = {0, publication.MipLevels, 0, publication.DepthOrArraySize, 0.0f};
            } else {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                srv.Texture1D = {0, publication.MipLevels, 0.0f};
            }
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            if (publication.DepthOrArraySize > 1) {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srv.Texture2DArray = {0, publication.MipLevels, 0, publication.DepthOrArraySize, 0, 0.0f};
            } else {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D = {0, publication.MipLevels, 0, 0.0f};
            }
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            srv.Texture3D = {0, publication.MipLevels, 0.0f};
            break;
        default:
            supported = false;
            break;
    }
    if (!supported || !destination || (destination & 15) ||
        !readable_range(reinterpret_cast<const void*>(destination), 32))
        return;
    // Through the real device: the layer encodes its own T# at the title's destination slot (a console-addressed heap's
    // CPU handle is the title's own address), and the heap mirror copies it to the GPU-visible stand-in. The PC method
    // returns void, as CreateConstantBufferView does; the layer logs a view it cannot make.
    self->real->CreateShaderResourceView(reinterpret_cast<ID3D12Resource*>(resource), &srv,
                                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(destination)});
    const std::uint64_t published = g_slot70_pairs_published.fetch_add(1, std::memory_order_relaxed) + 1;
    if (published <= 8 || call % 4096 == 0)
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 70 pair: publication dim=%u 0x%llx x 0x%x format=0x%x mips=0x%x array=0x%x -> "
                     "slot %u resource 0x%llx at distance %llu of %u candidate(s), %u full match(es); the layer SRV "
                     "wrote its T# at dest page=0x%llx off=0x%llx, call=%llu, published #%llu\n",
                     static_cast<unsigned>(publication.Dimension),
                     static_cast<unsigned long long>(publication.Width), static_cast<unsigned>(publication.Height),
                     static_cast<unsigned>(publication.Format), static_cast<unsigned>(publication.MipLevels),
                     static_cast<unsigned>(publication.DepthOrArraySize), source,
                     static_cast<unsigned long long>(resource), static_cast<unsigned long long>(distance), candidates,
                     matched, static_cast<unsigned long long>(destination & ~std::uint64_t(0xFFFF)),
                     static_cast<unsigned long long>(destination & 0xFFFF), static_cast<unsigned long long>(call),
                     static_cast<unsigned long long>(published));
}

void slot70_texture_probe(const D3D12_RESOURCE_DESC* desc, const std::uint8_t* record, bool record_readable,
                           std::uint64_t dest, std::uint64_t call) {
    if (!desc || !((dest & 15) == 0) || !readable_range(reinterpret_cast<const void*>(dest), 32)) return;
    std::uint32_t before[8] = {};
    std::memcpy(before, reinterpret_cast<const void*>(dest), sizeof(before));
    std::uint32_t count = 0, declared = 0, format_word = 0;
    unsigned block_non_zero = 0;
    std::uint64_t block = 0;
    if (record_readable && record) {
        count = *reinterpret_cast<const std::uint32_t*>(record + 0x18);
        declared = *reinterpret_cast<const std::uint32_t*>(record + 0x08);
        format_word = *reinterpret_cast<const std::uint32_t*>(record + 0x30);
        block = *reinterpret_cast<const std::uint64_t*>(record + 0x28);
        if (block && readable_range(reinterpret_cast<const void*>(block), 64)) {
            const auto* words = reinterpret_cast<const std::uint32_t*>(block);
            for (unsigned i = 0; i < 16; ++i) block_non_zero += words[i] != 0 ? 1u : 0u;
        }
    }
    const std::lock_guard lock(g_slot70_texture_probe_mutex);
    unsigned at = g_slot70_texture_probe_count.load(std::memory_order_relaxed);
    bool known = false;
    for (unsigned i = 0; i < at; ++i)
        if (g_slot70_texture_probes[i].dest.load(std::memory_order_relaxed) == dest) known = true;
    if (!known) {
        if (at >= kSlot70TextureProbes) return;
        Slot70TextureProbe& entry = g_slot70_texture_probes[at];
        entry.dest.store(dest, std::memory_order_relaxed);
        entry.dumps.store(0, std::memory_order_relaxed);
        for (unsigned i = 0; i < 8; ++i) entry.last[i].store(0, std::memory_order_relaxed);
        g_slot70_texture_probe_count.store(at + 1, std::memory_order_relaxed);
    }
    std::fprintf(stderr,
                 "[rt] d3d12_xs: slot 70 texture probe: desc dim=%u width=0x%llx height=0x%x format=0x%x mips=0x%x "
                 "array=0x%x; record count=0x%x declared=0x%x format_word=0x%x block_non_zero=%u/16; dest page=0x%llx "
                 "off=0x%llx call=%llu; descriptor before: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 static_cast<unsigned>(desc->Dimension), static_cast<unsigned long long>(desc->Width),
                 static_cast<unsigned>(desc->Height), static_cast<unsigned>(desc->Format),
                 static_cast<unsigned>(desc->MipLevels), static_cast<unsigned>(desc->DepthOrArraySize), count, declared,
                 format_word, block_non_zero, static_cast<unsigned long long>(dest & ~std::uint64_t(0xFFFF)),
                 static_cast<unsigned long long>(dest & 0xFFFF), static_cast<unsigned long long>(call), before[0],
                 before[1], before[2], before[3], before[4], before[5], before[6], before[7]);
}

void slot70_texture_probe_redump(std::uint64_t call) {
    const std::lock_guard lock(g_slot70_texture_probe_mutex);
    const unsigned count = g_slot70_texture_probe_count.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < count; ++i) {
        Slot70TextureProbe& entry = g_slot70_texture_probes[i];
        if (entry.dumps.load(std::memory_order_relaxed) >= 3) continue;
        const std::uint64_t dest = entry.dest.load(std::memory_order_relaxed);
        if (!dest || !readable_range(reinterpret_cast<const void*>(dest), 32)) continue;
        std::uint32_t now[8] = {};
        std::memcpy(now, reinterpret_cast<const void*>(dest), sizeof(now));
        bool same = true;
        for (unsigned w = 0; w < 8; ++w) same = same && now[w] == entry.last[w].load(std::memory_order_relaxed);
        if (same) continue;
        for (unsigned w = 0; w < 8; ++w) entry.last[w].store(now[w], std::memory_order_relaxed);
        const unsigned dumps = entry.dumps.fetch_add(1, std::memory_order_relaxed) + 1;
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 70 texture probe: re-dump #%u at call %llu dest page=0x%llx off=0x%llx: %08x "
                     "%08x %08x %08x %08x %08x %08x %08x\n",
                     dumps, static_cast<unsigned long long>(call),
                     static_cast<unsigned long long>(dest & ~std::uint64_t(0xFFFF)),
                     static_cast<unsigned long long>(dest & 0xFFFF), now[0], now[1], now[2], now[3], now[4], now[5],
                     now[6], now[7]);
    }
}

std::uint64_t proxy_console_slot70(DeviceProxy* self, std::uint64_t caller, std::uint64_t a1, std::uint64_t a2,
                                   std::uint64_t a3, std::uint64_t word4) {
    const std::uint64_t call = g_slot70_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool probe = slot70_probe_enabled();
    // The first four distinct call sites: a fixed array, no allocation on this path.
    bool new_site = false;
    for (unsigned i = 0; i < kSlot70Sites; ++i) {
        const std::uint64_t known = g_slot70_site_caller[i].load(std::memory_order_relaxed);
        if (known == caller) {
            g_slot70_site_hits[i].fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (known != 0) continue;
        std::uint64_t expected = 0;
        if (g_slot70_site_caller[i].compare_exchange_strong(expected, caller, std::memory_order_relaxed)) {
            new_site = true;
            g_slot70_site_hits[i].fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
    const auto* resource = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a1);
    const auto* view = reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(a2);
    const bool view_readable = readable_range(reinterpret_cast<const void*>(a2), sizeof(D3D12_CONSTANT_BUFFER_VIEW_DESC));
    const bool desc_readable = readable_range(reinterpret_cast<const void*>(a1), sizeof(D3D12_RESOURCE_DESC));
    // The destination is a 32-byte-aligned slot in memory this process can write.
    // The destination is a 16-byte-aligned descriptor slot in memory this process can write. The publication boots
    // measured 32-byte-aligned offsets in three boots and a 16-byte-aligned ring base in the fourth (the object array
    // is 16-byte aligned), so 16 is the rule the measurement supports; the V# itself is 16 bytes.
    const bool dest_readable = (a3 & 15) == 0 && readable_range(reinterpret_cast<const void*>(a3), 16);
    std::uint32_t before[8] = {};
    bool dest_zero = false;
    if (dest_readable) {
        std::memcpy(before, reinterpret_cast<const void*>(a3), sizeof(before));
        dest_zero = before[0] == 0 && before[1] == 0 && before[2] == 0 && before[3] == 0;
    }
    slot70_note_dest(a3, dest_zero);
    if (probe && (new_site || call == 1 || call % 2048 == 0)) {
        char where[64];
        streamed_blocks::caller_text(caller, where, sizeof where);
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 70 probe: call %llu caller %s args 0x%llx 0x%llx 0x%llx 0x%llx "
                     "dest page=0x%llx off=0x%llx zero=%d\n",
                     static_cast<unsigned long long>(call), where, static_cast<unsigned long long>(a1),
                     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
                     static_cast<unsigned long long>(word4),
                     static_cast<unsigned long long>(a3 & ~std::uint64_t(0xFFFF)),
                     static_cast<unsigned long long>(a3 & 0xFFFF), dest_zero ? 1 : 0);
        if (new_site || call == 1) {
            // 0x80 bytes of each record: the first 56 are the D3D12_RESOURCE_DESC the console passes, and the rest is
            // dumped so a field the PC structure does not name cannot be missed.
            slot70_dump("arg1 record", a1, 0x80);
            slot70_dump("arg2 record", a2, 0x40);
            slot70_dump("destination before", a3, 32);
            // The stack word the title passed past the four register homes next to what the record itself holds. At the
            // wrapper site the fifth word is the wrapper's saved rsi (the wrapper's own push), which is how the
            // argument/stack split was established.
            std::fprintf(stderr, "[rt] d3d12_xs: slot 70 probe: fifth word 0x%llx, record first word 0x%llx\n",
                         static_cast<unsigned long long>(word4),
                         static_cast<unsigned long long>(view_readable
                                                             ? *reinterpret_cast<const std::uint64_t*>(
                                                                   reinterpret_cast<const void*>(a2))
                                                             : 0));
        }
    }
    if (probe && desc_readable && resource->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        readable_range(reinterpret_cast<const void*>(a2), 0x40))
        slot70_texture_probe(resource, reinterpret_cast<const std::uint8_t*>(a2), true, a3, call);
    if (probe && call % 1024 == 0) slot70_texture_probe_redump(call);
    if (slot70_census_enabled() && readable_range(reinterpret_cast<const void*>(a2), 0x40))
        slot70_census_note(resource, desc_readable, reinterpret_cast<const std::uint8_t*>(a2), true, a3, call);
    else if (slot70_census_enabled())
        slot70_census_note(resource, desc_readable, nullptr, false, a3, call);
    if (call % 4096 == 0) {
        slot70_report_identity();
        slot70_summary(call);
        if (slot70_census_enabled()) slot70_census_summary(call);
    }

    // The measured shape: a readable buffer description, a readable view record with a nonzero address and size, and a
    // 32-byte-aligned writable destination slot. Anything else is not this call and is refused rather than guessed.
    const bool buffer_desc = desc_readable && resource->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && resource->Width;

    // ---- The matrix publication (PALMETTO_SLOT70_PUBLISH=1, docs/device_slot70.md section 6) ----------------------
    // The measured record is the console's publication of the per-frame transform: 0x20 elements, a 128-byte buffer
    // description, and a CPU block at +0x28 that the caller filled. The console device writes the descriptor that view
    // needs; this tree's answer is the refusal below, so the destination slot stays empty. The publication writes a
    // MATRIX V# over that block:
    //   w0 = block VA bits 31:0, w1 = block VA bits 47:32 (stride 0, so w2 is a byte count), w2 = the record's declared
    //   byte size (>= 64: the translated module reads 16 dwords = 64 bytes, each gated by declared >= 4*(k+1)),
    //   w3 = 0x30000FAC (raw out-of-bounds select + identity destination select; the module never reads w3 for a matrix
    //   fetch, and 0x30000FAC is the layer's raw-buffer convention for the other raw views).
    // The base is written as the block's TITLE VA, not an arena VA: the layer's console-heap mirror and in-place scan
    // rebase every 48-bit V# base that names a registered title buffer at submission (measured: "2 V# base(s) rebased"),
    // and the replay's page pull then copies the V#'s declared range from the title pages into the arena before the GPU
    // reads it (docs/page_content.md). Only a block inside a buffer the title placed at console slot 133 has such a
    // registration, so a block outside the ranges this proxy accepted there is refused instead of published into a V#
    // nothing backs.
    const bool publish = slot70_publish_enabled();
    const auto* record_bytes = reinterpret_cast<const std::uint8_t*>(a2);
    const std::uint32_t record_count =
        view_readable ? *reinterpret_cast<const std::uint32_t*>(record_bytes + 0x18) : 0;
    const std::uint64_t block = view_readable ? *reinterpret_cast<const std::uint64_t*>(record_bytes + 0x28) : 0;
    const std::uint32_t declared = view_readable ? *reinterpret_cast<const std::uint32_t*>(record_bytes + 0x08) : 0;
    const std::uint32_t format_word = view_readable ? *reinterpret_cast<const std::uint32_t*>(record_bytes + 0x30) : 0;
    if (publish && buffer_desc && view_readable && dest_readable && record_count == 0x20 && block != 0 && declared >= 64) {
        unsigned non_zero = 0;
        const bool block_readable = readable_range(reinterpret_cast<const void*>(block), 64);
        if (block_readable) {
            const auto* words = reinterpret_cast<const std::uint32_t*>(block);
            for (unsigned i = 0; i < 16; ++i) non_zero += words[i] != 0 ? 1u : 0u;
        }
        std::uint64_t placed_base = 0, placed_width = 0;
        const bool placed = block_readable && non_zero != 0 && find_placed_buffer_range(block, placed_base, placed_width);
        if (!block_readable) {
            g_slot70_matrix_unreadable.fetch_add(1, std::memory_order_relaxed);
            if (!g_slot70_matrix_logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 70 publish: the record's block 0x%llx is not readable from this "
                             "process; publishing nothing\n",
                             static_cast<unsigned long long>(block));
        } else if (non_zero == 0) {
            g_slot70_matrix_empty.fetch_add(1, std::memory_order_relaxed);
            if (!g_slot70_matrix_logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 70 publish: the record's block pages are empty (first 64 bytes all "
                             "zero); publishing nothing, the publication carries no data yet\n");
        } else if (!placed) {
            g_slot70_matrix_unplaced.fetch_add(1, std::memory_order_relaxed);
            if (!g_slot70_matrix_logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 70 publish: block 0x%llx is not inside a buffer placed through "
                             "console slot 133; publishing nothing (its V# base would not be rebased or pulled)\n",
                             static_cast<unsigned long long>(block));
        } else {
            std::uint32_t descriptor[8] = {};
            descriptor[0] = static_cast<std::uint32_t>(block & 0xFFFFFFFFu);
            descriptor[1] = static_cast<std::uint32_t>((block >> 32) & 0xFFFFu);  // stride 0 in bits 29:16
            descriptor[2] = declared < 64 ? 64u : declared;
            descriptor[3] = 0x30000FACu;  // raw OOB select + identity destination select (never read for a matrix V#)
            // The V# is 16 bytes; the following 16 bytes of a 32-byte descriptor stay as the title left them.
            std::memcpy(reinterpret_cast<void*>(a3), descriptor, 16);
            const std::uint64_t published = g_slot70_matrix_published.fetch_add(1, std::memory_order_relaxed) + 1;
            if (probe && (published <= 4 || call % 4096 == 0)) {
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 70 publish #%llu: block 0x%llx (placed range 0x%llx + 0x%llx, offset "
                             "0x%llx), declared 0x%x, format 0x%x -> dest page=0x%llx off=0x%llx V# %08x %08x %08x "
                             "%08x\n",
                             static_cast<unsigned long long>(published), static_cast<unsigned long long>(block),
                             static_cast<unsigned long long>(placed_base),
                             static_cast<unsigned long long>(placed_width),
                             static_cast<unsigned long long>(block - placed_base), declared, format_word,
                             static_cast<unsigned long long>(a3 & ~std::uint64_t(0xFFFF)),
                             static_cast<unsigned long long>(a3 & 0xFFFF), descriptor[0], descriptor[1], descriptor[2],
                             descriptor[3]);
            }
            return hr_to_u64(S_OK);
        }
    }

    // ---- Pairing a texture-shaped publication with a recorded creation (PALMETTO_SLOT70_PAIR=1) ------------------
    // docs/device_slot70.md section 9: the publication carries only a resource description, so the resource is looked for
    // among every console creation recorded before the call; a proven pair publishes the layer's own T# at the title's
    // destination slot, and no pair keeps the measured refusal below.
    if (desc_readable && resource->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) slot70_report_identity();
    if (slot70_pair_enabled() && desc_readable && resource->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        readable_range(reinterpret_cast<const void*>(a2), 0x40)) {
        std::uint64_t resource_id = 0, pairing_title_va = 0, pairing_gpu_va = 0, distance = 0;
        unsigned source = 0, candidates = 0, matched = 0;
        if (slot70_find_pairing(*resource, g_device_call_index.load(std::memory_order_relaxed), resource_id, pairing_title_va, pairing_gpu_va, source, distance,
                                candidates, matched)) {
            g_slot70_pairs_found.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<unsigned> decision_lines{0};
            if (decision_lines.fetch_add(1, std::memory_order_relaxed) < 12)
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 70 pair: publication dim=%u 0x%llx x 0x%x format=0x%x mips=0x%x "
                             "array=0x%x call=%llu: %u creation(s) recorded before it, %u full match(es), chosen slot %u "
                             "distance %llu device call(s); resource 0x%llx gpu VA=0x%llx title VA=0x%llx\n",
                             static_cast<unsigned>(resource->Dimension),
                             static_cast<unsigned long long>(resource->Width),
                             static_cast<unsigned>(resource->Height), static_cast<unsigned>(resource->Format),
                             static_cast<unsigned>(resource->MipLevels),
                             static_cast<unsigned>(resource->DepthOrArraySize),
                             static_cast<unsigned long long>(call), candidates, matched, source,
                             static_cast<unsigned long long>(distance),
                             static_cast<unsigned long long>(resource_id),
                             static_cast<unsigned long long>(pairing_gpu_va),
                             static_cast<unsigned long long>(pairing_title_va));
            slot70_publish_texture_view(self, *resource, resource_id, a3, call, source, distance, candidates, matched);
            return hr_to_u64(S_OK);
        }
        g_slot70_pairs_none.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<unsigned> none_lines{0};
        if (none_lines.fetch_add(1, std::memory_order_relaxed) < 8)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 70 pair: publication dim=%u 0x%llx x 0x%x format=0x%x mips=0x%x array=0x%x "
                         "call=%llu: no recorded creation matches the publication (%u creation(s) recorded before it, "
                         "%u full match(es)); keeping the measured refusal\n",
                         static_cast<unsigned>(resource->Dimension),
                         static_cast<unsigned long long>(resource->Width), static_cast<unsigned>(resource->Height),
                         static_cast<unsigned>(resource->Format), static_cast<unsigned>(resource->MipLevels),
                         static_cast<unsigned>(resource->DepthOrArraySize), static_cast<unsigned long long>(call),
                         candidates, matched);
    }

    // A texture-shaped description is where the sampled-T# question lives (docs/device_slot70.md section 6). The census
    // measured 60 such calls at the startup wrapper site with the record's format word 0 and its declared size constant,
    // and no dimension matching the sampled images; nothing in the record pins a T# encoding, so writing one would be a
    // guess. Refuse by name and keep the measured refusal.
    if (publish && desc_readable && resource->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        g_slot70_texture_refused.fetch_add(1, std::memory_order_relaxed);
        if (!g_slot70_texture_logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 70 publish: a texture-shaped description (dimension=%u width=0x%llx "
                         "height=0x%x format=0x%x mips=0x%x array=0x%x) is not pinned to a T# encoding; refusing "
                         "instead of writing a T# (the record's format word and size are the same as the buffer calls')\n",
                         static_cast<unsigned>(resource->Dimension),
                         static_cast<unsigned long long>(resource->Width), static_cast<unsigned>(resource->Height),
                         static_cast<unsigned>(resource->Format), static_cast<unsigned>(resource->MipLevels),
                         static_cast<unsigned>(resource->DepthOrArraySize));
    }

    const bool shaped = buffer_desc && view_readable && view->BufferLocation && view->SizeInBytes && dest_readable;
    if (slot70_cbv_enabled() && shaped) {
        // PC CreateConstantBufferView returns void; it writes the descriptor at the destination (or leaves it zeroed for
        // a null description), so there is no host HRESULT to forward.
        self->real->CreateConstantBufferView(view, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a3)});
        g_slot70_published.fetch_add(1, std::memory_order_relaxed);
        if (probe && (new_site || call == 1 || call % 2048 == 0)) {
            std::fprintf(stderr, "[rt] d3d12_xs: slot 70 probe: published CBV location 0x%llx size 0x%llx\n",
                         static_cast<unsigned long long>(view->BufferLocation),
                         static_cast<unsigned long long>(view->SizeInBytes));
            slot70_dump("destination after", a3, 32);
        }
        return hr_to_u64(S_OK);
    }
    if (slot70_cbv_enabled() && !shaped && !g_slot70_shape_logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 70: the call does not have the measured shape (arg1 buffer description=%d, "
                     "arg2 view readable=%d, arg3 aligned/writable slot=%d); refusing instead of writing a "
                     "descriptor\n",
                     buffer_desc ? 1 : 0, view_readable ? 1 : 0, dest_readable ? 1 : 0);
    }
    // The title does not test this HRESULT, so its destination descriptors stay as they were.
    if (trace_enabled())
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot 70 called with 3 arguments (0x%llx, 0x%llx, 0x%llx); the console "
                     "shape is not the PC CreatePlacedResource1 one, refusing\n",
                     static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                     static_cast<unsigned long long>(a3));
    return hr_to_u64(E_NOTIMPL);
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
    // build() never fills the stream-output declaration (port/src/graphics_pso.h), and a description copied with it
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
    // PALMETTO_PIPELINE_TIMELINE: the conversion traffic as counters, independent of the histogram's lock.
    g_pl_slot57_calls.fetch_add(1, std::memory_order_relaxed);
    if (call.reached_create) {
        g_pl_slot57_converted.fetch_add(1, std::memory_order_relaxed);
        if (SUCCEEDED(call.result)) {
            g_pl_graphics_created.fetch_add(1, std::memory_order_relaxed);
            pipeline_timeline_note_event();
        }
    } else {
        g_pl_slot57_refused.fetch_add(1, std::memory_order_relaxed);
    }
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
    // ("XboxID3D12CommandQueue::Slot40"); d3d12_trace::record takes a fixed string (port/README.md, [d3d12-trace]).
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

// PALMETTO_SLOT67_PROGRAM_LOG=1 (measurement aid, docs/decomp_renderer.md section 12): one bounded line per slot-67
// call with the kind-6 program's size and SHA-256 (and the kind-0 state's when it is readable) and the call's result.
// The FSR upscale answer is a request count: the 29 translated FidelityFX programs carry their fspx_code_sha256 in the
// shader corpus index, so the boot's lines are matched against those to measure 0 of 29 before this support and n of
// 29 after it. Off by default; nothing about the call changes.
bool slot67_program_probe() {
    static const bool enabled = [] {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA("PALMETTO_SLOT67_PROGRAM_LOG", value, sizeof(value));
        return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
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
    if (slot67_program_probe()) {
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 4096) {
            const auto program_sha = [](const void* code, std::uint64_t size) {
                if (!code || !size || size > (16u << 20) || !readable_range(code, static_cast<std::size_t>(size)))
                    return std::string("unreadable");
                return palmetto::sha256_hex({static_cast<const std::uint8_t*>(code),
                                             static_cast<std::size_t>(size)});
            };
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 67 program #%llu kind6 %llu B sha256 %s kind0 %llu B sha256 %s -> "
                         "0x%08lx\n",
                         static_cast<unsigned long long>(call),
                         static_cast<unsigned long long>(xbox_desc->kind6_size),
                         program_sha(xbox_desc->kind6, xbox_desc->kind6_size).c_str(),
                         static_cast<unsigned long long>(xbox_desc->kind0_size),
                         program_sha(xbox_desc->kind0, xbox_desc->kind0_size).c_str(),
                         static_cast<unsigned long>(hr));
        }
    }
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

// The title's one PE export is __XMemAllocationHooks__, a pointer table at image+0x6844380 (checked from the image's
// export directory: one function, one name, RVA 0x684380, in the fixed-base retail 1.0.2.1 image). Entry 0 is the
// XMemAlloc hook, entry 1 the XMemFree hook; the console xmem runtime calls them so the title can put its own
// allocation scope in front of the GDK allocator, and the hook chain's bottom calls XMemAllocDefault/XMemFreeDefault.
// The same table is a fixed RVA for this image version, like every other RVA in this port.
constexpr std::uintptr_t kTitleAllocationHooksRva = 0x6844380;

// The word that routes the XMemAlloc hook chain to the display-surface window scope, from the two measured sides:
//   * the title registers its scope by pushing a {handler, &pair, ctx} node into bucket 0x80 of the hook table
//     (game+0x247ad64 -> game+0x145c8f990 writes image+0x6843770, which is bucket (0x6843770-0x6843370)/8 = 0x80;
//     game+0x4deb467 selects the bucket from (attributes >> 16) & 0xFF);
//   * the handler game+0x56a2a60 carves the window only when `(attributes & 0xc000) == 0x8000`, and passes
//     `attributes` through to the next handler unchanged.
// So bits 23:16 = 0x80 and bits 15:14 = 10 route one allocation through the title's window scope. The console's exact
// word is not observable from the title image; this is the routing part, and the shim's allocator ignores the rest.
constexpr std::uint32_t kTitleSurfaceAttributes = 0x00808000;

// The byte size the display-surface window accounts for: the measured shape is a single-sample, single-mip, 1-plane
// 2D RGBA8 texture (the title's two 3840x2160 surfaces, console format 28), and the console runtime's allocation of
// one is what the title's hook sees. Any other shape is not the measured surface and is left unaccounted; the doc
// names that limit.
std::uint64_t display_surface_bytes(const D3D12_RESOURCE_DESC& desc) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count > 1 || desc.MipLevels > 1 ||
        desc.DepthOrArraySize > 1 || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return 0;
    return desc.Width * desc.Height * 4;
}

// The default surface allocator: run the title's exported allocation hook chain the way the console runtime does. The
// hook returns the address the title's scope handed out (inside its window) or the default allocator's block. Before a
// title is mapped (the GPU-free tests) there is nothing to call and the answer is null.
void* title_surface_allocate(std::uint64_t bytes, std::uint32_t attributes) {
    const auto base = reinterpret_cast<std::uintptr_t>(rt::startup_trace::detail::crash_image_base());
    if (!base || !bytes) return nullptr;
    const std::uintptr_t hook = reinterpret_cast<const std::uintptr_t*>(base + kTitleAllocationHooksRva)[0];
    // A hook that is not an address inside the mapped image is not this image's export: refuse rather than call it.
    if (hook < base + 0x1000 || hook >= base + rt::startup_trace::detail::crash_image_size()) return nullptr;
    using AllocHook = std::uintptr_t (*)(std::uint64_t, std::uint32_t);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 127: display-surface memory is allocated through the title's XMem hook "
                     "(game+0x%llx): %llu byte(s), attributes 0x%x\n",
                     static_cast<unsigned long long>(hook - base), static_cast<unsigned long long>(bytes), attributes);
    return reinterpret_cast<void*>(reinterpret_cast<AllocHook>(hook)(bytes, attributes));
}

std::atomic<TitleSurfaceAllocator>& title_surface_allocator_slot() {
    static std::atomic<TitleSurfaceAllocator> slot{&title_surface_allocate};
    return slot;
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
    // The display-surface memory is what the title's 64 MiB window scope is for: the console runtime allocates it
    // through the title's exported XMem allocation hook, and game+0x247af25 then checks that the window is nearly used
    // up. The proxy replaced the runtime for this slot, so it makes that call for a display surface; without it the
    // title's window stays at 0 used and its check fires every boot (docs/range_assert.md).
    if ((heap_flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) != 0) {
        if (const std::uint64_t bytes = display_surface_bytes(pc_desc)) {
            if (const TitleSurfaceAllocator allocate = title_surface_allocator_slot().load(std::memory_order_acquire))
                (void)allocate(bytes, kTitleSurfaceAttributes);
        }
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
    if (SUCCEEDED(hr) && out && *out)
        note_console_resource_creation(127, *xbox_desc, *out, 0);
    if (SUCCEEDED(hr) && out && *out)
        note_console_resource_creation(76, *xbox_desc, *out, 0);
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
    const HRESULT hr = handler(self->real, va, pc_desc, state, *riid, out);
    // Measurement (docs/vertex_buffer_va.md): every slot-133 buffer creation that FAILED, and every large one, with
    // the VA the title placed it at and the handler's answer, so a VA a later command names can be matched to (or
    // definitively not matched to) a placement, and an arena that ran out of room is visible by VA. Small successes are
    // not printed (the boot makes thousands). It runs with the existing PALMETTO_ARENA_LOG (docs/dev-tools.md), because
    // a refused placement is the arena's own answer; bounded, no call changes.
    if (pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        static const bool placed_trace = [] {
            char value[8] = {};
            return GetEnvironmentVariableA("PALMETTO_ARENA_LOG", value, sizeof(value)) > 0 && value[0] != '\0' &&
                   value[0] != '0';
        }();
        const bool important = FAILED(hr) || pc_desc.Width >= (4ull << 20);
        if (placed_trace && important) {
            static std::atomic<unsigned> shown{0};
            const unsigned n = shown.fetch_add(1, std::memory_order_relaxed);
            if (n < 160)
                std::fprintf(stderr,
                             "[rt] d3d12_xs: placed trace %u: va=0x%llx width=0x%llx hr=0x%08x%s%s\n", n,
                             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(va)),
                             static_cast<unsigned long long>(pc_desc.Width), static_cast<unsigned>(hr),
                             SUCCEEDED(hr) ? "" : " (refused)",
                             SUCCEEDED(hr) && out && *out ? "" : " (no object)");
        }
    }
    // The title range this buffer stands for, kept for console slot 70's matrix publication: a V# base only names a
    // range the layer registered (and therefore rebases and pulls) when the buffer was made through this slot.
    if (SUCCEEDED(hr) && out && *out) {
        if (pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && pc_desc.Width)
            note_placed_buffer_range(reinterpret_cast<std::uint64_t>(va), pc_desc.Width);
        note_console_resource_creation(133, *xbox_desc, *out, reinterpret_cast<std::uint64_t>(va));
    }
    return hr;
}

// Device slot 41: CreateCommandSignature at its PC position. See the command-signature block near the top of this file
// for the measured console descriptions and the mapping rule. `caller` is the title's return address, for the probe.
HRESULT proxy_create_command_signature(DeviceProxy* self, const void* caller, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                       std::uint64_t root_signature, const IID* riid, void** out) {
    note_slot(41);
    if (out) *out = nullptr;
    // The title's description is never changed in place: when a console-only argument type has to be translated it is
    // copied into a bounded stack array, and the host is handed that copy. Every other description goes on unchanged.
    D3D12_INDIRECT_ARGUMENT_DESC translated[kCommandSignatureArguments];
    D3D12_COMMAND_SIGNATURE_DESC pc_desc{};
    const D3D12_COMMAND_SIGNATURE_DESC* forward_desc = desc;
    std::uint32_t probe_types[kCommandSignatureArguments] = {};
    unsigned probe_type_count = 0;
    bool console_dispatch = false;
    bool mesh_standin = false;
    if (desc && desc->pArgumentDescs && desc->NumArgumentDescs <= kCommandSignatureArguments &&
        readable_range(desc, sizeof(*desc)) &&
        readable_range(desc->pArgumentDescs, desc->NumArgumentDescs * sizeof(D3D12_INDIRECT_ARGUMENT_DESC))) {
        pc_desc = *desc;
        for (UINT i = 0; i < desc->NumArgumentDescs; ++i) {
            translated[i] = desc->pArgumentDescs[i];
            const std::uint32_t type = indirect_argument_type(translated[i]);
            probe_types[probe_type_count++] = type;
            if (type == kConsoleIndirectArgumentDispatch) {
                set_indirect_argument_type(translated[i], D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH);
                console_dispatch = true;
            } else if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH) {
                // The layer carries no mesh signature: a named dispatch-shaped stand-in is created for the title's
                // setup and the list proxy refuses an ExecuteIndirect that names it.
                set_indirect_argument_type(translated[i], D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH);
                mesh_standin = true;
            }
        }
        if (console_dispatch || mesh_standin) {
            pc_desc.pArgumentDescs = translated;
            forward_desc = &pc_desc;
        }
    }
    if (console_dispatch) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 41: console argument type 105 (stride %u) has no PC value; created as "
                         "D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH (docs/command_signature.md)\n",
                         desc ? static_cast<unsigned>(desc->ByteStride) : 0u);
    }
    if (mesh_standin) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 41: console DISPATCH_MESH description (stride %u) is created as a "
                         "named dispatch-shaped stand-in; ExecuteIndirect with it is refused (mesh indirect replay is "
                         "not implemented)\n",
                         desc ? static_cast<unsigned>(desc->ByteStride) : 0u);
    }
    HRESULT hr = E_NOTIMPL;
    if (41u >= self->slot_count) {
        hr = refuse_beyond_device(41, self->slot_count);
    } else {
        using Fn = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, std::uint64_t, const IID*,
                                    void**);
        hr = reinterpret_cast<Fn>(self->real_vtable[41])(self->real, forward_desc, root_signature, riid, out);
    }
    if (SUCCEEDED(hr) && mesh_standin && out && *out) note_command_signature_standin(*out);
    if (command_signature_probe()) {
        char caller_text[64];
        streamed_blocks::caller_text(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller)),
                                     caller_text, sizeof(caller_text));
        std::fprintf(stderr, "[rt] d3d12_xs: slot 41 call at %s: stride %u count %u node 0x%x root 0x%llx types",
                     caller_text, desc ? static_cast<unsigned>(desc->ByteStride) : 0u,
                     desc ? static_cast<unsigned>(desc->NumArgumentDescs) : 0u,
                     desc ? static_cast<unsigned>(desc->NodeMask) : 0u,
                     static_cast<unsigned long long>(root_signature));
        for (unsigned i = 0; i < probe_type_count; ++i) std::fprintf(stderr, " %x", probe_types[i]);
        std::fprintf(stderr, " -> 0x%08lx\n", static_cast<unsigned long>(hr));
    }
    return hr;
}

template <unsigned Slot>
std::uint64_t WINAPI proxy_slot(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    g_device_call_index.fetch_add(1, std::memory_order_relaxed);
    g_pl_console_calls.fetch_add(1, std::memory_order_relaxed);
    pipeline_timeline_start();
    if (callers_enabled()) note_caller(CallerKind::device, Slot, _ReturnAddress(), a1, a2, a3, a4);
    if (streamed_blocks::active()) stream_note_device_call(Slot, _ReturnAddress(), a1, a2, a3, a4, a5, a6);
    if (view_shape_enabled()) {
        const std::uint64_t shape_words[kViewShapeWords] = {a1,  a2,  a3,  a4,  a5,  a6,  a7,
                                                            a8,  a9,  a10, a11, a12, a13, a14};
        view_shape_probe(Slot, _ReturnAddress(), shape_words);
    }
    if (load_trace_window().on && load_tids::contains(static_cast<unsigned long>(GetCurrentThreadId()))) {
        const std::uint64_t load_words[4] = {a1, a2, a3, a4};
        load_trace_note("device", Slot, _ReturnAddress(), load_words, 4, 0, false);
    }
    title_watch_start();
    title_peek_start();
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
    } else if constexpr (Slot == 41) {
        return hr_to_u64(proxy_create_command_signature(self, _ReturnAddress(),
                                                        reinterpret_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(a1),
                                                        a2, reinterpret_cast<const IID*>(a3),
                                                        reinterpret_cast<void**>(a4)));
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
        g_pl_slot55_calls.fetch_add(1, std::memory_order_relaxed);
        const HRESULT hr = proxy_create_derived_pipeline_state(
            self, a1, a2, a3, reinterpret_cast<const IID*>(a4), reinterpret_cast<void**>(a5));
        if (SUCCEEDED(hr)) {
            g_pl_graphics_created.fetch_add(1, std::memory_order_relaxed);
            pipeline_timeline_note_event();
        }
        return hr_to_u64(hr);
    } else if constexpr (Slot == 57) {
        g_pl_last_slot57_caller.store(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress())),
                                      std::memory_order_relaxed);
        pipeline_timeline_note_event();
        return hr_to_u64(proxy_create_graphics_pipeline_probe(
            self, reinterpret_cast<const void*>(a1), reinterpret_cast<const IID*>(a2),
            reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 62) {
        g_pl_compute_calls.fetch_add(1, std::memory_order_relaxed);
        const HRESULT hr = proxy_create_compute_pipeline_state(
            self, reinterpret_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(a1), a2, a3,
            reinterpret_cast<const IID*>(a4), reinterpret_cast<void**>(a5));
        if (SUCCEEDED(hr) && a5 && *reinterpret_cast<void**>(a5)) {
            g_pl_compute_created.fetch_add(1, std::memory_order_relaxed);
            pipeline_timeline_note_event();
        }
        return hr_to_u64(hr);
    } else if constexpr (Slot == 67) {
        g_pl_compute_calls.fetch_add(1, std::memory_order_relaxed);
        const HRESULT hr = proxy_create_compute_pipeline_from_native_code(
            self, reinterpret_cast<const void*>(a1), reinterpret_cast<const IID*>(a2),
            reinterpret_cast<void**>(a3));
        if (SUCCEEDED(hr) && a3 && *reinterpret_cast<void**>(a3)) {
            g_pl_compute_created.fetch_add(1, std::memory_order_relaxed);
            pipeline_timeline_note_event();
        }
        return hr_to_u64(hr);
    } else if constexpr (Slot == 133) {
        return hr_to_u64(proxy_create_placed_resource_x(self, reinterpret_cast<void*>(a1),
                                                        reinterpret_cast<const D3D12_RESOURCE_DESC*>(a2),
                                                        static_cast<std::uint32_t>(a3), reinterpret_cast<const IID*>(a5),
                                                        reinterpret_cast<void**>(a6)));
    } else if constexpr (Slot == 74) {
        // Console slot 74 takes three words (pointer, 0x185c0, pointer) at its measured call site game+0x1e6c2c1 and
        // the title asserts when the answer is negative (int3 at game+0x1e6c2c5). The PC method at this vtable index
        // (ID3D12Device9::ShaderCacheControl) has another shape, so forwarding the console's words asks it for a mode
        // and a blob it cannot validate and it refuses. The console call's meaning is not measured beyond the three
        // words, so the measured answer is S_OK; the first call's words are printed once so a later reader can compare.
        note_slot(74);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: device slot 74 (console three-word call, game+0x1e6c2c1) returns S_OK: "
                         "words 0x%llx 0x%llx 0x%llx\n",
                         static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                         static_cast<unsigned long long>(a3));
        return hr_to_u64(S_OK);
    } else if constexpr (Slot == 25) {
        // GetResourceAllocationInfo returns D3D12_RESOURCE_ALLOCATION_INFO (16 bytes) by value: the same class as
        // slot 26 (docs/boot_audit.md A1). The console order puts the hidden return buffer in RCX, which this thunk
        // receives as `self`, and the device proxy in RDX (`a1`).
        note_slot(25);
        if (is_device_proxy(reinterpret_cast<const void*>(a1))) {
            auto* device = reinterpret_cast<DeviceProxy*>(a1);
            if (device->real && device->real_vtable) {
                const D3D12_RESOURCE_ALLOCATION_INFO info = slot25_allocation_info(
                    device->real, static_cast<UINT>(a2), static_cast<UINT>(a3),
                    reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4));
                std::memcpy(reinterpret_cast<void*>(self), &info, sizeof(info));
                return reinterpret_cast<std::uint64_t>(self);
            }
        }
        // PC order: the device is `self`, the hidden return buffer is a1.
        if (a1) {
            const D3D12_RESOURCE_ALLOCATION_INFO info = slot25_allocation_info(
                self->real, static_cast<UINT>(a2), static_cast<UINT>(a3),
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
        // Console extension (docs/device_slot70.md): the measured answer is the refusal below, written inline so the
        // default path - the one the boot was measured on - is unchanged and imports no call here. The measurement
        // switches (PALMETTO_SLOT70_PROBE=1, PALMETTO_SLOT70_CBV=1, read once) are what call into
        // proxy_console_slot70, which dumps the arguments and, with the conversion on, answers with the PC
        // CreateConstantBufferView.
        if (slot70_measurement_enabled())
            return proxy_console_slot70(self, reinterpret_cast<std::uint64_t>(_ReturnAddress()), a1, a2, a3, a4);
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

void set_title_surface_allocator(TitleSurfaceAllocator allocator) {
    title_surface_allocator_slot().store(allocator, std::memory_order_release);
}

TitleSurfaceAllocator title_surface_allocator() { return title_surface_allocator_slot().load(std::memory_order_acquire); }

ID3D12Device* device_proxy_real_device(const void* device) {
    if (!is_device_proxy(device)) return nullptr;
    return static_cast<const DeviceProxy*>(device)->real;
}

bool device_slot_overridden(unsigned slot) {
    if (slot <= 2 || slot == 25 || slot == 26 || slot == 8 || slot == 9 || slot == 14 || slot == 31 || slot == 32 ||
        slot == 34 || slot == 35 || slot == 36 || slot == 41 || slot == 45 || slot == 46 || slot == 50 || slot == 55 ||
        slot == 62 || slot == 67 || slot == 74 || slot == 75 || slot == 76 || slot == 127 || slot == 133)
        return true;
    const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
    return slot == routes.frame_interval || slot == routes.schedule_event || slot == routes.wait_event ||
           slot == routes.queue_x || slot == routes.list_x;
}

bool command_signature_standin(const void* signature) { return is_command_signature_standin(signature); }

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

AllocatorCallback allocator_proxy_callback(const void* allocator) {
    AllocatorCallback record;
    if (!is_allocator_proxy(allocator)) return record;
    const auto* const proxy = static_cast<const ChildProxy*>(allocator);
    record.callback = proxy->console_callback.load(std::memory_order_acquire);
    if (record.callback) record.context = proxy->console_callback_context.load(std::memory_order_acquire);
    return record;
}

ConsoleListExtraCounts console_list_extra_counts() {
    const auto& c = console_list_extras::counts();
    ConsoleListExtraCounts out;
    out.slot70_calls = c.slot70_calls.load(std::memory_order_relaxed);
    out.slot70_forwarded = c.slot70_forwarded.load(std::memory_order_relaxed);
    out.slot71_calls = c.slot71_calls.load(std::memory_order_relaxed);
    out.slot71_writes = c.slot71_writes.load(std::memory_order_relaxed);
    out.slot71_refused = c.slot71_refused.load(std::memory_order_relaxed);
    out.slot72_calls = c.slot72_calls.load(std::memory_order_relaxed);
    out.slot72_writes = c.slot72_writes.load(std::memory_order_relaxed);
    out.slot72_timestamps = c.slot72_timestamps.load(std::memory_order_relaxed);
    out.slot72_refused = c.slot72_refused.load(std::memory_order_relaxed);
    out.slot74_calls = c.slot74_calls.load(std::memory_order_relaxed);
    out.slot74_met = c.slot74_met.load(std::memory_order_relaxed);
    out.slot74_unmet = c.slot74_unmet.load(std::memory_order_relaxed);
    out.slot74_unreadable = c.slot74_unreadable.load(std::memory_order_relaxed);
    out.slot74_refused = c.slot74_refused.load(std::memory_order_relaxed);
    return out;
}

void set_console_list_extras_enabled(int mode) {
    console_list_extras::g_mode.store(mode == 0 || mode == 1 ? mode : -2, std::memory_order_relaxed);
}

bool console_list_extras_enabled() {
    const int mode = console_list_extras::g_mode.load(std::memory_order_relaxed);
    return mode == 1 || (mode == -2 && console_list_extras::env_enabled());
}

AllocatorCallbackCounts allocator_callback_counts() {
    const auto& c = allocator_callback_invocation::counts();
    AllocatorCallbackCounts out;
    out.registrations = c.registrations.load(std::memory_order_relaxed);
    out.first_registrations = c.first_registrations.load(std::memory_order_relaxed);
    out.invocations = c.invocations.load(std::memory_order_relaxed);
    out.refusals = c.refusals.load(std::memory_order_relaxed);
    return out;
}

void set_allocator_callback_invocation_enabled(int mode) {
    allocator_callback_invocation::g_mode.store(mode == 0 || mode == 1 ? mode : -2, std::memory_order_relaxed);
}

bool allocator_callback_invocation_enabled() {
    const int mode = allocator_callback_invocation::g_mode.load(std::memory_order_relaxed);
    return mode == 1 || (mode == -2 && allocator_callback_invocation::callback_env_enabled());
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
