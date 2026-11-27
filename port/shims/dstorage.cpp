// Xbox DirectStorage for a mapped title: DStorageGetFactory and the IDStorageFactoryX2 / IDStorageQueueX2 /
// IDStorageFileX / IDStorageStatusArrayX objects of the console API (dstorage_xs_abi.h).
//
// Served as dstorage_xs.dll, xcrdapi.dll and dstorage.dll!DStorageGetFactory; docs/directstorage.md has the evidence
// for those names, the console ABI and what each method does here. In short:
//   - OpenFile resolves kernel32.dll!CreateFileW through the shim registry, so a GAME: path goes through the
//     runtime's console path mapping and write overlay (file_system.cpp) exactly as the title's own CreateFileW.
//   - Each queue has one worker thread that runs its submitted entries in order: a request reads a file range or
//     copies a memory range into the destination (inflating a zlib request's source first), a status entry, fence
//     signal or event runs when every entry before it has. Completion is therefore in queue order, as the API
//     guarantees.
//   - A request's zlib stage (the reference's ZlibDecompress, RFC 1950) runs here: its compressed source is inflated
//     with the runtime's own inflate (src/shader_library.h) into the destination the request names. The advanced
//     stages have an opt-in slice behind PALMETTO_DS_* switches (docs/directstorage_advanced.md): a bounded staging
//     area, pinning of write-watched destinations, the physical page-list plan and its live transfer through xmem's
//     page resolver (docs/page_resolver.md), the swizzle/macro-unshuffle stage, the runtime BCPack block decoder and
//     the local BCPack predictor file it reads. Off by default, so a boot without the switches behaves exactly as
//     before.
// Nothing here goes online or touches the GPU.
#include <windows.h>
#include <intrin.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../src/shader_library.h"  // palmetto::zlib_inflate: the runtime's own RFC 1950/1951 inflate
#include "dstorage_advanced.h"     // staging, destination pinning, page-list plan, predictor file, unshuffle
#include "dstorage_xs_abi.h"
#include "first_touch.h"           // the shared first-touch page commit used by the file-range read repair
#include "shims.h"
#include "startup_trace.h"         // the named, bounded "dstorage_read_noaccess" measurement on the repair path
#include "xmem.h"                  // resolve_physical_page_fn: the page-number -> CPU view seam of a page-list request

using namespace dsx;

// Defined in d3d12_xs_device.cpp (linked into the same executable): append a completed destination block to the device
// proxy's range registry, and publish the title image base so its traces print game+RVA callers. Never called when
// PALMETTO_STREAM_CONSUMER is off.
extern "C" void palmetto_stream_block_completed(void* va, size_t bytes);
extern "C" void palmetto_stream_set_title_base(std::uint64_t base);
// Defined in d3d12_xs_device.cpp: register a loading thread id, so the load-window trace there can filter the frame
// loop's device calls out and keep the loading threads' calls. Cheap (a short atomic scan); safe from the guard handler.
extern "C" void palmetto_load_tid_note(unsigned long tid);
// Defined in d3d12_xs_device.cpp: a guarded read of any committed, readable address (the loader-state snapshot below).
extern "C" unsigned palmetto_mem_read(unsigned long long address, void* out, unsigned size);

namespace {

// ---- Logging ------------------------------------------------------------------------------------------------------

// Process-lifetime state is allocated once and never freed: a worker thread may still run while statics are destroyed
// at exit.
std::mutex& log_mutex() {
    static auto* m = new std::mutex;
    return *m;
}

void log_once(const std::string& key, const std::string& line) {
    static auto* seen = new std::set<std::string>;
    std::lock_guard lock(log_mutex());
    if (!seen->insert(key).second) return;
    std::fprintf(stderr, "[rt] dstorage: %s\n", line.c_str());
}

std::string guid_text(const GUID& g) {
    char b[48];
    std::snprintf(b, sizeof b, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

bool same_guid(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

std::string hr_text(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lx", static_cast<unsigned long>(hr));
    return b;
}

// PALMETTO_DSTORAGE_TRACE=1: one bounded line per queue call and per entry the worker runs, so a boot shows what the
// title asked the queue to do (requests, status, fence signals, events set) and what the shim delivered. Handles,
// fence pointers, values and HRESULTs only; no file data, no paths (docs/wait_state.md). Off by default.
bool storage_trace_enabled() {
    static const bool on = [] {
        char value[8] = {};
        size_t length = 0;
        return getenv_s(&length, value, "PALMETTO_DSTORAGE_TRACE") == 0 && length > 1 && value[0] != '0' &&
               value[0] != 'n';
    }();
    return on;
}

void storage_trace(const std::string& line) {
    if (!storage_trace_enabled()) return;
    std::lock_guard lock(log_mutex());
    std::fprintf(stderr, "[rt] dstorage trace: %s\n", line.c_str());
}

// The same for a repeated kind of line: the first few per site, so the trace shows what was asked without a flood of
// one repeated request (the progress counters carry the totals).
void storage_trace_site(const char* site, const std::string& line) {
    if (!storage_trace_enabled()) return;
    static std::mutex mutex;
    static std::unordered_map<std::string, unsigned> printed;
    {
        const std::lock_guard lock(mutex);
        if (++printed[site] > 8) return;
    }
    std::lock_guard lock(log_mutex());
    std::fprintf(stderr, "[rt] dstorage trace: %s\n", line.c_str());
}

// The same, with the title-side caller's module+RVA: the return address names which title subsystem drives the queue
// (docs/wait_state.md). Only formatted when the line is actually printed.
void storage_trace_site_at(const char* site, const std::string& line, void* caller) {
    if (!storage_trace_enabled()) return;
    static std::mutex mutex;
    static std::unordered_map<std::string, unsigned> printed;
    {
        const std::lock_guard lock(mutex);
        if (++printed[site] > 8) return;
    }
    std::lock_guard lock(log_mutex());
    std::fprintf(stderr, "[rt] dstorage trace: %s caller %s\n", line.c_str(),
                 rt::shims::module_offset((std::uint64_t)(std::uintptr_t)caller).c_str());
}

// dstorageerr.h's decompression failure code ("a generic error has happened during decompression"; the console ABI
// header declares the errors this file already reported, so the one the zlib stage needs stays local until that header
// carries it).
constexpr HRESULT kDecompressionError = static_cast<HRESULT>(0x89240030L);

// Which band a request buffer's size falls in, for the one-time logs: no byte counts, no addresses, and the bands are
// the reference's own request limits.
const char* size_band(uint32_t bytes) {
    if (bytes <= (64u << 10)) return "up to 64 KiB";
    if (bytes <= (1u << 20)) return "over 64 KiB, up to 1 MiB";
    if (bytes <= (8u << 20)) return "over 1 MiB, up to 8 MiB";
    if (bytes <= kMax4kPageDestinationSize) return "over 8 MiB, up to 32 MiB";
    return "over 32 MiB";
}

// ---- Advanced request mechanisms (opt-in; docs/directstorage_advanced.md) ------------------------------------------
//
// The measured boot asks for zlib file requests only (docs/wait_state.md section 3), so none of these mechanisms is on
// the boot's critical path today. Each one is behind its own switch, off by default, and the default path stays
// byte-identical: no extra line, no extra allocation, no extra call. A boot with the switches on reports the same
// counts in the trace, so the measurement is a comparison of the same instrumentation.

struct AdvancedConfig {
    std::size_t staging_cap = 0;      // PALMETTO_DS_STAGING: bytes; 0 = off
    bool pin = false;                 // PALMETTO_DS_PIN: hand a write-watched destination back to the consumer
    bool pin_resident = false;        // PALMETTO_DS_PIN_RESIDENT: VirtualLock the destination for the write
    bool pages = false;               // PALMETTO_DS_PAGES: transfer a page-list request live through the xmem resolver
    bool pages_view = false;          // PALMETTO_DS_PAGES_VIEW: deliver a page-list destination through xmem's page write
    bool unshuffle = false;           // PALMETTO_DS_UNSHUFFLE: run a swizzle request's macro-unshuffle stage
    bool bcpack = false;              // PALMETTO_DS_BCPACK: load the predictor file and refuse by named limitation
    std::wstring predictor;           // PALMETTO_DS_BCPACK_PREDICTOR: the local predictor path (outside the repository)
};

const AdvancedConfig& advanced_config() {
    static const AdvancedConfig config = [] {
        AdvancedConfig c;
        c.staging_cap = palmetto::dstorage::env_size("PALMETTO_DS_STAGING");
        c.pin = palmetto::dstorage::env_switch("PALMETTO_DS_PIN");
        c.pin_resident = c.pin && palmetto::dstorage::env_switch("PALMETTO_DS_PIN_RESIDENT");
        c.pages = palmetto::dstorage::env_switch("PALMETTO_DS_PAGES");
        c.pages_view = c.pages && palmetto::dstorage::env_switch("PALMETTO_DS_PAGES_VIEW");
        c.unshuffle = palmetto::dstorage::env_switch("PALMETTO_DS_UNSHUFFLE");
        c.bcpack = palmetto::dstorage::env_switch("PALMETTO_DS_BCPACK");
        wchar_t path[32768] = {};
        const DWORD capacity = static_cast<DWORD>(sizeof path / sizeof path[0]);
        const DWORD n = GetEnvironmentVariableW(L"PALMETTO_DS_BCPACK_PREDICTOR", path, capacity);
        if (n > 0 && n < capacity) c.predictor.assign(path, n);
        if (c.pages_view)
            std::fprintf(stderr,
                         "[rt] dstorage: PALMETTO_DS_PAGES_VIEW=1: a physical-page destination is written through "
                         "xmem's page write, so every title mapping sees the bytes and the page-watch tracker records "
                         "the write (docs/peer_port_streaming.md section 13)\n");
        return c;
    }();
    return config;
}

// Counters for the periodic trace line: what shapes the title asked for and what the mechanisms did. Atomics because
// several queues (each with a worker) share them.
struct AdvancedCounters {
    std::atomic<std::uint64_t> requests{0}, file_source{0}, memory_source{0}, zlib{0}, bcpack{0}, swizzle{0},
        destination_pages{0}, source_pages{0};
    std::atomic<std::uint64_t> staging_hits{0}, staging_fallbacks{0}, staging_bytes{0};
    std::atomic<std::uint64_t> pins{0}, pinned_pages{0}, write_watched{0}, pin_refusals{0}, pin_lock_failures{0};
    std::atomic<std::uint64_t> unshuffle_ok{0}, unshuffle_refused{0};
    std::atomic<std::uint64_t> bcpack_refused{0}, bcpack_decoded{0}, bcpack_blocks{0}, bcpack_bytes{0},
        bcpack_history_reads{0}, pages_planned{0}, pages_refused{0}, pages_transferred{0}, pages_bytes{0};
    // PALMETTO_DS_PAGES_VIEW: the destination write through xmem (docs/peer_port_streaming.md section 13).
    std::atomic<std::uint64_t> page_write_delivered{0}, page_write_bytes{0}, page_write_views{0}, page_write_watch_pages{0},
        page_write_refused{0};
};
AdvancedCounters g_advanced;

// One bounded line per request kind (the first few of each) plus the counters the periodic line prints.
void note_request_shape(const DSTORAGE_REQUEST& r) {
    const std::uint64_t o = r.Options.AsUINT64;
    ++g_advanced.requests;
    const bool memory = options::source_type(o) == DSTORAGE_REQUEST_SOURCE_MEMORY;
    (memory ? g_advanced.memory_source : g_advanced.file_source).fetch_add(1, std::memory_order_relaxed);
    if (options::zlib(o)) g_advanced.zlib.fetch_add(1, std::memory_order_relaxed);
    if (options::bcpack(o)) g_advanced.bcpack.fetch_add(1, std::memory_order_relaxed);
    if (options::swizzle(o)) g_advanced.swizzle.fetch_add(1, std::memory_order_relaxed);
    if (o & options::kDestinationPages) g_advanced.destination_pages.fetch_add(1, std::memory_order_relaxed);
    if (o & options::kSourcePages) g_advanced.source_pages.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<unsigned> printed{0};
    if (printed.fetch_add(1, std::memory_order_relaxed) >= 6) return;
    std::string line = std::string("shape: source=") + (memory ? "memory" : "file") + " zlib=" +
                       (options::zlib(o) ? "1" : "0") + " bcpack=" + std::to_string(options::bcpack(o)) +
                       " swizzle=" + std::to_string(options::swizzle(o)) + " destination=" +
                       ((o & options::kDestinationPages) ? "pages" : "buffer") + " source-pages=" +
                       ((o & options::kSourcePages) ? "1" : "0") + " source " + size_band(r.SourceSize) +
                       ", destination " + size_band(r.DestinationSize);
    storage_trace(line);
}

// Per-queue staging stats, folded into the process counters when a request finishes.
void note_staging(const palmetto::dstorage::StagingStats& before, const palmetto::dstorage::StagingStats& after) {
    g_advanced.staging_hits.fetch_add(after.hits - before.hits, std::memory_order_relaxed);
    g_advanced.staging_fallbacks.fetch_add(after.fallbacks - before.fallbacks, std::memory_order_relaxed);
    g_advanced.staging_bytes.fetch_add(after.bytes - before.bytes, std::memory_order_relaxed);
}

void note_pin(const palmetto::dstorage::PinStats& stats) {
    g_advanced.pins.fetch_add(stats.pinned, std::memory_order_relaxed);
    g_advanced.pinned_pages.fetch_add(stats.pages, std::memory_order_relaxed);
    g_advanced.write_watched.fetch_add(stats.write_watched, std::memory_order_relaxed);
    g_advanced.pin_refusals.fetch_add(stats.refusals, std::memory_order_relaxed);
    g_advanced.pin_lock_failures.fetch_add(stats.lock_failures, std::memory_order_relaxed);
}

// The predictor file is loaded once, on the first BCPack request. It supplies every built-in endpoint/index history
// row the strict block decoder may read; nothing is compiled in. A missing path, an unreadable file and an invalid
// table are distinct named refusals, and a request is refused before decoding when the file is not usable, so a chunk
// that would need a row cannot be guessed at. docs/bcpack_runtime.md owns the policy.
struct PredictorState {
    palmetto::dstorage::PredictorError error = palmetto::dstorage::PredictorError::missing;
    palmetto::dstorage::PredictorReport report;
    palmetto::dstorage::BcpackHistory history;
};
const PredictorState& predictor_state() {
    static const PredictorState state = [] {
        PredictorState result;
        result.error = palmetto::dstorage::load_dstorage_predictor(advanced_config().predictor.c_str(), result.report,
                                                                   &result.history);
        if (result.error == palmetto::dstorage::PredictorError::ok) {
            std::string counts = "loaded (";
            if (result.report.bc6h)
                counts += "BC6H endpoints " + std::to_string(result.report.bc6h_endpoint_rows) + "/256, indices " +
                          std::to_string(result.report.bc6h_index_rows) + "/256; ";
            if (result.report.bc7)
                counts += "BC7 endpoints " + std::to_string(result.report.bc7_endpoint_rows) + "/256, indices " +
                          std::to_string(result.report.bc7_index_rows) + "/256; ";
            counts += "BC1 rows " + std::to_string(result.report.bc1_rows) + ", BC3 rows " +
                      std::to_string(result.report.bc3_rows) + ", BC4 rows " + std::to_string(result.report.bc4_rows) +
                      ", BC5 rows " + std::to_string(result.report.bc5_rows) + ")";
            log_once("predictor-ok", "the local BCPack predictor file " + counts);
        } else {
            log_once(std::string("predictor-") + palmetto::dstorage::predictor_error_name(result.error),
                     std::string("the local BCPack predictor file is ") +
                         palmetto::dstorage::predictor_error_name(result.error) +
                         " (PALMETTO_DS_BCPACK_PREDICTOR names a path outside the repository, written by "
                         "tooling/bcpack_history_calib.py)");
        }
        return result;
    }();
    return state;
}

const char* predictor_state_name() {
    if (!advanced_config().bcpack) return "off";
    return palmetto::dstorage::predictor_error_name(predictor_state().error);
}

// ---- Load-window trace (measurement only; PALMETTO_DS_LOAD_TRACE=<from_s>,<to_s>) -----------------------------------
//
// docs/streamed_consumer.md: the title's CPU deserializer reads the first sixteen streamed blocks at ~2.3 s and then
// stops. This trace answers what the loader asks DirectStorage next: every status-array poll (index and value, with the
// polling caller as module+RVA), every queue call (enqueue, submit, query) and a 1 s heartbeat that keeps counting
// after the window. The window argument is seconds since the first traced DirectStorage call; `PALMETTO_DS_LOAD_TRACE=1`
// means 0..15 s. Bounded (2,000 window lines, 20 heartbeat lines), off by default, changes no call.

struct DsLoadWindow {
    bool on = false;
    double from = 0.0;
    double to = 15.0;
};

const DsLoadWindow& ds_load_window() {
    static const DsLoadWindow window = [] {
        DsLoadWindow w;
        char value[64] = {};
        if (GetEnvironmentVariableA("PALMETTO_DS_LOAD_TRACE", value, sizeof(value)) == 0 || value[0] == '\0') return w;
        w.on = true;
        char* end = nullptr;
        w.from = std::strtod(value, &end);
        if (end == value) return w;  // "1" and anything unparsable keep 0..15
        if (*end == ',') {
            const double to = std::strtod(end + 1, nullptr);
            if (to > w.from) w.to = to;
        }
        if (w.from != 0.0) w.on = true;
        return w;
    }();
    return window;
}

// Seconds since the first traced DirectStorage call (the start is taken on first use, before the title's streaming).
double ds_load_seconds() {
    static const auto started = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

struct DsLoadCounters {
    std::atomic<std::uint64_t> window_lines{0};
    std::atomic<std::uint64_t> requests_enqueued{0};
    std::atomic<std::uint64_t> requests_done{0};
    std::atomic<std::uint64_t> submits{0};
    std::atomic<std::uint64_t> status_polls{0};
    std::atomic<std::uint64_t> pending_polls{0};
    std::atomic<std::uint64_t> queries{0};
    std::atomic<std::uint64_t> stat_requests{0};
    std::atomic<std::uint64_t> last_poll_index{~std::uint32_t(0)};
    std::atomic<std::uint64_t> last_poll_value{0};
    std::atomic<std::uint64_t> last_poll_caller{0};
    std::atomic<std::uint64_t> max_queue_depth{0};
};
DsLoadCounters g_ds_load;

bool ds_load_visible() { return ds_load_window().on; }

// Prints one window line when `what` happens inside [from, to). `caller` is the title return address; `detail` is
// already formatted (counts/values only).
void ds_load_note(const char* what, const std::string& detail, void* caller) {
    if (!ds_load_visible()) return;
    const double at = ds_load_seconds();
    const DsLoadWindow& window = ds_load_window();
    if (at < window.from || at >= window.to) return;
    if (g_ds_load.window_lines.fetch_add(1, std::memory_order_relaxed) >= 2000) return;
    std::lock_guard lock(log_mutex());
    std::fprintf(stderr, "[rt] ds load +%.3fs tid %lu %s: %s caller %s\n", at,
                 static_cast<unsigned long>(GetCurrentThreadId()), what, detail.c_str(),
                 rt::shims::module_offset((std::uint64_t)(std::uintptr_t)caller).c_str());
}

// One late line (outside the window) for a rare event: the loader still enqueuing or polling after the window. Bounded
// to 64 lines, so a title that polls a status array every frame cannot flood the log; the heartbeat counted the polls.
std::atomic<std::uint64_t> g_ds_late_lines{0};

void ds_load_note_late(const char* what, const std::string& detail, void* caller) {
    if (!ds_load_visible()) return;
    if (g_ds_late_lines.fetch_add(1, std::memory_order_relaxed) >= 64) return;
    const double at = ds_load_seconds();
    std::lock_guard lock(log_mutex());
    std::fprintf(stderr, "[rt] ds load late +%.1fs tid %lu %s: %s caller %s\n", at,
                 static_cast<unsigned long>(GetCurrentThreadId()), what, detail.c_str(),
                 rt::shims::module_offset((std::uint64_t)(std::uintptr_t)caller).c_str());
}

// ---- Title-side stack scan for the loader gate (measurement only; PALMETTO_DS_STACK=1) -----------------------------
//
// docs/streamed_consumer.md section 9. The status poll's recorded caller is our own inlined GetHResult, so the title
// polling loop is invisible, and the loading manager reaches the DirectStorage interface through an indirect dispatch.
// This switch scans the calling thread's stack on the first few status polls and request enqueues and prints the title
// frames it holds: those are the indirect frames above the polling / submission loop, i.e. the loading manager. Bounded
// (8 scans, 12 frames), off by default, changes no call.

// The title image base, captured from the first title caller a poll/enqueue stack scan sees (the stream consumer block
// below uses the same value, and the guard handler must never look a module up, so it subtracts this base instead).
std::atomic<std::uint64_t> g_title_image_base{0};

bool ds_stack_enabled() {
    static const bool on = [] {
        char value[8] = {};
        size_t length = 0;
        return getenv_s(&length, value, "PALMETTO_DS_STACK") == 0 && length > 1 && value[0] != '0' &&
               value[0] != 'n';
    }();
    return on;
}

// The title image base: from the stream registry when already known, else from this caller address (the loader registers
// its module lookup at startup, so a poll/enqueue caller resolves).
bool ensure_title_base(void* caller) {
    if (g_title_image_base.load(std::memory_order_relaxed) != 0) return true;
    if (!caller) return false;
    std::uint64_t image = 0;
    if (!rt::shims::mapped_image_base(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller)), image))
        return false;
    g_title_image_base.store(image, std::memory_order_relaxed);
    palmetto_stream_set_title_base(image);
    return true;
}

// The scan itself: a real stack walk (RtlCaptureStackBackTrace), so only actual return addresses are seen - the raw
// word scan in the previous revision mixed saved registers and data pointers into the chain. Title frames are printed as
// game+RVA; shim/host frames are skipped. No C++ object with a destructor may live here: MSVC C2712 otherwise.
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long frames_to_skip,
                                                                                unsigned long frames_to_capture,
                                                                                void** back_trace,
                                                                                unsigned long* back_trace_hash);

void ds_stack_scan_walk(const void* return_slot, char* line, size_t capacity, std::uint64_t base) {
    unsigned printed = 0;
    int used = static_cast<int>(std::strlen(line));
    // A real stack walk: only actual return addresses.
    void* frames[32] = {};
    const unsigned depth = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    for (unsigned i = 0; i < depth && printed < 34 && used > 0 && static_cast<size_t>(used) < capacity - 32; ++i) {
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(frames[i]);
        if (address < base || address - base >= 0x10000000ull) continue;
        const int n = std::snprintf(line + used, capacity - static_cast<size_t>(used), " game+0x%llx",
                                    static_cast<unsigned long long>(address - base));
        if (n < 0) break;
        used += n;
        ++printed;
    }
    if (printed == 0) std::snprintf(line + used, capacity - static_cast<size_t>(used), " none");
    // The raw scan of the caller's stack words, as a cross-check for frames the unwinder drops.
    used = static_cast<int>(std::strlen(line));
    printed = 0;
    if (return_slot && used > 0 && static_cast<size_t>(used) < capacity - 200) {
        used += std::snprintf(line + used, capacity - static_cast<size_t>(used), " raw:");
        __try {
            const auto* words = static_cast<const std::uintptr_t*>(return_slot);
            for (unsigned i = 0; i < 160 && printed < 34 && used > 0 && static_cast<size_t>(used) < capacity - 32; ++i) {
                const std::uintptr_t word = words[i];
                if (word < base || word - base >= 0x10000000ull) continue;
                const int n = std::snprintf(line + used, capacity - static_cast<size_t>(used), " game+0x%llx",
                                            static_cast<unsigned long long>(word - base));
                if (n < 0) break;
                used += n;
                ++printed;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (printed == 0 && used > 0 && static_cast<size_t>(used) < capacity - 8)
            std::snprintf(line + used, capacity - static_cast<size_t>(used), " none");
    }
}

void ds_stack_scan(const char* what, void* caller, const void* return_slot, unsigned index, HRESULT value, bool poll) {
    if (!ds_stack_enabled()) return;
    static std::atomic<unsigned> poll_scans{0};
    static std::atomic<unsigned> enqueue_scans{0};
    std::atomic<unsigned>& counter = poll ? poll_scans : enqueue_scans;
    const unsigned count = counter.fetch_add(1, std::memory_order_relaxed);
    if (count >= (poll ? 8u : 4u)) return;
    if (!ensure_title_base(caller)) return;
    const std::uint64_t base = g_title_image_base.load(std::memory_order_relaxed);
    char line[2400];
    std::snprintf(line, sizeof line, "[rt] ds stack walk #%u %s (%s index %u value 0x%08lx) tid %lu title frames:",
                  count, poll ? "poll" : "enqueue", what, index, static_cast<unsigned long>(value),
                  static_cast<unsigned long>(GetCurrentThreadId()));
    ds_stack_scan_walk(return_slot, line, sizeof line, base);
    std::lock_guard lock(log_mutex());
    std::fprintf(stderr, "%s\n", line);
}

// ---- Console-faithful completion ordering (diagnostic and opt-in fix; PALMETTO_DS_ORDER=<microseconds>) -------------
//
// docs/streamed_consumer.md section 6c. On the console a batch's completion is DMA/GPU-latency bound, so the title's
// load spinner always observes the job while it is still armed. Here the CPU worker can finish a submit before the
// spinner's first poll, so a completion can be observable "too early". With PALMETTO_DS_ORDER set (off by default: a
// cached zero and no extra work), a status entry stays observably E_PENDING until both (a) N microseconds have passed
// since its EnqueueStatus call, and (b) the spinner has polled the array at least once (bounded to 100 ms, so a title
// that never polls cannot hang the worker). The ordering trace prints when a grace is set or the load window is on.

std::chrono::steady_clock::time_point ds_order_origin() {
    static const auto origin = std::chrono::steady_clock::now();
    return origin;
}

std::uint64_t ds_order_now_us() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - ds_order_origin())
                                          .count());
}

std::uint64_t ds_order_grace_us() {
    static const std::uint64_t grace = [] {
        char value[32] = {};
        if (GetEnvironmentVariableA("PALMETTO_DS_ORDER", value, sizeof value) == 0 || value[0] == '\0') return 0ull;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 0);
        return end != value ? static_cast<std::uint64_t>(parsed) : 0ull;
    }();
    return grace;
}

