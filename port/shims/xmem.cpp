// xmem.dll: the GDK's tracked allocator. Mapped onto the CRT's aligned heap and VirtualQueryEx.
//
// The measured ABI (docs from the clean-room spec) shows four imports:
//   XMemAlloc(size, attributes)
//   XMemFree(ptr, attributes)
//   XMemVirtualQuery(process, address, out64)
//   XMemGetWorkingSetStatistics(kind, out)   (kind 0x20000001 at all seven FSR call sites)
//
// ASSUMPTION: the attribute word is not decoded by the measured call sites (one passes 0x5e50000). Until a call
// site shows the alignment/type fields, every block is 64-byte aligned (covers cache lines and SIMD) and a size of
// zero is rounded up to one byte so the result is a stable, non-null pointer that XMemFree can release.
//
// The default allocators, the physical-page functions and XMemIsToolsMemAvailable follow argument shapes measured
// locally at a title's call sites (each function says which). The page functions are built on the Windows placeholder
// APIs (VirtualAlloc2, MapViewOfFile3), whose flags the title's own XMemVirtualAlloc calls already use.
#include <windows.h>
#include <heapapi.h>
#include <intrin.h>
#include <malloc.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../src/alloc_trace.h"
#include "shims.h"
#include "startup_trace.h"
#include "xmem.h"

namespace {

constexpr size_t kAlign = 64;
constexpr size_t kNoSize = static_cast<size_t>(-1);  // XMemFree has no size field

std::string caller_field(const void* caller) {
    return ",\"caller\":" + rt::startup_trace::quote(rt::startup_trace::where(reinterpret_cast<uint64_t>(caller)));
}

// palmetto --trace FILE: the first XMemAlloc, XMemFree, XMemAllocDefault, XMemFreeDefault and XMemVirtualAlloc call for
// each distinct raw attribute word go to the startup trace as an "xmem_attributes" event (api, the word, the size for an
// allocation, the caller as module+RVA). The bits of the attribute word are not known yet; this records them from a
// real run without logging every allocation, which would fill the file. Off: one relaxed load per call.
void note_attributes(const char* api, uint64_t attributes, size_t size, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<std::pair<std::string, uint64_t>> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert({api, attributes}).second) return;
    }
    std::string fields = std::string("\"api\":\"") + api +
                         "\",\"attributes\":" + rt::startup_trace::quote(rt::startup_trace::hex(attributes));
    if (size != kNoSize) fields += ",\"size\":" + std::to_string(size);
    fields += caller_field(caller);
    rt::startup_trace::event("xmem_attributes", fields);
}

// palmetto --trace FILE with PALMETTO_XMEM_CALLERS=1: the first call of each allocator entry point from each distinct
// caller goes to the startup trace as an "xmem_caller" event (api, the caller as module+RVA). xmem_attributes above
// dedupes on the attribute word, so a later call with a word already seen is invisible; this records the caller
// instead, which is how a boot shows whether a separately mapped module that imports XMemAlloc/XMemFree (the package's
// Bink 2 library, for one) runs any of its code at all. Bounded to 256 records; off by default, one relaxed load per
// call when it is.
void note_caller(const char* api, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    const char* switch_value = std::getenv("PALMETTO_XMEM_CALLERS");
    if (!switch_value || *switch_value == '0') return;
    const std::string where = rt::startup_trace::where(reinterpret_cast<uint64_t>(caller));
    static std::mutex mutex;
    static std::set<std::pair<std::string, std::string>> seen;
    static int left = 256;
    {
        const std::lock_guard lock(mutex);
        if (left <= 0 || !seen.insert({api, where}).second) return;
        --left;
    }
    rt::startup_trace::event("xmem_caller", std::string("\"api\":\"") + api + "\",\"caller\":" +
                                                  rt::startup_trace::quote(where));
}

// ---- PALMETTO_MEM_TRACE=<seconds>: the owner counters a long boot needs ------------------------------------------
//
// The 300 s boot's process private bytes grow from 16.7 GB to 83.6 GB while nothing is read, no DirectStorage request
// is made and no platform call starts after 115 s (docs/story_load_probe.md), so the growth is a retention in host
// memory, not content arriving. These counters split that growth by the allocator entry point the title called and by
// the title caller (module+RVA): the XMemAlloc/XMemAllocDefault heap (live bytes), XMemVirtualAlloc's requested bytes
// and its commit requests, kernel32!VirtualAlloc as the title calls it, and the physical-page tables (live pages,
// mapped pages, sections and the pages the first-touch handler committed). A caller table of 48 entries keeps live and
// total bytes per (kind, caller); when it is full the rest is counted only in the totals.
//
// Off by default: one cached environment parse, then one relaxed load per call. On, one stderr line per <seconds>,
// bounded to 240 lines, and the heap path keeps a 64-byte header per block so XMemFree - which receives no size - can
// attribute the free. Counters only: no allocation is served differently and nothing is read from the title.
std::atomic<std::uint64_t> g_mem_period_ms{0};  // 0: off

struct MemTrace {
    std::atomic<std::uint64_t> heap_live{0}, heap_peak{0}, heap_blocks{0};
    std::atomic<std::uint64_t> xva_calls{0}, xva_bytes{0}, xva_commit_calls{0}, xva_commit_bytes{0};
    std::atomic<std::uint64_t> tva_calls{0}, tva_bytes{0}, tva_commit_bytes{0};
    std::atomic<std::uint64_t> phys_calls{0}, phys_pages{0};
    std::atomic<std::uint64_t> map_calls{0}, map_pages{0};
    std::atomic<std::uint64_t> committed_pages{0};  // pages the first-touch handler / placed-at-VA hook committed
};
MemTrace g_mem_trace;

bool mem_trace_enabled() { return g_mem_period_ms.load(std::memory_order_relaxed) != 0; }

enum MemCallerKind : std::uint8_t { mem_heap = 0, mem_xmem_va = 1, mem_title_va = 2, mem_phys = 3 };

constexpr unsigned kMemCallerSlots = 48;
struct MemCaller {
    std::uint64_t caller = 0;
    std::uint8_t kind = 0;
    std::uint64_t live = 0;   // heap only
    std::uint64_t total = 0;  // bytes requested through this entry point
    std::uint64_t count = 0;
};
MemCaller g_mem_callers[kMemCallerSlots];
unsigned g_mem_caller_count = 0;
std::mutex g_mem_caller_mutex;

// A caller's entry (adding one when the table has room), under g_mem_caller_mutex. A contended lock skips only the
// per-caller split, never a total.
MemCaller* mem_caller_slot(std::uint8_t kind, std::uint64_t caller) {
    for (unsigned i = 0; i < g_mem_caller_count; ++i)
        if (g_mem_callers[i].caller == caller && g_mem_callers[i].kind == kind) return &g_mem_callers[i];
    if (g_mem_caller_count >= kMemCallerSlots) return nullptr;
    MemCaller& slot = g_mem_callers[g_mem_caller_count++];
    slot.caller = caller;
    slot.kind = kind;
    return &slot;
}