bool ds_order_trace() { return ds_order_grace_us() > 0 || ds_load_visible(); }

std::atomic<std::uint64_t> g_ds_order_submits{0};
std::atomic<std::uint64_t> g_ds_order_last_submit_us{0};
std::atomic<std::uint64_t> g_ds_order_lines{0};
std::atomic<std::uint64_t> g_ds_order_completions{0};
std::atomic<std::uint64_t> g_ds_order_late_polls{0};

// The loader object's armed byte and in-flight count, as the title's spinner would see them right now. Empty until the
// title image base and the loader global are readable.
void ds_order_loader_text(char* buffer, size_t capacity) {
    if (!buffer || capacity < 16) return;
    buffer[0] = '\0';
    const std::uint64_t base = g_title_image_base.load(std::memory_order_relaxed);
    if (base == 0) return;
    std::uint64_t object = 0;
    if (palmetto_mem_read(base + 0x79F6D70ull, &object, sizeof object) != sizeof object || object == 0) return;
    std::uint64_t fields[16] = {};
    if (palmetto_mem_read(object, fields, sizeof fields) != sizeof fields) return;
    const unsigned armed = static_cast<unsigned>(fields[15] & 0xffu);                      // byte at +0x78
    const unsigned inflight = static_cast<unsigned>((fields[12] >> 32) & 0xffffffffu);    // dword at +0x64
    const unsigned capacity60 = static_cast<unsigned>(fields[12] & 0xffffffffu);          // dword at +0x60
    std::snprintf(buffer, capacity, "armed=%u inflight=%u capacity=%u object=0x%llx", armed, inflight, capacity60,
                  static_cast<unsigned long long>(object));
}

void ds_order_note_submit(unsigned entries, unsigned status_entries, void* caller) {
    if (!ds_order_trace()) return;
    const std::uint64_t submit = g_ds_order_submits.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint64_t at = ds_order_now_us();
    g_ds_order_last_submit_us.store(at, std::memory_order_relaxed);
    if (submit > 3 && submit % 64 != 0) return;
    if (g_ds_order_lines.fetch_add(1, std::memory_order_relaxed) >= 96) return;
    char loader[160];
    ds_order_loader_text(loader, sizeof loader);
    std::lock_guard lock(log_mutex());
    std::fprintf(stderr, "[rt] ds order +%.3fs submit #%llu: %u entr(ies) %u status, loader %s caller %s\n",
                 static_cast<double>(at) / 1.0e6, static_cast<unsigned long long>(submit), entries, status_entries,
                 loader[0] ? loader : "(loader not set)",
                 rt::shims::module_offset(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller))).c_str());
}

void ds_order_note_poll(const char* what, std::uint64_t since_us, HRESULT hr, std::uint64_t polls, void* caller) {
    if (g_ds_order_lines.fetch_add(1, std::memory_order_relaxed) >= 160) return;
    char loader[160];
    ds_order_loader_text(loader, sizeof loader);
    std::lock_guard lock(log_mutex());
    std::fprintf(stderr, "[rt] ds order +%.3fs %s: dt %llu us, value %s, polls %llu, loader %s caller %s\n",
                 static_cast<double>(ds_order_now_us()) / 1.0e6, what, static_cast<unsigned long long>(since_us),
                 hr == E_PENDING ? "E_PENDING" : hr_text(hr).c_str(), static_cast<unsigned long long>(polls),
                 loader[0] ? loader : "(loader not set)",
                 rt::shims::module_offset(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller))).c_str());
}

// The heartbeat thread: one line per second for the first 20 s, cumulative counters and the last status poll. It starts
// on the first status array or queue the title creates; a boot without the switch starts no thread.
void ds_load_heartbeat_start() {
    static std::atomic<bool> started{false};
    if (!ds_load_visible() || started.exchange(true, std::memory_order_relaxed)) return;
    static const auto zero = ds_load_seconds();
    std::thread([] {
        for (unsigned second = 1; second <= 20; ++second) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            const double at = ds_load_seconds();
            std::uint64_t poll_index = g_ds_load.last_poll_index.load(std::memory_order_relaxed);
            std::uint64_t poll_caller = g_ds_load.last_poll_caller.load(std::memory_order_relaxed);
            std::string caller =
                poll_caller ? rt::shims::module_offset(poll_caller) : std::string("(none, never polled)");
            std::lock_guard lock(log_mutex());
            std::fprintf(stderr,
                         "[rt] ds load heartbeat +%.1fs: requests enqueued %llu, done %llu, submits %llu, status polls "
                         "%llu (pending %llu; last index %s value 0x%llx caller %s), queries %llu, stats %llu, max "
                         "queue depth %llu\n",
                         at, static_cast<unsigned long long>(g_ds_load.requests_enqueued.load()),
                         static_cast<unsigned long long>(g_ds_load.requests_done.load()),
                         static_cast<unsigned long long>(g_ds_load.submits.load()),
                         static_cast<unsigned long long>(g_ds_load.status_polls.load()),
                         static_cast<unsigned long long>(g_ds_load.pending_polls.load()),
                         poll_index == ~std::uint32_t(0) ? "none" : std::to_string(poll_index).c_str(),
                         static_cast<unsigned long long>(g_ds_load.last_poll_value.load()), caller.c_str(),
                         static_cast<unsigned long long>(g_ds_load.queries.load()),
                         static_cast<unsigned long long>(g_ds_load.stat_requests.load()),
                         static_cast<unsigned long long>(g_ds_load.max_queue_depth.load()));
        }
        (void)zero;
    }).detach();
}

// ---- Loading-record trace (measurement only; PALMETTO_DS_RECORD=1) -------------------------------------------------
//
// docs/decomp_stage.md section 5.2 and its ranked hypothesis 1: the session-3 poll FUN_1421b7070 (RVA 0x21B7070) loops
// every 33 ms on FUN_14041e8c0(image+0x78AC9C8, [image+0x78ACBA0]) and requests session 4 only when that predicate
// returns true. The predicate stores its poll tick into entry [0x78ACBA0]*0x50 and returns entry [own]*0x50 + 0x18 != 0,
// where `own` is the int at the pointer stored at record+0x190. The record is the title's loading/streaming completion
// table, filled by the title's streaming systems; the shim question is whether the read/completion path we answer leaves
// that slot empty. This trace starts on the first queue call (the loader drains DirectStorage from the boot's first
// seconds), then samples the record once per interval and prints one compact line per sample: the poll index, the five
// 0x50-byte slots' poll tick / completion / state dwords, the record header fields the reset and list code use (+0x1A8,
// +0x1C0, +0x1C8, +0x1D0, +0x1D4), and the three state words with the stage machine's current/next, so the record's
// timeline reads against PALMETTO_STATE_TRACE. A separate line marks the first time a slot's completion dword becomes
// non-zero. Off by default (one cached boolean, no thread), bounded (480 lines, at most 600 s), reads only committed
// pages through palmetto_mem_read, values and offsets only.
//
// PALMETTO_DS_RECORD=1 uses 500 ms samples for 300 s; <ms> overrides the interval; <ms>,<seconds> both; 0/unset is off.
// The grammar and its clamp live in dstorage_advanced.h (parse_record_trace_spec), where the test checks them.

const palmetto::dstorage::RecordTraceSpec& ds_record_trace_spec() {
    static const palmetto::dstorage::RecordTraceSpec spec = [] {
        char value[64] = {};
        size_t length = 0;
        if (getenv_s(&length, value, "PALMETTO_DS_RECORD") != 0 || length <= 1)
            return palmetto::dstorage::RecordTraceSpec{};
        return palmetto::dstorage::parse_record_trace_spec(value);
    }();
    return spec;
}

// One guarded read of title memory; false when the range is not committed/readable. palmetto_mem_read is the runtime's
// own guarded read (d3d12_xs_device.cpp), safe from this detached thread.
bool ds_record_read(std::uint64_t address, void* out, unsigned size) {
    return palmetto_mem_read(address, out, size) == size;
}

// Starts the trace on the first queue call. The title image base comes from that call's return address (the queue API's
// caller is title code); no base, no thread. Off and unstarted without the switch.
void ds_record_trace_start(void* caller) {
    const palmetto::dstorage::RecordTraceSpec& spec = ds_record_trace_spec();
    if (!spec.on) return;
    static std::atomic<bool> started{false};
    if (started.load(std::memory_order_relaxed)) return;
    if (g_title_image_base.load(std::memory_order_relaxed) == 0 && !ensure_title_base(caller)) return;
    if (started.exchange(true, std::memory_order_relaxed)) return;
    const std::uint64_t base = g_title_image_base.load(std::memory_order_relaxed);
    std::thread([base, spec] {
        constexpr std::uint32_t kRecordRva = 0x78AC9C8;       // the loading/streaming completion table
        constexpr std::uint32_t kPollIndexRva = 0x78ACBA0;    // the slot 0x21B7070 passes to the predicate
        constexpr std::uint32_t kInitStateRva = 0x7446510;    // the stage word
        constexpr std::uint32_t kLoadingStateRva = 0x7453ED0; // the loading word
        constexpr std::uint32_t kSessionStateRva = 0x98F828C; // the session word
        constexpr std::uint32_t kMachineRva = 0x71273C8;      // [machine] -> current u16 +0x10, requested u16 +0x12
        const std::uint64_t record = base + kRecordRva;
        const unsigned deadline_ms = static_cast<unsigned>(spec.seconds * 1000.0);
        unsigned lines = 0;
        std::uint32_t previous_completion[5] = {};
        bool have_completion[5] = {};
        for (unsigned waited = 0; waited <= deadline_ms && lines < 480; waited += spec.interval_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(spec.interval_ms));
            std::uint32_t poll_index = 0;
            const bool ok_index = ds_record_read(base + kPollIndexRva, &poll_index, 4);
            std::uint64_t owner = 0;
            const bool ok_owner = ds_record_read(record + 0x190, &owner, 8) && owner != 0;
            std::uint32_t own = 0;
            if (ok_owner) ds_record_read(owner, &own, 4);
            std::uint32_t slot_tick[5] = {}, slot_completion[5] = {}, slot_state[5] = {};
            bool slots_ok = true;
            for (unsigned slot = 0; slot < 5; ++slot) {
                const std::uint64_t slot_base = record + slot * 0x50ull;
                slots_ok = ds_record_read(slot_base, &slot_tick[slot], 4) &&
                           ds_record_read(slot_base + 0x18, &slot_completion[slot], 4) &&
                           ds_record_read(slot_base + 0x20, &slot_state[slot], 4) && slots_ok;
            }
            std::uint64_t header_a8 = 0, header_c0 = 0, header_c8 = 0, header_d0 = 0;
            std::uint32_t header_d4 = 0;
            ds_record_read(record + 0x1a8, &header_a8, 8);
            ds_record_read(record + 0x1c0, &header_c0, 8);
            ds_record_read(record + 0x1c8, &header_c8, 8);
            ds_record_read(record + 0x1d0, &header_d0, 8);
            ds_record_read(record + 0x1d4, &header_d4, 4);
            std::uint32_t init_state = 0, loading = 0, session = 0;
            ds_record_read(base + kInitStateRva, &init_state, 4);
            ds_record_read(base + kLoadingStateRva, &loading, 4);
            ds_record_read(base + kSessionStateRva, &session, 4);
            std::uint64_t machine = 0;
            ds_record_read(base + kMachineRva, &machine, 8);
            std::uint16_t current = 0, next = 0;
            if (machine && (machine & 7) == 0) {
                ds_record_read(machine + 0x10, &current, 2);
                ds_record_read(machine + 0x12, &next, 2);
            }
            // A slot completion becoming non-zero is the event this trace exists for: the session-3 predicate
            // FUN_14041e8c0 reads entry [own]+0x18 != 0, so a non-zero dword here is the exact value that would move the
            // session word off 3. The transition is also marked on its own line.
            for (unsigned slot = 0; slot < 5; ++slot) {
                if ((!have_completion[slot] || previous_completion[slot] == 0) && slot_completion[slot] != 0) {
                    std::lock_guard lock(log_mutex());
                    std::fprintf(stderr,
                                 "[rt] ds record +%.2fs: SLOT %u COMPLETION dword 0x%x (own %s%u) - the session-3 "
                                 "predicate 0x41E8C0 now reads true for this slot\n",
                                 ds_load_seconds(), slot, slot_completion[slot], ok_owner ? "" : "?", own);
                    ++lines;
                }
                have_completion[slot] = true;
                previous_completion[slot] = slot_completion[slot];
            }
            char index_text[16], own_text[16];
            if (ok_index) std::snprintf(index_text, sizeof index_text, "%u", poll_index);
            else std::snprintf(index_text, sizeof index_text, "?");
            if (ok_owner) std::snprintf(own_text, sizeof own_text, "%u", own);
            else std::snprintf(own_text, sizeof own_text, "?");
            std::lock_guard lock(log_mutex());
            std::fprintf(stderr,
                         "[rt] ds record +%.2fs: index %s own %s%s; slots 0..4 tick/comp/state: 0:%u/%u/%u "
                         "1:%u/%u/%u 2:%u/%u/%u 3:%u/%u/%u 4:%u/%u/%u; hdr 1a8=0x%llx 1c0=0x%llx 1c8=0x%llx "
                         "1d0=0x%llx 1d4=0x%x; init %u loading %u session %u machine %u/%u\n",
                         ds_load_seconds(), index_text, own_text, slots_ok ? "" : "?", slot_tick[0],
                         slot_completion[0], slot_state[0], slot_tick[1], slot_completion[1], slot_state[1],
                         slot_tick[2], slot_completion[2], slot_state[2], slot_tick[3], slot_completion[3],
                         slot_state[3], slot_tick[4], slot_completion[4], slot_state[4],
                         static_cast<unsigned long long>(header_a8), static_cast<unsigned long long>(header_c0),
                         static_cast<unsigned long long>(header_c8), static_cast<unsigned long long>(header_d0),
                         header_d4, init_state, loading, session, static_cast<unsigned>(current),
                         static_cast<unsigned>(next));
            ++lines;
        }
    }).detach();
}

// ---- Streamed-block consumer diagnostics (measurement only; PALMETTO_STREAM_CONSUMER=1) ---------------------------
//
// docs/streamed_consumer.md. The title streams ~256 MiB as 64 KiB decompressed blocks into ordinary process-heap
// addresses, and over a 300 s boot no device entry point names those ranges, so the consumer is either title CPU code
// or nothing. With PALMETTO_STREAM_CONSUMER=1 this file (the queue worker owns all of it):
//   * reports every completed destination to the device proxy (d3d12_xs_device.cpp, same executable): any device call
//     whose argument points into one of those ranges is printed there - that is the GPU-side naming, if it happens;
//   * prints the first bytes' content class for the first blocks (a label only, never a byte value);
//   * puts PAGE_GUARD on the pages wholly inside the tracked destinations (the first sixteen requests, then one
//     destination every 256th request, up to 32): the first access after the worker's write faults into a small vectored
//     handler that prints the accessing code (title RVA, host-runtime RVA or neither) and the title frames on its stack,
//     then clears the guard. A host-runtime first touch is a shim read - the XG texture-computer copy is the one this
//     measurement is looking for. The handler does not change the access, allocate or read the block, so the boot runs as
//     without the switch. Everything is bounded (72 tracked destinations, 96 guard slots, 128 fault lines, fixed lines)
//     and off by default; with the switch unset no guard, handler or range is created and every function here returns on
//     one cached boolean.

bool stream_consumer_enabled() {
    static const bool on = [] {
        char value[8] = {};
        size_t length = 0;
        return getenv_s(&length, value, "PALMETTO_STREAM_CONSUMER") == 0 && length > 1 && value[0] != '0' &&
               value[0] != 'n';
    }();
    return on;
}

// (g_title_image_base is declared above, before the load-window stack scan, which also publishes it.)

// The runtime's own image (palmetto.exe) base, captured outside the handler when the first guard is armed: a first touch
// from host code is a shim read (the XG texture-computer copy path is the one this measurement is looking for), and it
// has to be told apart from the title's own code without a module lookup.
std::atomic<std::uint64_t> g_host_image_base{0};