void mem_note_bytes(std::uint8_t kind, std::uint64_t caller, std::uint64_t bytes) {
    if (!mem_trace_enabled() || !caller) return;
    std::unique_lock lock(g_mem_caller_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (MemCaller* slot = mem_caller_slot(kind, caller)) {
        slot->total += bytes;
        ++slot->count;
    }
}

void mem_note_heap(std::uint64_t caller, std::uint64_t bytes, bool alloc) {
    if (!mem_trace_enabled()) return;
    if (alloc) {
        const std::uint64_t live = g_mem_trace.heap_live.fetch_add(bytes) + bytes;
        std::uint64_t peak = g_mem_trace.heap_peak.load();
        while (live > peak && !g_mem_trace.heap_peak.compare_exchange_weak(peak, live)) {
        }
        g_mem_trace.heap_blocks.fetch_add(1);
        mem_note_bytes(mem_heap, caller, bytes);
        return;
    }
    std::uint64_t live = g_mem_trace.heap_live.load();
    while (true) {
        const std::uint64_t freed = bytes < live ? bytes : live;
        if (g_mem_trace.heap_live.compare_exchange_weak(live, live - freed)) break;
    }
    if (!caller) return;
    std::unique_lock lock(g_mem_caller_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (MemCaller* slot = mem_caller_slot(mem_heap, caller))
        slot->live = slot->live > bytes ? slot->live - bytes : 0;
}

// The 64-byte header a traced heap block keeps in front of its payload: XMemFree gets no size, so the header is where
// the size and the allocating caller are. It exists only when the switch is on, so the default path is unchanged.
constexpr std::size_t kMemHeapHeader = 64;
constexpr std::uint64_t kMemHeapMagic = 0x45434152544D454Dull;  // "MEMTRACE" in little-endian order
struct MemHeapHeader {
    std::uint64_t size;
    std::uint64_t caller;
    std::uint64_t magic;
};

// The print thread (defined after the physical-page tables it reads).
void start_mem_trace();

// PALMETTO_MEM_TRACE's value as a period in milliseconds (0: off). Called from the entry points, cached: the switch's
// text cannot change while the process runs.
void note_mem_trace_switch() {
    static const bool parsed = [] {
        char value[32] = {};
        if (GetEnvironmentVariableA("PALMETTO_MEM_TRACE", value, sizeof value) != 0 && value[0] != '\0') {
            char* end = nullptr;
            const double seconds = std::strtod(value, &end);
            if (end != value && seconds > 0.0)
                g_mem_period_ms.store(std::uint64_t(seconds * 1000.0) ? std::uint64_t(seconds * 1000.0) : 1);
        }
        // The allocation attribution shares this thread: PALMETTO_ALLOC_TRACE=1 rewrites the loaded modules' allocation
        // imports and the print below reports the largest stacks.
        char alloc[8] = {};
        const DWORD alloc_length = GetEnvironmentVariableA("PALMETTO_ALLOC_TRACE", alloc, sizeof alloc);
        palmetto::alloc_trace::start(alloc_length ? alloc : "");
        return true;
    }();
    (void)parsed;
    start_mem_trace();
}

void* allocate_block(size_t size, const void* caller) {
    note_mem_trace_switch();
    if (!mem_trace_enabled()) return _aligned_malloc(size ? size : 1, kAlign);
    const std::size_t payload = size ? size : 1;
    auto* base = static_cast<std::uint8_t*>(_aligned_malloc(payload + kMemHeapHeader, kAlign));
    if (!base) return nullptr;
    auto* header = reinterpret_cast<MemHeapHeader*>(base);
    header->size = payload;
    header->caller = reinterpret_cast<std::uint64_t>(caller);
    header->magic = kMemHeapMagic;
    mem_note_heap(header->caller, header->size, true);
    return base + kMemHeapHeader;
}

void free_block(void* p) {
    if (!p) return;
    if (!mem_trace_enabled()) {
        _aligned_free(p);
        return;
    }
    auto* base = static_cast<std::uint8_t*>(p) - kMemHeapHeader;
    auto* header = reinterpret_cast<MemHeapHeader*>(base);
    if (header->magic == kMemHeapMagic) {
        mem_note_heap(header->caller, header->size, false);
        header->magic = 0;
        _aligned_free(base);
        return;
    }
    // A block without the header: this shim did not allocate it through the traced path. Free it at its own address
    // (the allocator never returns a pointer into the middle of a block) and say so once.
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] xmem: XMemFree of a block without the PALMETTO_MEM_TRACE header\n");
    _aligned_free(p);
}

void* __stdcall XMemAlloc(size_t size, uint64_t attributes) {
    note_attributes("XMemAlloc", attributes, size, _ReturnAddress());
    note_caller("XMemAlloc", _ReturnAddress());
    return allocate_block(size, _ReturnAddress());
}

// ---- The allocation probe's range registry (docs/vertex_buffer_va.md section 15) ---------------------------------
//
// The title allocator's arena is MEM_PRIVATE committed memory, and VirtualQuery cannot say whether this shim or the
// title's own host calls created it. When PALMETTO_ALLOC_WATCH is set, every XMemVirtualAlloc answer is recorded as a
// real range with the title caller that asked for it, so the probe can answer "was this title page served by our XMem
// hook, and from which title caller". The registry is append-only and bounded (1024 ranges, then recording stops): a
// range freed later still answers yes, because the probe asks where the bytes came from, not whether they are live.
// Off unless the probe switch is set, so the default path is unchanged (one cached switch read, no lock).
struct ServedRange {
    std::uintptr_t begin = 0, end = 0;
    std::uint64_t caller = 0;
    std::uint8_t kind = 0;
};
constexpr unsigned kServedRangeSlots = 1024;
ServedRange g_served_ranges[kServedRangeSlots];
unsigned g_served_count = 0;
std::mutex g_served_mutex;

bool served_registry_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("PALMETTO_ALLOC_WATCH");
        return value && *value && *value != '0';
    }();
    return enabled;
}

void note_served_range(void* address, size_t bytes, const void* caller, std::uint8_t kind) {
    if (!served_registry_enabled() || !address || !bytes) return;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin > (std::numeric_limits<std::uintptr_t>::max)() - bytes) return;
    const std::lock_guard lock(g_served_mutex);
    if (g_served_count >= kServedRangeSlots) return;
    ServedRange& range = g_served_ranges[g_served_count++];
    range.begin = begin;
    range.end = begin + bytes;
    range.caller = reinterpret_cast<std::uint64_t>(caller);
    range.kind = kind;
}

// The recorded range containing `address`, if any: locked linear search over at most 1024 entries.
bool find_served_range(std::uintptr_t address, std::uint8_t* kind, std::uint64_t* caller) {
    if (!served_registry_enabled()) return false;
    const std::lock_guard lock(g_served_mutex);
    for (unsigned i = 0; i < g_served_count; ++i) {
        const ServedRange& range = g_served_ranges[i];
        if (address >= range.begin && address < range.end) {
            if (kind) *kind = range.kind;
            if (caller) *caller = range.caller;
            return true;
        }
    }
    return false;
}

// The success answer XMemVirtualAlloc returns, recorded when it succeeded: one call site instead of one per return in
// the function.
void* served_virtual(void* address, size_t bytes, const void* caller) {
    note_served_range(address, bytes, caller, 1);
    return address;
}

void __stdcall XMemFree(void* p, uint64_t attributes) {
    note_attributes("XMemFree", attributes, kNoSize, _ReturnAddress());
    note_caller("XMemFree", _ReturnAddress());
    free_block(p);
}

// XMemAllocDefault / XMemFreeDefault: the GDK's own allocator behind XMemAlloc and XMemFree, which a title that hooks
// XMemAlloc forwards to. The measured title reaches each through one wrapper that is a bare tail jump to the import,
// so they take exactly XMemAlloc's and XMemFree's arguments. They share the heap: a block from either allocator may be
// released by either free.
void* __stdcall XMemAllocDefault(size_t size, uint64_t attributes) {
    note_attributes("XMemAllocDefault", attributes, size, _ReturnAddress());
    note_caller("XMemAllocDefault", _ReturnAddress());
    return allocate_block(size, _ReturnAddress());
}

void __stdcall XMemFreeDefault(void* p, uint64_t attributes) {
    note_attributes("XMemFreeDefault", attributes, kNoSize, _ReturnAddress());
    note_caller("XMemFreeDefault", _ReturnAddress());
    free_block(p);
}

// BOOL XMemIsToolsMemAvailable(void). The measured title calls it from one helper, which six out-of-memory paths reach
// only after an allocation has failed; the helper breaks into the debugger when the result is zero. Tools memory is
// the extra memory of a development kit. A PC has none, so the answer is FALSE, and the title's own breakpoint marks
// where it ran out of memory.
BOOL WINAPI XMemIsToolsMemAvailable() {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr, "[rt] xmem: XMemIsToolsMemAvailable -> FALSE (no devkit tools memory; logged once)\n");
    }
    return FALSE;
}

// Measured call sites pass a zeroed 64-byte structure. Only the MEMORY_BASIC_INFORMATION-compatible prefix is
// known: callers read at least the base and size fields. Fill that prefix from VirtualQueryEx and zero the rest.
constexpr size_t kVirtualQueryOutSize = 64;
static_assert(sizeof(MEMORY_BASIC_INFORMATION) <= kVirtualQueryOutSize,
              "the 64-byte XMemVirtualQuery output must hold MEMORY_BASIC_INFORMATION");

// Returns the number of bytes written (the VirtualQuery convention) or 0 on failure. The exact return type and the
// meaning of every output field are not fully known; callers in the package only test for success.
SIZE_T __stdcall XMemVirtualQuery(HANDLE process, const void* address, void* out) {
    if (!out) return 0;
    std::memset(out, 0, kVirtualQueryOutSize);
    if (!address) return 0;

    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] xmem: XMemVirtualQuery: fields after MEMORY_BASIC_INFORMATION are unknown; "
                     "the 64-byte output is zero-filled there\n");
    }

    // A null handle means the current process; GetCurrentProcess() is the (HANDLE)-1 pseudo handle.
    HANDLE target = process ? process : GetCurrentProcess();
    MEMORY_BASIC_INFORMATION info{};
    SIZE_T written = VirtualQueryEx(target, address, &info, sizeof(info));
    if (!written) return 0;

    std::memcpy(out, &info, written < kVirtualQueryOutSize ? written : kVirtualQueryOutSize);
    return written;
}

// BOOL XMemGetWorkingSetStatistics(UINT32 kind, XMEM_WORKING_SET_STATISTICS* stats). Measured at all seven call sites
// in amd_fidelityfx_upscaler_xbox.dll (1.0.2.1; calls at RVAs 0x26b3, 0x2ea1, 0x39a9, 0x7452, 0x74d2, 0x7568,
// 0x75b1): ECX is the constant 0x20000001 (its meaning is not known) and RDX points at a stack buffer (0x80 bytes
// where the frame shows it). The caller reads a 64-bit size at +8 and treats a zero return as "no data". Taking the
// pointer from the first argument wrote to address 0x20000001.
// The measured FSR sites establish argument shape, kind and the 64-bit working-set read at +8; +0's title-total memory
// limit interpretation is inferred from other Series implementations (xcl, xbox-emulator) and title engine telemetry
// sizing memory pools (RT-220). Note that if GlobalMemoryStatusEx reports 0 physical memory, XMem falls back to the
// 12 GiB title limit, whereas XSystemGetAppMemoryUsageLimit remains 0.
BOOL __stdcall XMemGetWorkingSetStatistics(uint32_t /*kind*/, void* stats) {
    if (!stats) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[rt] xmem: XMemGetWorkingSetStatistics(nullptr) -> FALSE\n");
        }
        return FALSE;
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return FALSE;

    MEMORYSTATUSEX ms{sizeof(ms)};
    uint64_t total_memory = 12ULL * 1024 * 1024 * 1024;
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys != 0) {
        total_memory = ms.ullTotalPhys;
    }

    uint8_t* bytes = static_cast<uint8_t*>(stats);
    std::memset(bytes, 0, 16);
    std::memcpy(bytes + 0, &total_memory, sizeof(total_memory));
    uint64_t working_set = pmc.WorkingSetSize;
    std::memcpy(bytes + 8, &working_set, sizeof(working_set));
    return TRUE;
}