// "game+0x..." for an address in the title image (a generous 192 MiB window over a 176 MiB image), else false.
bool title_rva(std::uintptr_t address, std::uintptr_t& rva) {
    const std::uint64_t base = g_title_image_base.load(std::memory_order_relaxed);
    if (!base || address < base || address - base >= 0x0C000000ull) return false;
    rva = static_cast<std::uintptr_t>(address - base);
    return true;
}

// "palmetto.exe+0x..." for an address in the runtime's image (a 64 MiB window), else false.
bool host_rva(std::uintptr_t address, std::uintptr_t& rva) {
    const std::uint64_t base = g_host_image_base.load(std::memory_order_relaxed);
    if (!base || address < base || address - base >= 0x04000000ull) return false;
    rva = static_cast<std::uintptr_t>(address - base);
    return true;
}

// The first bytes' content class as a label. A match names a standard container tag (a format identification, not its
// contents); otherwise the label is a byte statistic. No byte value is ever printed.
const char* classify_block(const void* data, size_t bytes) {
    const auto* b = static_cast<const uint8_t*>(data);
    if (!b || bytes == 0) return "empty";
    const size_t n = bytes < 256 ? bytes : 256;
    static constexpr struct {
        char magic[4];
        const char* label;
    } kMagics[] = {
        {{'R', 'S', 'C', '8'}, "RSC8 console resource container"},
        {{'R', 'S', 'C', '7'}, "RSC7 console resource container"},
        {{'A', 'D', 'A', 'T'}, "ADAT audio bank"},
        {{'R', 'I', 'F', 'F'}, "RIFF container"},
        {{'W', 'A', 'V', 'E'}, "WAVE audio"},
        {{'D', 'D', 'S', ' '}, "DDS texture"},
        {{'R', 'B', 'F', '0'}, "RBF0 animation blob"},
    };
    if (n >= 4)
        for (const auto& m : kMagics)
            if (std::memcmp(b, m.magic, 4) == 0) return m.label;
    if (n >= 2 && b[0] == 0x78 && (b[1] == 0x9c || b[1] == 0x01 || b[1] == 0xda))
        return "still-compressed zlib stream";
    size_t nonzero = 0, printable = 0;
    size_t distinct = 0;
    bool seen[256] = {};
    for (size_t i = 0; i < n; ++i) {
        if (b[i]) ++nonzero;
        if (b[i] >= 0x20 && b[i] < 0x7f) ++printable;
        if (!seen[b[i]]) {
            seen[b[i]] = true;
            ++distinct;
        }
    }
    if (nonzero == 0) return "all-zero";
    if (printable * 20 >= n * 19) return "mostly printable text";
    if (n >= 64 && std::memcmp(b, b + 16, 16) == 0) return "16-byte repeating pattern";
    if (distinct <= 8) return "low-entropy structured";
    if (distinct >= 160) return "high-entropy (compressed, packed or random)";
    return "structured binary";
}

// One tracked destination: a sample of the stream's 64 KiB blocks. The queue worker is the only writer of this table.
struct TrackedDestination {
    std::uintptr_t base = 0;
    size_t bytes = 0;
    unsigned arms = 0;
    bool written_again = false;
};
// The load-stall increment (docs/streamed_consumer.md): the first 64 destinations are guarded so the exact block at
// which the consumer stops is measured, not bracketed; eight later destinations (every 512th request) keep the
// whole-stream sample.
constexpr unsigned kTrackedDestinations = 160;

// A destination's interior pages under PAGE_GUARD. The fault handler runs on whichever thread touched them, so these
// fields are atomic; the handler clears `armed` before it resumes the access.
struct GuardedBlock {
    std::atomic<std::uintptr_t> page_base{0};
    std::atomic<size_t> page_bytes{0};
    std::atomic<std::uintptr_t> block_base{0};
    std::atomic<unsigned> tracked_index{0};
    std::atomic<unsigned> faults{0};
    std::atomic<bool> armed{false};
};
constexpr unsigned kGuardSlots = 192;
GuardedBlock g_guards[kGuardSlots];
std::atomic<unsigned> g_fault_lines{0};

// The title frames on the faulting thread's stack, as game+RVAs. A shallow scan like the wait sampler's: the top stack
// words are readable by construction, and the loop is bounded and guarded so a bad stack word cannot fault the handler.
void print_title_stack(std::uintptr_t stack_pointer) {
    char line[768];
    int used = std::snprintf(line, sizeof line, "[rt] dstorage guard: stack title frames:");
    unsigned printed = 0;
    if (used < 0) return;
    __try {
        for (unsigned i = 0; i < 48 && printed < 8; ++i) {
            const std::uintptr_t word =
                *reinterpret_cast<const std::uintptr_t*>(stack_pointer + i * sizeof(std::uintptr_t));
            std::uintptr_t rva = 0;
            if (!title_rva(word, rva)) continue;
            const int n = std::snprintf(line + used, sizeof line - static_cast<size_t>(used), " game+0x%llx",
                                        static_cast<unsigned long long>(rva));
            if (n < 0) break;
            used += n;
            if (static_cast<size_t>(used) >= sizeof line - 32) break;
            ++printed;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!printed) std::snprintf(line + used, sizeof line - static_cast<size_t>(used), " none");
    std::fprintf(stderr, "%s\n", line);
}

// The one-shot guard handler: name the first access to a guarded streamed block, clear the guard and let the access
// retry. Any other exception (and any guard page that is not ours) goes on to the next handler untouched.
LONG CALLBACK stream_guard_handler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (record->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || record->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const unsigned access = static_cast<unsigned>(record->ExceptionInformation[0]);
    const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
    for (unsigned i = 0; i < kGuardSlots; ++i) {
        GuardedBlock& guard = g_guards[i];
        const std::uintptr_t page = guard.page_base.load(std::memory_order_relaxed);
        const size_t width = guard.page_bytes.load(std::memory_order_relaxed);
        if (width == 0 || address < page || address - page >= width) continue;
        // A second thread can fault on the same page after the first one's handler already cleared the guard: the page
        // range still matches, the protection is cleared again (idempotent) and the access retries, so a concurrent
        // first touch cannot end the process with an unhandled guard violation.
        const bool first = guard.armed.load(std::memory_order_acquire);
        const std::uintptr_t rip = info->ContextRecord ? static_cast<std::uintptr_t>(info->ContextRecord->Rip) : 0;
        const std::uintptr_t rsp = info->ContextRecord ? static_cast<std::uintptr_t>(info->ContextRecord->Rsp) : 0;
        const std::uintptr_t block = guard.block_base.load(std::memory_order_relaxed);
        const unsigned tracked = guard.tracked_index.load(std::memory_order_relaxed);
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(page), width, PAGE_READWRITE, &old);
        guard.armed.store(false, std::memory_order_release);
        if (!first) return EXCEPTION_CONTINUE_EXECUTION;
        guard.faults.fetch_add(1, std::memory_order_relaxed);
        palmetto_load_tid_note(static_cast<unsigned long>(GetCurrentThreadId()));  // the consumer thread
        if (g_fault_lines.fetch_add(1, std::memory_order_relaxed) < 128) {
            char where[80];
            std::uintptr_t rva = 0;
            if (title_rva(rip, rva))
                std::snprintf(where, sizeof where, "game+0x%llx", static_cast<unsigned long long>(rva));
            else if (host_rva(rip, rva))
                std::snprintf(where, sizeof where, "palmetto.exe+0x%llx (a shim read, not the title's code)",
                              static_cast<unsigned long long>(rva));
            else
                std::snprintf(where, sizeof where, "0x%llx (neither image)", static_cast<unsigned long long>(rip));
            std::fprintf(stderr, "[rt] dstorage guard: streamed block #%u first %s at its +0x%llx by %s (tid %lu)\n",
                         tracked, access == 1 ? "write" : access == 8 ? "execute" : "read",
                         static_cast<unsigned long long>(address > block ? address - block : 0), where,
                         static_cast<unsigned long>(GetCurrentThreadId()));
            print_title_stack(rsp);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// The tracked destinations (worker-thread only; the guard table's entry for a tracked destination is atomic because the
// handler touches it).
TrackedDestination g_tracked[kTrackedDestinations];
unsigned g_tracked_count = 0;
TrackedDestination& g_tracked_slot(unsigned index) { return g_tracked[index % kTrackedDestinations]; }

// Guard the pages wholly inside a tracked destination (the block's first and last partial pages stay readable, so the
// title heap's own metadata and neighbours cannot fault through this). Returns 1 when armed, 0 when the block has no
// whole page inside it, -1 when the protection failed (logged here).
int arm_stream_guard(unsigned tracked_index) {
    const TrackedDestination& tracked = g_tracked_slot(tracked_index);
    GuardedBlock& guard = g_guards[tracked_index % kGuardSlots];
    if (guard.armed.load(std::memory_order_acquire)) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(guard.page_base.load(std::memory_order_relaxed)),
                       guard.page_bytes.load(std::memory_order_relaxed), PAGE_READWRITE, &old);
        guard.armed.store(false, std::memory_order_release);
    }
    const std::uintptr_t first = (tracked.base + 0xFFFull) & ~static_cast<std::uintptr_t>(0xFFF);
    const std::uintptr_t end = (tracked.base + tracked.bytes) & ~static_cast<std::uintptr_t>(0xFFF);
    if (end <= first) return 0;
    guard.page_base.store(first, std::memory_order_relaxed);
    guard.page_bytes.store(static_cast<size_t>(end - first), std::memory_order_relaxed);
    guard.block_base.store(tracked.base, std::memory_order_relaxed);
    guard.tracked_index.store(tracked_index, std::memory_order_relaxed);
    guard.armed.store(true, std::memory_order_release);
    static std::atomic<bool> handler_installed{false};
    if (!handler_installed.exchange(true, std::memory_order_relaxed)) {
        // Outside the handler, where a module lookup is safe: the runtime's own image base, so a fault from shim code can
        // be labelled (the XG texture-computer copy is the shim read this measurement looks for).
        g_host_image_base.store(reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)), std::memory_order_relaxed);
        AddVectoredExceptionHandler(1, stream_guard_handler);
    }
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(first), static_cast<size_t>(end - first), PAGE_READWRITE | PAGE_GUARD,
                        &old)) {
        const DWORD error = GetLastError();
        guard.armed.store(false, std::memory_order_release);
        std::fprintf(stderr,
                     "[rt] dstorage guard: PAGE_GUARD on 0x%llx+0x%llx failed (error %lu); the block is not tracked "
                     "for a first touch\n",
                     static_cast<unsigned long long>(first), static_cast<unsigned long long>(end - first), error);
        return -1;
    }
    return 1;
}

// ---- The tracked destinations, the range report and the periodic summary -------------------------------------------

size_t g_distinct_destinations = 0;  // worker only, capped
std::unordered_set<std::uintptr_t> g_distinct_seen;  // worker only

// Called from the queue worker after a request wrote its destination. Reports the range to the device proxy, classifies
// the first blocks, tracks the first few distinct destinations and re-arms their guard after each write.
void stream_note_completed(void* destination, size_t bytes, void* caller) {
    if (!stream_consumer_enabled() || !destination || bytes == 0) return;
    const auto base = reinterpret_cast<std::uintptr_t>(destination);
    if (g_title_image_base.load(std::memory_order_relaxed) == 0 && caller) {
        std::uint64_t image = 0;
        if (rt::shims::mapped_image_base(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller)), image)) {
            g_title_image_base.store(image, std::memory_order_relaxed);
            palmetto_stream_set_title_base(image);
        }
    }
    palmetto_stream_block_completed(destination, bytes);
    static std::uint64_t requests = 0;  // worker only
    static std::uint64_t total_bytes = 0;
    static unsigned first_logged = 0;
    ++requests;
    total_bytes += bytes;
    if (g_distinct_seen.size() < 65536 && g_distinct_seen.insert(base).second) ++g_distinct_destinations;
    if (first_logged < 8) {
        ++first_logged;
        std::fprintf(stderr, "[rt] dstorage guard: block #%u range 0x%llx+0x%llx content class: %s\n", first_logged - 1,
                     static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes),
                     classify_block(destination, bytes));
    }
    TrackedDestination* tracked = nullptr;
    for (unsigned i = 0; i < g_tracked_count; ++i)
        if (g_tracked[i].base == base) {
            tracked = &g_tracked[i];
            break;
        }
    // The loader-gate increment: the first 64 requests are tracked one by one (the consumer's stop block), the block at
    // request ~4,074 (the one the frame loop keeps a pointer into, docs/streamed_consumer.md section 6) in the window
    // 4,060-4,096, and every 512th request samples the whole stream.
    if (!tracked && g_tracked_count < kTrackedDestinations &&
        (requests <= 64 || (requests >= 4060 && requests <= 4096) || requests % 512 == 0)) {
        tracked = &g_tracked[g_tracked_count];
        const unsigned index = g_tracked_count++;
        tracked->base = base;
        tracked->bytes = bytes;
        tracked->arms = arm_stream_guard(index) > 0 ? 1u : 0u;
        if (tracked->arms == 0)
            std::fprintf(stderr,
                         "[rt] dstorage guard: block #%u (0x%llx+0x%llx) has no whole page inside it (or failed to "
                         "guard; see above); not tracked for a first touch\n",
                         index, static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes));
    } else if (tracked) {
        const unsigned index = static_cast<unsigned>(tracked - g_tracked);
        if (!tracked->written_again) {
            tracked->written_again = true;
            std::fprintf(stderr,
                         "[rt] dstorage guard: destination 0x%llx is written again by the queue (its guard fired %u "
                         "time(s) before this write)\n",
                         static_cast<unsigned long long>(base), g_guards[index % kGuardSlots].faults.load());
        }
        if (tracked->arms < 8 && arm_stream_guard(index) > 0) ++tracked->arms;
    }
    if (requests % 1024 == 0) {
        unsigned faults = 0, armed = 0;
        for (unsigned i = 0; i < kGuardSlots; ++i) {
            faults += g_guards[i].faults.load(std::memory_order_relaxed);
            if (g_guards[i].armed.load(std::memory_order_relaxed)) ++armed;
        }
        std::fprintf(stderr,
                     "[rt] dstorage guard: %llu request(s), %.1f MiB, %zu distinct destination(s), %u tracked, %u "
                     "first-touch fault(s), %u still guarded\n",
                     static_cast<unsigned long long>(requests), double(total_bytes) / double(1 << 20),
                     g_distinct_destinations, g_tracked_count, faults, armed);
        // One line per 12 tracked blocks keeps the log line bounded with 32 tracked destinations.
        for (unsigned start = 0; start < g_tracked_count; start += 12) {
            char tracked_line[768];
            int at = std::snprintf(tracked_line, sizeof tracked_line, "[rt] dstorage guard: tracked blocks %u-%u:", start,
                                   start + 11 < g_tracked_count ? start + 11 : g_tracked_count - 1);
            for (unsigned i = start; i < g_tracked_count && i < start + 12 && at > 0 &&
                                  static_cast<size_t>(at) < sizeof tracked_line - 64;
                 ++i) {
                const GuardedBlock& guard = g_guards[i % kGuardSlots];
                at += std::snprintf(tracked_line + at, sizeof tracked_line - static_cast<size_t>(at),
                                    " #%u(0x%llx: %u fault(s), %s)", i,
                                    static_cast<unsigned long long>(g_tracked[i].base),
                                    guard.faults.load(std::memory_order_relaxed),
                                    guard.armed.load(std::memory_order_relaxed) ? "guarded" : "seen");
            }
            std::fprintf(stderr, "%s\n", tracked_line);
        }
    }
}

// ---- Interface ids ------------------------------------------------------------------------------------------------
//
// The X interfaces' ids are not public. A creating call (DStorageGetFactory, CreateQueue*, OpenFile,
// CreateStatusArray) accepts an id it has not seen for the kind of object it hands out and logs it the first time, so
// the first boot records the console's ids. It refuses an id seen for another kind, a PC DirectStorage id of another
// shape, and the system COM interfaces (IMarshal, IAgileObject, ...), which a DirectStorage object never implements.
// QueryInterface is stricter (docs/d3d12_vulkan.md: an unknown id is E_NOINTERFACE): it accepts IUnknown, the
// declared ids (dstorage_xs_abi.h) and the ids a creating call accepted for that kind, and logs anything else. This is
// a policy, not a fact; it lives here so a measured id list can replace it.

enum class Kind { Factory, Queue, File, StatusArray };

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Factory: return "factory";
        case Kind::Queue: return "queue";
        case Kind::File: return "file";
        case Kind::StatusArray: return "status array";
    }
    return "?";
}

// System interfaces: the COM base range {xxxxxxxx-0000-0000-C000-000000000046} (IClassFactory, IMarshal,
// IStdMarshalInfo, IExternalConnection, IMultiQI, IClientSecurity, IServerSecurity, IRpcOptions, IMarshal2,
// IWeakReferenceSource, IDispatch, IPersist, ...) and these others.
constexpr GUID kSystemInterfaceIds[] = {
    {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}},  // IAgileObject
    {0xecc8691b, 0xc1db, 0x4dc0, {0x85, 0x5e, 0x65, 0xf6, 0xc5, 0x51, 0xaf, 0x49}},  // INoMarshal
    {0x1c733a30, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}},  // ICallFactory
    {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}},  // IInspectable
    {0xdf0b3d60, 0x548f, 0x101b, {0x8e, 0x65, 0x08, 0x00, 0x2b, 0x2b, 0xd1, 0x19}},  // ISupportErrorInfo
    {0xb196b283, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}},  // IProvideClassInfo
    {0xb196b284, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}},  // IConnectionPointContainer
    {0xfc4801a3, 0x2ba9, 0x11cf, {0xa2, 0x29, 0x00, 0xaa, 0x00, 0x3d, 0x73, 0x52}},  // IObjectWithSite
};

bool system_interface(const GUID& g) {
    static const uint8_t kComBase[8] = {0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    if (g.Data2 == 0 && g.Data3 == 0 && std::memcmp(g.Data4, kComBase, 8) == 0) return true;
    for (const GUID& s : kSystemInterfaceIds)
        if (same_guid(g, s)) return true;
    return false;
}

// The ids declared for a kind: the PC IDStorageStatusArray id, whose interface has the console one's shape (console
// D3D12 binaries reuse the PC SDK's ids). No other X interface id is known.
bool declared(const GUID& g, Kind kind) {
    for (const GUID& id : kStatusArrayIds)
        if (same_guid(g, id)) return kind == Kind::StatusArray;
    return false;
}

bool declared_for_any(const GUID& g) {
    for (const GUID& id : kStatusArrayIds)
        if (same_guid(g, id)) return true;
    return false;
}

class IidPolicy {
public:
    bool accept_creating(REFIID riid, Kind kind, const char* caller) {
        if (same_guid(riid, __uuidof(IUnknown)) || declared(riid, kind)) return true;
        if (system_interface(riid)) return refuse(riid, caller, "a system COM interface");
        if (declared_for_any(riid)) return refuse(riid, caller, "an id declared for another kind of object");
        for (const GUID& pc : kPcInterfaceIds)
            if (same_guid(riid, pc))
                return refuse(riid, caller, "a PC DirectStorage interface; only the console ABI is served");
        std::lock_guard lock(mutex_);
        for (const auto& [id, k] : learned_)
            if (same_guid(id, riid)) {
                if (k == kind) return true;
                return refuse(riid, caller, std::string("seen before as a ") + kind_name(k) + " interface");
            }
        learned_.push_back({riid, kind});
        log_once("learn" + guid_text(riid), std::string(caller) + " asked for " + guid_text(riid) + ": serving the " +
                                                kind_name(kind) + " interface");
        return true;
    }

    bool accept_query(REFIID riid, Kind kind, const char* caller) {
        if (same_guid(riid, __uuidof(IUnknown)) || declared(riid, kind)) return true;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [id, k] : learned_)
                if (same_guid(id, riid) && k == kind) return true;
        }
        return refuse(riid, caller, std::string("not an id a ") + kind_name(kind) + " was created with");
    }

private:
    static bool refuse(REFIID riid, const char* caller, const std::string& why) {
        log_once(std::string("refuse") + caller + guid_text(riid),
                 std::string(caller) + " asked for " + guid_text(riid) + ": " + why + " (E_NOINTERFACE)");
        return false;
    }

    std::mutex mutex_;
    std::vector<std::pair<GUID, Kind>> learned_;
};

IidPolicy& iid_policy() {
    static auto* p = new IidPolicy;
    return *p;
}

// A bad argument to a factory or queue method, logged once per method and kind: a wrong vtable guess for the console
// interfaces would show up here first, as arguments that make no sense for the method.
HRESULT bad_call(const char* method, HRESULT hr, const char* what) {
    log_once(std::string("bad") + method + what,
             std::string(method) + ": " + what + " (" + hr_text(hr) +
                 "; logged once; a wrong vtable slot guess would show up like this)");
    return hr;
}

// ---- Statistics (IDStorageFactoryX1::GetStats) --------------------------------------------------------------------

struct Stats {
    std::mutex mutex;
    DSTORAGE_STATS s{};
    uint32_t live_queues = 0;  // all priorities; SetStagingBufferSize is refused while queues or files exist
    uint32_t live_files = 0;   // file objects not yet released, closed or not: the staging-size guard counts these
};

Stats& stats() {
    static auto* s = new Stats;
    return *s;
}

DSTORAGE_PRIORITY_STATS& priority_stats(DSTORAGE_STATS& s, DSTORAGE_PRIORITY p) {
    switch (p) {
        case DSTORAGE_PRIORITY_LOW: return s.LowPriorityStats;
        case DSTORAGE_PRIORITY_HIGH: return s.HighPriorityStats;
        case DSTORAGE_PRIORITY_REALTIME: return s.RealtimePriorityStats;
        default: return s.NormalPriorityStats;
    }
}

void count_error() {
    std::lock_guard lock(stats().mutex);
    ++stats().s.ErrorCount;
}

// ---- Live objects ---------------------------------------------------------------------------------------------------
//
// A request names its file and a status entry its array by pointer. Only objects this shim made and that are still
// alive are used; anything else is an error in the request, never a call through a stranger's vtable.

void ds_arrays_dump_start();  // PALMETTO_DS_ARRAYS: the status-array state dump (defined after the StatusArray class)
bool ds_arrays_enabled();

std::mutex& live_mutex() {
    static auto* m = new std::mutex;
    return *m;
}
// One set per kind, so a status array passed as a file (or the other way round) is not found.
std::unordered_set<const void*>& live_files() {
    static auto* s = new std::unordered_set<const void*>;
    return *s;
}
std::unordered_set<const void*>& live_status_arrays() {
    static auto* s = new std::unordered_set<const void*>;
    return *s;
}

// AddRef only if the count has not reached zero (an object being released is not revived).
bool add_ref_if_live(std::atomic<ULONG>& refs) {
    ULONG r = refs.load();
    while (r != 0)
        if (refs.compare_exchange_weak(r, r + 1)) return true;
    return false;
}

// ---- Status arrays ------------------------------------------------------------------------------------------------

std::atomic<std::uint64_t> g_ds_array_ids{0};

class StatusArray final : public IDStorageStatusArrayX {
public:
    StatusArray(uint32_t capacity, std::unique_ptr<std::atomic<HRESULT>[]> entries)
        : capacity_(capacity), entries_(std::move(entries)) {
        for (uint32_t i = 0; i < capacity_; ++i) entries_[i].store(S_OK, std::memory_order_relaxed);
        id_ = g_ds_array_ids.fetch_add(1, std::memory_order_relaxed) + 1;
        created_us_ = ds_order_now_us();
        {
            std::lock_guard lock(live_mutex());
            live_status_arrays().insert(static_cast<IDStorageStatusArrayX*>(this));
        }
        ds_arrays_dump_start();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::StatusArray, "IDStorageStatusArrayX::QueryInterface"))
            return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageStatusArrayX*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r;
        {
            std::lock_guard lock(live_mutex());
            r = --refs_;
            if (r == 0) live_status_arrays().erase(static_cast<IDStorageStatusArrayX*>(this));
        }
        if (r == 0) delete this;
        return r;
    }

    // IsComplete is GetHResult(index) != E_PENDING (the reference says so); an index past the array is not pending.
    bool STDMETHODCALLTYPE IsComplete(UINT32 index) override {
        // The title's polling loop is this call's caller. GetHResult's own recorded caller is this shim, so the title
        // address is stored after it (for the heartbeat), and the stack scan recovers the frames above the loop.
        void* const caller = _ReturnAddress();
        const void* const return_slot = _AddressOfReturnAddress();
        const std::uint64_t polls_before = polls_.fetch_add(1, std::memory_order_relaxed);
        const HRESULT hr = GetHResult(index);
        if (ds_load_visible())
            g_ds_load.last_poll_caller.store(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller)),
                                             std::memory_order_relaxed);
        if (ds_order_trace()) {
            // The first poll after any submit is the spinner's first observation of the batch: record how long after the
            // submit it happened and what the loader object looked like at that instant.
            const std::uint64_t submits = g_ds_order_submits.load(std::memory_order_relaxed);
            if (submits != 0 && seen_submits_.load(std::memory_order_relaxed) != submits) {
                seen_submits_.store(submits, std::memory_order_relaxed);
                const std::uint64_t submit_at = g_ds_order_last_submit_us.load(std::memory_order_relaxed);
                const std::uint64_t now = ds_order_now_us();
                ds_order_note_poll("first poll after a submit", now > submit_at ? now - submit_at : 0, hr, polls_before,
                                   caller);
            }
            if (ds_order_now_us() >= 5'000'000ull && g_ds_order_late_polls.fetch_add(1, std::memory_order_relaxed) < 40)
                ds_order_note_poll("poll after 5 s", 0, hr, polls_before, caller);
        }
        ds_stack_scan("poll", caller, return_slot, index, hr, true);
        return hr != E_PENDING;
    }
    std::uint64_t poll_count() const { return polls_.load(std::memory_order_relaxed); }
    HRESULT STDMETHODCALLTYPE GetHResult(UINT32 index) override {
        const HRESULT hr = index >= capacity_ ? E_DSTORAGE_INDEX_BOUND : entries_[index].load(std::memory_order_acquire);
        if (ds_arrays_enabled()) note_poll(hr);
        if (ds_load_visible()) {
            void* const caller = _ReturnAddress();
            const std::uint64_t polls = g_ds_load.status_polls.fetch_add(1, std::memory_order_relaxed) + 1;
            if (polls == 1) palmetto_load_tid_note(static_cast<unsigned long>(GetCurrentThreadId()));  // polling thread
            if (hr == E_PENDING) g_ds_load.pending_polls.fetch_add(1, std::memory_order_relaxed);
            g_ds_load.last_poll_index.store(index, std::memory_order_relaxed);
            g_ds_load.last_poll_value.store(static_cast<std::uint32_t>(hr), std::memory_order_relaxed);
            g_ds_load.last_poll_caller.store(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller)),
                                             std::memory_order_relaxed);
            if (polls <= 2000 || polls % 512 == 0) {
                char detail[128];
                std::snprintf(detail, sizeof detail, "status poll #%llu index %u value %s (capacity %u)",
                              static_cast<unsigned long long>(polls), index,
                              hr == E_PENDING ? "E_PENDING" : hr_text(hr).c_str(), capacity_);
                ds_load_note("poll", detail, caller);
            }
            if (ds_load_seconds() >= ds_load_window().to && (polls <= 2032 || polls % 8192 == 0)) {
                char detail[128];
                std::snprintf(detail, sizeof detail,
                              "status poll #%llu index %u value %s after the load window", 
                              static_cast<unsigned long long>(polls), index,
                              hr == E_PENDING ? "E_PENDING" : hr_text(hr).c_str());
                ds_load_note_late("late-poll", detail, caller);
            }
        }
        return hr;
    }

    uint32_t capacity() const { return capacity_; }
    void set_pending(uint32_t index) {
        entries_[index].store(E_PENDING, std::memory_order_release);
        pending_.fetch_add(1, std::memory_order_relaxed);
        last_pending_us_.store(ds_order_now_us(), std::memory_order_relaxed);
    }
    void complete(uint32_t index, HRESULT hr) {
        entries_[index].store(hr, std::memory_order_release);
        pending_.fetch_sub(1, std::memory_order_relaxed);
        completions_.fetch_add(1, std::memory_order_relaxed);
        last_complete_us_.store(ds_order_now_us(), std::memory_order_relaxed);
    }
    std::atomic<ULONG>& refs() { return refs_; }

    // ---- PALMETTO_DS_ARRAYS: the status-array state dump's view ---------------------------------------------------
    std::uint64_t id() const { return id_; }
    std::uint64_t created_us() const { return created_us_; }
    std::uint64_t pending_count() const { return pending_.load(std::memory_order_relaxed); }
    std::uint64_t enqueue_count() const { return enqueues_.load(std::memory_order_relaxed); }
    std::uint64_t completion_count() const { return completions_.load(std::memory_order_relaxed); }
    std::uint64_t last_pending_us() const { return last_pending_us_.load(std::memory_order_relaxed); }
    std::uint64_t last_complete_us() const { return last_complete_us_.load(std::memory_order_relaxed); }
    std::uint64_t last_enqueue_us() const { return last_enqueue_us_.load(std::memory_order_relaxed); }
    std::uint64_t last_enqueue_caller() const { return last_enqueue_caller_.load(std::memory_order_relaxed); }
    HRESULT last_polled_value() const { return last_hr_.load(std::memory_order_relaxed); }
    HRESULT entry_value(uint32_t index) const {
        return index < capacity_ ? entries_[index].load(std::memory_order_acquire) : E_DSTORAGE_INDEX_BOUND;
    }
    void note_enqueue(std::uint64_t at_us, void* caller) {
        enqueues_.fetch_add(1, std::memory_order_relaxed);
        last_enqueue_us_.store(at_us, std::memory_order_relaxed);
        last_enqueue_caller_.store(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller)),
                                   std::memory_order_relaxed);
    }
    void note_poll(HRESULT hr) {
        last_hr_.store(hr, std::memory_order_relaxed);
        note_poll_tid(static_cast<unsigned long>(GetCurrentThreadId()));
    }
    // The threads that polled this array (up to 4): two polling threads at once would mean two concurrent spinner
    // invocations sharing the loader object, which is the section 6d contradiction's likely mechanism.
    void note_poll_tid(unsigned long tid) {
        const unsigned long known = poll_tid_count_.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < known && i < 4; ++i)
            if (poll_tids_[i].load(std::memory_order_relaxed) == tid) return;
        const unsigned index = poll_tid_count_.fetch_add(1, std::memory_order_relaxed);
        if (index < 4) poll_tids_[index].store(tid, std::memory_order_relaxed);
    }
    unsigned poll_tid_count() const { return std::min(poll_tid_count_.load(std::memory_order_relaxed), 4u); }
    unsigned long poll_tid(unsigned index) const {
        return index < 4 ? poll_tids_[index].load(std::memory_order_relaxed) : 0;
    }

private:
    std::atomic<ULONG> refs_{1};
    const uint32_t capacity_;
    std::unique_ptr<std::atomic<HRESULT>[]> entries_;
    std::atomic<std::uint64_t> polls_{0};          // IsComplete observations (ordering trace)
    std::atomic<std::uint64_t> seen_submits_{0};   // last submit count the trace has logged a first poll for
    std::uint64_t id_ = 0;
    std::uint64_t created_us_ = 0;
    std::atomic<std::uint64_t> pending_{0};
    std::atomic<std::uint64_t> enqueues_{0};
    std::atomic<std::uint64_t> completions_{0};
    std::atomic<std::uint64_t> last_pending_us_{0};
    std::atomic<std::uint64_t> last_complete_us_{0};
    std::atomic<std::uint64_t> last_enqueue_us_{0};
    std::atomic<std::uint64_t> last_enqueue_caller_{0};
    std::atomic<HRESULT> last_hr_{S_OK};
    std::atomic<unsigned> poll_tid_count_{0};
    std::atomic<unsigned long> poll_tids_[4];
};

// ---- PALMETTO_DS_ARRAYS: the status-array state dump -------------------------------------------------------------
//
// docs/streamed_consumer.md section 6d. The loader spinner can only stall on a status array whose entry stays
// E_PENDING, so this prints every live status array's state - capacity, pending/completed/enqueue/poll counts, the last
// polled value and each entry's value - plus the loader object's [obj+0x30] owner, the owner's vtable and the exact
// function the completion path would call (owner vtable+0x18) and whether the owner is one of our arrays. Off by
// default; bounded to ~1,600 lines; no runtime change.

bool ds_arrays_enabled() {
    static const bool on = [] {
        char value[8] = {};
        size_t length = 0;
        return getenv_s(&length, value, "PALMETTO_DS_ARRAYS") == 0 && length > 1 && value[0] != '0' &&
               value[0] != 'n';
    }();
    return on;
}

std::atomic<std::uint64_t> g_ds_arrays_lines{0};

void ds_arrays_dump_once() {
    if (!ds_arrays_enabled()) return;
    if (g_ds_arrays_lines.fetch_add(1, std::memory_order_relaxed) >= 1600) return;
    const double at = static_cast<double>(ds_order_now_us()) / 1.0e6;
    std::lock_guard lock(log_mutex());
    std::lock_guard live(live_mutex());
    std::fprintf(stderr, "[rt] ds arrays +%.3fs arrays:\n", at);
    for (const void* pointer : live_status_arrays()) {
        const auto* array = static_cast<const StatusArray*>(reinterpret_cast<const IDStorageStatusArrayX*>(pointer));
        char entries[224];
        int used = 0;
        const unsigned shown = std::min(array->capacity(), 8u);
        for (unsigned i = 0; i < shown && used < static_cast<int>(sizeof entries) - 16; ++i)
            used += std::snprintf(entries + used, sizeof entries - static_cast<size_t>(used), " %08lx",
                                  static_cast<unsigned long>(array->entry_value(i)));
        char poll_tids[64] = {};
        int tid_used = 0;
        for (unsigned i = 0; i < array->poll_tid_count(); ++i)
            tid_used += std::snprintf(poll_tids + tid_used, sizeof poll_tids - static_cast<size_t>(tid_used), " %lu",
                                      array->poll_tid(i));
        std::fprintf(stderr,
                     "[rt] ds arrays +%.3fs array #%llu 0x%llx cap %u pending %llu completed %llu enqueues %llu "
                     "polls %llu last_poll %08lx created +%.3fs last_enqueue +%.3fs by %s last_complete +%.3fs "
                     "entries%s poll_tids %u:%s\n",
                     at, static_cast<unsigned long long>(array->id()),
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pointer)), array->capacity(),
                     static_cast<unsigned long long>(array->pending_count()),
                     static_cast<unsigned long long>(array->completion_count()),
                     static_cast<unsigned long long>(array->enqueue_count()),
                     static_cast<unsigned long long>(array->poll_count()),
                     static_cast<unsigned long>(array->last_polled_value()),
                     static_cast<double>(array->created_us()) / 1.0e6,
                     static_cast<double>(array->last_enqueue_us()) / 1.0e6,
                     array->last_enqueue_caller()
                         ? rt::shims::module_offset(array->last_enqueue_caller()).c_str()
                         : "(none)",
                     static_cast<double>(array->last_complete_us()) / 1.0e6, entries, array->poll_tid_count(),
                     poll_tids);
    }
    // The loader object and the completion path the spinner would call: owner = [obj+0x30], target = owner's
    // vtable+0x18, and whether the owner (or a word in its first 128 bytes) is one of our arrays.
    const std::uint64_t base = g_title_image_base.load(std::memory_order_relaxed);
    if (base == 0) return;
    std::uint64_t object = 0;
    if (palmetto_mem_read(base + 0x79F6D70ull, &object, sizeof object) != sizeof object || object == 0) return;
    std::uint64_t fields[16] = {};
    if (palmetto_mem_read(object, fields, sizeof fields) != sizeof fields) return;
    const unsigned armed = static_cast<unsigned>(fields[15] & 0xffu);
    const unsigned inflight = static_cast<unsigned>((fields[12] >> 32) & 0xffffffffu);
    const std::uint64_t owner = fields[6];
    std::uint64_t owner_words[16] = {};
    bool owner_readable = owner != 0 && palmetto_mem_read(owner, owner_words, sizeof owner_words) == sizeof owner_words;
    std::uint64_t vtable = owner_readable ? owner_words[0] : 0;
    std::uint64_t target = 0;
    if (vtable != 0) palmetto_mem_read(vtable + 0x18, &target, sizeof target);
    char match[64] = "none";
    for (const void* pointer : live_status_arrays()) {
        if (pointer == reinterpret_cast<const void*>(owner)) {
            std::snprintf(match, sizeof match, "owner");
            break;
        }
        if (!owner_readable) continue;
        bool found = false;
        for (unsigned w = 0; w < 16 && !found; ++w)
            if (owner_words[w] == reinterpret_cast<std::uint64_t>(pointer)) {
                std::snprintf(match, sizeof match, "owner word %u", w);
                found = true;
            }
        if (found) break;
    }
    std::fprintf(stderr,
                 "[rt] ds arrays +%.3fs loader 0x%llx armed %u inflight %u owner 0x%llx owner_vtable 0x%llx "
                 "vtable+0x18 -> %s owner_match %s\n",
                 at, static_cast<unsigned long long>(object), armed, inflight,
                 static_cast<unsigned long long>(owner), static_cast<unsigned long long>(vtable),
                 target != 0 ? rt::shims::module_offset(target).c_str() : "(unreadable)", match);
}

void ds_arrays_dump_start() {
    static std::atomic<bool> started{false};
    if (!ds_arrays_enabled() || started.exchange(true, std::memory_order_relaxed)) return;
    std::thread([] {
        for (unsigned tick = 0; tick < 150; ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(tick < 100 ? 250 : 1000));
            ds_arrays_dump_once();
        }
    }).detach();
}

// ---- Files --------------------------------------------------------------------------------------------------------

class File final : public IDStorageFileX {
public:
    File(HANDLE handle, std::wstring path) : handle_(handle), path_(std::move(path)) {
        {
            std::lock_guard lock(live_mutex());
            live_files().insert(static_cast<IDStorageFileX*>(this));
        }
        std::lock_guard lock(stats().mutex);
        auto& s = stats().s;
        ++s.CurrentOpenFileCount;
        ++s.CumulativeOpenFileCount;
        s.MaxOpenFileCount = std::max(s.MaxOpenFileCount, s.CurrentOpenFileCount);
        ++stats().live_files;
    }
    ~File() {
        Close();   // takes the file out of CurrentOpenFileCount if the title never called Close
        std::lock_guard lock(stats().mutex);
        --stats().live_files;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::File, "IDStorageFileX::QueryInterface")) return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageFileX*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r;
        {
            std::lock_guard lock(live_mutex());
            r = --refs_;
            if (r == 0) live_files().erase(static_cast<IDStorageFileX*>(this));
        }
        if (r == 0) delete this;
        return r;
    }

    // Waits for the submitted requests that read this file, then closes the handle. A request submitted later fails
    // with E_DSTORAGE_FILE_NOT_OPEN. Waiting only for submitted requests keeps a Close from blocking forever on a
    // request that sits unsubmitted in a queue with auto-submit disabled.
    void STDMETHODCALLTYPE Close() override {
        std::unique_lock lock(mutex_);
        closed_ = true;
        idle_.wait(lock, [&] { return pending_ == 0; });
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            // CurrentOpenFileCount counts open files, and Close closes the file whatever its reference count
            // (GDK reference, IDStorageFileX::Close and DSTORAGE_STATS): it drops here, once.
            std::lock_guard stats_lock(stats().mutex);
            --stats().s.CurrentOpenFileCount;
        }
    }

    // A new handle the caller closes (GENERIC_READ, FILE_SHARE_READ, as the reference describes it). ReOpenFile,
    // not DuplicateHandle: a duplicate shares the file position with the shim's own reads, so the title's ReadFile
    // through it would start wherever the worker's last read ended.
    HANDLE STDMETHODCALLTYPE GetHandle() override {
        std::lock_guard lock(mutex_);
        if (closed_ || handle_ == INVALID_HANDLE_VALUE) {
            SetLastError(ERROR_INVALID_HANDLE);
            return INVALID_HANDLE_VALUE;
        }
        return ReOpenFile(handle_, GENERIC_READ, FILE_SHARE_READ, 0);
    }

    // Called when a request naming this file is submitted. False once the file is closed.
    bool add_pending() {
        std::lock_guard lock(mutex_);
        if (closed_) return false;
        ++pending_;
        return true;
    }
    void finish_pending() {
        std::lock_guard lock(mutex_);
        if (--pending_ == 0) idle_.notify_all();
    }
    // Valid while the caller holds a pending count: Close does not close the handle before it drops to zero.
    HANDLE handle() const { return handle_; }
    const std::wstring& path() const { return path_; }
    std::atomic<ULONG>& refs() { return refs_; }

private:
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    std::condition_variable idle_;
    HANDLE handle_;
    const std::wstring path_;
    uint32_t pending_ = 0;
    bool closed_ = false;
};

StatusArray* live_status_array(IDStorageStatusArrayX* p) {
    std::lock_guard lock(live_mutex());
    if (!p || !live_status_arrays().count(p)) return nullptr;
    auto* a = static_cast<StatusArray*>(p);
    return add_ref_if_live(a->refs()) ? a : nullptr;
}

File* live_file(IDStorageFileX* p) {
    std::lock_guard lock(live_mutex());
    if (!p || !live_files().count(p)) return nullptr;
    auto* f = static_cast<File*>(p);
    return add_ref_if_live(f->refs()) ? f : nullptr;
}

// ---- Reads --------------------------------------------------------------------------------------------------------