// The placeholder APIs (Windows 10 1803 and later) come from kernelbase.dll at run time, so neither palmetto nor the
// tests link onecore.lib. A host without them fails the calls that need them with ERROR_NOT_SUPPORTED.
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*,
                                        ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

struct PlaceholderApi {
    VirtualAlloc2Fn virtual_alloc2 = nullptr;
    MapViewOfFile3Fn map_view3 = nullptr;
    UnmapViewOfFile2Fn unmap_view2 = nullptr;
    bool available() const { return virtual_alloc2 && map_view3 && unmap_view2; }
};

const PlaceholderApi& placeholder_api() {
    static const PlaceholderApi api = [] {
        PlaceholderApi resolved;
        if (const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll")) {
            resolved.virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelbase, "VirtualAlloc2"));
            resolved.map_view3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelbase, "MapViewOfFile3"));
            resolved.unmap_view2 =
                reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(kernelbase, "UnmapViewOfFile2"));
        }
        return resolved;
    }();
    return api;
}

// XMem allocation-type page bits select the guest page granularity and must not be forwarded as Win32 flags.
constexpr DWORD kXmemLargePages = 0x20000000;  // 2 MiB pages
constexpr DWORD kXmem64kPages = 0x20400000;    // 64 KiB pages
constexpr DWORD kXmemPageModeMask = kXmem64kPages;
constexpr SIZE_T kXmemPageSize64k = 0x10000;
constexpr SIZE_T kXmemPageSize2m = 0x200000;
constexpr DWORD kXmemHostAllocationTypeMask = MEM_COMMIT | MEM_RESERVE | MEM_REPLACE_PLACEHOLDER |
                                              MEM_RESERVE_PLACEHOLDER | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH |
                                              MEM_RESET_UNDO;

bool round_up_size(SIZE_T value, SIZE_T alignment, SIZE_T& rounded) {
    const SIZE_T max = (std::numeric_limits<SIZE_T>::max)();
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || value > max - (alignment - 1)) return false;
    rounded = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

// The console's PAGE_GRAPHICS_* protection bits (winnt.h: PAGE_GRAPHICS_NOACCESS 0x800 .. PAGE_GRAPHICS_NOCACHE
// 0x40000). The host refuses them with ERROR_INVALID_PARAMETER, and the D3D12 layer reads guest memory through the
// CPU mapping, so only the CPU protection is applied. The retail executable passes 0x801 and 0x22004.
constexpr DWORD kPageGraphicsMask = 0x0007F800;

DWORD host_protect(DWORD p) {
    const DWORD cpu = p & ~kPageGraphicsMask;
    return cpu ? cpu : PAGE_NOACCESS;
}

// Physical pages and the windows they are mapped into. One mutex covers both tables and every map and unmap.
//
// A physical-page window is a reserve-only XMemVirtualAlloc in a guest page mode, or a reserve-only replacement of
// part of a placeholder. On the host it is a placeholder, the only kind of reservation a section view can later
// replace, and its protection is kept here: the map calls take none, so a mapped page gets its window's protection, as
// AWE's MapUserPhysicalPages does.
//
// ASSUMPTION: a title does not commit memory inside such a window with kernel32!VirtualAlloc. A placeholder refuses a
// plain commit; the measured windows are only ever passed to the map calls.
struct Window {
    SIZE_T size;
    DWORD protect;
};

struct Section {
    HANDLE handle;
    void* scratch;
    explicit Section(HANDLE h) : handle(h), scratch(MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, 0)) {}
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
    ~Section() {
        if (scratch) UnmapViewOfFile(scratch);
        CloseHandle(handle);
    }
};

struct PhysicalPage {
    std::shared_ptr<Section> section;  // a mapped view holds its own reference to the section
    uint64_t offset;
};

constexpr uint64_t kPhysicalPageSize = 0x10000;
// Page numbers are 4 KiB frame numbers of 64 KiB pages: the measured title stores them packed and assumes each is a
// non-zero multiple of 16 below 2^25, with 0 for an entry it unmaps. So a page number is slot * 16 with slot in
// [1, 2^21), and freed slots are reused so a long run never runs out.
constexpr uint64_t kFramesPerPage = kPhysicalPageSize / 0x1000;
constexpr uint64_t kPageSlotLimit = uint64_t{1} << 21;

std::mutex g_phys_mutex;
std::map<uintptr_t, Window> g_windows;              // base -> window
std::unordered_map<uint64_t, PhysicalPage> g_pages;  // page number -> backing
std::vector<uint64_t> g_free_slots;
uint64_t g_next_slot = 1;
struct MappedPage {
    uint64_t page;
    std::shared_ptr<Section> section;
    uint64_t offset;
};

std::unordered_map<uintptr_t, MappedPage> g_mapped;
SRWLOCK g_fault_lock = SRWLOCK_INIT;

struct FaultLockExclusive {
    FaultLockExclusive() { AcquireSRWLockExclusive(&g_fault_lock); }
    ~FaultLockExclusive() { ReleaseSRWLockExclusive(&g_fault_lock); }
    FaultLockExclusive(const FaultLockExclusive&) = delete;
    FaultLockExclusive& operator=(const FaultLockExclusive&) = delete;
};

struct FaultLockShared {
    FaultLockShared() { AcquireSRWLockShared(&g_fault_lock); }
    ~FaultLockShared() { ReleaseSRWLockShared(&g_fault_lock); }
    FaultLockShared(const FaultLockShared&) = delete;
    FaultLockShared& operator=(const FaultLockShared&) = delete;
};

// With g_phys_mutex held. A window replaces every window it overlaps: the title released those regions (with
// kernel32!VirtualFree, which the shim does not see) before the host could hand the addresses out again.
void record_window(void* base, SIZE_T size, DWORD protect) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(base);
    const uintptr_t end = start + size;
    auto it = g_windows.lower_bound(start);
    if (it != g_windows.begin()) {
        const auto prev = std::prev(it);
        if (prev->first + prev->second.size > start) it = prev;
    }
    while (it != g_windows.end() && it->first < end) it = g_windows.erase(it);
    g_windows[start] = Window{size, protect};
}

// With g_phys_mutex held: the window holding [address, address + bytes), if one does.
const Window* find_window(uintptr_t address, uint64_t bytes) {
    auto it = g_windows.upper_bound(address);
    if (it == g_windows.begin()) return nullptr;
    --it;
    const uint64_t offset = address - it->first;
    if (offset >= it->second.size || bytes > it->second.size - offset) return nullptr;
    return &it->second;
}

// [address, address + size) as one placeholder of its own: split it out of the placeholder that holds it. Fails when
// the range is not inside one placeholder.
bool isolate_placeholder(void* address, SIZE_T size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_RESERVE) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    if (info.AllocationBase == address && info.RegionSize == size) return true;
    return VirtualFree(address, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != FALSE;
}

PVOID reserve_placeholder(void* address, SIZE_T size, SIZE_T alignment) {
    MEM_ADDRESS_REQUIREMENTS requirements{};
    MEM_EXTENDED_PARAMETER parameter{};
    ULONG parameter_count = 0;
    if (!address && alignment > kXmemPageSize64k) {
        requirements.Alignment = alignment;
        parameter.Type = MemExtendedParameterAddressRequirements;
        parameter.Pointer = &requirements;
        parameter_count = 1;
    }
    return placeholder_api().virtual_alloc2(GetCurrentProcess(), address, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                            PAGE_NOACCESS, parameter_count ? &parameter : nullptr, parameter_count);
}