// memmove that turns an access violation into an error instead of ending the process. No C++ objects here: a
// function with __try cannot have any that need unwinding.
bool guarded_move(void* destination, const void* source, size_t size) {
    __try {
        std::memmove(destination, source, size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

HRESULT read_error(DWORD error) {
    switch (error) {
        case ERROR_NOACCESS:
        case ERROR_INVALID_USER_BUFFER: return E_DSTORAGE_ACCESS_VIOLATION;
        case ERROR_HANDLE_EOF: return E_DSTORAGE_END_OF_FILE;
        default: return HRESULT_FROM_WIN32(error);
    }
}

// RT-DSTORAGE-NOACCESS: a file-range read straight into title memory that is committed only on first touch.
//
// Same class as the file shim's RT-READFILE-NOACCESS repair (docs/readfile_first_touch.md), reached through the queue
// worker instead of the title's own ReadFile helper. The read is a ReadFile under the hood, so a destination page nobody
// has touched yet fails with ERROR_NOACCESS/ERROR_INVALID_USER_BUFFER before a byte is copied, and read_error would
// report E_DSTORAGE_ACCESS_VIOLATION to the title. Measured on our tree: the first boot after the ReadFile repair let
// the loader resume and one enqueued request failed with 0x89240009 once the request shapes grew to 5,123 (the previous
// measurement, with the loader stalled, had zero failures and dest-pages=0). The reference's RT-READFILE-NOACCESS
// commit repairs read_file_range for the identical reason; this is our own implementation of the same mechanism.
//
// The repair is error-gated (a successful read pays one branch and nothing else): on the two first-touch errors the
// destination pages are committed from user mode with rt::first_touch::commit_range(), and the read is re-issued
// exactly once. A destination whose retry failed is remembered, so a title that re-enqueues the same destination is
// answered with the host's error instead of being walked again. The named measurement is one stderr line per run and a
// bounded "dstorage_read_noaccess" startup-trace event.
std::atomic<std::uint64_t> g_ds_first_touch_failures{0};
std::atomic<std::uintptr_t> g_ds_repair_failed_destination{0};
std::atomic<std::uint32_t> g_ds_repair_failed_size{0};

constexpr std::uint64_t kDsFirstTouchLogLimit = 128;

void note_ds_first_touch(DWORD error, std::uint32_t size, DWORD pages, bool repaired, const void* caller) {
    const std::uint64_t count = g_ds_first_touch_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > kDsFirstTouchLogLimit && (count & 255) != 0) return;
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] dstorage: file-range read into uncommitted title memory (error %lu) at %s; touched %lu "
                     "destination page(s) from user mode and %s (%lu byte(s) of destination; logged once per run)\n",
                     static_cast<unsigned long>(error),
                     rt::startup_trace::where(reinterpret_cast<std::uint64_t>(caller)).c_str(),
                     static_cast<unsigned long>(pages),
                     repaired ? "the read then succeeded" : "the retry failed too",
                     static_cast<unsigned long>(size));
    if (!rt::startup_trace::enabled()) return;
    rt::startup_trace::event("dstorage_read_noaccess",
                             "\"api\":\"IDStorageQueueX::EnqueueRequest\",\"error\":" + std::to_string(error) +
                                 ",\"size\":" + std::to_string(size) + ",\"pages\":" + std::to_string(pages) +
                                 ",\"repaired\":" + (repaired ? "true" : "false") + ",\"count\":" +
                                 std::to_string(count) + ",\"caller\":" +
                                 rt::startup_trace::quote(
                                     rt::startup_trace::where(reinterpret_cast<std::uint64_t>(caller))));
}

// PALMETTO_READ_TERMINAL on the storage path (docs/peer_port_streaming.md section 12). A file-range request's outcome
// is already terminal to the title: the worker runs each entry exactly once (no requeue, no retry beyond the one
// first-touch repair) and the status array entry reports the first failure since the previous status entry - the
// measured delivery of docs/streamed_consumer.md and docs/peer_port_streaming.md section 3. This half of the rule
// therefore changes no HRESULT; it names and counts the read failure that survived the one repair retry, so a boot can
// tell a repaired read from one the title received as a terminal request failure. With the switch off it returns the
// same HRESULT as before and writes nothing.
std::atomic<std::uint64_t> g_ds_terminal_reads{0};

HRESULT ds_read_failure(DWORD error, std::uint32_t size, DWORD pages, bool retried, const void* caller) {
    const HRESULT hr = read_error(error);
    if (!rt::first_touch::terminal_reads_enabled(std::getenv("PALMETTO_READ_TERMINAL"))) return hr;
    const std::uint64_t count = g_ds_terminal_reads.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1)
        std::fprintf(stderr,
                     "[rt] dstorage: a file-range read failure reaches the title as the terminal request status 0x%08lx "
                     "(PALMETTO_READ_TERMINAL; %lu byte(s) of destination, %lu page(s) touched, retried=%s; counted, "
                     "logged once per run)\n",
                     static_cast<unsigned long>(hr), static_cast<unsigned long>(size),
                     static_cast<unsigned long>(pages), retried ? "yes" : "no");
    if (rt::startup_trace::enabled() && count <= kDsFirstTouchLogLimit) {
        char fields[256];
        std::snprintf(fields, sizeof fields,
                      "\"api\":\"IDStorageQueueX::EnqueueRequest\",\"error\":%lu,\"hr\":\"0x%08lx\",\"size\":%lu,"
                      "\"pages\":%lu,\"retried\":%s,\"count\":%llu,\"caller\":%s",
                      static_cast<unsigned long>(error), static_cast<unsigned long>(hr),
                      static_cast<unsigned long>(size), static_cast<unsigned long>(pages),
                      retried ? "true" : "false", static_cast<unsigned long long>(count),
                      rt::startup_trace::quote(rt::startup_trace::where(reinterpret_cast<std::uint64_t>(caller)))
                          .c_str());
        rt::startup_trace::event("dstorage_read_terminal", fields);
    }
    return hr;
}

// Reads exactly "size" bytes at "offset" of a synchronous handle into "destination".
HRESULT read_file_range(HANDLE handle, uint64_t offset, void* destination, uint32_t size) {
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle, &length)) return HRESULT_FROM_WIN32(GetLastError());
    const uint64_t file_size = static_cast<uint64_t>(length.QuadPart);
    if (offset > file_size || size > file_size - offset) return E_DSTORAGE_END_OF_FILE;
    auto* out = static_cast<uint8_t*>(destination);
    uint32_t done = 0;
    while (done < size) {
        OVERLAPPED at{};
        const uint64_t position = offset + done;
        at.Offset = static_cast<DWORD>(position);
        at.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD got = 0;
        if (!ReadFile(handle, out + done, size - done, &got, &at)) {
            const DWORD error = GetLastError();
            if (!rt::first_touch::is_kernel_write_blocked(error))
                return ds_read_failure(error, size - done, 0, false, _ReturnAddress());
            const std::uint32_t left = size - done;
            const auto chunk = reinterpret_cast<std::uintptr_t>(out + done);
            if (g_ds_repair_failed_destination.load(std::memory_order_relaxed) == chunk &&
                g_ds_repair_failed_size.load(std::memory_order_relaxed) == left) {
                // Walked once already and the retry failed: do not walk it again; the status reports this failure.
                return ds_read_failure(error, left, 0, false, _ReturnAddress());
            }
            const DWORD pages = rt::first_touch::commit_range(out + done, left);
            OVERLAPPED retry{};
            retry.Offset = at.Offset;
            retry.OffsetHigh = at.OffsetHigh;
            if (ReadFile(handle, out + done, left, &got, &retry)) {  // the one and only retry
                note_ds_first_touch(error, left, pages, true, _ReturnAddress());
            } else {
                const DWORD retry_error = GetLastError();
                g_ds_repair_failed_size.store(left, std::memory_order_relaxed);
                g_ds_repair_failed_destination.store(chunk, std::memory_order_relaxed);
                note_ds_first_touch(retry_error, left, pages, false, _ReturnAddress());
                return ds_read_failure(retry_error, left, pages, true, _ReturnAddress());
            }
        }
        if (got == 0) return E_DSTORAGE_END_OF_FILE;  // the file shrank since the size check
        done += got;
    }
    return S_OK;
}

// ---- Queues -------------------------------------------------------------------------------------------------------

struct Entry {
    enum class Type { Request, Status, Signal, Event } type = Type::Request;
    DSTORAGE_REQUEST request{};
    void* caller = nullptr;        // the title's EnqueueRequest return address (PALMETTO_STREAM_CONSUMER: image base)
    File* file = nullptr;          // a reference, for a file request naming a live file
    bool file_counted = false;     // the file's pending count was raised at submit
    bool cancelled = false;        // CancelRequestsWithTag matched it before it started
    bool counted = false;          // a request the title passed: counted in the statistics when it is submitted
    HRESULT enqueue_error = S_OK;  // found at enqueue: reported when the entry is reached, in order
    StatusArray* status = nullptr;  // a reference
    IDStorageStatusArrayX* status_as_given = nullptr;
    uint32_t index = 0;
    ID3D12Fence* fence = nullptr;  // a reference
    uint64_t value = 0;
    HANDLE event = nullptr;  // a duplicate the entry owns
    std::uint64_t order_not_before_us = 0;  // PALMETTO_DS_ORDER: earliest time a status entry may complete
};

class Queue final : public IDStorageQueueX2 {
public:
    explicit Queue(const DSTORAGE_QUEUE_DESC1& desc) : desc_(desc) {
        error_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        clear_record_locked();
        std::lock_guard lock(stats().mutex);
        auto& p = priority_stats(stats().s, desc_.Priority);
        ++p.CurrentQueueCount;
        ++p.CumulativeQueueCount;
        p.MaxQueueCount = std::max(p.MaxQueueCount, p.CurrentQueueCount);
        ++stats().live_queues;
    }
    // Separate from the constructor so a failure to start the thread can be reported.
    bool start() {
        if (!error_event_) return false;
        ds_load_heartbeat_start();  // no thread without PALMETTO_DS_LOAD_TRACE
        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            return false;
        }
        return true;
    }
    ~Queue() {
        Close();
        if (error_event_) CloseHandle(error_event_);
        std::lock_guard lock(stats().mutex);
        --priority_stats(stats().s, desc_.Priority).CurrentQueueCount;
        --stats().live_queues;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::Queue, "IDStorageQueueX::QueryInterface")) return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageQueueX2*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    // The last Release closes the queue, which runs everything enqueued first.
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    void STDMETHODCALLTYPE EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        ds_record_trace_start(_ReturnAddress());  // PALMETTO_DS_RECORD: no thread without the switch
        ds_stack_scan("enqueue", _ReturnAddress(), _AddressOfReturnAddress(), 0, S_OK, false);
        Entry e;
        e.type = Entry::Type::Request;
        if (!request) {
            e.enqueue_error = bad_call("IDStorageQueueX::EnqueueRequest", E_INVALIDARG, "null request");
        } else {
            e.request = *request;  // the title may reuse its structure as soon as this returns
            e.caller = _ReturnAddress();  // PALMETTO_STREAM_CONSUMER names the title image from this return address
            if (storage_trace_enabled()) note_request_shape(e.request);  // the advanced counters are trace-only
            const uint64_t o = e.request.Options.AsUINT64;
            if (options::source_type(o) == DSTORAGE_REQUEST_SOURCE_FILE &&
                desc_.SourceType == DSTORAGE_REQUEST_SOURCE_FILE && e.request.File) {
                e.file = live_file(e.request.File);
                if (!e.file) e.enqueue_error = E_DSTORAGE_INVALID_FILE_HANDLE;
            }
            e.counted = true;  // TotalRequestCount is "submitted requests" (DSTORAGE_PRIORITY_STATS): see submit_locked
        }
        if (storage_trace_enabled() && request) {
            const uint64_t o = request->Options.AsUINT64;
            char b[192];
            std::snprintf(b, sizeof b,
                          "enqueue request source=%s src=%llu dst=%llu dst_ptr=%p zlib=%d bcpack=%llu swizzle=%llu "
                          "enqueue_hr=%s",
                          options::source_type(o) == DSTORAGE_REQUEST_SOURCE_FILE ? "file" : "memory",
                          static_cast<unsigned long long>(request->SourceSize),
                          static_cast<unsigned long long>(request->DestinationSize),
                          const_cast<void*>(request->Destination), options::zlib(o) ? 1 : 0,
                          static_cast<unsigned long long>(options::bcpack(o)),
                          static_cast<unsigned long long>(options::swizzle(o)), hr_text(e.enqueue_error).c_str());
            storage_trace_site_at("request", b, _ReturnAddress());
        }
        if (ds_load_visible() && request) {
            const std::uint64_t n = g_ds_load.requests_enqueued.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1) palmetto_load_tid_note(static_cast<unsigned long>(GetCurrentThreadId()));  // enqueueing thread
            const bool in_window = n <= 3 || n % 1024 == 0;
            const bool late = n > 4096;  // beyond the initial 4,096-request burst
            if (in_window || late) {
                const uint64_t o = request->Options.AsUINT64;
                char detail[160];
                std::snprintf(detail, sizeof detail, "enqueue request #%llu source=%s src=%u dst=%u zlib=%d",
                              static_cast<unsigned long long>(n),
                              options::source_type(o) == DSTORAGE_REQUEST_SOURCE_FILE ? "file" : "memory",
                              request->SourceSize, request->DestinationSize, options::zlib(o) ? 1 : 0);
                if (in_window) ds_load_note("enqueue", detail, _ReturnAddress());
                if (late) ds_load_note_late("late-enqueue", detail, _ReturnAddress());
            }
        }
        enqueue(std::move(e));
    }

    void STDMETHODCALLTYPE EnqueueStatus(IDStorageStatusArrayX* status_array, UINT32 index) override {
        ds_record_trace_start(_ReturnAddress());  // PALMETTO_DS_RECORD: a queue whose first call is a status entry
        Entry e;
        e.type = Entry::Type::Status;
        e.status_as_given = status_array;
        e.index = index;
        e.status = live_status_array(status_array);
        if (!e.status) {
            e.enqueue_error = E_DSTORAGE_INVALID_STATUS_ARRAY;
        } else if (index >= e.status->capacity()) {
            e.enqueue_error = E_DSTORAGE_INDEX_BOUND;
        } else {
            e.status->set_pending(index);  // polling after this call sees E_PENDING until the entry is reached
            const std::uint64_t grace = ds_order_grace_us();
            if (grace != 0) e.order_not_before_us = ds_order_now_us() + grace;
            e.status->note_enqueue(ds_order_now_us(), _ReturnAddress());
            if (ds_arrays_enabled()) {
                char b[160];
                std::snprintf(b, sizeof b, "status array #%llu index %u enqueued, pending now %llu",
                              static_cast<unsigned long long>(e.status->id()), index,
                              static_cast<unsigned long long>(e.status->pending_count()));
                std::lock_guard lock(log_mutex());
                std::fprintf(stderr, "[rt] ds arrays +%.3fs enqueue: %s caller %s\n",
                             static_cast<double>(ds_order_now_us()) / 1.0e6, b,
                             rt::shims::module_offset(static_cast<std::uint64_t>(
                                                          reinterpret_cast<std::uintptr_t>(_ReturnAddress())))
                                 .c_str());
            }
        }
        {
            std::lock_guard lock(stats().mutex);
            ++priority_stats(stats().s, desc_.Priority).TotalStatusCount;
        }
        if (storage_trace_enabled()) {
            char b[96];
            std::snprintf(b, sizeof b, "enqueue status index=%u enqueue_hr=%s", index, hr_text(e.enqueue_error).c_str());
            storage_trace_site_at("status", b, _ReturnAddress());
        }
        enqueue(std::move(e));
    }

    void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, UINT64 value) override {
        Entry e;
        e.type = Entry::Type::Signal;
        e.fence = fence;
        e.value = value;
        if (fence) {
            fence->AddRef();
        } else {
            e.enqueue_error = E_DSTORAGE_INVALID_FENCE;
        }
        {
            std::lock_guard lock(stats().mutex);
            ++priority_stats(stats().s, desc_.Priority).TotalFenceCount;
        }
        if (storage_trace_enabled()) {
            char b[96];
            std::snprintf(b, sizeof b, "enqueue signal fence=%p value=%llu enqueue_hr=%s",
                          static_cast<void*>(fence), static_cast<unsigned long long>(value),
                          hr_text(e.enqueue_error).c_str());
            storage_trace_site_at("signal", b, _ReturnAddress());
        }
        enqueue(std::move(e));
    }

    // The handle is duplicated here and the duplicate closed once it has been set (or dropped), so an event the
    // title closes, and whose handle value is then reused for another object, is never the one set.
    void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE handle) override {
        Entry e;
        e.type = Entry::Type::Event;
        if (!handle) {
            e.enqueue_error = bad_call("IDStorageQueueX1::EnqueueSetEvent", HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE),
                                       "null event handle");
        } else if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &e.event, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS)) {
            e.event = nullptr;
            e.enqueue_error = HRESULT_FROM_WIN32(GetLastError());
        }
        if (storage_trace_enabled()) {
            char b[112];
            std::snprintf(b, sizeof b, "enqueue set-event handle=%p duplicate=%p enqueue_hr=%s",
                          static_cast<void*>(handle), static_cast<void*>(e.event), hr_text(e.enqueue_error).c_str());
            storage_trace_site_at("setevent", b, _ReturnAddress());
        }
        enqueue(std::move(e));
    }

    void STDMETHODCALLTYPE Submit() override {
        {
            std::lock_guard lock(mutex_);
            if (storage_trace_enabled()) {
                char b[96];
                std::snprintf(b, sizeof b, "submit: %zu entr(ies), %zu submitted before", entries_.size(), submitted_);
                storage_trace_site_at("submit", b, _ReturnAddress());
            }
            if (ds_load_visible()) {
                const std::uint64_t n = g_ds_load.submits.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n == 1) palmetto_load_tid_note(static_cast<unsigned long>(GetCurrentThreadId()));  // submitting thread
                char detail[96];
                std::snprintf(detail, sizeof detail, "submit #%llu: %zu entr(ies), %zu submitted before",
                              static_cast<unsigned long long>(n), entries_.size(), submitted_);
                ds_load_note("submit", detail, _ReturnAddress());
                if (ds_load_seconds() >= ds_load_window().to && entries_.size() <= 8)
                    ds_load_note_late("late-submit", detail, _ReturnAddress());
            }
        }
        std::lock_guard lock(mutex_);
        submit_locked();
    }

    // Marks the requests enqueued before this call that have not started. A cancelled request completes as a success
    // without being read (the overview: the title may no longer use its result).
    void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64 mask, UINT64 value) override {
        std::lock_guard lock(mutex_);
        for (Entry& e : entries_)
            if (e.type == Entry::Type::Request && (e.request.CancellationTag & mask) == value) e.cancelled = true;
    }

    // Submits what is enqueued, waits until the worker has run all of it and stops the worker. Later enqueues are
    // dropped and recorded as E_DSTORAGE_QUEUE_CLOSED.
    void STDMETHODCALLTYPE Close() override {
        std::lock_guard close_lock(close_mutex_);
        {
            std::unique_lock lock(mutex_);
            if (closed_) return;
            closing_ = true;
            submit_locked();
            space_.notify_all();  // an enqueue waiting for space gives up (the queue is closing)
            idle_.wait(lock, [&] { return entries_.empty() && !in_flight_; });
            closed_ = true;
            work_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
    }

    HANDLE STDMETHODCALLTYPE GetErrorEvent() override {
        HANDLE dup = nullptr;
        if (!error_event_ || !DuplicateHandle(GetCurrentProcess(), error_event_, GetCurrentProcess(), &dup, 0, FALSE,
                                              DUPLICATE_SAME_ACCESS))
            return nullptr;
        return dup;  // the reference says the title closes it
    }

    void STDMETHODCALLTYPE RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        if (!record) {
            bad_call("IDStorageQueueX::RetrieveErrorRecord", E_POINTER, "null record");
            return;
        }
        std::lock_guard lock(error_mutex_);
        *record = record_;
        clear_record_locked();
        if (error_event_) ResetEvent(error_event_);
    }

    void STDMETHODCALLTYPE Query(DSTORAGE_QUEUE_INFO* info) override {
        if (!info) {
            bad_call("IDStorageQueueX::Query", E_POINTER, "null info");
            return;
        }
        DSTORAGE_QUEUE_INFO1 full{};
        Query1(&full);
        info->Desc.SourceType = full.Desc.SourceType;
        info->Desc.Capacity = full.Desc.Capacity;
        info->Desc.Priority = full.Desc.Priority;
        info->Desc.Name = full.Desc.Name;
        info->EmptySlotCount = full.EmptySlotCount;
        info->RequestCountUntilAutoSubmit = full.RequestCountUntilAutoSubmit;
    }

    // An empty queue has capacity - 1 empty slots (one slot tells empty from full). The automatic submit happens when
    // the entries enqueued since the last submit reach half the capacity; with it disabled the count is 0.
    void STDMETHODCALLTYPE Query1(DSTORAGE_QUEUE_INFO1* info) override {
        if (!info) {
            bad_call("IDStorageQueueX2::Query1", E_POINTER, "null info");
            return;
        }
        std::lock_guard lock(mutex_);
        info->Desc = desc_;
        info->EmptySlotCount = static_cast<UINT16>(usable() - used_locked());
        const size_t unsubmitted = entries_.size() - submitted_;
        info->RequestCountUntilAutoSubmit =
            auto_submit() ? static_cast<UINT16>(half() > unsubmitted ? half() - unsubmitted : 0) : 0;
        if (ds_load_visible()) {
            g_ds_load.queries.fetch_add(1, std::memory_order_relaxed);
            char detail[128];
            std::snprintf(detail, sizeof detail, "query1: empty slots %u, request count until auto submit %u",
                          static_cast<unsigned>(info->EmptySlotCount),
                          static_cast<unsigned>(info->RequestCountUntilAutoSubmit));
            ds_load_note("query", detail, _ReturnAddress());
        }
    }