// PVOID XMemVirtualAlloc(address, size, allocationType, attributes, protect). All 23 calls of the measured title pass a
// nonzero attribute word; its bits are not decoded and nothing on the host depends on them, so it is accepted and only
// recorded (xmem_attributes). Its types cover committed 2 MiB and 64 KiB pages, reserve-only 64 KiB windows for physical pages, one
// MEM_RESERVE_PLACEHOLDER reservation and five MEM_REPLACE_PLACEHOLDER windows carved out of it, each then filled with
// XMemMapPhysicalPages. The guest page size sets the alignment and rounding of the region; the host's own pages stay
// 4 KiB (host large pages need SeLockMemoryPrivilege). A 2 MiB region the host places is reserved 2 MiB-aligned through
// VirtualAlloc2.
void* WINAPI XMemVirtualAlloc(void* address, SIZE_T size, DWORD allocation_type, ULONGLONG attributes, DWORD protect) {
    const void* const caller = _ReturnAddress();
    note_attributes("XMemVirtualAlloc", attributes, size, caller);
    if (size == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    const DWORD page_mode = allocation_type & kXmemPageModeMask;
    if (page_mode != 0 && page_mode != kXmem64kPages && page_mode != kXmemLargePages) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    const DWORD unsupported = allocation_type & ~(kXmemHostAllocationTypeMask | kXmemPageModeMask);
    if (unsupported != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    DWORD host_type = allocation_type & kXmemHostAllocationTypeMask;
    if (host_type == 0) host_type = MEM_RESERVE | MEM_COMMIT;
    const DWORD host_prot = host_protect(protect);

    const bool commit = (host_type & MEM_COMMIT) != 0;
    const bool new_placeholder = (host_type & MEM_RESERVE_PLACEHOLDER) != 0;
    const bool replace = (host_type & MEM_REPLACE_PLACEHOLDER) != 0;
    const bool window = page_mode != 0 && !commit && !new_placeholder;
    note_mem_trace_switch();
    if (mem_trace_enabled()) {
        const auto caller = reinterpret_cast<std::uint64_t>(_ReturnAddress());
        const auto bytes = static_cast<std::uint64_t>(size);
        g_mem_trace.xva_calls.fetch_add(1, std::memory_order_relaxed);
        g_mem_trace.xva_bytes.fetch_add(bytes, std::memory_order_relaxed);
        if (commit) {
            g_mem_trace.xva_commit_calls.fetch_add(1, std::memory_order_relaxed);
            g_mem_trace.xva_commit_bytes.fetch_add(bytes, std::memory_order_relaxed);
            mem_note_bytes(mem_xmem_va, caller, bytes);
        }
    }
    if ((new_placeholder || replace || window || page_mode == kXmemLargePages) && !placeholder_api().available()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    SIZE_T page_granularity = system_info.dwPageSize;
    SIZE_T base_granularity = (host_type & MEM_RESERVE) ? system_info.dwAllocationGranularity : system_info.dwPageSize;
    if (page_mode == kXmem64kPages) page_granularity = base_granularity = kXmemPageSize64k;
    if (page_mode == kXmemLargePages) page_granularity = base_granularity = kXmemPageSize2m;
    if (new_placeholder || replace) {
        // Placeholders are split and replaced on allocation-granularity boundaries only.
        if (page_granularity < system_info.dwAllocationGranularity) page_granularity = system_info.dwAllocationGranularity;
        if (base_granularity < system_info.dwAllocationGranularity) base_granularity = system_info.dwAllocationGranularity;
    }
    if (page_granularity == 0 || (page_granularity & (page_granularity - 1)) != 0 || base_granularity == 0 ||
        (base_granularity & (base_granularity - 1)) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    if (address != nullptr) {
        const uintptr_t requested = reinterpret_cast<uintptr_t>(address);
        const uintptr_t max = (std::numeric_limits<uintptr_t>::max)();
        if (size > max - requested) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const uintptr_t requested_end = requested + size;
        const uintptr_t base = requested & ~(static_cast<uintptr_t>(base_granularity) - 1);
        if (requested_end > max - (static_cast<uintptr_t>(page_granularity) - 1)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const uintptr_t end = (requested_end + static_cast<uintptr_t>(page_granularity) - 1) &
                              ~(static_cast<uintptr_t>(page_granularity) - 1);
        address = reinterpret_cast<void*>(base);
        size = static_cast<SIZE_T>(end - base);
    } else if (replace) {
        SetLastError(ERROR_INVALID_PARAMETER);  // a replacement names the placeholder it replaces
        return nullptr;
    } else if (page_granularity > system_info.dwPageSize && !round_up_size(size, page_granularity, size)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    if (replace) {
        const std::lock_guard lock(g_phys_mutex);
        if (!isolate_placeholder(address, size)) return nullptr;
        if (window) {
            // A reserve-only replacement stays a placeholder: the physical pages are mapped into it.
            record_window(address, size, host_prot);
            return served_virtual(address, size, caller);
        }
        return served_virtual(placeholder_api().virtual_alloc2(GetCurrentProcess(), address, size,
                                                               host_type | MEM_RESERVE, host_prot, nullptr, 0),
                              size, caller);
    }
    if (new_placeholder || window) {
        void* const result = reserve_placeholder(address, size, base_granularity);
        if (result && window) {
            const std::lock_guard lock(g_phys_mutex);
            record_window(result, size, host_prot);
        }
        return served_virtual(result, size, caller);
    }
    if (page_mode == kXmemLargePages && address == nullptr) {
        MEM_ADDRESS_REQUIREMENTS requirements{};
        requirements.Alignment = kXmemPageSize2m;
        MEM_EXTENDED_PARAMETER parameter{};
        parameter.Type = MemExtendedParameterAddressRequirements;
        parameter.Pointer = &requirements;
        return served_virtual(placeholder_api().virtual_alloc2(GetCurrentProcess(), nullptr, size,
                                                               host_type | MEM_RESERVE, host_prot, &parameter, 1),
                              size, caller);
    }
    return served_virtual(VirtualAlloc(address, size, host_type, host_prot), size, caller);
}

// With g_phys_mutex held. Unmaps the page at `address` if one is mapped there; the address becomes a placeholder again.
bool unmap_page(uintptr_t address) {
    const FaultLockExclusive lock;
    const auto it = g_mapped.find(address);
    if (it == g_mapped.end()) return true;
    if (!placeholder_api().unmap_view2(GetCurrentProcess(), reinterpret_cast<void*>(address),
                                       MEM_PRESERVE_PLACEHOLDER)) {
        return false;
    }
    g_mapped.erase(it);
    return true;
}

// With g_phys_mutex held. Maps physical page `page` (0: unmap) at the 64 KiB-aligned `address`, replacing what was
// mapped there. The caller has found the window.
bool map_page(uintptr_t address, uint64_t page, DWORD protect) {
    if (!unmap_page(address)) return false;
    if (page == 0) return true;
    const auto it = g_pages.find(page);
    if (it == g_pages.end()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    void* const target = reinterpret_cast<void*>(address);
    if (!isolate_placeholder(target, kPhysicalPageSize)) return false;
    const FaultLockExclusive lock;
    void* const view = placeholder_api().map_view3(it->second.section->handle, GetCurrentProcess(), target,
                                                   it->second.offset, kPhysicalPageSize, MEM_REPLACE_PLACEHOLDER,
                                                   protect, nullptr, 0);
    if (!view) return false;
    g_mapped[address] = MappedPage{page, it->second.section, it->second.offset};
    return true;
}

enum class PageCommit { not_ours, committed_before, committed_now, failed };

PageCommit commit_mapped_page(uintptr_t address, uintptr_t accessed, DWORD& protect) {
    const auto it = g_mapped.find(address);
    if (it == g_mapped.end() || !it->second.section->scratch) return PageCommit::not_ours;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(accessed), &info, sizeof(info)) || info.Type != MEM_MAPPED) {
        return PageCommit::not_ours;
    }
    if (info.State == MEM_COMMIT) {
        protect = info.Protect;
        return PageCommit::committed_before;
    }
    void* const backing = static_cast<uint8_t*>(it->second.section->scratch) + it->second.offset;
    if (!VirtualAlloc(backing, kPhysicalPageSize, MEM_COMMIT, PAGE_READWRITE)) return PageCommit::failed;
    if (mem_trace_enabled()) g_mem_trace.committed_pages.fetch_add(1, std::memory_order_relaxed);
    return PageCommit::committed_now;
}

bool access_allowed(DWORD protect, ULONG_PTR access) {
    if (protect & PAGE_GUARD) return false;
    switch (protect & 0xff) {
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
        return access != 8;
    case PAGE_EXECUTE_READ:
        return access != 1;
    case PAGE_READONLY:
        return access == 0;
    case PAGE_EXECUTE:
        return access == 8;
    default:
        return false;
    }
}

LONG CALLBACK physical_page_fault(EXCEPTION_POINTERS* info) {
    thread_local bool busy = false;
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (busy || !record || record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    busy = true;
    const uintptr_t accessed = static_cast<uintptr_t>(record->ExceptionInformation[1]);
    const uintptr_t address = accessed & ~static_cast<uintptr_t>(kPhysicalPageSize - 1);
    DWORD protect = 0;
    PageCommit result;
    {
        const FaultLockShared lock;
        result = commit_mapped_page(address, accessed, protect);
    }
    busy = false;
    if (result == PageCommit::committed_now) return EXCEPTION_CONTINUE_EXECUTION;
    if (result == PageCommit::committed_before && access_allowed(protect, record->ExceptionInformation[0])) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool install_physical_page_fault_handler() {
    static const bool installed = AddVectoredExceptionHandler(1, &physical_page_fault) != nullptr;
    return installed;
}

// Two of the measured windows are reserved PAGE_NOACCESS for the CPU and the GPU alike; pages mapped there are still
// GPU memory, which the D3D12 layer reads on the CPU.
DWORD view_protect(const Window& window) {
    return window.protect == PAGE_NOACCESS ? PAGE_READWRITE : window.protect;
}

bool aligned_page_address(uintptr_t address) { return (address & (kPhysicalPageSize - 1)) == 0; }

// palmetto --trace FILE: the first XMemAllocatePhysicalPages for each distinct attribute word goes to the startup trace
// as an "xmem_physical_pages" event with the page count and the caller.
void note_physical_attributes(uint64_t attributes, uint64_t count, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<uint64_t> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert(attributes).second) return;
    }
    rt::startup_trace::event("xmem_physical_pages",
                             "\"api\":\"XMemAllocatePhysicalPages\",\"attributes\":" +
                                 rt::startup_trace::quote(rt::startup_trace::hex(attributes)) +
                                 ",\"pages\":" + std::to_string(count) + caller_field(caller));
}

// BOOL XMemAllocatePhysicalPages(attributes, UINT64* pageCount, UINT64* pageArray). Measured at four call sites: RCX is
// a constant attribute word, RDX points at the number of 64 KiB pages (every caller computes it as the byte size
// rounded up and shifted right by 16), R8 at an array that receives one page number per page. The result is tested as
// a 32-bit BOOL. After success every caller compares the count with the one it asked for and frees the pages if they
// differ, so the count is in/out.
//
// Each call is backed by one pagefile-backed section of count * 64 KiB: committed (charged) on touch, given physical
// memory as it is touched, zero-filled. Pages keep their contents while unmapped or mapped elsewhere, and one page can
// be mapped at several addresses. ASSUMPTION: the attribute word (memory type, perhaps the page size) is not decoded;
// every page is 64 KiB, which all four callers' arithmetic agrees with.
BOOL WINAPI XMemAllocatePhysicalPages(uint64_t attributes, uint64_t* page_count, uint64_t* page_array) {
    note_physical_attributes(attributes, page_count ? *page_count : 0, _ReturnAddress());
    if (!page_count || (*page_count && !page_array)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const uint64_t count = *page_count;
    if (count == 0) return TRUE;

    note_mem_trace_switch();
    if (mem_trace_enabled()) {
        g_mem_trace.phys_calls.fetch_add(1, std::memory_order_relaxed);
        g_mem_trace.phys_pages.fetch_add(count, std::memory_order_relaxed);
        mem_note_bytes(mem_phys, reinterpret_cast<std::uint64_t>(_ReturnAddress()), count * kPhysicalPageSize);
    }

    const std::lock_guard lock(g_phys_mutex);
    if (count > g_free_slots.size() + (kPageSlotLimit - g_next_slot)) {
        *page_count = 0;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (!install_physical_page_fault_handler()) {
        *page_count = 0;
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    const uint64_t bytes = count * kPhysicalPageSize;  // below 2^37: count is below 2^21
    const HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                             static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    if (!handle) {
        *page_count = 0;
        return FALSE;
    }
    const auto section = std::make_shared<Section>(handle);
    if (!section->scratch) {
        *page_count = 0;
        return FALSE;
    }
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t slot;
        if (!g_free_slots.empty()) {
            slot = g_free_slots.back();
            g_free_slots.pop_back();
        } else {
            slot = g_next_slot++;
        }
        const uint64_t page = slot * kFramesPerPage;
        g_pages[page] = PhysicalPage{section, i * kPhysicalPageSize};
        page_array[i] = page;
    }
    return TRUE;
}

// BOOL XMemFreePhysicalPages(UINT64 pageCount, UINT64* pageArray). Measured at three call sites: RCX is the count (the
// one XMemAllocatePhysicalPages returned, or the sum of several allocations), RDX the array. The callers unmap the
// pages first; a page that is still mapped keeps its memory until it is unmapped. FALSE, with the entries before it
// freed, for a number that is not a live page.
BOOL WINAPI XMemFreePhysicalPages(uint64_t page_count, const uint64_t* page_array) {
    if (page_count && !page_array) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::lock_guard lock(g_phys_mutex);
    for (uint64_t i = 0; i < page_count; ++i) {
        const auto it = g_pages.find(page_array[i]);
        if (it == g_pages.end()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        g_pages.erase(it);
        g_free_slots.push_back(page_array[i] / kFramesPerPage);
    }
    return TRUE;
}

// PVOID XMemMapPhysicalPages(PVOID address, UINT64 pageCount, UINT64* pageArray). Measured at twelve call sites: RCX
// is an address in a window XMemVirtualAlloc reserved, RDX a 64-bit count, R8 the pages to map there one after another,
// or null to unmap that many (four calls at shutdown). Callers test the 64-bit result for zero, so it is the address on
// success. A page number of 0 in the array unmaps that one page.
//
// Each page is its own view (MapViewOfFile3 into a 64 KiB placeholder), so any page can be remapped or unmapped later
// without touching its neighbours. A failure part-way leaves the pages before it mapped.
void* WINAPI XMemMapPhysicalPages(void* address, uint64_t page_count, const uint64_t* page_array) {
    note_mem_trace_switch();
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (!aligned_page_address(start) || page_count > (std::numeric_limits<uintptr_t>::max)() / kPhysicalPageSize ||
        start > (std::numeric_limits<uintptr_t>::max)() - page_count * kPhysicalPageSize) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!placeholder_api().available()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    const std::lock_guard lock(g_phys_mutex);
    if (!page_array) {
        for (uint64_t i = 0; i < page_count; ++i) {
            if (!unmap_page(start + i * kPhysicalPageSize)) return nullptr;
        }
        return address;
    }
    const Window* const window = find_window(start, page_count * kPhysicalPageSize);
    if (!window) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    const DWORD protect = view_protect(*window);
    if (mem_trace_enabled()) {
        g_mem_trace.map_calls.fetch_add(1, std::memory_order_relaxed);
        g_mem_trace.map_pages.fetch_add(page_count, std::memory_order_relaxed);
    }
    for (uint64_t i = 0; i < page_count; ++i) {
        if (!map_page(start + i * kPhysicalPageSize, page_array[i], protect)) return nullptr;
    }
    return address;
}

// BOOL XMemMapPhysicalPagesScatter(PVOID* addresses, UINT32 pageCount, UINT64* pageArray). Measured at two call sites:
// RCX is an array of addresses, EDX a 32-bit count, R8 the page for each address (an entry of 0, or a null array,
// unmaps). The result is tested as a 32-bit BOOL. Like MapUserPhysicalPagesScatter: entry i maps pageArray[i] at
// addresses[i].
BOOL WINAPI XMemMapPhysicalPagesScatter(void* const* addresses, uint32_t page_count, const uint64_t* page_array) {
    note_mem_trace_switch();
    if (page_count && !addresses) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!placeholder_api().available()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    const std::lock_guard lock(g_phys_mutex);
    for (uint32_t i = 0; i < page_count; ++i) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        if (!aligned_page_address(address)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        const uint64_t page = page_array ? page_array[i] : 0;
        if (page == 0) {
            if (!unmap_page(address)) return FALSE;
            continue;
        }
        const Window* const window = find_window(address, kPhysicalPageSize);
        if (!window) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        if (!map_page(address, page, view_protect(*window))) return FALSE;
    }
    return TRUE;
}

// rt::xmem::copy_page_mappings_fn (xmem.h), for console queue slot 31: every destination page becomes a second view of
// the physical page already mapped at its source page, which is what two XMemMapPhysicalPages calls with one page
// number make; a page with nothing mapped at its source leaves the destination page unmapped. Every source page is
// looked up before any destination page changes, so a source range that overlaps the destination maps what was there
// before the call. The destination window's protection is used (view_protect turns a PAGE_NOACCESS window into
// PAGE_READWRITE, so the D3D12 layer reads pages mapped into the title's GPU-only windows on the CPU).
bool copy_page_mappings(void* destination, const void* source, std::uint64_t pages, bool repeat_first) {
    const uintptr_t to = reinterpret_cast<uintptr_t>(destination);
    const uintptr_t from = reinterpret_cast<uintptr_t>(source);
    constexpr uintptr_t kMax = (std::numeric_limits<uintptr_t>::max)();
    if (!aligned_page_address(to) || (source && !aligned_page_address(from)) ||
        pages > kMax / kPhysicalPageSize || to > kMax - pages * kPhysicalPageSize ||
        (source && !repeat_first && from > kMax - pages * kPhysicalPageSize)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    if (!placeholder_api().available()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    const std::lock_guard lock(g_phys_mutex);
    if (!source) {
        for (uint64_t i = 0; i < pages; ++i) {
            if (!unmap_page(to + i * kPhysicalPageSize)) return false;
        }
        return true;
    }
    const Window* const window = find_window(to, pages * kPhysicalPageSize);
    if (!window) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    std::vector<uint64_t> found(static_cast<size_t>(pages), 0);
    for (uint64_t i = 0; i < pages; ++i) {
        const auto it = g_mapped.find(from + (repeat_first ? 0 : i) * kPhysicalPageSize);
        if (it != g_mapped.end()) found[static_cast<size_t>(i)] = it->second.page;
    }
    const DWORD protect = view_protect(*window);
    for (uint64_t i = 0; i < pages; ++i) {
        if (!map_page(to + i * kPhysicalPageSize, found[static_cast<size_t>(i)], protect)) return false;
    }
    return true;
}
[[maybe_unused]] const bool g_copy_page_mappings_installed =
    (rt::xmem::copy_page_mappings_fn = &copy_page_mappings, true);

// ---- The guard-page write tracker (docs/card_copies.md) ----------------------------------------------------------
//
// The layer mirrors the title's CPU descriptor-heap bytes into GPU-visible arena ranges and can only tell whether a
// mirror still matches them by comparing all of them: a measured 3.4-4.4 ms pass over 38-40 MB per submission
// (docs/page_address_space.md section 4, reproduced at 3.5 ms on this machine). The title's heap is pagefile-section
// views, and GetWriteWatch answers ERROR_INVALID_PARAMETER for that memory (measured, docs/card_copies.md), so the OS
// write watch cannot answer. This tracker uses the mechanism the peer project documents instead - a PAGE_GUARD on
// every page of a watched range: the first access to a guarded 4 KiB page raises EXCEPTION_GUARD_PAGE and the system
// clears that page's guard, so a guard exception is a "this page was accessed" signal that costs a handler call, not a
// scan. A write is recorded as a change (the mirror is stale); a read is recorded only as "guard this page again", so
// the layer's own reads of the title bytes never fake a change.
//
// Measured limits (docs/card_copies.md section 2): arming a 39.8 MB heap costs 0.6 ms (one VirtualProtect per 64 KiB
// view - one call over the whole multi-view range fails), a guarded write ~10 us, and a full scan of an armed heap
// ~35 ms because every 4 KiB page of it trips once. That last figure is why the change signal alone is not a fix: the
// full refresh scan must be narrowed to the changed pages (device-side, docs/card_copies.md "what remains") before a
// gate on this tracker can pay, and why the tracker is landed behind switches and off by default.
namespace {

constexpr std::uintptr_t kWatchPage = 0x1000;     // the guard granularity: one PTE
constexpr std::uintptr_t kWatchBlock = 0x10000;   // the measured console heap page: what one arm call covers
constexpr unsigned kWatchRanges = 8;              // watched ranges (the title's shader-visible heaps)
constexpr unsigned kWatchTrips = 16384;           // recorded pages between polls; past it the poll answers "unavailable"

// Published for the exception handler, which must not take a lock (a fault can arrive on any thread, including one
// that holds the tracker's mutex inside an arm/forget loop). Written under the mutex, read lock-free.
std::atomic<std::uintptr_t> g_watch_begin[kWatchRanges];
std::atomic<std::uintptr_t> g_watch_end[kWatchRanges];
std::atomic<unsigned> g_watch_published{0};

// The trip record. The handler appends the 4 KiB page a guard exception named; a poll drains it and guards each page
// again. An overflow is not a lost write silently ignored: it makes the poll answer "unavailable", so the caller falls
// back to the full work.
std::atomic<std::uintptr_t> g_watch_trips[kWatchTrips];
std::atomic<unsigned> g_watch_trip_count{0};
std::atomic<bool> g_watch_overflow{false};
std::atomic<bool> g_watch_changed{false};
std::atomic<std::uint64_t> g_watch_write_trips{0}, g_watch_read_trips{0};
std::atomic<std::uint64_t> g_watch_pages_guarded{0};

std::mutex g_watch_mutex;
struct WatchRange {
    std::uintptr_t start;
    std::uintptr_t end;
};
std::vector<WatchRange> g_watch_ranges;

// Whether [address, address + kWatchBlock) can hold a guard: every host page committed and writable, and no guard
// already set (a title may guard its own memory; the tracker must not adopt or clear it).
bool watchable_block(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD protect = info.Protect & 0xff;
    return !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE)) &&
           (writable & protect) != 0;
}

// Guards [address, address + bytes): one VirtualProtect per 64 KiB block where the range allows it (the console heap
// is whole 64 KiB views, so this is one call per view and sets the guard bit on all sixteen of its 4 KiB pages -
// measured: a later scan of the armed heap trips once per 4 KiB page), one per 4 KiB page elsewhere. A block that
// cannot be guarded fails the whole arm (the caller has nothing safe to poll), so the caller restores what this armed.
std::size_t arm_range(std::uintptr_t start, std::uintptr_t end) {
    std::size_t guarded = 0;
    for (std::uintptr_t at = start; at < end;) {
        const bool block = (at & (kWatchBlock - 1)) == 0 && end - at >= kWatchBlock;
        const std::size_t size = block ? kWatchBlock : kWatchPage;
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_READWRITE | PAGE_GUARD, &old)) break;
        guarded += block ? kWatchBlock / kWatchPage : 1;
        at += size;
    }
    return guarded;
}

// Restores ordinary writable protection on [address, address + bytes); used to undo an arm and by forget/suspend.
void disarm_range(std::uintptr_t start, std::uintptr_t end) {
    for (std::uintptr_t at = start; at < end;) {
        const bool block = (at & (kWatchBlock - 1)) == 0 && end - at >= kWatchBlock;
        const std::size_t size = block ? kWatchBlock : kWatchPage;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT) {
            at += size;
            continue;
        }
        if (info.Protect & PAGE_GUARD) {
            DWORD old = 0;
            VirtualProtect(reinterpret_cast<void*>(at), size, info.Protect & ~DWORD(PAGE_GUARD), &old);
        }
        at += size;
    }
}

bool range_is_watched(std::uintptr_t start) {
    for (const auto& range : g_watch_ranges)
        if (range.start == start) return true;
    return false;
}

// Re-guards the pages the handler recorded and clears the record. With `read_verdict`, the change flag is taken too.
rt::xmem::WriteWatchPoll drain_trips(bool read_verdict) {
    if (g_watch_overflow.exchange(false)) return rt::xmem::WriteWatchPoll::unavailable;
    const bool changed = read_verdict ? g_watch_changed.exchange(false) : g_watch_changed.load();
    for (unsigned i = 0, count = g_watch_trip_count.exchange(0); i < count; ++i) {
        const auto page = g_watch_trips[i].load(std::memory_order_relaxed);
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(page), kWatchPage, PAGE_READWRITE | PAGE_GUARD, &old);
    }
    return changed ? rt::xmem::WriteWatchPoll::changed : rt::xmem::WriteWatchPoll::unchanged;
}

LONG CALLBACK watch_handler(EXCEPTION_POINTERS* info) {
    thread_local bool busy = false;
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (busy || !record || record->ExceptionCode != EXCEPTION_GUARD_PAGE || record->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto page = static_cast<std::uintptr_t>(record->ExceptionInformation[1]) & ~(kWatchPage - 1);
    const unsigned published = g_watch_published.load(std::memory_order_acquire);
    bool ours = false;
    for (unsigned i = 0; i < published && i < kWatchRanges; ++i) {
        if (page >= g_watch_begin[i].load(std::memory_order_relaxed) &&
            page < g_watch_end[i].load(std::memory_order_relaxed)) {
            ours = true;
            break;
        }
    }
    // Not a page this tracker guards: the title's own guard pages must keep their own exception handling.
    if (!ours) return EXCEPTION_CONTINUE_SEARCH;
    busy = true;
    const unsigned slot = g_watch_trip_count.fetch_add(1, std::memory_order_relaxed);
    if (slot < kWatchTrips) {
        g_watch_trips[slot].store(page, std::memory_order_relaxed);
        // 0 = read, 1 = write, 8 = execute (EXCEPTION_INFORMATION for an access violation; the guard page uses the
        // same layout). A read or an execute is not a change: only a store makes the mirror stale.
        if (record->ExceptionInformation[0] == 0 || record->ExceptionInformation[0] == 8) {
            g_watch_read_trips.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_watch_write_trips.fetch_add(1, std::memory_order_relaxed);
            g_watch_changed.store(true, std::memory_order_relaxed);
        }
    } else {
        g_watch_overflow.store(true, std::memory_order_relaxed);
    }
    busy = false;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void install_watch_handler() {
    static const bool installed = AddVectoredExceptionHandler(1, &watch_handler) != nullptr;
    (void)installed;
}

bool watch_arm(const void* address, std::size_t bytes) {
    if (!address || !bytes) return false;
    install_watch_handler();
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(address) & ~(kWatchPage - 1);
    const std::uintptr_t end =
        (reinterpret_cast<std::uintptr_t>(address) + bytes + kWatchPage - 1) & ~(kWatchPage - 1);
    std::lock_guard lock(g_watch_mutex);
    if (range_is_watched(start)) return true;  // already watched; arming twice would leak the original protection
    for (std::uintptr_t at = start; at < end; at += kWatchBlock)
        if (!watchable_block(at)) return false;
    if (g_watch_ranges.size() >= kWatchRanges) return false;
    // Publish the range before arming it: a fault that arrives between the two is then recognised as ours (a page
    // that is not guarded yet cannot fault, and one that is guarded by a previous range must not be orphaned).
    const unsigned slot = g_watch_published.load(std::memory_order_relaxed);
    g_watch_begin[slot].store(start, std::memory_order_relaxed);
    g_watch_end[slot].store(end, std::memory_order_relaxed);
    g_watch_published.store(slot + 1, std::memory_order_release);
    const std::size_t guarded = arm_range(start, end);
    if (guarded != (end - start) / kWatchPage) {
        disarm_range(start, end);
        g_watch_published.store(slot, std::memory_order_release);
        return false;
    }
    g_watch_ranges.push_back({start, end});
    g_watch_pages_guarded.fetch_add(guarded, std::memory_order_relaxed);
    return true;
}

void watch_forget(const void* address) {
    if (!address) return;
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(address) & ~(kWatchPage - 1);
    std::lock_guard lock(g_watch_mutex);
    for (std::size_t i = 0; i < g_watch_ranges.size(); ++i) {
        if (g_watch_ranges[i].start != start) continue;
        const std::uint64_t pages = (g_watch_ranges[i].end - g_watch_ranges[i].start) / kWatchPage;
        disarm_range(g_watch_ranges[i].start, g_watch_ranges[i].end);
        // Close the publication hole: move the last published range into this slot.
        const unsigned last = g_watch_published.load(std::memory_order_relaxed) - 1;
        if (last != i) {
            g_watch_begin[i] = g_watch_begin[last].load(std::memory_order_relaxed);
            g_watch_end[i] = g_watch_end[last].load(std::memory_order_relaxed);
        }
        g_watch_published.store(last, std::memory_order_release);
        g_watch_ranges.erase(g_watch_ranges.begin() + std::ptrdiff_t(i));
        g_watch_pages_guarded.fetch_sub(pages, std::memory_order_relaxed);
        return;
    }
}

rt::xmem::WriteWatchPoll watch_poll() {
    std::lock_guard lock(g_watch_mutex);
    if (g_watch_ranges.empty()) return rt::xmem::WriteWatchPoll::unavailable;
    return drain_trips(true);
}

void watch_rearm() {
    std::lock_guard lock(g_watch_mutex);
    if (g_watch_ranges.empty()) return;
    (void)drain_trips(false);
}

// Suspends the guard state so a full refresh scan does not pay one exception per 4 KiB page (measured ~35 ms for the
// 39.8 MB heap), and resumes it afterwards. Used only when the caller runs the full work a poll could not skip.
void watch_suspend() {
    std::lock_guard lock(g_watch_mutex);
    for (const auto& range : g_watch_ranges) disarm_range(range.start, range.end);
    // Trips recorded before the suspension are drained here: the full work that follows reads every page anyway.
    g_watch_trip_count.store(0, std::memory_order_relaxed);
}

void watch_resume() {
    std::lock_guard lock(g_watch_mutex);
    g_watch_changed.store(false, std::memory_order_relaxed);
    for (const auto& range : g_watch_ranges) {
        if (arm_range(range.start, range.end) != (range.end - range.start) / kWatchPage) {
            // A page that cannot be guarded again: the caller's poll will answer "unavailable" (range dropped).
            g_watch_overflow.store(true, std::memory_order_relaxed);
        }
    }
}

std::uint64_t watch_write_trips() { return g_watch_write_trips.load(std::memory_order_relaxed); }
std::uint64_t watch_read_trips() { return g_watch_read_trips.load(std::memory_order_relaxed); }
std::uint64_t watch_pages() { return g_watch_pages_guarded.load(std::memory_order_relaxed); }

}  // namespace

[[maybe_unused]] const bool g_page_watch_installed = (rt::xmem::page_watch_arm = &watch_arm,
                                                      rt::xmem::page_watch_forget = &watch_forget,
                                                      rt::xmem::page_watch_poll = &watch_poll,
                                                      rt::xmem::page_watch_rearm = &watch_rearm,
                                                      rt::xmem::page_watch_suspend = &watch_suspend,
                                                      rt::xmem::page_watch_resume = &watch_resume,
                                                      rt::xmem::page_watch_write_trips = &watch_write_trips,
                                                      rt::xmem::page_watch_read_trips = &watch_read_trips,
                                                      rt::xmem::page_watch_pages = &watch_pages, true);

UINT WINAPI XMemGetWriteWatch(DWORD flags, void* base_address, SIZE_T region_size, void** addresses, ULONG_PTR* count,
                              ULONG* granularity) {
    return GetWriteWatch(flags, base_address, region_size, addresses, count, granularity);
}

UINT WINAPI XMemResetWriteWatch(void* base_address, SIZE_T region_size) {
    return ResetWriteWatch(base_address, region_size);
}

// BOOL XMemTransferMemory(XMEM_TRANSFER_TYPE type, SIZE_T size). Its reference page and the XMEM_TRANSFER_TYPE enum
// page (gaming/gdk/docs/reference/system/xmem/{functions/xmemtransfermemory,enums/xmem_transfer_type}) are NDA
// topics on Microsoft Learn, so the shape comes from public code built against the GDK's xmem.h: Microsoft's
// XBOX-MonoGame-NetRumble sample (console/NetRumble.Bootstrap/Main.cpp) and FNA-XNA/NativeAOT-Xbox (Bootstrap/Main.cpp)
// both call `if (!XMemTransferMemory(XMemTransferTitleToSystem, additionalKernelPoolMemory))` with a `const size_t`,
// commented "Transfer additional memory to kernel pool" and "Must be multiples of 2MB sizes". So: two arguments, the
// enum (32 bits) in ECX and the size in RDX, and a zero return is failure. The public XLaunchNewGame page adds that
// "Memory transferred to the system using XMemTransferMemory (NDA topic) is persisted", and the game-save debugging
// page suggests it for E_OUTOFMEMORY "to transfer some memory to the system partition".
//
// On the console it moves budget between the title's partition and the system's. A PC has one pool and no budget
// split, so there is nothing to move: every direction and size returns TRUE and nothing is allocated or reserved. Not
// known (the pages are NDA): the numeric XMEM_TRANSFER_TYPE values, and whether the console refuses or rounds a size
// that is not a multiple of 2 MiB. Refusing would stop a title at startup (both samples return -1), while succeeding
// changes nothing here. TRUE is 1 in EAX, so it also reads as true if the real return type is a one-byte bool.
//
// palmetto --trace FILE: the first call for each distinct (type, size) pair goes to the startup trace as an
// "xmem_transfer" event with the caller as module+RVA, so a run with the executable records the values it passes.
BOOL WINAPI XMemTransferMemory(uint32_t type, SIZE_T size) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] xmem: XMemTransferMemory(type %u, %zu bytes): a PC has no title/system memory partition; "
                     "returning TRUE without moving anything (logged once)\n",
                     type, static_cast<size_t>(size));
    }
    if (rt::startup_trace::enabled()) {
        static std::mutex mutex;
        static std::set<std::pair<uint32_t, uint64_t>> seen;
        bool first = false;
        {
            const std::lock_guard lock(mutex);
            first = seen.insert({type, static_cast<uint64_t>(size)}).second;
        }
        if (first) {
            rt::startup_trace::event(
                "xmem_transfer",
                "\"api\":\"XMemTransferMemory\",\"type\":" + std::to_string(type) +
                    ",\"size\":" + std::to_string(static_cast<uint64_t>(size)) + ",\"caller\":" +
                    rt::startup_trace::quote(
                        rt::startup_trace::where(reinterpret_cast<uint64_t>(_ReturnAddress()))));
        }
    }
    return TRUE;
}

// kernel32!VirtualAlloc as a title calls it. The console's allocation type has two bits of its own, MEM_GRAPHICS
// (0x10000000, memory the GPU reads) and MEM_TITLE (0x40000000, the title's own heap). On a PC the host rejects both
// with ERROR_INVALID_PARAMETER (measured on Windows 11 26200: VirtualAlloc(NULL, 64 KiB, MEM_COMMIT | MEM_RESERVE |
// 0x10000000, PAGE_READWRITE) returns NULL; so does 0x40000000), so an engine allocating its heaps this way gets
// NULL at boot. Both are hints about where the memory lives; the host has no such split, so they are dropped and
// the rest is passed through. The PAGE_GRAPHICS_* protection bits are dropped (host_protect). Not done: whether
// graphics memory has to come from an arena the D3D12 layer can see (#313).
constexpr DWORD kMemGraphics = 0x10000000;
constexpr DWORD kMemTitle = 0x40000000;

LPVOID WINAPI TitleVirtualAlloc(LPVOID address, SIZE_T size, DWORD allocation_type, DWORD protect) {
    note_mem_trace_switch();
    if (mem_trace_enabled()) {
        const DWORD type = allocation_type & ~(kMemGraphics | kMemTitle);
        const auto caller = reinterpret_cast<std::uint64_t>(_ReturnAddress());
        const auto bytes = static_cast<std::uint64_t>(size);
        g_mem_trace.tva_calls.fetch_add(1, std::memory_order_relaxed);
        g_mem_trace.tva_bytes.fetch_add(bytes, std::memory_order_relaxed);
        if (type & MEM_COMMIT) {
            g_mem_trace.tva_commit_bytes.fetch_add(bytes, std::memory_order_relaxed);
            mem_note_bytes(mem_title_va, caller, bytes);
        }
    }
    return VirtualAlloc(address, size, allocation_type & ~(kMemGraphics | kMemTitle), host_protect(protect));
}

// The print thread. Every line is counts and RVAs only: no game data, no path, no title bytes.
void start_mem_trace() {
    static std::once_flag once;
    std::call_once(once, [] {
        const std::uint64_t period = g_mem_period_ms.load(std::memory_order_relaxed);
        if (!period) return;
        std::thread([period] {
            const double every = double(period) / 1000.0;
            const auto origin = std::chrono::steady_clock::now();
            for (unsigned line = 1; line <= 240; ++line) {
                std::this_thread::sleep_until(origin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                           std::chrono::duration<double>(every * line)));
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                pmc.cb = sizeof pmc;
                const bool have_process =
                    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                         sizeof pmc) != FALSE;
                // The process heaps' committed bytes: the title's XMemAlloc heap and the layer's own C++ heap are both
                // here, so a growth the allocator counters below do not explain is the layer's own host memory.
                std::uint64_t heap_committed = 0;
                unsigned heaps = 0;
                {
                    HANDLE handles[64];
                    const DWORD found = GetProcessHeaps(64, handles);
                    for (DWORD i = 0; i < found; ++i) {
                        HEAP_SUMMARY summary{};
                        summary.cb = sizeof summary;
                        if (HeapSummary(handles[i], 0, &summary)) {
                            heap_committed += summary.cbCommitted;
                            ++heaps;
                        }
                    }
                }
                // The two physical-page tables, under their lock (a short scan: the entries are per 64 KiB page).
                std::size_t live_pages = 0, mapped_pages = 0, sections = 0;
                {
                    std::lock_guard lock(g_phys_mutex);
                    live_pages = g_pages.size();
                    mapped_pages = g_mapped.size();
                    std::set<const Section*> seen;
                    for (const auto& [page, backing] : g_pages) seen.insert(backing.section.get());
                    sections = seen.size();
                }
                // The caller table, copied under its lock and sorted by live+total bytes.
                MemCaller top[kMemCallerSlots];
                unsigned count = 0;
                {
                    std::lock_guard lock(g_mem_caller_mutex);
                    count = g_mem_caller_count;
                    for (unsigned i = 0; i < count; ++i) top[i] = g_mem_callers[i];
                }
                std::sort(top, top + count, [](const MemCaller& a, const MemCaller& b) {
                    return a.live + a.total > b.live + b.total;
                });
                std::string callers;
                char text[160];
                const unsigned shown = count < 10 ? count : 10;
                static const char* const kinds[] = {"heap", "xmemva", "titleva", "phys"};
                for (unsigned i = 0; i < shown; ++i) {
                    if (i) callers += "; ";
                    callers += kinds[top[i].kind & 3];
                    callers += " " + rt::startup_trace::where(top[i].caller);
                    std::snprintf(text, sizeof text, " live %.1f MB total %.1f MB %llu call(s)",
                                  double(top[i].live) / (1 << 20), double(top[i].total) / (1 << 20),
                                  static_cast<unsigned long long>(top[i].count));
                    callers += text;
                }
                const double mib = 1.0 / (1 << 20);
                std::fprintf(stderr,
                             "[rt] mem: +%.1fs private %.1f MB ws %.1f MB%s | process heaps %.1f MB in %u heap(s) | "
                             "xmem heap live %.1f MB peak %.1f MB "
                             "blocks %llu | XMemVirtualAlloc %llu call(s) %.1f MB, %llu commit(s) %.1f MB | "
                             "VirtualAlloc %llu call(s) %.1f MB, commit %.1f MB | physical pages %llu call(s) %llu "
                             "page(s) %.1f MB, mapped %llu call(s) %llu page(s), %zu section(s) live, %llu page(s) "
                             "committed by the fault handler %.1f MB\n",
                             every * line, have_process ? double(pmc.PrivateUsage) * mib : 0.0,
                             have_process ? double(pmc.WorkingSetSize) * mib : 0.0,
                             have_process ? "" : " (no process counters)", double(heap_committed) * mib, heaps,
                             double(g_mem_trace.heap_live.load()) * mib, double(g_mem_trace.heap_peak.load()) * mib,
                             static_cast<unsigned long long>(g_mem_trace.heap_blocks.load()),
                             static_cast<unsigned long long>(g_mem_trace.xva_calls.load()),
                             double(g_mem_trace.xva_bytes.load()) * mib,
                             static_cast<unsigned long long>(g_mem_trace.xva_commit_calls.load()),
                             double(g_mem_trace.xva_commit_bytes.load()) * mib,
                             static_cast<unsigned long long>(g_mem_trace.tva_calls.load()),
                             double(g_mem_trace.tva_bytes.load()) * mib,
                             double(g_mem_trace.tva_commit_bytes.load()) * mib,
                             static_cast<unsigned long long>(g_mem_trace.phys_calls.load()),
                             static_cast<unsigned long long>(g_mem_trace.phys_pages.load()),
                             double(g_mem_trace.phys_pages.load() * kPhysicalPageSize) * mib,
                             static_cast<unsigned long long>(g_mem_trace.map_calls.load()),
                             static_cast<unsigned long long>(g_mem_trace.map_pages.load()), sections,
                             static_cast<unsigned long long>(g_mem_trace.committed_pages.load()),
                             double(g_mem_trace.committed_pages.load() * kPhysicalPageSize) * mib);
                if (!callers.empty())
                    std::fprintf(stderr, "[rt] mem:       top callers: %s\n", callers.c_str());
                // The allocation attribution: patch the modules loaded since the last interval, then print the largest
                // allocation stacks seen so far (PALMETTO_ALLOC_TRACE=1; a no-op otherwise).
                palmetto::alloc_trace::tick();
                palmetto::alloc_trace::print();
            }
        }).detach();
    });
}

}  // namespace