private:
    size_t usable() const { return desc_.Capacity - 1u; }
    size_t half() const { return desc_.Capacity / 2u; }
    bool auto_submit() const { return !desc_.Options.AutoSubmitDisabled; }
    size_t used_locked() const { return entries_.size() + (in_flight_ ? 1 : 0); }

    void enqueue(Entry&& e) {
        {
            std::unique_lock lock(mutex_);
            // A full queue waits for the worker to free a slot. With auto-submit on, half the queue is always
            // submitted by then; with it off, only a Submit from another thread frees one (as the reference warns).
            space_.wait(lock, [&] { return closing_ || used_locked() < usable(); });
            if (!closing_) {
                entries_.push_back(std::move(e));
                if (auto_submit() && entries_.size() - submitted_ >= half()) submit_locked();
                return;
            }
        }
        DSTORAGE_ERROR_FIRST_FAILURE f{};
        f.HResult = E_DSTORAGE_QUEUE_CLOSED;
        describe(e, f);
        record_failure(f);
        log_once("closed", "an entry was enqueued on a closed queue (E_DSTORAGE_QUEUE_CLOSED); it never completes");
        release_references(e);
    }

    void submit_locked() {
        const size_t first_new = submitted_;
        uint32_t requests = 0;
        uint64_t source_bytes = 0, destination_bytes = 0;
        for (size_t i = submitted_; i < entries_.size(); ++i) {
            Entry& e = entries_[i];
            if (e.file) e.file_counted = e.file->add_pending();
            if (e.counted) {
                ++requests;
                source_bytes += e.request.SourceSize;
                destination_bytes += e.request.DestinationSize;
            }
        }
        if (requests) {
            std::lock_guard stats_lock(stats().mutex);
            auto& p = priority_stats(stats().s, desc_.Priority);
            p.TotalRequestCount += requests;
            p.TotalRequestSourceByteCount += source_bytes;
            p.TotalRequestDestinationByteCount += destination_bytes;
        }
        if (entries_.size() > submitted_) {
            submitted_ = entries_.size();
            work_.notify_one();
        }
        if (entries_.size() > first_new) {
            unsigned status_entries = 0;
            for (size_t i = first_new; i < entries_.size(); ++i)
                if (entries_[i].type == Entry::Type::Status) ++status_entries;
            ds_order_note_submit(static_cast<unsigned>(entries_.size() - first_new), status_entries, _ReturnAddress());
        }
    }

    void run() {
        for (;;) {
            Entry e;
            {
                std::unique_lock lock(mutex_);
                work_.wait(lock, [&] { return submitted_ > 0 || closed_; });
                if (submitted_ == 0) return;  // closed and drained
                e = std::move(entries_.front());
                entries_.pop_front();
                --submitted_;
                in_flight_ = true;
            }
            process(e);
            trace_progress(e);
            release_references(e);
            {
                std::lock_guard lock(mutex_);
                in_flight_ = false;
                space_.notify_all();
                if (entries_.empty()) idle_.notify_all();
            }
        }
    }

    // Trace-level progress counters, so a long streaming run says how much it moved (requests, bytes, rate) instead of
    // only the first bounded lines (docs/wait_state.md).
    void trace_progress(const Entry& e) {
        if (!storage_trace_enabled()) return;
        static std::atomic<std::uint64_t> requests{0}, bytes{0};
        static const auto started = std::chrono::steady_clock::now();
        if (e.type != Entry::Type::Request) return;
        const std::uint64_t now_requests = requests.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t now_bytes =
            bytes.fetch_add(e.request.DestinationSize, std::memory_order_relaxed) + e.request.DestinationSize;
        if (now_requests % 1024 != 0) return;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        char b[160];
        std::snprintf(b, sizeof b, "progress: %llu request(s), %.1f MiB decompressed in %.1f s (%.1f MiB/s)",
                      static_cast<unsigned long long>(now_requests), double(now_bytes) / double(1 << 20), seconds,
                      seconds > 0.0 ? double(now_bytes) / double(1 << 20) / seconds : 0.0);
        storage_trace(b);
        // The advanced slice's counters: the request shapes the title asks for (so a negative is measured, not assumed)
        // and what the opt-in mechanisms did. Every switch is off by default, so these are the shapes the default boot
        // already had; with a switch on the same line shows its work.
        char advanced[640];
        std::snprintf(advanced, sizeof advanced,
                      "advanced: shapes file=%llu memory=%llu zlib=%llu bcpack=%llu swizzle=%llu dest-pages=%llu "
                      "source-pages=%llu | staging hits=%llu fallbacks=%llu bytes=%llu | pins=%llu pages=%llu "
                      "write-watched=%llu lock-failures=%llu refusals=%llu | unshuffle ok=%llu refused=%llu | "
                      "bcpack refused=%llu decoded=%llu blocks=%llu bytes=%llu history=%llu | pages planned=%llu "
                      "transferred=%llu bytes=%llu refused=%llu | page-write delivered=%llu bytes=%llu views=%llu "
                      "watch-pages=%llu refused=%llu | predictor=%s",
                      static_cast<unsigned long long>(g_advanced.file_source.load()),
                      static_cast<unsigned long long>(g_advanced.memory_source.load()),
                      static_cast<unsigned long long>(g_advanced.zlib.load()),
                      static_cast<unsigned long long>(g_advanced.bcpack.load()),
                      static_cast<unsigned long long>(g_advanced.swizzle.load()),
                      static_cast<unsigned long long>(g_advanced.destination_pages.load()),
                      static_cast<unsigned long long>(g_advanced.source_pages.load()),
                      static_cast<unsigned long long>(g_advanced.staging_hits.load()),
                      static_cast<unsigned long long>(g_advanced.staging_fallbacks.load()),
                      static_cast<unsigned long long>(g_advanced.staging_bytes.load()),
                      static_cast<unsigned long long>(g_advanced.pins.load()),
                      static_cast<unsigned long long>(g_advanced.pinned_pages.load()),
                      static_cast<unsigned long long>(g_advanced.write_watched.load()),
                      static_cast<unsigned long long>(g_advanced.pin_lock_failures.load()),
                      static_cast<unsigned long long>(g_advanced.pin_refusals.load()),
                      static_cast<unsigned long long>(g_advanced.unshuffle_ok.load()),
                      static_cast<unsigned long long>(g_advanced.unshuffle_refused.load()),
                      static_cast<unsigned long long>(g_advanced.bcpack_refused.load()),
                      static_cast<unsigned long long>(g_advanced.bcpack_decoded.load()),
                      static_cast<unsigned long long>(g_advanced.bcpack_blocks.load()),
                      static_cast<unsigned long long>(g_advanced.bcpack_bytes.load()),
                      static_cast<unsigned long long>(g_advanced.bcpack_history_reads.load()),
                      static_cast<unsigned long long>(g_advanced.pages_planned.load()),
                      static_cast<unsigned long long>(g_advanced.pages_transferred.load()),
                      static_cast<unsigned long long>(g_advanced.pages_bytes.load()),
                      static_cast<unsigned long long>(g_advanced.pages_refused.load()),
                      static_cast<unsigned long long>(g_advanced.page_write_delivered.load()),
                      static_cast<unsigned long long>(g_advanced.page_write_bytes.load()),
                      static_cast<unsigned long long>(g_advanced.page_write_views.load()),
                      static_cast<unsigned long long>(g_advanced.page_write_watch_pages.load()),
                      static_cast<unsigned long long>(g_advanced.page_write_refused.load()), predictor_state_name());
        storage_trace(advanced);
    }

    // Runs on the worker. since_notify_ is the first failure since the last status entry or fence signal: the
    // HRESULT a status entry reports.
    void process(Entry& e) {
        HRESULT hr = e.enqueue_error;
        switch (e.type) {
            case Entry::Type::Request:
                // A cancelled request is a success whatever else is wrong with it (the GDK: not counted as a failure).
                if (e.cancelled) {
                    hr = S_OK;
                } else if (SUCCEEDED(hr)) {
                    // Pending: processing has started and not completed (DSTORAGE_PRIORITY_STATS).
                    set_pending(+1);
                    hr = execute_request(e);
                    set_pending(-1);
                    // PALMETTO_STREAM_CONSUMER: report the completed destination range to the device proxy and arm the
                    // first-touch guard (docs/streamed_consumer.md). One cached boolean when the switch is off. A
                    // page-list request's Destination field is its page array, not a data range, so it is not a
                    // streamed block and stays out of the registry.
                    const bool page_list_destination = (e.request.Options.AsUINT64 & options::kDestinationPages) != 0;
                    if (SUCCEEDED(hr) && !page_list_destination && e.request.Destination && e.request.DestinationSize)
                        stream_note_completed(e.request.Destination, e.request.DestinationSize, e.caller);
                }
                if (e.file && e.file_counted) e.file->finish_pending();
                if (ds_load_visible()) g_ds_load.requests_done.fetch_add(1, std::memory_order_relaxed);
                if (FAILED(hr) && SUCCEEDED(since_notify_)) since_notify_ = hr;
                break;
            case Entry::Type::Status:
                if (SUCCEEDED(hr)) {
                    // PALMETTO_DS_ORDER (docs/streamed_consumer.md section 6c): keep the completion observably pending
                    // until the submitting call has returned plus the grace, and until the spinner has polled once.
                    if (const std::uint64_t grace = ds_order_grace_us(); grace != 0) {
                        const auto started = std::chrono::steady_clock::now();
                        const auto elapsed = [&started] {
                            return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                        };
                        while (ds_order_now_us() < e.order_not_before_us && elapsed() < 0.5)
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                        while (e.status && e.status->poll_count() == 0 && elapsed() < 0.1)
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                        if (g_ds_order_completions.fetch_add(1, std::memory_order_relaxed) < 32) {
                            char loader[160];
                            ds_order_loader_text(loader, sizeof loader);
                            const std::uint64_t completed_at = ds_order_now_us();
                            const std::uint64_t enqueued_at =
                                e.order_not_before_us >= grace ? e.order_not_before_us - grace : 0;
                            std::lock_guard lock(log_mutex());
                            std::fprintf(stderr,
                                         "[rt] ds order +%.3fs status index %u completed: %llu us after enqueue "
                                         "(grace %llu), polls %llu, loader %s\n",
                                         static_cast<double>(completed_at) / 1.0e6, e.index,
                                         static_cast<unsigned long long>(completed_at > enqueued_at
                                                                             ? completed_at - enqueued_at
                                                                             : 0),
                                         static_cast<unsigned long long>(grace),
                                         static_cast<unsigned long long>(e.status ? e.status->poll_count() : 0),
                                         loader[0] ? loader : "(loader not set)");
                        }
                    }
                    e.status->complete(e.index, since_notify_);
                    since_notify_ = S_OK;
                }
                break;
            case Entry::Type::Signal:
                if (SUCCEEDED(hr)) hr = e.fence->Signal(e.value);
                since_notify_ = S_OK;
                break;
            case Entry::Type::Event:
                if (SUCCEEDED(hr) && !SetEvent(e.event)) hr = HRESULT_FROM_WIN32(GetLastError());
                break;
        }
        // Trace: what the worker delivered for this entry, so the boot log shows whether a signal/event the title waits
        // for was actually set (docs/wait_state.md).
        if (storage_trace_enabled()) {
            char b[192];
            switch (e.type) {
                case Entry::Type::Request:
                    std::snprintf(b, sizeof b, "request done hr=%s enqueue_hr=%s", hr_text(hr).c_str(),
                                  hr_text(e.enqueue_error).c_str());
                    break;
                case Entry::Type::Status:
                    std::snprintf(b, sizeof b, "status done index=%u hr=%s", e.index, hr_text(hr).c_str());
                    break;
                case Entry::Type::Signal:
                    std::snprintf(b, sizeof b, "signal fence=%p value=%llu hr=%s", static_cast<void*>(e.fence),
                                  static_cast<unsigned long long>(e.value), hr_text(hr).c_str());
                    break;
                case Entry::Type::Event:
                    std::snprintf(b, sizeof b, "set event=%p hr=%s", static_cast<void*>(e.event), hr_text(hr).c_str());
                    break;
            }
            storage_trace_site("done", b);
        }
        if (FAILED(hr)) {
            DSTORAGE_ERROR_FIRST_FAILURE f{};
            f.HResult = hr;
            describe(e, f);
            record_failure(f);
        }
    }

    // PALMETTO_DS_PIN: run one destination write with its range pinned for the duration of the write (the console's
    // pinned destination). With the switch off the call is the write itself, so the default path adds nothing.
    template <typename Write>
    HRESULT write_pinned(void* destination, std::size_t bytes, Write write) {
        if (!advanced_config().pin) return write();
        palmetto::dstorage::DestinationPinner pin;
        pin.set_residency(advanced_config().pin_resident);
        pin.pin(destination, bytes);
        const HRESULT hr = write();
        pin.unpin();
        note_pin(pin.stats());
        return hr;
    }

    HRESULT execute_request(const Entry& e) {
        const DSTORAGE_REQUEST& r = e.request;
        const uint64_t o = r.Options.AsUINT64;
        if (o & options::kReservedMask) return E_DSTORAGE_RESERVED_FIELDS;
        if (options::source_type(o) != desc_.SourceType) return E_DSTORAGE_INVALID_SOURCE_TYPE;
        if (options::bcpack(o) > DSTORAGE_BCPACK_MODE_BC7) return E_DSTORAGE_INVALID_BCPACK_MODE;
        if (options::swizzle(o) > DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K) return E_DSTORAGE_INVALID_SWIZZLE_MODE;
        if (o & (options::kDestinationPages | options::kSourcePages)) {
            if (advanced_config().pages) return page_list_request(e);
            log_once("pages", "physical-page requests need PALMETTO_DS_PAGES=1 (E_NOTIMPL)");
            return E_NOTIMPL;
        }
        // The advanced stages. With their switches unset the branches below refuse exactly as before (same line, same
        // order); with a switch on the request goes to the opt-in mechanism, which either runs it or refuses it with a
        // named limitation of its own (docs/directstorage_advanced.md).
        const unsigned bcpack_mode = static_cast<unsigned>(options::bcpack(o));
        const unsigned swizzle_mode = static_cast<unsigned>(options::swizzle(o));
        if (bcpack_mode != 0 || swizzle_mode != 0) {
            if (bcpack_mode != 0 && advanced_config().bcpack) return bcpack_request(e);
            if (bcpack_mode == 0 && advanced_config().unshuffle) return unshuffle_request(e);
            log_once("bcpack-swizzle",
                     std::string("BCPack and swizzle requests are not implemented yet (E_NOTIMPL; command type ") +
                         std::to_string(static_cast<int>(DSTORAGE_COMMAND_TYPE_REQUEST)) + "; BCPack mode " +
                         std::to_string(options::bcpack(o)) + ", swizzle mode " +
                         std::to_string(options::swizzle(o)) + ", zlib " + (options::zlib(o) ? "on" : "off") +
                         "; source " + size_band(r.SourceSize) + ", destination " + size_band(r.DestinationSize) +
                         ")");
            return E_NOTIMPL;
        }
        if (options::zlib(o)) return decompress_zlib_request(e);
        if (r.IntermediateSize != 0) return E_DSTORAGE_INVALID_INTERMEDIATE_SIZE;  // only zlib + BCPack use it
        if (r.DestinationSize != r.SourceSize) return E_DSTORAGE_INVALID_DESTINATION_SIZE;
        const bool memory = desc_.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY;
        const uint64_t total = static_cast<uint64_t>(r.DestinationSize) + (memory ? r.SourceSize : 0u);
        if (total > kMaxRequestBytes) return E_DSTORAGE_REQUEST_TOO_LARGE;
        // DSTORAGE_REQUEST: a destination over 32 MiB needs a 64 KiB or 2 MiB physical page list, which a request
        // names with the page flags (the branch above takes those before this one). The source equals the
        // destination here, so this covers both linear buffers.
        if (r.DestinationSize > kMax4kPageDestinationSize) return E_DSTORAGE_REQUEST_TOO_LARGE;
        if (!r.Destination) return E_INVALIDARG;
        if (memory) {
            if (!r.Source) return E_INVALIDARG;
        } else {
            if (!r.File) return E_INVALIDARG;
            if (!e.file_counted) return E_DSTORAGE_FILE_NOT_OPEN;
        }
        if (r.SourceSize == 0) return S_OK;  // nothing to move; the reference does not say this fails
        return write_pinned(r.Destination, r.SourceSize, [&]() -> HRESULT {
            if (memory) return guarded_move(r.Destination, r.Source, r.SourceSize) ? S_OK : E_DSTORAGE_ACCESS_VIOLATION;
            return read_file_range(e.file->handle(), r.FileOffset, r.Destination, r.SourceSize);
        });
    }

    // The zlib stage of a request. The compressed source is read exactly as an uncompressed request reads its source
    // (a file range or a memory range), the whole stream is inflated into a scratch buffer, and the destination is
    // written only once the stream ended at exactly DestinationSize with a matching Adler-32: a failed request leaves
    // no partial result behind. DestinationSize is the decompressed size (the reference: it must be greater than
    // SourceSize when decompression is used) and IntermediateSize is the BCPack size, valid only with BCPack, which is
    // refused before this point.
    HRESULT decompress_zlib_request(const Entry& e) {
        const DSTORAGE_REQUEST& r = e.request;
        const bool memory = desc_.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY;
        // One-time per failure reason: names the command type and the request's size bands, never its data or buffers.
        const auto fail = [&r](HRESULT hr, const char* why) {
            log_once(std::string("zlib") + hr_text(hr) + why,
                     std::string("a zlib request failed: ") + why + " (" + hr_text(hr) + "; command type " +
                         std::to_string(static_cast<int>(DSTORAGE_COMMAND_TYPE_REQUEST)) + "; source " +
                         size_band(r.SourceSize) + ", destination " + size_band(r.DestinationSize) + ")");
            return hr;
        };
        if (r.IntermediateSize != 0)
            return fail(E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, "an intermediate size without BCPack");
        if (r.DestinationSize <= r.SourceSize)
            return fail(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a destination no larger than the compressed source");
        const uint64_t total = static_cast<uint64_t>(r.DestinationSize) + (memory ? r.SourceSize : 0u);
        if (total > kMaxRequestBytes) return fail(E_DSTORAGE_REQUEST_TOO_LARGE, "source and destination over 1 GiB");
        // The same 32 MiB rule as for an uncompressed linear destination: a larger linear buffer needs a 64 KiB or
        // 2 MiB physical page list, and a request that names one takes the page-list branch above instead.
        if (r.DestinationSize > kMax4kPageDestinationSize)
            return fail(E_DSTORAGE_REQUEST_TOO_LARGE, "a linear destination over 32 MiB");
        if (!r.Destination) return fail(E_INVALIDARG, "a null destination");
        if (memory) {
            if (!r.Source) return fail(E_INVALIDARG, "a null source");
        } else {
            if (!r.File) return fail(E_INVALIDARG, "a null file");
            if (!e.file_counted) return fail(E_DSTORAGE_FILE_NOT_OPEN, "a file that is not open");
            // The reference: FileOffset is 16-byte aligned when decompression is used.
            if (r.FileOffset % 16) return fail(E_DSTORAGE_INVALID_FILE_OFFSET, "a file offset not 16-byte aligned");
        }
        if (r.SourceSize == 0) return S_OK;  // nothing to inflate; mirrors the uncompressed path
        // PALMETTO_DS_STAGING: the compressed source and the inflated buffer come from the queue's reusable staging
        // area when they fit its cap, else from owned allocations (the default). The bytes read, the inflate and every
        // failure report are the same either way; only where the memory comes from changes.
        const palmetto::dstorage::StagingStats source_before = source_scratch_.stats();
        const palmetto::dstorage::StagingStats output_before = output_scratch_.stats();
        const bool staging_on = source_scratch_.enabled() || output_scratch_.enabled();
        const auto note = [&] {
            if (staging_on) {
                note_staging(source_before, source_scratch_.stats());
                note_staging(output_before, output_scratch_.stats());
            }
        };
        std::vector<uint8_t> owned_compressed;
        std::span<uint8_t> staged_source;
        if (source_scratch_.enabled()) staged_source = source_scratch_.borrow(r.SourceSize);
        uint8_t* compressed = nullptr;
        if (staged_source.empty()) {
            owned_compressed.resize(r.SourceSize);
            compressed = owned_compressed.data();
        } else {
            compressed = staged_source.data();
        }
        if (memory) {
            if (!guarded_move(compressed, r.Source, r.SourceSize)) {
                note();
                return fail(E_DSTORAGE_ACCESS_VIOLATION, "an inaccessible source");
            }
        } else if (const HRESULT hr = read_file_range(e.file->handle(), r.FileOffset, compressed, r.SourceSize);
                   FAILED(hr)) {
            note();
            return hr;  // E_DSTORAGE_END_OF_FILE or the Win32 error, as an uncompressed request reports it
        }
        // The runtime's inflate appends into a std::vector, so the staged form is the queue's reusable output vector;
        // its allocation survives between requests, and a request above the cap uses an owned vector.
        std::vector<uint8_t> owned_decompressed;
        std::vector<uint8_t>* decompressed = &owned_decompressed;
        if (output_scratch_.enabled() && r.DestinationSize <= output_scratch_.capacity())
            decompressed = &output_scratch_.vector();
        if (!palmetto::zlib_inflate(std::span<const std::uint8_t>(compressed, r.SourceSize), *decompressed,
                                    r.DestinationSize) ||
            decompressed->size() != static_cast<size_t>(r.DestinationSize)) {
            note();
            return fail(kDecompressionError, "a malformed stream or a decompressed size that does not match");
        }
        const HRESULT written = write_pinned(r.Destination, decompressed->size(), [&]() -> HRESULT {
            return guarded_move(r.Destination, decompressed->data(), decompressed->size()) ? S_OK
                                                                                           : E_DSTORAGE_ACCESS_VIOLATION;
        });
        note();
        if (FAILED(written)) return fail(written, "an inaccessible destination");
        log_once("zlib-ok", std::string("a zlib request decompressed (command type ") +
                                std::to_string(static_cast<int>(DSTORAGE_COMMAND_TYPE_REQUEST)) + "; source " +
                                size_band(r.SourceSize) + ", destination " + size_band(r.DestinationSize) + ")");
        return S_OK;
    }

    // PALMETTO_DS_UNSHUFFLE: a swizzle request's macro-unshuffle stage (the ABI's swizzle modes 1-4; the layout and its
    // provenance are in docs/directstorage_advanced.md). With zlib on, the inflated bytes are the shuffled intermediate
    // and the destination is the unshuffled result; without it the source is the intermediate and SourceSize must equal
    // DestinationSize. The destination is written once the stage has succeeded, so a failed request leaves no partial
    // unswizzled data behind.
    HRESULT unshuffle_request(const Entry& e) {
        const DSTORAGE_REQUEST& r = e.request;
        const uint64_t o = r.Options.AsUINT64;
        const bool memory = desc_.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY;
        const bool zlib = options::zlib(o);
        const unsigned mode = static_cast<unsigned>(options::swizzle(o));
        const auto fail = [&r, mode, zlib](HRESULT hr, const char* why) {
            g_advanced.unshuffle_refused.fetch_add(1, std::memory_order_relaxed);
            log_once(std::string("unshuffle") + hr_text(hr) + why,
                     std::string("a swizzle request failed: ") + why + " (" + hr_text(hr) + "; swizzle mode " +
                         std::to_string(mode) + ", zlib " + (zlib ? "on" : "off") + "; source " +
                         size_band(r.SourceSize) + ", destination " + size_band(r.DestinationSize) + ")");
            return hr;
        };
        if (r.IntermediateSize != 0) return fail(E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, "an intermediate size without BCPack");
        palmetto::dstorage::SwizzleLayout layout;
        if (!palmetto::dstorage::swizzle_layout(mode, layout) || r.DestinationSize % layout.group_bytes != 0)
            return fail(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a destination that is not whole swizzle groups");
        if (zlib) {
            if (r.DestinationSize <= r.SourceSize)
                return fail(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a destination no larger than the compressed source");
        } else if (r.DestinationSize != r.SourceSize) {
            return fail(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a destination that does not match the source size");
        }
        const uint64_t total = static_cast<uint64_t>(r.DestinationSize) + (memory ? r.SourceSize : 0u);
        if (total > kMaxRequestBytes) return fail(E_DSTORAGE_REQUEST_TOO_LARGE, "source and destination over 1 GiB");
        if (r.DestinationSize > kMax4kPageDestinationSize)
            return fail(E_DSTORAGE_REQUEST_TOO_LARGE, "a linear destination over 32 MiB");
        if (!r.Destination) return fail(E_INVALIDARG, "a null destination");
        if (memory) {
            if (!r.Source) return fail(E_INVALIDARG, "a null source");
        } else {
            if (!r.File) return fail(E_INVALIDARG, "a null file");
            if (!e.file_counted) return fail(E_DSTORAGE_FILE_NOT_OPEN, "a file that is not open");
            if (zlib && r.FileOffset % 16) return fail(E_DSTORAGE_INVALID_FILE_OFFSET, "a file offset not 16-byte aligned");
        }
        if (r.SourceSize == 0) return S_OK;
        const palmetto::dstorage::StagingStats source_before = source_scratch_.stats();
        const palmetto::dstorage::StagingStats output_before = output_scratch_.stats();
        const bool staging_on = source_scratch_.enabled() || output_scratch_.enabled();
        const auto note = [&] {
            if (staging_on) {
                note_staging(source_before, source_scratch_.stats());
                note_staging(output_before, output_scratch_.stats());
            }
        };
        std::vector<uint8_t> owned_source;
        std::span<uint8_t> staged_source;
        if (source_scratch_.enabled()) staged_source = source_scratch_.borrow(r.SourceSize);
        uint8_t* source = nullptr;
        if (staged_source.empty()) {
            owned_source.resize(r.SourceSize);
            source = owned_source.data();
        } else {
            source = staged_source.data();
        }
        if (memory) {
            if (!guarded_move(source, r.Source, r.SourceSize)) {
                note();
                return fail(E_DSTORAGE_ACCESS_VIOLATION, "an inaccessible source");
            }
        } else if (const HRESULT hr = read_file_range(e.file->handle(), r.FileOffset, source, r.SourceSize);
                   FAILED(hr)) {
            note();
            return hr;
        }
        std::vector<uint8_t> owned_inflated;
        const std::uint8_t* shuffled = source;
        if (zlib) {
            std::vector<uint8_t>* inflated = &owned_inflated;
            if (output_scratch_.enabled() && r.DestinationSize <= output_scratch_.capacity())
                inflated = &output_scratch_.vector();
            if (!palmetto::zlib_inflate(std::span<const std::uint8_t>(source, r.SourceSize), *inflated,
                                        r.DestinationSize) ||
                inflated->size() != static_cast<size_t>(r.DestinationSize)) {
                note();
                return fail(kDecompressionError, "a malformed stream or a decompressed size that does not match");
            }
            shuffled = inflated->data();
        }
        const HRESULT written = write_pinned(r.Destination, r.DestinationSize, [&]() -> HRESULT {
            return palmetto::dstorage::unshuffle(mode, shuffled, r.DestinationSize,
                                                 static_cast<std::uint8_t*>(r.Destination), r.DestinationSize)
                       ? S_OK
                       : E_DSTORAGE_INVALID_DESTINATION_SIZE;
        });
        note();
        if (FAILED(written)) return fail(written, "a destination the macro-unshuffle could not fill");
        g_advanced.unshuffle_ok.fetch_add(1, std::memory_order_relaxed);
        log_once("unshuffle-ok", std::string("a swizzle request unshuffled (mode ") + std::to_string(mode) +
                                     "; source " + size_band(r.SourceSize) + ", destination " +
                                     size_band(r.DestinationSize) + ", zlib " + (zlib ? "on" : "off") + ")");
        return S_OK;
    }

    // PALMETTO_DS_BCPACK: the BCPack stage. The request's compressed source (a file range or a memory range; with
    // zlib on, the inflated intermediate) is stretched by the runtime block decoder into the destination's BCn block
    // stream, then optionally unswizzled, and the destination is written once - a refused request leaves nothing
    // behind. The stage order and the size/offset rules are the reference's (zlib, then BCPack, then the macro
    // unshuffle; a decompressing destination is larger than its source and a BCPack destination is whole blocks;
    // IntermediateSize exists only when zlib pairs with BCPack). History comes from the local predictor file only:
    // without a usable file the request is refused by name before decoding, and a block whose built-in history entry
    // the file does not confirm is refused by the decoder, never guessed.
    HRESULT bcpack_request(const Entry& e) {
        const DSTORAGE_REQUEST& r = e.request;
        const std::uint64_t o = r.Options.AsUINT64;
        const bool zlib = options::zlib(o);
        const bool memory = desc_.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY;
        const unsigned mode = static_cast<unsigned>(options::bcpack(o));
        const unsigned swizzle_mode = static_cast<unsigned>(options::swizzle(o));
        const auto refuse = [&r, mode, zlib, swizzle_mode](HRESULT hr, const std::string& why) {
            g_advanced.bcpack_refused.fetch_add(1, std::memory_order_relaxed);
            log_once("bcpack" + std::to_string(mode) + hr_text(hr) + why.substr(0, 16),
                     "a BCPack request is refused: " + why + " (" + hr_text(hr) + "; BCPack mode " +
                         std::to_string(mode) + ", zlib " + (zlib ? "on" : "off") + ", swizzle " +
                         std::to_string(swizzle_mode) + "; source " + size_band(r.SourceSize) + ", destination " +
                         size_band(r.DestinationSize) + ")");
            return hr;
        };
        if (mode > DSTORAGE_BCPACK_MODE_BC7) return refuse(E_DSTORAGE_INVALID_BCPACK_MODE, "a BCPack mode out of range");
        if (mode == DSTORAGE_BCPACK_MODE_BC2) return refuse(E_NOTIMPL, "BC2 has no known BCPack block decoder");
        if (!advanced_config().bcpack) return refuse(E_NOTIMPL, "the BCPack stage is off (PALMETTO_DS_BCPACK)");
        if (swizzle_mode > DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K)
            return refuse(E_DSTORAGE_INVALID_SWIZZLE_MODE, "a swizzle mode out of range");
        // The reference's validation: IntermediateSize exists exactly when zlib feeds BCPack, both stages only grow the
        // data, a BCPack destination is whole blocks and a swizzle destination whole unshuffle groups.
        if (zlib != (r.IntermediateSize != 0))
            return refuse(E_DSTORAGE_INVALID_INTERMEDIATE_SIZE,
                          zlib ? "a zlib request with no intermediate size for the BCPack stage"
                               : "an intermediate size without the zlib stage that produces it");
        if (r.IntermediateSize > kMaxRequestBytes)
            return refuse(E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, "an intermediate size over 1 GiB");
        if (r.DestinationSize <= r.SourceSize)
            return refuse(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a destination no larger than the compressed source");
        const unsigned block = (mode == DSTORAGE_BCPACK_MODE_BC1 || mode == DSTORAGE_BCPACK_MODE_BC4) ? 8u : 16u;
        if (r.DestinationSize % block != 0)
            return refuse(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a destination that is not whole BC blocks");
        palmetto::dstorage::SwizzleLayout layout;
        if (swizzle_mode != 0) {
            if (!palmetto::dstorage::swizzle_layout(swizzle_mode, layout) ||
                r.DestinationSize % layout.group_bytes != 0)
                return refuse(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a destination that is not whole swizzle groups");
            if (!advanced_config().unshuffle)
                return refuse(E_NOTIMPL,
                              "a BCPack request with a swizzle stage needs PALMETTO_DS_UNSHUFFLE for the second stage");
        }
        const std::uint64_t total = static_cast<std::uint64_t>(r.DestinationSize) + (memory ? r.SourceSize : 0u);
        if (total > kMaxRequestBytes)
            return refuse(E_DSTORAGE_REQUEST_TOO_LARGE, "source and destination over 1 GiB");
        if (r.DestinationSize > kMax4kPageDestinationSize)
            return refuse(E_DSTORAGE_REQUEST_TOO_LARGE, "a linear destination over 32 MiB");
        if (!memory && r.FileOffset % 16 != 0)
            return refuse(E_DSTORAGE_INVALID_FILE_OFFSET, "a file offset not 16-byte aligned");
        if (!r.Destination) return refuse(E_INVALIDARG, "a null destination");
        if (memory) {
            if (!r.Source) return refuse(E_INVALIDARG, "a null source");
        } else {
            if (!r.File) return refuse(E_INVALIDARG, "a null file");
            if (!e.file_counted) return refuse(E_DSTORAGE_FILE_NOT_OPEN, "a file that is not open");
        }
        if (r.SourceSize == 0) return S_OK;
        // The predictor policy: the strict decoder reads only confirmed built-in history rows, and they live in the
        // local file the environment names. Without a usable file the stage is not "off": it is a named limitation.
        const PredictorState& state = predictor_state();
        if (state.error != palmetto::dstorage::PredictorError::ok) {
            if (state.error == palmetto::dstorage::PredictorError::missing)
                return refuse(E_NOTIMPL, "no local predictor file (set PALMETTO_DS_BCPACK_PREDICTOR to the "
                                         "tooling/bcpack_history_calib.py output path, outside the repository)");
            return refuse(E_NOTIMPL, std::string("the local predictor file is ") +
                                         palmetto::dstorage::predictor_error_name(state.error));
        }
        // Read the compressed source exactly as the zlib stage does (staging when enabled, owned otherwise).
        const palmetto::dstorage::StagingStats source_before = source_scratch_.stats();
        const palmetto::dstorage::StagingStats output_before = output_scratch_.stats();
        const bool staging_on = source_scratch_.enabled() || output_scratch_.enabled();
        const auto note = [&] {
            if (staging_on) {
                note_staging(source_before, source_scratch_.stats());
                note_staging(output_before, output_scratch_.stats());
            }
        };
        std::vector<uint8_t> owned_compressed;
        std::span<uint8_t> staged_source;
        if (source_scratch_.enabled()) staged_source = source_scratch_.borrow(r.SourceSize);
        uint8_t* compressed = nullptr;
        if (staged_source.empty()) {
            owned_compressed.resize(r.SourceSize);
            compressed = owned_compressed.data();
        } else {
            compressed = staged_source.data();
        }
        if (memory) {
            if (!guarded_move(compressed, r.Source, r.SourceSize)) {
                note();
                return refuse(E_DSTORAGE_ACCESS_VIOLATION, "an inaccessible source");
            }
        } else if (const HRESULT hr = read_file_range(e.file->handle(), r.FileOffset, compressed, r.SourceSize);
                   FAILED(hr)) {
            note();
            return hr;
        }
        // The zlib stage, when the request asks for it: inflate to IntermediateSize exactly.
        const std::uint8_t* packed = compressed;
        std::size_t packed_size = r.SourceSize;
        std::vector<uint8_t> owned_intermediate;
        if (zlib) {
            std::vector<uint8_t>* inflated = &owned_intermediate;
            if (output_scratch_.enabled() && r.IntermediateSize <= output_scratch_.capacity())
                inflated = &output_scratch_.vector();
            if (!palmetto::zlib_inflate(std::span<const std::uint8_t>(compressed, r.SourceSize), *inflated,
                                        r.IntermediateSize) ||
                inflated->size() != static_cast<size_t>(r.IntermediateSize)) {
                note();
                return refuse(kDecompressionError, "a malformed zlib stream or an inflated size that does not match the "
                                                   "intermediate size");
            }
            packed = inflated->data();
            packed_size = inflated->size();
        }
        // The block stage decodes into scratch; only a fully rebuilt stream is copied to the destination.
        std::vector<uint8_t> decoded(r.DestinationSize);
        palmetto::dstorage::bcpack::DecodeStats stats;
        const palmetto::dstorage::bcpack::DecodeError error = palmetto::dstorage::bcpack::decode(
            mode, packed, packed_size, decoded.data(), decoded.size(), &state.history, &stats);
        note();
        if (error != palmetto::dstorage::bcpack::DecodeError::ok) {
            const std::string reason = palmetto::dstorage::bcpack::decode_error_name(error);
            switch (error) {
                case palmetto::dstorage::bcpack::DecodeError::destination_size:
                    return refuse(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a BCPack destination the decoder cannot fill");
                case palmetto::dstorage::bcpack::DecodeError::history_unknown:
                case palmetto::dstorage::bcpack::DecodeError::history_unconfirmed:
                case palmetto::dstorage::bcpack::DecodeError::history_depth:
                    return refuse(E_NOTIMPL, std::string("the local predictor file does not confirm every built-in "
                                                         "history entry this chunk reads (") +
                                                 reason + "; regenerate with tooling/bcpack_history_calib.py)");
                case palmetto::dstorage::bcpack::DecodeError::codec:
                    return refuse(E_NOTIMPL, std::string("no block rebuilder for this BCPack mode (") + reason + ")");
                default:
                    return refuse(kDecompressionError, std::string("the BCPack block stream was refused (") + reason +
                                                           "; source " + size_band(static_cast<uint32_t>(packed_size)) +
                                                           ")");
            }
        }
        // The optional swizzle stage runs on the decoded block stream, into its own buffer, before the single write.
        std::vector<uint8_t> unshuffled;
        const std::uint8_t* result = decoded.data();
        if (swizzle_mode != 0) {
            unshuffled.resize(r.DestinationSize);
            if (!palmetto::dstorage::unshuffle(swizzle_mode, decoded.data(), decoded.size(), unshuffled.data(),
                                              unshuffled.size()))
                return refuse(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a destination the macro-unshuffle could not fill");
            result = unshuffled.data();
            g_advanced.unshuffle_ok.fetch_add(1, std::memory_order_relaxed);
        }
        const HRESULT written = write_pinned(r.Destination, r.DestinationSize, [&]() -> HRESULT {
            return guarded_move(r.Destination, result, r.DestinationSize) ? S_OK : E_DSTORAGE_ACCESS_VIOLATION;
        });
        if (FAILED(written)) return refuse(written, "an inaccessible destination");
        g_advanced.bcpack_decoded.fetch_add(1, std::memory_order_relaxed);
        g_advanced.bcpack_blocks.fetch_add(stats.blocks, std::memory_order_relaxed);
        g_advanced.bcpack_bytes.fetch_add(stats.bytes, std::memory_order_relaxed);
        g_advanced.bcpack_history_reads.fetch_add(stats.history_reads, std::memory_order_relaxed);
        log_once("bcpack-ok", std::string("a BCPack request decoded (mode ") + std::to_string(mode) + ", zlib " +
                                           (zlib ? "on" : "off") + ", swizzle " + std::to_string(swizzle_mode) +
                                           "; source " + size_band(static_cast<uint32_t>(packed_size)) +
                                           ", destination " + size_band(r.DestinationSize) + ")");
        return S_OK;
    }

    // PALMETTO_DS_PAGES: a physical-page request's live transfer (docs/page_resolver.md). The page numbers are read
    // once, bounded by the plan's fixed span table; the plan is the tested arithmetic in dstorage_advanced.h; every
    // page the plan needs is resolved through xmem's page table before the first byte moves, so a number with no live
    // backing refuses the whole request and no partial destination is ever visible to the title. The bytes are
    // materialised in the queue worker's own vector (a file read or memory copy, then the zlib stage, or a gather from
    // a source page list) and only then scattered into the destination page list - or copied into a linear destination
    // for a source page list. BCPack and swizzle over a page list stay a named refusal: those decode stages write a
    // linear destination, and composing them with the scatter belongs to the BCPack runtime slice's seam.
    HRESULT page_list_request(const Entry& e) {
        const DSTORAGE_REQUEST& r = e.request;
        const uint64_t o = r.Options.AsUINT64;
        const bool destination_pages = (o & options::kDestinationPages) != 0;
        const bool source_pages = (o & options::kSourcePages) != 0;
        const bool memory = desc_.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY;
        const ULONG_PTR* const pages = source_pages ? r.SourcePageArray : r.DestinationPageArray;
        const uint16_t page_offset = source_pages ? r.SourcePageOffset : r.DestinationPageOffset;
        const std::size_t page_bytes = source_pages ? r.SourceSize : r.DestinationSize;
        const auto refuse = [destination_pages, page_offset, &r](HRESULT hr, const std::string& why) {
            g_advanced.pages_refused.fetch_add(1, std::memory_order_relaxed);
            log_once("pages" + hr_text(hr) + why.substr(0, 12),
                     "a physical-page request is refused: " + why + " (" + hr_text(hr) + "; " +
                         (destination_pages ? "destination" : "source") + " page list, page offset " +
                         std::to_string(page_offset) + ", " + (destination_pages ? "destination " : "source ") +
                         size_band(destination_pages ? r.DestinationSize : r.SourceSize) + ")");
            return hr;
        };
        const rt::xmem::ResolvePhysicalPageFn resolver_hook = rt::xmem::resolve_physical_page_fn;
        if (resolver_hook == nullptr)
            return refuse(E_NOTIMPL, "this build has no xmem page resolver, so a page list cannot be transferred");
        if (destination_pages && source_pages)
            return refuse(E_INVALIDARG, "a request that names a source and a destination page list");
        if (source_pages && !memory)
            return refuse(E_DSTORAGE_INVALID_SOURCE_TYPE, "a page-list source on a file queue (a page list is memory)");
        if (pages == nullptr) return refuse(E_INVALIDARG, "a null page array");
        if (options::bcpack(o) != 0 || options::swizzle(o) != 0)
            return refuse(E_NOTIMPL,
                          "BCPack or swizzle over a physical page list (the decode stages write a linear destination)");
        if (r.IntermediateSize != 0)
            return refuse(E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, "an intermediate size without BCPack");
        const bool zlib = options::zlib(o) != 0;
        const uint64_t total = static_cast<uint64_t>(r.DestinationSize) +
                               (memory ? static_cast<uint64_t>(r.SourceSize) : 0u);
        if (total > kMaxRequestBytes) return refuse(E_DSTORAGE_REQUEST_TOO_LARGE, "source and destination over 1 GiB");
        if (zlib && r.DestinationSize <= r.SourceSize)
            return refuse(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a destination no larger than the compressed source");
        if (!zlib && r.DestinationSize != r.SourceSize)
            return refuse(E_DSTORAGE_INVALID_DESTINATION_SIZE, "a destination size that does not match the source");
        // A linear buffer keeps the PC 32 MiB rule; the page-list side is bounded by the plan's span table instead.
        if (!destination_pages && r.DestinationSize > kMax4kPageDestinationSize)
            return refuse(E_DSTORAGE_REQUEST_TOO_LARGE, "a linear destination over 32 MiB");
        if (r.SourceSize == 0) return S_OK;  // nothing to move; mirrors the uncompressed and zlib paths
        // Read only the page numbers the transfer needs, bounded by the plan's fixed span table (+1 so an over-long
        // transfer is refused as too many pages rather than attributed to a short list).
        const std::size_t needed =
            1 + (static_cast<std::size_t>(page_offset) + page_bytes - 1) / palmetto::dstorage::kPageListPageBytes;
        const std::size_t count =
            (std::min)(needed, static_cast<std::size_t>(palmetto::dstorage::PagePlan::kMaxSpans) + 1);
        std::uint64_t numbers[palmetto::dstorage::PagePlan::kMaxSpans + 1] = {};
        if (!guarded_move(numbers, pages, count * sizeof(std::uint64_t)))
            return refuse(E_DSTORAGE_ACCESS_VIOLATION, "an inaccessible page array");
        palmetto::dstorage::PagePlan plan;
        const palmetto::dstorage::PagePlanError planned =
            palmetto::dstorage::plan_page_transfer(numbers, count, page_offset, page_bytes, plan);
        if (planned != palmetto::dstorage::PagePlanError::ok)
            return refuse(E_DSTORAGE_INVALID_DESTINATION_SIZE,
                          std::string("the page plan failed: ") + palmetto::dstorage::page_plan_error_name(planned));
        g_advanced.pages_planned.fetch_add(1, std::memory_order_relaxed);
        // The adapter from xmem's hook (a page number -> a CPU view) to the copy path's context-free resolver shape.
        const palmetto::dstorage::PageResolver resolve = [](void*, std::uint64_t page_number) -> const std::uint8_t* {
            const rt::xmem::ResolvePhysicalPageFn hook = rt::xmem::resolve_physical_page_fn;
            return hook ? hook(page_number) : nullptr;
        };
        const auto refused_transfer = [&refuse](const palmetto::dstorage::PageTransferResult& transfer) {
            return refuse(E_INVALIDARG, "page " + std::to_string(transfer.page) + " has no live physical page (" +
                                            palmetto::dstorage::page_transfer_error_name(transfer.error) + " at " +
                                            std::to_string(transfer.spans) + " span(s))");
        };
        // 1. The source bytes: a file read, a memory copy, or a gather out of a source page list.
        std::vector<std::uint8_t> data(r.SourceSize);
        if (source_pages) {
            const auto gathered = palmetto::dstorage::transfer_page_list(numbers, count, page_offset, r.SourceSize,
                                                                        resolve, nullptr, data.data(), false);
            if (gathered.error != palmetto::dstorage::PageTransferError::ok) return refused_transfer(gathered);
        } else if (memory) {
            if (!guarded_move(data.data(), r.Source, data.size()))
                return refuse(E_DSTORAGE_ACCESS_VIOLATION, "an inaccessible source");
        } else {
            if (!r.File) return refuse(E_INVALIDARG, "a null file");
            if (!e.file_counted) return refuse(E_DSTORAGE_FILE_NOT_OPEN, "a file that is not open");
            if (zlib && r.FileOffset % 16)
                return refuse(E_DSTORAGE_INVALID_FILE_OFFSET, "a file offset not 16-byte aligned");
            if (const HRESULT hr = read_file_range(e.file->handle(), r.FileOffset, data.data(), r.SourceSize); FAILED(hr))
                return refuse(hr, "the source file read failed");
        }
        // 2. The zlib stage: exactly DestinationSize bytes, or the request fails before touching a page.
        if (zlib) {
            std::vector<std::uint8_t> inflated;
            if (!palmetto::zlib_inflate(std::span<const std::uint8_t>(data.data(), data.size()), inflated,
                                        r.DestinationSize) ||
                inflated.size() != static_cast<std::size_t>(r.DestinationSize))
                return refuse(kDecompressionError, "a malformed stream or a decompressed size that does not match");
            data = std::move(inflated);
        }
        // 3. Publish: scatter into the destination page list, or copy into the linear destination.
        if (destination_pages) {
            if (advanced_config().pages_view) {
                // PALMETTO_DS_PAGES_VIEW (docs/peer_port_streaming.md section 13): the destination pages are written
                // through xmem's physical-page write, which repeats the bytes through every armed page-watch mapping of
                // the page and records the written pages for the tracker - the delivery difference the page watch needs
                // (docs/page_resolver.md section 6 named it as its limit). Off, this branch is the resolver scatter the
                // pre-port path runs.
                const rt::xmem::WritePhysicalPagesFn write_hook = rt::xmem::write_physical_pages;
                if (write_hook == nullptr)
                    return refuse(E_NOTIMPL,
                                  "this build has no physical-page write hook, so a page-list destination cannot be "
                                  "delivered through xmem");
                rt::xmem::PhysicalPageWrite write_spans[palmetto::dstorage::PagePlan::kMaxSpans] = {};
                for (unsigned i = 0; i < plan.count; ++i) {
                    write_spans[i].page = plan.spans[i].page;
                    write_spans[i].offset = plan.spans[i].offset;
                    write_spans[i].bytes = plan.spans[i].bytes;
                }
                rt::xmem::PhysicalPageWriteStats write_stats;
                std::uint64_t failed_page = 0;
                if (!write_hook(write_spans, plan.count, data.data(), &write_stats, &failed_page)) {
                    g_advanced.page_write_refused.fetch_add(1, std::memory_order_relaxed);
                    return refuse(E_INVALIDARG,
                                  failed_page ? "page " + std::to_string(failed_page) +
                                                    " has no live physical page (unresolved_page; the physical-page "
                                                    "write is all-or-nothing)"
                                              : "the physical-page write refused its arguments");
                }
                g_advanced.page_write_delivered.fetch_add(1, std::memory_order_relaxed);
                g_advanced.page_write_bytes.fetch_add(write_stats.bytes, std::memory_order_relaxed);
                g_advanced.page_write_views.fetch_add(write_stats.watch_views, std::memory_order_relaxed);
                g_advanced.page_write_watch_pages.fetch_add(write_stats.watch_pages, std::memory_order_relaxed);
                g_advanced.pages_transferred.fetch_add(1, std::memory_order_relaxed);
                g_advanced.pages_bytes.fetch_add(write_stats.bytes, std::memory_order_relaxed);
                log_once("pages-view-ok",
                         "a physical-page request was delivered through the page write (destination page list, " +
                             std::to_string(write_stats.spans) + " span(s) over " +
                             std::to_string(write_stats.bytes) + " byte(s), " +
                             std::to_string(write_stats.watch_views) + " armed title mapping(s) written through, " +
                             std::to_string(write_stats.watch_pages) + " page(s) recorded for the page watch, zlib " +
                             (zlib ? "on" : "off") + "; source " + size_band(r.SourceSize) + ")");
            } else {
                const auto scattered = palmetto::dstorage::transfer_page_list(numbers, count, page_offset, data.size(),
                                                                             resolve, nullptr, data.data(), true);
                if (scattered.error != palmetto::dstorage::PageTransferError::ok) return refused_transfer(scattered);
                g_advanced.pages_transferred.fetch_add(1, std::memory_order_relaxed);
                g_advanced.pages_bytes.fetch_add(scattered.bytes, std::memory_order_relaxed);
                log_once("pages-ok", "a physical-page request transferred (destination page list, " +
                                         std::to_string(scattered.spans) + " span(s) over " +
                                         std::to_string(scattered.bytes) + " byte(s), zlib " + (zlib ? "on" : "off") +
                                         "; source " + size_band(r.SourceSize) + ")");
            }
        } else {
            if (!r.Destination) return refuse(E_INVALIDARG, "a null destination");
            if (!guarded_move(r.Destination, data.data(), data.size()))
                return refuse(E_DSTORAGE_ACCESS_VIOLATION, "an inaccessible destination");
            g_advanced.pages_transferred.fetch_add(1, std::memory_order_relaxed);
            g_advanced.pages_bytes.fetch_add(data.size(), std::memory_order_relaxed);
            log_once("pages-ok", "a physical-page request transferred (source page list, " +
                                     std::to_string(plan.count) + " span(s) over " + std::to_string(r.SourceSize) +
                                     " byte(s), zlib " + (zlib ? "on" : "off") + "; destination " +
                                     size_band(r.DestinationSize) + ")");
        }
        return S_OK;
    }

    void set_pending(int delta) {
        std::lock_guard lock(stats().mutex);
        priority_stats(stats().s, desc_.Priority).PendingRequestCount += static_cast<uint32_t>(delta);
    }

    void describe(const Entry& e, DSTORAGE_ERROR_FIRST_FAILURE& f) const {
        switch (e.type) {
            case Entry::Type::Request: {
                f.CommandType = DSTORAGE_COMMAND_TYPE_REQUEST;
                auto& q = f.Request;
                const DSTORAGE_REQUEST& r = e.request;
                q.Options = r.Options;
                q.Destination = r.Destination;
                q.DestinationSize = r.DestinationSize;
                q.SourceSize = r.SourceSize;
                q.CancellationTag = r.CancellationTag;
                if (options::source_type(r.Options.AsUINT64) == DSTORAGE_REQUEST_SOURCE_MEMORY) {
                    q.Source = r.Source;
                } else {
                    q.FileOffset = r.FileOffset;
                    if (e.file) wcsncpy_s(q.Filename, e.file->path().c_str(), _TRUNCATE);
                }
                // The reference says the name stays readable until the request completes.
                if (r.Name)
                    for (size_t i = 0; i + 1 < kRequestMaxName && r.Name[i]; ++i) q.Name[i] = r.Name[i];
                break;
            }
            case Entry::Type::Status:
                f.CommandType = DSTORAGE_COMMAND_TYPE_STATUS;
                f.Status.StatusArray = e.status_as_given;
                f.Status.Index = e.index;
                break;
            case Entry::Type::Signal:
                f.CommandType = DSTORAGE_COMMAND_TYPE_SIGNAL;
                f.Signal.Fence = e.fence;
                f.Signal.Value = e.value;
                break;
            case Entry::Type::Event:
                f.CommandType = kCommandTypeEvent;
                break;
        }
    }

    void record_failure(const DSTORAGE_ERROR_FIRST_FAILURE& f) {
        count_error();
        {
            std::lock_guard lock(error_mutex_);
            if (record_.FailureCount++ == 0) record_.FirstFailure = f;
            if (error_event_) SetEvent(error_event_);
        }
        log_once("fail" + hr_text(f.HResult) + std::to_string(f.CommandType),
                 "an entry failed with " + hr_text(f.HResult) + " (command type " + std::to_string(f.CommandType) +
                     "; logged once per code)");
    }

    void clear_record_locked() {
        record_ = DSTORAGE_ERROR_RECORD{};
        record_.FirstFailure.CommandType = DSTORAGE_COMMAND_TYPE_NONE;
    }

    static void release_references(Entry& e) {
        if (e.file) e.file->Release();
        if (e.status) e.status->Release();
        if (e.fence) e.fence->Release();
        if (e.event) CloseHandle(e.event);
        e.event = nullptr;
        e.file = nullptr;
        e.status = nullptr;
        e.fence = nullptr;
    }

    std::atomic<ULONG> refs_{1};
    const DSTORAGE_QUEUE_DESC1 desc_;
    std::mutex close_mutex_;  // one Close at a time
    std::mutex mutex_;        // entries_, submitted_, in_flight_, closing_, closed_
    std::condition_variable work_, space_, idle_;
    std::deque<Entry> entries_;
    size_t submitted_ = 0;  // the first submitted_ entries are submitted
    bool in_flight_ = false;
    bool closing_ = false;
    bool closed_ = false;
    // PALMETTO_DS_STAGING: one reusable buffer per transform stage, per queue (its worker is single-threaded). Both
    // hold no allocation until the first request borrows from them.
    palmetto::dstorage::BoundedScratch source_scratch_{advanced_config().staging_cap};
    palmetto::dstorage::BoundedScratch output_scratch_{advanced_config().staging_cap};
    std::thread worker_;
    HRESULT since_notify_ = S_OK;  // worker only
    std::mutex error_mutex_;
    DSTORAGE_ERROR_RECORD record_{};
    HANDLE error_event_ = nullptr;
};

// ---- The factory ----------------------------------------------------------------------------------------------------

HRESULT validate_queue_desc(const DSTORAGE_QUEUE_DESC1& d) {
    if (d.SourceType != DSTORAGE_REQUEST_SOURCE_FILE && d.SourceType != DSTORAGE_REQUEST_SOURCE_MEMORY)
        return E_DSTORAGE_INVALID_SOURCE_TYPE;
    if (d.Capacity < kMinQueueCapacity || d.Capacity > kMaxQueueCapacity) return E_DSTORAGE_INVALID_QUEUE_CAPACITY;
    if (d.Priority < DSTORAGE_PRIORITY_FIRST || d.Priority > DSTORAGE_PRIORITY_LAST)
        return E_DSTORAGE_INVALID_QUEUE_PRIORITY;
    if (d.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY && d.Priority != DSTORAGE_PRIORITY_REALTIME)
        return E_DSTORAGE_INVALID_MEMORY_QUEUE_PRIORITY;
    if (d.Options.Reserved) return E_DSTORAGE_RESERVED_FIELDS;
    return S_OK;
}

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

// The title's own CreateFileW: the GAME: mapping and overlay when file_system.cpp is linked, else the host's.
CreateFileWFn create_file_w() {
    if (void* fn = rt::shims::find("kernel32.dll", "CreateFileW")) return reinterpret_cast<CreateFileWFn>(fn);
    return &::CreateFileW;
}

class Factory final : public IDStorageFactoryX2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::Factory, "IDStorageFactoryX::QueryInterface")) return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageFactoryX2*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    // One factory for the process; its count never drops below one, so it is never freed.
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = refs_.load();
        while (r > 1 && !refs_.compare_exchange_weak(r, r - 1)) {}
        return r > 1 ? r - 1 : 1;
    }

    HRESULT STDMETHODCALLTYPE CreateQueue(const DSTORAGE_QUEUE_DESC* desc, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX::CreateQueue", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (!desc) return bad_call("IDStorageFactoryX::CreateQueue", E_INVALIDARG, "null desc");
        DSTORAGE_QUEUE_DESC1 d{};
        d.SourceType = desc->SourceType;
        d.Capacity = desc->Capacity;
        d.Priority = desc->Priority;
        d.Name = desc->Name;
        return create_queue(d, riid, ppv, "IDStorageFactoryX::CreateQueue");
    }

    HRESULT STDMETHODCALLTYPE CreateQueue1(const DSTORAGE_QUEUE_DESC1* desc, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX2::CreateQueue1", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (!desc) return bad_call("IDStorageFactoryX2::CreateQueue1", E_INVALIDARG, "null desc");
        return create_queue(*desc, riid, ppv, "IDStorageFactoryX2::CreateQueue1");
    }

    HRESULT STDMETHODCALLTYPE OpenFile(const WCHAR* path, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX::OpenFile", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (!path || !*path) return bad_call("IDStorageFactoryX::OpenFile", E_INVALIDARG, "null or empty path");
        if (!iid_policy().accept_creating(riid, Kind::File, "IDStorageFactoryX::OpenFile")) return E_NOINTERFACE;
        const HANDLE h = create_file_w()(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(error ? error : ERROR_FILE_NOT_FOUND);
        }
        File* f = new (std::nothrow) File(h, path);
        if (!f) {
            CloseHandle(h);
            return E_OUTOFMEMORY;
        }
        *ppv = static_cast<IDStorageFileX*>(f);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32 capacity, PCSTR, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX::CreateStatusArray", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (capacity == 0) return bad_call("IDStorageFactoryX::CreateStatusArray", E_INVALIDARG, "capacity 0");
        if (!iid_policy().accept_creating(riid, Kind::StatusArray, "IDStorageFactoryX::CreateStatusArray"))
            return E_NOINTERFACE;
        std::unique_ptr<std::atomic<HRESULT>[]> entries(new (std::nothrow) std::atomic<HRESULT>[capacity]);
        if (!entries) return E_OUTOFMEMORY;
        auto* a = new (std::nothrow) StatusArray(capacity, std::move(entries));
        if (!a) return E_OUTOFMEMORY;
        *ppv = static_cast<IDStorageStatusArrayX*>(a);
        if (ds_arrays_enabled()) {
            std::lock_guard lock(log_mutex());
            std::fprintf(stderr, "[rt] ds arrays array #%llu created: capacity %u caller %s\n",
                         static_cast<unsigned long long>(a->id()), capacity,
                         rt::shims::module_offset(static_cast<std::uint64_t>(
                                                      reinterpret_cast<std::uintptr_t>(_ReturnAddress())))
                             .c_str());
        }
        if (ds_load_visible()) {
            ds_load_heartbeat_start();
            palmetto_load_tid_note(static_cast<unsigned long>(GetCurrentThreadId()));
            char detail[96];
            std::snprintf(detail, sizeof detail, "create status array capacity %u", capacity);
            ds_load_note("status-array", detail, _ReturnAddress());
        }
        return S_OK;
    }

    // DirectStorage's own worker work would be pinned to these cores on the console. The mask names console cores,
    // so it is stored and not applied to the host's.
    HRESULT STDMETHODCALLTYPE SetCpuAffinity(UINT64 affinity) override {
        cpu_affinity_.store(affinity);
        return S_OK;
    }
    void STDMETHODCALLTYPE SetDebugFlags(UINT32 flags) override { debug_flags_.store(flags); }

    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32 size) override {
        if (std::find(std::begin(kStagingBufferSizes), std::end(kStagingBufferSizes), size) ==
            std::end(kStagingBufferSizes))
            return bad_call("IDStorageFactoryX::SetStagingBufferSize", E_DSTORAGE_INVALID_STAGING_BUFFER_SIZE,
                            "not one of 0, 16, 20, 24, 28 or 32 MiB");
        std::lock_guard lock(stats().mutex);
        if (stats().live_queues || stats().live_files) return E_DSTORAGE_STAGING_BUFFER_LOCKED;
        staging_size_ = size;  // there is no staging buffer here; the size is only kept
        return S_OK;
    }

    void STDMETHODCALLTYPE GetStats(DSTORAGE_STATS* out) override {
        if (!out) {
            bad_call("IDStorageFactoryX1::GetStats", E_POINTER, "null stats");
            return;
        }
        std::lock_guard lock(stats().mutex);
        *out = stats().s;
        if (ds_load_visible()) {
            g_ds_load.stat_requests.fetch_add(1, std::memory_order_relaxed);
            const DSTORAGE_PRIORITY_STATS* priorities[4] = {&out->LowPriorityStats, &out->NormalPriorityStats,
                                                             &out->HighPriorityStats, &out->RealtimePriorityStats};
            std::uint64_t requests = 0, source = 0, destination = 0, statuses = 0, fences = 0, pending = 0;
            for (const DSTORAGE_PRIORITY_STATS* p : priorities) {
                requests += p->TotalRequestCount;
                source += p->TotalRequestSourceByteCount;
                destination += p->TotalRequestDestinationByteCount;
                statuses += p->TotalStatusCount;
                fences += p->TotalFenceCount;
                pending += p->PendingRequestCount;
            }
            char detail[224];
            std::snprintf(detail, sizeof detail,
                          "get stats: requests %llu (source %llu, destination %llu), status %llu, fence %llu, "
                          "pending %llu, open files %u, errors %u",
                          static_cast<unsigned long long>(requests), static_cast<unsigned long long>(source),
                          static_cast<unsigned long long>(destination), static_cast<unsigned long long>(statuses),
                          static_cast<unsigned long long>(fences), static_cast<unsigned long long>(pending),
                          out->CurrentOpenFileCount, out->ErrorCount);
            ds_load_note("stats", detail, _ReturnAddress());
        }
    }

private:
    HRESULT create_queue(const DSTORAGE_QUEUE_DESC1& desc, REFIID riid, void** ppv, const char* caller) {
        if (const HRESULT hr = validate_queue_desc(desc); FAILED(hr)) return bad_call(caller, hr, "invalid queue desc");
        if (!iid_policy().accept_creating(riid, Kind::Queue, caller)) return E_NOINTERFACE;
        auto* q = new (std::nothrow) Queue(desc);
        if (!q) return E_OUTOFMEMORY;
        if (!q->start()) {
            q->Release();
            return E_OUTOFMEMORY;
        }
        *ppv = static_cast<IDStorageQueueX2*>(q);
        return S_OK;
    }

    std::atomic<ULONG> refs_{1};
    std::atomic<uint64_t> cpu_affinity_{~0ull};
    std::atomic<uint32_t> debug_flags_{0};
    uint32_t staging_size_ = 32u << 20;  // under stats().mutex
};

Factory& factory() {
    static auto* f = new Factory;
    return *f;
}

HRESULT WINAPI dstorage_get_factory(REFIID riid, void** ppv) {
    if (!ppv) return bad_call("DStorageGetFactory", E_POINTER, "null ppv");
    *ppv = nullptr;
    if (!iid_policy().accept_creating(riid, Kind::Factory, "DStorageGetFactory")) return E_NOINTERFACE;
    Factory& f = factory();
    f.AddRef();
    *ppv = static_cast<IDStorageFactoryX2*>(&f);
    return S_OK;
}

}  // namespace

// The console module names; docs/directstorage.md has the evidence and the confidence for each.
SHIM_TABLE(dstorage,
    {"dstorage_xs.dll", "DStorageGetFactory", reinterpret_cast<void*>(&dstorage_get_factory)},
    {"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&dstorage_get_factory)},
    {"dstorage.dll", "DStorageGetFactory", reinterpret_cast<void*>(&dstorage_get_factory)},
);