namespace rt::xmem {

// Which XMem entry point served `address`, and from which title caller (raw address; the caller resolves it to a
// module+RVA). False when no recorded XMemVirtualAlloc answer contains it, or when the registry switch is off.
bool served_range(const void* address, std::uint64_t* caller, std::uint8_t* kind) {
    if (!address) return false;
    return find_served_range(reinterpret_cast<std::uintptr_t>(address), kind, caller);
}

bool commit_physical_range(const void* address, size_t size) {
    if (size == 0) return true;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (start > (std::numeric_limits<uintptr_t>::max)() - (size - 1)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    const uintptr_t mask = ~static_cast<uintptr_t>(kPhysicalPageSize - 1);
    const uintptr_t first = start & mask;
    const uintptr_t pages = (((start + (size - 1)) & mask) - first) / kPhysicalPageSize + 1;
    const FaultLockShared lock;
    for (uintptr_t i = 0; i < pages; ++i) {
        DWORD protect = 0;
        const uintptr_t page = first + i * kPhysicalPageSize;
        if (commit_mapped_page(page, page, protect) == PageCommit::failed) return false;
    }
    return true;
}

bool commit_physical_pages(const uint64_t* page_array, size_t count) {
    if (count == 0) return true;
    if (!page_array) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    const std::lock_guard lock(g_phys_mutex);
    size_t first = 0;
    while (first < count) {
        const auto it = g_pages.find(page_array[first]);
        if (it == g_pages.end() || !it->second.section->scratch) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        size_t last = first + 1;
        while (last < count) {
            const auto next = g_pages.find(page_array[last]);
            if (next == g_pages.end() || next->second.section != it->second.section ||
                next->second.offset != it->second.offset + (last - first) * kPhysicalPageSize) {
                break;
            }
            ++last;
        }
        void* const backing = static_cast<uint8_t*>(it->second.section->scratch) + it->second.offset;
        if (!VirtualAlloc(backing, (last - first) * kPhysicalPageSize, MEM_COMMIT, PAGE_READWRITE)) return false;
        if (mem_trace_enabled()) g_mem_trace.committed_pages.fetch_add(last - first, std::memory_order_relaxed);
        first = last;
    }
    return true;
}

// The DirectStorage page-list resolver (xmem.h, docs/page_resolver.md). A page-list request names physical pages, not
// addresses, and the console DMA writes the page's memory; here the page's memory is the section view this file keeps
// as its backing, so that view is the write target and every mapping of the page is the same memory. The page is
// committed on the way out: a page allocated but never touched is still SEC_RESERVE and a store into its backing would
// fault, and the fault handler only knows mapped views (commit_mapped_page), not the backing. PAGE_READWRITE is what
// commit_physical_pages uses for the same reason. Locking: the copy runs after this returns, so the address is only as
// stable as the console contract makes it - a request's page list stays live until its status completes, and a title
// that frees a page in the middle of its own transfer is out of contract (docs/page_resolver.md section 6).
std::uint8_t* resolve_physical_page(std::uint64_t page_number) {
    if (page_number == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const std::lock_guard lock(g_phys_mutex);
    const auto it = g_pages.find(page_number);
    if (it == g_pages.end() || !it->second.section->scratch) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto* const backing = static_cast<std::uint8_t*>(it->second.section->scratch) + it->second.offset;
    if (!VirtualAlloc(backing, kPhysicalPageSize, MEM_COMMIT, PAGE_READWRITE)) return nullptr;
    return backing;
}
[[maybe_unused]] const bool g_resolve_physical_page_installed =
    (rt::xmem::resolve_physical_page_fn = &resolve_physical_page, true);

}

#define TITLE_VIRTUAL_ALLOC(dll) {dll, "VirtualAlloc", reinterpret_cast<void*>(&TitleVirtualAlloc)}

// The classic libraries and the memory api sets a title's imports name.
SHIM_TABLE(title_memory,
    TITLE_VIRTUAL_ALLOC("kernel32.dll"),
    TITLE_VIRTUAL_ALLOC("kernelbase.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-0.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-1.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-2.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-3.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-4.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-5.dll"),
    TITLE_VIRTUAL_ALLOC("api-ms-win-core-memory-l1-1-6.dll"),
);

SHIM_TABLE(xmem,
    {"xmem.dll", "XMemAlloc", reinterpret_cast<void*>(&XMemAlloc)},
    {"xmem.dll", "XMemFree", reinterpret_cast<void*>(&XMemFree)},
    {"xmem.dll", "XMemVirtualQuery", reinterpret_cast<void*>(&XMemVirtualQuery)},
    {"xmem.dll", "XMemGetWorkingSetStatistics", reinterpret_cast<void*>(&XMemGetWorkingSetStatistics)},
    {"xmem.dll", "XMemVirtualAlloc", reinterpret_cast<void*>(&XMemVirtualAlloc)},
    {"xmem.dll", "XMemGetWriteWatch", reinterpret_cast<void*>(&XMemGetWriteWatch)},
    {"xmem.dll", "XMemResetWriteWatch", reinterpret_cast<void*>(&XMemResetWriteWatch)},
    {"xmem.dll", "XMemTransferMemory", reinterpret_cast<void*>(&XMemTransferMemory)},
    {"xmem.dll", "XMemAllocDefault", reinterpret_cast<void*>(&XMemAllocDefault)},
    {"xmem.dll", "XMemFreeDefault", reinterpret_cast<void*>(&XMemFreeDefault)},
    {"xmem.dll", "XMemIsToolsMemAvailable", reinterpret_cast<void*>(&XMemIsToolsMemAvailable)},
    {"xmem.dll", "XMemAllocatePhysicalPages", reinterpret_cast<void*>(&XMemAllocatePhysicalPages)},
    {"xmem.dll", "XMemFreePhysicalPages", reinterpret_cast<void*>(&XMemFreePhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPages", reinterpret_cast<void*>(&XMemMapPhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPagesScatter", reinterpret_cast<void*>(&XMemMapPhysicalPagesScatter)},
);
