// Bounded, opt-in wait tracer (PALMETTO_WAIT_TRACE): which host wait call each thread enters, from which title code,
// on what object, with what timeout, and which of those calls are still outstanding at a named time.
//
// Method. Every loaded system module's import address table (and the mapped title image's) is rewritten so that the
// wait/set API names in the target table below point at a wrapper in this file; the wrapper calls the original through
// the pointer it replaced. The wrappers record, per call: thread id, API, the nearest title-image frame on the stack
// (the "site"), the object (handle, or the address a wait-on-address wait keys on), its object type and current state,
// the timeout, the return time and the outcome (signalled / timed out / other). Set and wake calls are matched against
// the objects that were waited on, so an object that is waited on and never set shows up as exactly that.
//
// Bounds (fixed at compile time; the numbers are repeated in docs/wait_forensics.md):
//   * kMaxKeys aggregate rows = (API, title site);      kMaxObjects wait objects tracked for set correlation;
//   * kMaxSites signal caller sites;                    kMaxApiCounters unmatched-set counters;
//   * kDetailTotal detail lines per boot, kDetailPerKey per aggregate row (the rest is counted, not printed);
//   * kMaxThreads outstanding-wait slots (further threads are counted, not followed);
//   * the dump schedule comes from PALMETTO_WAIT_TRACE=<seconds,...> ("1" = 60,120), at most kMaxDumps dumps;
//   * kMaxLateScans module rescans after start.
// With the variable unset no code of this file runs: the default path is unchanged.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "../shims/shims.h"

namespace palmetto::wait_trace {

// ---- bounds -----------------------------------------------------------------------------------------------------
constexpr unsigned kMaxApis = 40;         // target API table
constexpr unsigned kMaxKeys = 96;         // (API, title site) aggregate rows
constexpr unsigned kMaxObjects = 192;     // wait objects tracked for set correlation
constexpr unsigned kMaxSites = 128;       // signal caller sites
constexpr unsigned kMaxApiCounters = 40;  // unmatched-set counter per API
constexpr unsigned kDetailTotal = 512;    // detail lines over the whole boot
constexpr unsigned kDetailPerKey = 4;     // detail lines per aggregate row
constexpr unsigned kMaxThreads = 128;     // outstanding-wait slots
constexpr unsigned kMaxDumps = 8;         // scheduled dumps
constexpr unsigned kStackFrames = 24;     // backtrace depth for site naming
constexpr unsigned kMaxLateScans = 6;     // module rescans after start
constexpr unsigned kNoSite = 0xFFFFFFFFu;

enum Kind : unsigned { kWaitSingle = 0, kWaitMulti, kWaitAddress, kDelay, kSetHandle, kSetAddress };
enum Flavor : unsigned { kWin32Wait = 0, kNtWait, kBoolWait };
enum ObjType : unsigned { kTypeUnknown = 0, kTypeEvent, kTypeSemaphore, kTypeMutant, kTypeThread, kTypeOther };
enum Outcome : unsigned { kSignalled = 0, kTimedOut, kOtherOutcome };

// Target API indices: fixed order, the name table and the wrapper table both follow it.
enum Api : unsigned {
    aWaitForSingleObject = 0,
    aWaitForSingleObjectEx,
    aWaitForMultipleObjects,
    aWaitForMultipleObjectsEx,
    aWaitOnAddress,
    aSleepConditionVariableSRW,
    aSleepConditionVariableCS,
    aSleep,
    aSleepEx,
    aNtWaitForSingleObject,
    aNtWaitForMultipleObjects,
    aNtWaitForAlertByThreadId,
    aNtDelayExecution,
    aRtlSleepConditionVariableSRW,
    aSetEvent,
    aNtSetEvent,
    aPulseEvent,
    aNtPulseEvent,
    aReleaseSemaphore,
    aNtReleaseSemaphore,
    aReleaseMutex,
    aNtReleaseMutant,
    aWakeByAddressSingle,
    aWakeByAddressAll,
    aRtlWakeAddressSingle,
    aRtlWakeAddressAll,
    aWakeConditionVariable,
    aWakeAllConditionVariable,
    aRtlWakeConditionVariable,
    aRtlWakeAllConditionVariable,
    kApiCount
};

static const char* const kApiNames[kApiCount] = {
    "WaitForSingleObject",       "WaitForSingleObjectEx",      "WaitForMultipleObjects",
    "WaitForMultipleObjectsEx",  "WaitOnAddress",              "SleepConditionVariableSRW",
    "SleepConditionVariableCS",  "Sleep",                      "SleepEx",
    "NtWaitForSingleObject",     "NtWaitForMultipleObjects",   "NtWaitForAlertByThreadId",
    "NtDelayExecution",          "RtlSleepConditionVariableSRW", "SetEvent",
    "NtSetEvent",                "PulseEvent",                 "NtPulseEvent",
    "ReleaseSemaphore",          "NtReleaseSemaphore",         "ReleaseMutex",
    "NtReleaseMutant",           "WakeByAddressSingle",        "WakeByAddressAll",
    "RtlWakeAddressSingle",      "RtlWakeAddressAll",          "WakeConditionVariable",
    "WakeAllConditionVariable",  "RtlWakeConditionVariable",   "RtlWakeAllConditionVariable",
};

// ---- state ------------------------------------------------------------------------------------------------------
struct Aggregate {
    std::atomic<bool> used{false};
    std::atomic<unsigned> api{0};
    std::atomic<unsigned> site{0};  // title-image RVA; kNoSite = title thread whose stack had no title frame
    std::atomic<unsigned long> calls{0};
    std::atomic<unsigned long> signalled{0};
    std::atomic<unsigned long> timed_out{0};
    std::atomic<unsigned long> other{0};
    std::atomic<unsigned long long> last_ms{0};
    std::atomic<unsigned long long> last_duration_ms{0};
    std::atomic<unsigned> details{0};
};

struct ObjectRec {
    std::atomic<bool> used{false};
    std::atomic<unsigned long long> object{0};
    std::atomic<unsigned> kind{0};
    std::atomic<unsigned> type{kTypeUnknown};
    std::atomic<unsigned long> waits{0};
    std::atomic<unsigned long> waiters{0};
    std::atomic<unsigned long> sets{0};
    std::atomic<unsigned long long> last_set_ms{0};
    std::atomic<unsigned> last_set_api{0};
    std::atomic<unsigned> last_set_site{kNoSite};
    std::atomic<unsigned> state_seen{0};  // 0 unknown, 1 set, 2 not set
};

struct SignalSite {
    std::atomic<bool> used{false};
    std::atomic<unsigned> api{0};
    std::atomic<unsigned> site{0};
    std::atomic<unsigned> caller{0};
    std::atomic<unsigned long> calls{0};
    std::atomic<unsigned long> matched{0};
};

struct Outstanding {
    std::atomic<unsigned> api{0xFFFFFFFFu};  // 0xFFFFFFFF = free
    std::atomic<unsigned long> tid{0};
    std::atomic<unsigned long long> start_ms{0};
    std::atomic<unsigned long long> object{0};
    std::atomic<unsigned> kind{0};
    std::atomic<unsigned> type{kTypeUnknown};
    std::atomic<unsigned> site{0};
    std::atomic<unsigned> count{0};
    std::atomic<long long> timeout_ms{0};
};

struct State {
    std::mutex mutex;
    std::uint64_t image_base = 0, image_end = 0;
    std::atomic<bool> started{false};
    std::atomic<bool> dumping{false};
    std::atomic<unsigned long long> detail_count{0};
    std::atomic<unsigned long long> host_calls{0};
    std::atomic<unsigned> titled_threads{0};
    std::atomic<unsigned long> unmatched_sets{0};
    std::atomic<unsigned long> unmatched_by_api[kMaxApiCounters];
    std::atomic<unsigned long> patched_slots{0};
    unsigned long long epoch_ms = 0;
    Aggregate aggregates[kMaxKeys];
    ObjectRec objects[kMaxObjects];
    SignalSite sites[kMaxSites];
    Outstanding threads[kMaxThreads];

    State() {
        for (auto& counter : unmatched_by_api) counter.store(0);
    }
};

inline State& state() {
    static State s;
    return s;
}

inline std::uint64_t now_ms() {
    return GetTickCount64() - state().epoch_ms;
}

// "game+0xRVA" for a title site, "-" when there is none. No allocation, safe on the wait path.
inline const char* site_text(unsigned rva) {
    thread_local char buffers[6][32];
    thread_local unsigned cursor = 0;
    char* out = buffers[cursor++ % 6];
    if (rva == kNoSite) std::snprintf(out, 32, "-");
    else std::snprintf(out, 32, "game+0x%X", rva);
    return out;
}

// ---- object inspection (ntdll queries; handles are only valid inside this process) -------------------------------
using NtQueryObjectFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
using NtQueryInfoFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);

inline void* nt_proc(const char* name) {
    static HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll ? reinterpret_cast<void*>(GetProcAddress(ntdll, name)) : nullptr;
}

inline void object_type_name(HANDLE handle, char* out, std::size_t cap) {
    out[0] = '\0';
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    static auto query = reinterpret_cast<NtQueryObjectFn>(nt_proc("NtQueryObject"));
    if (!query) return;
    struct TypeName {
        USHORT length, maximum;
        wchar_t* buffer;
    } name{};
    unsigned char buffer[1024] = {};
    if (query(handle, 2 /*ObjectTypeInformation*/, buffer, sizeof(buffer), nullptr) < 0) return;
    std::memcpy(&name, buffer, sizeof(name));
    if (!name.buffer || !name.length) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name.buffer, name.length / 2, out, static_cast<int>(cap) - 1,
                                          nullptr, nullptr);
    if (bytes > 0) out[bytes] = '\0';
}

inline unsigned object_type_id(const char* type) {
    if (std::strcmp(type, "Event") == 0) return kTypeEvent;
    if (std::strcmp(type, "Semaphore") == 0) return kTypeSemaphore;
    if (std::strcmp(type, "Mutant") == 0) return kTypeMutant;
    if (std::strcmp(type, "Thread") == 0) return kTypeThread;
    return type[0] ? kTypeOther : kTypeUnknown;
}

inline const char* object_type_text(unsigned id) {
    switch (id) {
        case kTypeEvent: return "Event";
        case kTypeSemaphore: return "Semaphore";
        case kTypeMutant: return "Mutant";
        case kTypeThread: return "Thread";
        case kTypeOther: return "other";
        default: return "unknown";
    }
}

// "Event set" / "Event not-set" / "Semaphore count N" / "Mutant count N"; empty when the handle cannot be queried.
inline void object_state_text(HANDLE handle, const char* type, char* out, std::size_t cap) {
    out[0] = '\0';
    if (!type[0] || !handle || handle == INVALID_HANDLE_VALUE) return;
    struct Info {
        ULONG a, b;
    } info{};
    if (std::strcmp(type, "Event") == 0) {
        static auto query = reinterpret_cast<NtQueryInfoFn>(nt_proc("NtQueryEvent"));
        if (query && query(handle, 0 /*EventBasicInformation*/, &info, sizeof(info), nullptr) >= 0)
            std::snprintf(out, cap, "Event %s", info.b ? "set" : "not-set");
    } else if (std::strcmp(type, "Semaphore") == 0) {
        static auto query = reinterpret_cast<NtQueryInfoFn>(nt_proc("NtQuerySemaphore"));
        if (query && query(handle, 0 /*SemaphoreBasicInformation*/, &info, sizeof(info), nullptr) >= 0)
            std::snprintf(out, cap, "Semaphore count %lu", static_cast<unsigned long>(info.a));
    } else if (std::strcmp(type, "Mutant") == 0) {
        static auto query = reinterpret_cast<NtQueryInfoFn>(nt_proc("NtQueryMutant"));
        if (query && query(handle, 0 /*MutantBasicInformation*/, &info, sizeof(info), nullptr) >= 0)
            std::snprintf(out, cap, "Mutant count %lu", static_cast<unsigned long>(info.a));
    }
}

inline unsigned state_code_of(const char* text) {
    if (std::strstr(text, "not-set")) return 2;
    if (std::strstr(text, " set")) return 1;
    return 0;
}

inline HANDLE as_handle(unsigned long long object) {
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(object));
}

// ---- call-site capture ------------------------------------------------------------------------------------------
struct Sites {
    unsigned caller_rva = 0;
    unsigned title_rva = 0;
    unsigned thread_start_rva = 0;
};

inline bool in_image(std::uint64_t address) {
    const State& s = state();
    return address >= s.image_base && address < s.image_end;
}

inline unsigned rva_of(std::uint64_t address) {
    return static_cast<unsigned>(address - state().image_base);
}

// The thread's start address, once per thread. It distinguishes a title-created thread whose current stack has no title
// frame (a wait inside a CRT/library callback) from a host thread.
inline unsigned thread_start_rva() {
    thread_local unsigned cached = kNoSite;
    if (cached != kNoSite) return cached;
    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    static auto query = reinterpret_cast<NtQueryInformationThreadFn>(nt_proc("NtQueryInformationThread"));
    unsigned rva = 0;
    void* start = nullptr;
    if (query && query(GetCurrentThread(), 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) >= 0 &&
        in_image(reinterpret_cast<std::uint64_t>(start)))
        rva = rva_of(reinterpret_cast<std::uint64_t>(start));
    cached = rva;
    return rva;
}

// RtlCaptureStackBackTrace is bounded by kStackFrames and only returns return addresses; every compiled frame here has
// unwind data. No allocation and no loader lock, so it is safe inside a wait wrapper.
inline Sites capture_sites() {
    Sites sites;
    void* frames[kStackFrames] = {};
    const USHORT count = RtlCaptureStackBackTrace(1, kStackFrames, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        const auto address = reinterpret_cast<std::uint64_t>(frames[i]);
        if (!in_image(address)) continue;
        if (i == 0) sites.caller_rva = rva_of(address);
        if (!sites.title_rva) sites.title_rva = rva_of(address);
    }
    sites.thread_start_rva = thread_start_rva();
    return sites;
}

// ---- table records (short critical sections; never with a stdio lock held by this thread) -----------------------
inline Aggregate* aggregate_for(unsigned api, unsigned site) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (auto& row : s.aggregates)
        if (row.used.load(std::memory_order_relaxed) && row.api.load(std::memory_order_relaxed) == api &&
            row.site.load(std::memory_order_relaxed) == site)
            return &row;
    for (auto& row : s.aggregates) {
        bool unused = false;
        if (row.used.compare_exchange_strong(unused, true, std::memory_order_acq_rel)) {
            row.api.store(api, std::memory_order_relaxed);
            row.site.store(site, std::memory_order_relaxed);
            return &row;
        }
    }
    return nullptr;
}

inline ObjectRec* object_for(unsigned long long object, unsigned kind, bool create) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (auto& row : s.objects)
        if (row.used.load(std::memory_order_relaxed) && row.object.load(std::memory_order_relaxed) == object) return &row;
    if (!create) return nullptr;
    for (auto& row : s.objects) {
        bool unused = false;
        if (row.used.compare_exchange_strong(unused, true, std::memory_order_acq_rel)) {
            row.object.store(object, std::memory_order_relaxed);
            row.kind.store(kind, std::memory_order_relaxed);
            return &row;
        }
    }
    return nullptr;
}

inline SignalSite* signal_site_for(unsigned api, unsigned site) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (auto& row : s.sites)
        if (row.used.load(std::memory_order_relaxed) && row.api.load(std::memory_order_relaxed) == api &&
            row.site.load(std::memory_order_relaxed) == site)
            return &row;
    for (auto& row : s.sites) {
        bool unused = false;
        if (row.used.compare_exchange_strong(unused, true, std::memory_order_acq_rel)) {
            row.api.store(api, std::memory_order_relaxed);
            row.site.store(site, std::memory_order_relaxed);
            return &row;
        }
    }
    return nullptr;
}

inline unsigned claim_thread_slot() {
    State& s = state();
    for (unsigned i = 0; i < kMaxThreads; ++i) {
        unsigned free_slot = 0xFFFFFFFFu;
        if (s.threads[i].api.compare_exchange_strong(free_slot, 0xFFFFFFFEu, std::memory_order_acq_rel)) return i;
    }
    return 0xFFFFFFFFu;
}

// ---- one wait record --------------------------------------------------------------------------------------------
struct Pending {
    bool active = false;
    unsigned slot = 0xFFFFFFFFu;
    unsigned api = 0;
    unsigned site = 0;
    unsigned caller = 0;
    unsigned count = 0;
    unsigned kind = 0;
    unsigned type_id = kTypeUnknown;
    unsigned state_code = 0;
    unsigned long long object = 0;
    unsigned long long start_ms = 0;
    long long timeout_ms = 0;
    bool detail = false;
};

inline long long timeout_from_nt(PLARGE_INTEGER timeout) {
    if (!timeout) return -1;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(timeout, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return -1;
    const long long value = timeout->QuadPart;
    if (value < 0) return -value / 10000;
    return -2;  // absolute time; reported as absolute rather than converted
}

inline Pending begin_wait(unsigned api, Kind kind, unsigned long long object, unsigned count, long long timeout_ms,
                          bool handle) {
    Pending pending;
    pending.api = api;
    pending.kind = kind;
    pending.count = count;
    pending.timeout_ms = timeout_ms;
    pending.object = object;
    State& s = state();
    if (!s.started.load(std::memory_order_relaxed) || s.dumping.load(std::memory_order_relaxed)) return pending;
    const Sites sites = capture_sites();
    pending.site = sites.title_rva;
    pending.caller = sites.caller_rva;
    if (!sites.title_rva && !sites.thread_start_rva) {
        s.host_calls.fetch_add(1, std::memory_order_relaxed);
        return pending;  // host thread: counted only
    }
    pending.active = true;
    pending.start_ms = now_ms();
    Aggregate* row = aggregate_for(api, sites.title_rva ? sites.title_rva : kNoSite);
    if (row) {
        row->calls.fetch_add(1, std::memory_order_relaxed);
        const unsigned long long total = s.detail_count.load(std::memory_order_relaxed);
        if (row->details.load(std::memory_order_relaxed) < kDetailPerKey && total < kDetailTotal) {
            row->details.fetch_add(1, std::memory_order_relaxed);
            s.detail_count.fetch_add(1, std::memory_order_relaxed);
            pending.detail = true;
        }
    }
    if (handle && object) {
        char type[64] = {};
        object_type_name(as_handle(object), type, sizeof(type));
        char state[64] = {};
        object_state_text(as_handle(object), type, state, sizeof(state));
        pending.type_id = object_type_id(type);
        pending.state_code = state_code_of(state);
        ObjectRec* record = object_for(object, kind, true);
        if (record) {
            record->waits.fetch_add(1, std::memory_order_relaxed);
            record->waiters.fetch_add(1, std::memory_order_relaxed);
            record->type.store(pending.type_id, std::memory_order_relaxed);
            if (pending.state_code) record->state_seen.store(pending.state_code, std::memory_order_relaxed);
        }
    } else if (!handle && object) {
        ObjectRec* record = object_for(object, kind, true);
        if (record) {
            record->waits.fetch_add(1, std::memory_order_relaxed);
            record->waiters.fetch_add(1, std::memory_order_relaxed);
        }
    }
    pending.slot = claim_thread_slot();
    if (pending.slot != 0xFFFFFFFFu) {
        Outstanding& slot = s.threads[pending.slot];
        slot.tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        slot.start_ms.store(pending.start_ms, std::memory_order_relaxed);
        slot.object.store(object, std::memory_order_relaxed);
        slot.kind.store(kind, std::memory_order_relaxed);
        slot.type.store(pending.type_id, std::memory_order_relaxed);
        slot.site.store(pending.site, std::memory_order_relaxed);
        slot.count.store(count, std::memory_order_relaxed);
        slot.timeout_ms.store(pending.timeout_ms, std::memory_order_relaxed);
        slot.api.store(api, std::memory_order_release);
        s.titled_threads.fetch_add(1, std::memory_order_relaxed);
    }
    return pending;
}

inline void finish_wait(Pending& pending, long result, Flavor flavor) {
    if (!pending.active) return;
    State& s = state();
    const unsigned long long duration = now_ms() - pending.start_ms;
    Outcome outcome = kOtherOutcome;
    if (flavor == kWin32Wait) {
        if (result == 0x102) outcome = kTimedOut;
        else if (result >= 0 && result < 0xC0) outcome = kSignalled;
    } else if (flavor == kNtWait) {
        if (result >= 0) outcome = kSignalled;
        else if (static_cast<unsigned long>(result) == 0x102) outcome = kTimedOut;
    } else {
        if (result) outcome = kSignalled;
        else outcome = GetLastError() == 1460 /*ERROR_TIMEOUT*/ ? kTimedOut : kOtherOutcome;
    }
    Aggregate* row = aggregate_for(pending.api, pending.site ? pending.site : kNoSite);
    if (row) {
        if (outcome == kSignalled) row->signalled.fetch_add(1, std::memory_order_relaxed);
        else if (outcome == kTimedOut) row->timed_out.fetch_add(1, std::memory_order_relaxed);
        else row->other.fetch_add(1, std::memory_order_relaxed);
        row->last_ms.store(now_ms(), std::memory_order_relaxed);
        row->last_duration_ms.store(duration, std::memory_order_relaxed);
    }
    if (pending.detail) {
        char timeout[32];
        if (pending.timeout_ms == -1) std::snprintf(timeout, sizeof timeout, "INF");
        else if (pending.timeout_ms == -2) std::snprintf(timeout, sizeof timeout, "abs");
        else std::snprintf(timeout, sizeof timeout, "%lld", pending.timeout_ms);
        std::fprintf(stderr,
                     "[rt] waitrec t=%llums tid=%lu api=%s site=%s caller=%s object=%s0x%llx(%s%s) n=%u timeout=%s "
                     "waited=%llums result=%s\n",
                     static_cast<unsigned long long>(pending.start_ms), static_cast<unsigned long>(GetCurrentThreadId()),
                     kApiNames[pending.api], site_text(pending.site), site_text(pending.caller),
                     pending.kind == kWaitAddress ? "addr " : "handle ",
                     static_cast<unsigned long long>(pending.object), object_type_text(pending.type_id),
                     pending.state_code == 1 ? " set" : pending.state_code == 2 ? " not-set" : "", pending.count,
                     timeout, static_cast<unsigned long long>(duration),
                     outcome == kSignalled ? "signalled" : outcome == kTimedOut ? "timeout" : "other");
    }
    if (pending.slot != 0xFFFFFFFFu) s.threads[pending.slot].api.store(0xFFFFFFFFu, std::memory_order_release);
}

// ---- one set/wake record ----------------------------------------------------------------------------------------
inline void record_signal(unsigned api, Kind kind, unsigned long long object, bool handle) {
    State& s = state();
    if (!s.started.load(std::memory_order_relaxed) || s.dumping.load(std::memory_order_relaxed) || !object) return;
    const Sites sites = capture_sites();
    if (!sites.title_rva && !sites.thread_start_rva) return;
    (void)handle;
    SignalSite* site = signal_site_for(api, sites.title_rva ? sites.title_rva : kNoSite);
    if (site) {
        site->calls.fetch_add(1, std::memory_order_relaxed);
        if (!site->caller.load(std::memory_order_relaxed) && sites.caller_rva)
            site->caller.store(sites.caller_rva, std::memory_order_relaxed);
    }
    ObjectRec* record = object_for(object, kind, false);
    if (!record) {
        s.unmatched_sets.fetch_add(1, std::memory_order_relaxed);
        if (api < kMaxApiCounters) s.unmatched_by_api[api].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    record->sets.fetch_add(1, std::memory_order_relaxed);
    record->last_set_ms.store(now_ms(), std::memory_order_relaxed);
    record->last_set_api.store(api, std::memory_order_relaxed);
    record->last_set_site.store(sites.title_rva ? sites.title_rva : kNoSite, std::memory_order_relaxed);
    if (site) site->matched.fetch_add(1, std::memory_order_relaxed);
}

// ---- reentrancy guard -------------------------------------------------------------------------------------------
// An outer wrapper traces; a wait the traced call itself makes (a Win32 wrapper's own Nt wait, the stdio lock) only
// runs the original, so one logical wait is one record.
inline bool& hook_busy() {
    thread_local bool busy = false;
    return busy;
}

struct BusyGuard {
    bool outer;
    BusyGuard() : outer(!hook_busy()) {
        hook_busy() = true;
    }
    ~BusyGuard() {
        if (outer) hook_busy() = false;
    }
};

// ---- typed originals (the address each import slot held before the patch) ----------------------------------------
using FnW1 = DWORD(WINAPI*)(HANDLE, DWORD);
using FnW1Ex = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
using FnWM = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD);
using FnWMEx = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD, BOOL);
using FnSleep = void(WINAPI*)(DWORD);
using FnSleepEx = DWORD(WINAPI*)(DWORD, BOOL);
using FnWaitOnAddress = BOOL(WINAPI*)(volatile void*, void*, SIZE_T, DWORD);
using FnSleepCV = BOOL(WINAPI*)(PCONDITION_VARIABLE, PSRWLOCK, DWORD, ULONG);
using FnSleepCVCS = BOOL(WINAPI*)(PCONDITION_VARIABLE, PCRITICAL_SECTION, DWORD);
using FnSetEvent = BOOL(WINAPI*)(HANDLE);
using FnReleaseSemaphore = BOOL(WINAPI*)(HANDLE, LONG, LPLONG);
using FnReleaseMutex = BOOL(WINAPI*)(HANDLE);
using FnWakeAddress = void(WINAPI*)(void*);
using FnWakeCV = void(WINAPI*)(PCONDITION_VARIABLE);
using FnNtWait1 = LONG(NTAPI*)(HANDLE, BOOLEAN, PLARGE_INTEGER);
using FnNtWaitM = LONG(NTAPI*)(ULONG, PHANDLE, ULONG, BOOLEAN, PLARGE_INTEGER);
using FnNtDelay = LONG(NTAPI*)(BOOLEAN, PLARGE_INTEGER);
using FnNtAlert = LONG(NTAPI*)(void*, PLARGE_INTEGER);
using FnNtSetEvent = LONG(NTAPI*)(HANDLE, PLONG);
using FnNtReleaseSemaphore = LONG(NTAPI*)(HANDLE, ULONG, PULONG);
using FnNtReleaseMutant = LONG(NTAPI*)(HANDLE, PLONG);
using FnRtlSleepCV = LONG(NTAPI*)(PCONDITION_VARIABLE, PSRWLOCK, PLARGE_INTEGER, ULONG);
using FnRtlWake = void(NTAPI*)(PCONDITION_VARIABLE);

inline FnW1& orig_WaitForSingleObject() {
    static FnW1 fn = nullptr;
    return fn;
}
inline FnW1Ex& orig_WaitForSingleObjectEx() {
    static FnW1Ex fn = nullptr;
    return fn;
}
inline FnWM& orig_WaitForMultipleObjects() {
    static FnWM fn = nullptr;
    return fn;
}
inline FnWMEx& orig_WaitForMultipleObjectsEx() {
    static FnWMEx fn = nullptr;
    return fn;
}
inline FnSleep& orig_Sleep() {
    static FnSleep fn = nullptr;
    return fn;
}
inline FnSleepEx& orig_SleepEx() {
    static FnSleepEx fn = nullptr;
    return fn;
}
inline FnWaitOnAddress& orig_WaitOnAddress() {
    static FnWaitOnAddress fn = nullptr;
    return fn;
}
inline FnSleepCV& orig_SleepConditionVariableSRW() {
    static FnSleepCV fn = nullptr;
    return fn;
}
inline FnSleepCVCS& orig_SleepConditionVariableCS() {
    static FnSleepCVCS fn = nullptr;
    return fn;
}
inline FnSetEvent& orig_SetEvent() {
    static FnSetEvent fn = nullptr;
    return fn;
}
inline FnSetEvent& orig_PulseEvent() {
    static FnSetEvent fn = nullptr;
    return fn;
}
inline FnReleaseSemaphore& orig_ReleaseSemaphore() {
    static FnReleaseSemaphore fn = nullptr;
    return fn;
}
inline FnReleaseMutex& orig_ReleaseMutex() {
    static FnReleaseMutex fn = nullptr;
    return fn;
}
inline FnWakeAddress& orig_WakeByAddressSingle() {
    static FnWakeAddress fn = nullptr;
    return fn;
}
inline FnWakeAddress& orig_WakeByAddressAll() {
    static FnWakeAddress fn = nullptr;
    return fn;
}
inline FnWakeAddress& orig_RtlWakeAddressSingle() {
    static FnWakeAddress fn = nullptr;
    return fn;
}
inline FnWakeAddress& orig_RtlWakeAddressAll() {
    static FnWakeAddress fn = nullptr;
    return fn;
}
inline FnWakeCV& orig_WakeConditionVariable() {
    static FnWakeCV fn = nullptr;
    return fn;
}
inline FnWakeCV& orig_WakeAllConditionVariable() {
    static FnWakeCV fn = nullptr;
    return fn;
}
inline FnWakeCV& orig_RtlWakeConditionVariable() {
    static FnWakeCV fn = nullptr;
    return fn;
}
inline FnWakeCV& orig_RtlWakeAllConditionVariable() {
    static FnWakeCV fn = nullptr;
    return fn;
}
inline FnNtWait1& orig_NtWaitForSingleObject() {
    static FnNtWait1 fn = nullptr;
    return fn;
}
inline FnNtWaitM& orig_NtWaitForMultipleObjects() {
    static FnNtWaitM fn = nullptr;
    return fn;
}
inline FnNtAlert& orig_NtWaitForAlertByThreadId() {
    static FnNtAlert fn = nullptr;
    return fn;
}
inline FnNtDelay& orig_NtDelayExecution() {
    static FnNtDelay fn = nullptr;
    return fn;
}
inline FnNtSetEvent& orig_NtSetEvent() {
    static FnNtSetEvent fn = nullptr;
    return fn;
}
inline FnNtSetEvent& orig_NtPulseEvent() {
    static FnNtSetEvent fn = nullptr;
    return fn;
}
inline FnNtReleaseSemaphore& orig_NtReleaseSemaphore() {
    static FnNtReleaseSemaphore fn = nullptr;
    return fn;
}
inline FnNtReleaseMutant& orig_NtReleaseMutant() {
    static FnNtReleaseMutant fn = nullptr;
    return fn;
}
inline FnRtlSleepCV& orig_RtlSleepConditionVariableSRW() {
    static FnRtlSleepCV fn = nullptr;
    return fn;
}

// ---- wrappers ---------------------------------------------------------------------------------------------------
#define WT_U64(value) static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(value))
#define WT_MS(ms) ((ms) == 0xFFFFFFFFu ? -1LL : static_cast<long long>(ms))

static DWORD WINAPI w_WaitForSingleObject(HANDLE handle, DWORD ms) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer) pending = begin_wait(aWaitForSingleObject, kWaitSingle, WT_U64(handle), 1, WT_MS(ms), true);
    const DWORD result = orig_WaitForSingleObject()(handle, ms);
    if (guard.outer) finish_wait(pending, static_cast<long>(result), kWin32Wait);
    return result;
}
static DWORD WINAPI w_WaitForSingleObjectEx(HANDLE handle, DWORD ms, BOOL alertable) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer) pending = begin_wait(aWaitForSingleObjectEx, kWaitSingle, WT_U64(handle), 1, WT_MS(ms), true);
    const DWORD result = orig_WaitForSingleObjectEx()(handle, ms, alertable);
    if (guard.outer) finish_wait(pending, static_cast<long>(result), kWin32Wait);
    return result;
}
static DWORD WINAPI w_WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer)
        pending = begin_wait(aWaitForMultipleObjects, kWaitMulti, count && handles ? WT_U64(handles[0]) : 0, count,
                             WT_MS(ms), true);
    const DWORD result = orig_WaitForMultipleObjects()(count, handles, wait_all, ms);
    if (guard.outer) finish_wait(pending, static_cast<long>(result), kWin32Wait);
    return result;
}
static DWORD WINAPI w_WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms,
                                               BOOL alertable) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer)
        pending = begin_wait(aWaitForMultipleObjectsEx, kWaitMulti, count && handles ? WT_U64(handles[0]) : 0, count,
                             WT_MS(ms), true);
    const DWORD result = orig_WaitForMultipleObjectsEx()(count, handles, wait_all, ms, alertable);
    if (guard.outer) finish_wait(pending, static_cast<long>(result), kWin32Wait);
    return result;
}
static void WINAPI w_Sleep(DWORD ms) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer) pending = begin_wait(aSleep, kDelay, 0, 0, static_cast<long long>(ms), false);
    orig_Sleep()(ms);
    if (guard.outer) finish_wait(pending, 0, kWin32Wait);
}
static DWORD WINAPI w_SleepEx(DWORD ms, BOOL alertable) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer) pending = begin_wait(aSleepEx, kDelay, 0, 0, WT_MS(ms), false);
    const DWORD result = orig_SleepEx()(ms, alertable);
    if (guard.outer) finish_wait(pending, result == 0 ? 0 : 0x102, kWin32Wait);
    return result;
}
static BOOL WINAPI w_WaitOnAddress(volatile void* address, void* compare, SIZE_T size, DWORD ms) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer) pending = begin_wait(aWaitOnAddress, kWaitAddress, WT_U64(address), 1, WT_MS(ms), false);
    const BOOL result = orig_WaitOnAddress()(address, compare, size, ms);
    if (guard.outer) finish_wait(pending, result, kBoolWait);
    return result;
}
static BOOL WINAPI w_SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer) pending = begin_wait(aSleepConditionVariableSRW, kWaitAddress, WT_U64(cv), 1, WT_MS(ms), false);
    const BOOL result = orig_SleepConditionVariableSRW()(cv, lock, ms, flags);
    if (guard.outer) finish_wait(pending, result, kBoolWait);
    return result;
}
static BOOL WINAPI w_SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer) pending = begin_wait(aSleepConditionVariableCS, kWaitAddress, WT_U64(cv), 1, WT_MS(ms), false);
    const BOOL result = orig_SleepConditionVariableCS()(cv, cs, ms);
    if (guard.outer) finish_wait(pending, result, kBoolWait);
    return result;
}
static LONG NTAPI w_NtWaitForSingleObject(HANDLE handle, BOOLEAN alertable, PLARGE_INTEGER timeout) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer)
        pending = begin_wait(aNtWaitForSingleObject, kWaitSingle, WT_U64(handle), 1, timeout_from_nt(timeout), true);
    const LONG result = orig_NtWaitForSingleObject()(handle, alertable, timeout);
    if (guard.outer) finish_wait(pending, result, kNtWait);
    return result;
}
static LONG NTAPI w_NtWaitForMultipleObjects(ULONG count, PHANDLE handles, ULONG wait_type, BOOLEAN alertable,
                                             PLARGE_INTEGER timeout) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer)
        pending = begin_wait(aNtWaitForMultipleObjects, kWaitMulti, count && handles ? WT_U64(handles[0]) : 0, count,
                             timeout_from_nt(timeout), true);
    const LONG result = orig_NtWaitForMultipleObjects()(count, handles, wait_type, alertable, timeout);
    if (guard.outer) finish_wait(pending, result, kNtWait);
    return result;
}
static LONG NTAPI w_NtWaitForAlertByThreadId(void* address, PLARGE_INTEGER timeout) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer)
        pending = begin_wait(aNtWaitForAlertByThreadId, kWaitAddress, WT_U64(address), 1, timeout_from_nt(timeout), false);
    const LONG result = orig_NtWaitForAlertByThreadId()(address, timeout);
    if (guard.outer) finish_wait(pending, result, kNtWait);
    return result;
}
static LONG NTAPI w_NtDelayExecution(BOOLEAN alertable, PLARGE_INTEGER timeout) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer) pending = begin_wait(aNtDelayExecution, kDelay, 0, 0, timeout_from_nt(timeout), false);
    const LONG result = orig_NtDelayExecution()(alertable, timeout);
    if (guard.outer) finish_wait(pending, result, kNtWait);
    return result;
}
static LONG NTAPI w_RtlSleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, PLARGE_INTEGER timeout,
                                                 ULONG flags) {
    BusyGuard guard;
    Pending pending;
    if (guard.outer)
        pending = begin_wait(aRtlSleepConditionVariableSRW, kWaitAddress, WT_U64(cv), 1, timeout_from_nt(timeout), false);
    const LONG result = orig_RtlSleepConditionVariableSRW()(cv, lock, timeout, flags);
    if (guard.outer) finish_wait(pending, result, kNtWait);
    return result;
}
static BOOL WINAPI w_SetEvent(HANDLE handle) {
    BusyGuard guard;
    if (guard.outer) record_signal(aSetEvent, kSetHandle, WT_U64(handle), true);
    return orig_SetEvent()(handle);
}
static BOOL WINAPI w_PulseEvent(HANDLE handle) {
    BusyGuard guard;
    if (guard.outer) record_signal(aPulseEvent, kSetHandle, WT_U64(handle), true);
    return orig_PulseEvent()(handle);
}
static BOOL WINAPI w_ReleaseSemaphore(HANDLE handle, LONG count, LPLONG previous) {
    BusyGuard guard;
    if (guard.outer) record_signal(aReleaseSemaphore, kSetHandle, WT_U64(handle), true);
    return orig_ReleaseSemaphore()(handle, count, previous);
}
static BOOL WINAPI w_ReleaseMutex(HANDLE handle) {
    BusyGuard guard;
    if (guard.outer) record_signal(aReleaseMutex, kSetHandle, WT_U64(handle), true);
    return orig_ReleaseMutex()(handle);
}
static LONG NTAPI w_NtSetEvent(HANDLE handle, PLONG previous) {
    BusyGuard guard;
    if (guard.outer) record_signal(aNtSetEvent, kSetHandle, WT_U64(handle), true);
    return orig_NtSetEvent()(handle, previous);
}
static LONG NTAPI w_NtPulseEvent(HANDLE handle, PLONG previous) {
    BusyGuard guard;
    if (guard.outer) record_signal(aNtPulseEvent, kSetHandle, WT_U64(handle), true);
    return orig_NtPulseEvent()(handle, previous);
}
static LONG NTAPI w_NtReleaseSemaphore(HANDLE handle, ULONG count, PULONG previous) {
    BusyGuard guard;
    if (guard.outer) record_signal(aNtReleaseSemaphore, kSetHandle, WT_U64(handle), true);
    return orig_NtReleaseSemaphore()(handle, count, previous);
}
static LONG NTAPI w_NtReleaseMutant(HANDLE handle, PLONG previous) {
    BusyGuard guard;
    if (guard.outer) record_signal(aNtReleaseMutant, kSetHandle, WT_U64(handle), true);
    return orig_NtReleaseMutant()(handle, previous);
}
static void WINAPI w_WakeByAddressSingle(void* address) {
    BusyGuard guard;
    if (guard.outer) record_signal(aWakeByAddressSingle, kSetAddress, WT_U64(address), false);
    orig_WakeByAddressSingle()(address);
}
static void WINAPI w_WakeByAddressAll(void* address) {
    BusyGuard guard;
    if (guard.outer) record_signal(aWakeByAddressAll, kSetAddress, WT_U64(address), false);
    orig_WakeByAddressAll()(address);
}
static void NTAPI w_RtlWakeAddressSingle(void* address) {
    BusyGuard guard;
    if (guard.outer) record_signal(aRtlWakeAddressSingle, kSetAddress, WT_U64(address), false);
    orig_RtlWakeAddressSingle()(address);
}
static void NTAPI w_RtlWakeAddressAll(void* address) {
    BusyGuard guard;
    if (guard.outer) record_signal(aRtlWakeAddressAll, kSetAddress, WT_U64(address), false);
    orig_RtlWakeAddressAll()(address);
}
static void WINAPI w_WakeConditionVariable(PCONDITION_VARIABLE cv) {
    BusyGuard guard;
    if (guard.outer) record_signal(aWakeConditionVariable, kSetAddress, WT_U64(cv), false);
    orig_WakeConditionVariable()(cv);
}
static void WINAPI w_WakeAllConditionVariable(PCONDITION_VARIABLE cv) {
    BusyGuard guard;
    if (guard.outer) record_signal(aWakeAllConditionVariable, kSetAddress, WT_U64(cv), false);
    orig_WakeAllConditionVariable()(cv);
}
static void NTAPI w_RtlWakeConditionVariable(PCONDITION_VARIABLE cv) {
    BusyGuard guard;
    if (guard.outer) record_signal(aRtlWakeConditionVariable, kSetAddress, WT_U64(cv), false);
    orig_RtlWakeConditionVariable()(cv);
}
static void NTAPI w_RtlWakeAllConditionVariable(PCONDITION_VARIABLE cv) {
    BusyGuard guard;
    if (guard.outer) record_signal(aRtlWakeAllConditionVariable, kSetAddress, WT_U64(cv), false);
    orig_RtlWakeAllConditionVariable()(cv);
}

// ---- import patching --------------------------------------------------------------------------------------------
struct Target {
    const char* name;
    Kind kind;
    void* wrapper;
    void** original;
    bool patched;
};

inline Target* targets(unsigned& count) {
    static Target table[] = {
        {"WaitForSingleObject", kWaitSingle, reinterpret_cast<void*>(&w_WaitForSingleObject),
         reinterpret_cast<void**>(&orig_WaitForSingleObject()), false},
        {"WaitForSingleObjectEx", kWaitSingle, reinterpret_cast<void*>(&w_WaitForSingleObjectEx),
         reinterpret_cast<void**>(&orig_WaitForSingleObjectEx()), false},
        {"WaitForMultipleObjects", kWaitMulti, reinterpret_cast<void*>(&w_WaitForMultipleObjects),
         reinterpret_cast<void**>(&orig_WaitForMultipleObjects()), false},
        {"WaitForMultipleObjectsEx", kWaitMulti, reinterpret_cast<void*>(&w_WaitForMultipleObjectsEx),
         reinterpret_cast<void**>(&orig_WaitForMultipleObjectsEx()), false},
        {"WaitOnAddress", kWaitAddress, reinterpret_cast<void*>(&w_WaitOnAddress),
         reinterpret_cast<void**>(&orig_WaitOnAddress()), false},
        {"SleepConditionVariableSRW", kWaitAddress, reinterpret_cast<void*>(&w_SleepConditionVariableSRW),
         reinterpret_cast<void**>(&orig_SleepConditionVariableSRW()), false},
        {"SleepConditionVariableCS", kWaitAddress, reinterpret_cast<void*>(&w_SleepConditionVariableCS),
         reinterpret_cast<void**>(&orig_SleepConditionVariableCS()), false},
        {"Sleep", kDelay, reinterpret_cast<void*>(&w_Sleep), reinterpret_cast<void**>(&orig_Sleep()), false},
        {"SleepEx", kDelay, reinterpret_cast<void*>(&w_SleepEx), reinterpret_cast<void**>(&orig_SleepEx()), false},
        {"NtWaitForSingleObject", kWaitSingle, reinterpret_cast<void*>(&w_NtWaitForSingleObject),
         reinterpret_cast<void**>(&orig_NtWaitForSingleObject()), false},
        {"NtWaitForMultipleObjects", kWaitMulti, reinterpret_cast<void*>(&w_NtWaitForMultipleObjects),
         reinterpret_cast<void**>(&orig_NtWaitForMultipleObjects()), false},
        {"NtWaitForAlertByThreadId", kWaitAddress, reinterpret_cast<void*>(&w_NtWaitForAlertByThreadId),
         reinterpret_cast<void**>(&orig_NtWaitForAlertByThreadId()), false},
        {"NtDelayExecution", kDelay, reinterpret_cast<void*>(&w_NtDelayExecution),
         reinterpret_cast<void**>(&orig_NtDelayExecution()), false},
        {"RtlSleepConditionVariableSRW", kWaitAddress, reinterpret_cast<void*>(&w_RtlSleepConditionVariableSRW),
         reinterpret_cast<void**>(&orig_RtlSleepConditionVariableSRW()), false},
        {"SetEvent", kSetHandle, reinterpret_cast<void*>(&w_SetEvent),
         reinterpret_cast<void**>(&orig_SetEvent()), false},
        {"NtSetEvent", kSetHandle, reinterpret_cast<void*>(&w_NtSetEvent),
         reinterpret_cast<void**>(&orig_NtSetEvent()), false},
        {"PulseEvent", kSetHandle, reinterpret_cast<void*>(&w_PulseEvent),
         reinterpret_cast<void**>(&orig_PulseEvent()), false},
        {"NtPulseEvent", kSetHandle, reinterpret_cast<void*>(&w_NtPulseEvent),
         reinterpret_cast<void**>(&orig_NtPulseEvent()), false},
        {"ReleaseSemaphore", kSetHandle, reinterpret_cast<void*>(&w_ReleaseSemaphore),
         reinterpret_cast<void**>(&orig_ReleaseSemaphore()), false},
        {"NtReleaseSemaphore", kSetHandle, reinterpret_cast<void*>(&w_NtReleaseSemaphore),
         reinterpret_cast<void**>(&orig_NtReleaseSemaphore()), false},
        {"ReleaseMutex", kSetHandle, reinterpret_cast<void*>(&w_ReleaseMutex),
         reinterpret_cast<void**>(&orig_ReleaseMutex()), false},
        {"NtReleaseMutant", kSetHandle, reinterpret_cast<void*>(&w_NtReleaseMutant),
         reinterpret_cast<void**>(&orig_NtReleaseMutant()), false},
        {"WakeByAddressSingle", kSetAddress, reinterpret_cast<void*>(&w_WakeByAddressSingle),
         reinterpret_cast<void**>(&orig_WakeByAddressSingle()), false},
        {"WakeByAddressAll", kSetAddress, reinterpret_cast<void*>(&w_WakeByAddressAll),
         reinterpret_cast<void**>(&orig_WakeByAddressAll()), false},
        {"RtlWakeAddressSingle", kSetAddress, reinterpret_cast<void*>(&w_RtlWakeAddressSingle),
         reinterpret_cast<void**>(&orig_RtlWakeAddressSingle()), false},
        {"RtlWakeAddressAll", kSetAddress, reinterpret_cast<void*>(&w_RtlWakeAddressAll),
         reinterpret_cast<void**>(&orig_RtlWakeAddressAll()), false},
        {"WakeConditionVariable", kSetAddress, reinterpret_cast<void*>(&w_WakeConditionVariable),
         reinterpret_cast<void**>(&orig_WakeConditionVariable()), false},
        {"WakeAllConditionVariable", kSetAddress, reinterpret_cast<void*>(&w_WakeAllConditionVariable),
         reinterpret_cast<void**>(&orig_WakeAllConditionVariable()), false},
        {"RtlWakeConditionVariable", kSetAddress, reinterpret_cast<void*>(&w_RtlWakeConditionVariable),
         reinterpret_cast<void**>(&orig_RtlWakeConditionVariable()), false},
        {"RtlWakeAllConditionVariable", kSetAddress, reinterpret_cast<void*>(&w_RtlWakeAllConditionVariable),
         reinterpret_cast<void**>(&orig_RtlWakeAllConditionVariable()), false},
    };
    count = sizeof(table) / sizeof(table[0]);
    return table;
}

// Rewrites the import slots named in the target table inside one mapped image. Each target keeps the first address it
// replaced; a slot that resolved somewhere else is left alone (and counted), so a differently bound copy is never
// redirected at a function it does not belong to.
inline void patch_image_imports(std::uint8_t* base, std::size_t size, bool count_skipped) {
    if (!base || size < 0x100) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.Size || !dir.VirtualAddress || dir.VirtualAddress >= size) return;
    unsigned target_count = 0;
    Target* table = targets(target_count);
    for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         descriptor->Name && reinterpret_cast<const std::uint8_t*>(descriptor) < base + size; ++descriptor) {
        if (!descriptor->OriginalFirstThunk) continue;  // no name table to match against
        if (descriptor->OriginalFirstThunk >= size || descriptor->FirstThunk >= size) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto name_rva = static_cast<std::size_t>(names->u1.AddressOfData) & 0x7FFFFFFFull;
            if (name_rva >= size) break;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name_rva);
            for (unsigned i = 0; i < target_count; ++i) {
                Target& target = table[i];
                if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), target.name) != 0) continue;
                auto* slot = reinterpret_cast<void**>(&iat->u1.Function);
                if (*slot == target.wrapper) break;
                if (!target.original || !*target.original) {
                    *target.original = *slot;
                    target.patched = true;
                } else if (*slot != *target.original) {
                    if (count_skipped) state().patched_slots.fetch_add(0, std::memory_order_relaxed);
                    break;  // differently bound copy: leave it alone
                }
                DWORD old = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) break;
                *slot = target.wrapper;
                VirtualProtect(slot, sizeof(void*), old, &old);
                state().patched_slots.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
}

inline void patch_loaded_modules(bool announce) {
    State& s = state();
    patch_image_imports(reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(s.image_base)),
                        static_cast<std::size_t>(s.image_end - s.image_base), false);
    HMODULE self = GetModuleHandleW(nullptr);
    unsigned modules = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry))
            do {
                if (entry.modBaseAddr == reinterpret_cast<BYTE*>(self)) continue;  // the host itself is not patched
                patch_image_imports(entry.modBaseAddr, entry.modBaseSize, false);
                ++modules;
            } while (Module32NextW(snapshot, &entry));
        CloseHandle(snapshot);
    }
    if (announce) {
        unsigned target_count = 0;
        Target* table = targets(target_count);
        unsigned patched = 0;
        for (unsigned i = 0; i < target_count; ++i) patched += table[i].patched ? 1 : 0;
        std::fprintf(stderr,
                     "[rt] wait trace: %u target API(s), %u of them patched, %llu import slot(s) in %u host module(s) "
                     "(note: ntdll/kernelbase resolve their own calls internally; the import boundary is what is traced)\n",
                     target_count, patched, static_cast<unsigned long long>(s.patched_slots.load()), modules);
    }
}

// ---- dump -------------------------------------------------------------------------------------------------------
inline void dump(const char* label) {
    State& s = state();
    if (s.dumping.exchange(true)) return;
    std::lock_guard<std::mutex> lock(s.mutex);
    std::fprintf(stderr,
                 "[rt] wait trace dump at %s s (t=%llums): title threads followed %u, host-only waits %llu, detail "
                 "lines %llu, unmatched set calls %lu, patched slots %llu\n",
                 label, static_cast<unsigned long long>(now_ms()), s.titled_threads.load(),
                 static_cast<unsigned long long>(s.host_calls.load()),
                 static_cast<unsigned long long>(s.detail_count.load()), s.unmatched_sets.load(),
                 static_cast<unsigned long long>(s.patched_slots.load()));
    std::fprintf(stderr, "[rt] wait aggregates at %s s: api | site | calls | signalled | timed-out | other | last ms | "
                         "last wait ms\n",
                 label);
    for (auto& row : s.aggregates) {
        if (!row.used.load(std::memory_order_relaxed)) continue;
        const unsigned api = row.api.load(std::memory_order_relaxed);
        std::fprintf(stderr, "[rt]   %s | %s | %lu | %lu | %lu | %lu | %llu | %llu\n",
                     api < kApiCount ? kApiNames[api] : "?", site_text(row.site.load(std::memory_order_relaxed)),
                     row.calls.load(std::memory_order_relaxed), row.signalled.load(std::memory_order_relaxed),
                     row.timed_out.load(std::memory_order_relaxed), row.other.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(row.last_ms.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(row.last_duration_ms.load(std::memory_order_relaxed)));
    }
    std::fprintf(stderr, "[rt] wait objects at %s s: object | kind | type | wait entries | waiters | set calls | last "
                         "set ms | last set api | last set site | state last seen at a wait entry\n",
                 label);
    for (auto& row : s.objects) {
        if (!row.used.load(std::memory_order_relaxed)) continue;
        const auto object = row.object.load(std::memory_order_relaxed);
        const unsigned kind = row.kind.load(std::memory_order_relaxed);
        char state[64] = {};
        if (kind != kWaitAddress) {
            char type[64] = {};
            object_type_name(as_handle(object), type, sizeof(type));
            object_state_text(as_handle(object), type, state, sizeof(state));
        }
        const unsigned last_api = row.last_set_api.load(std::memory_order_relaxed);
        std::fprintf(stderr, "[rt]   %s0x%llx | %s | %s | %lu | %lu | %lu | %llu | %s | %s | %s\n",
                     kind == kWaitAddress ? "addr " : "handle ", object, kind == kWaitAddress ? "address" : "handle",
                     object_type_text(row.type.load(std::memory_order_relaxed)),
                     row.waits.load(std::memory_order_relaxed), row.waiters.load(std::memory_order_relaxed),
                     row.sets.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(row.last_set_ms.load(std::memory_order_relaxed)),
                     last_api < kApiCount ? kApiNames[last_api] : "-",
                     site_text(row.last_set_site.load(std::memory_order_relaxed)), state[0] ? state : "-");
    }
    std::fprintf(stderr, "[rt] wait signal sites at %s s: api | site | caller | calls | matched tracked objects\n", label);
    for (auto& row : s.sites) {
        if (!row.used.load(std::memory_order_relaxed)) continue;
        const unsigned api = row.api.load(std::memory_order_relaxed);
        std::fprintf(stderr, "[rt]   %s | %s | %s | %lu | %lu\n", api < kApiCount ? kApiNames[api] : "?",
                     site_text(row.site.load(std::memory_order_relaxed)),
                     site_text(row.caller.load(std::memory_order_relaxed)), row.calls.load(std::memory_order_relaxed),
                     row.matched.load(std::memory_order_relaxed));
    }
    std::fprintf(stderr,
                 "[rt] wait outstanding at %s s: tid | api | object | type | timeout | site | elapsed ms | state now\n",
                 label);
    for (auto& slot : s.threads) {
        const unsigned api = slot.api.load(std::memory_order_acquire);
        if (api == 0xFFFFFFFFu || api == 0xFFFFFFFEu) continue;
        const auto object = slot.object.load(std::memory_order_relaxed);
        const unsigned kind = slot.kind.load(std::memory_order_relaxed);
        char state[64] = {};
        if (kind != kWaitAddress) {
            char type[64] = {};
            object_type_name(as_handle(object), type, sizeof(type));
            object_state_text(as_handle(object), type, state, sizeof(state));
        }
        const long long timeout = slot.timeout_ms.load(std::memory_order_relaxed);
        char timeout_text[32];
        if (timeout == -1) std::snprintf(timeout_text, sizeof timeout_text, "INF");
        else if (timeout == -2) std::snprintf(timeout_text, sizeof timeout_text, "abs");
        else std::snprintf(timeout_text, sizeof timeout_text, "%lld", timeout);
        const auto started = slot.start_ms.load(std::memory_order_relaxed);
        std::fprintf(stderr, "[rt]   %lu | %s | %s0x%llx | %s | %s | %s | %llu | %s\n",
                     slot.tid.load(std::memory_order_relaxed), api < kApiCount ? kApiNames[api] : "?",
                     kind == kWaitAddress ? "addr " : "handle ", object, object_type_text(slot.type.load()),
                     timeout_text, site_text(slot.site.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(now_ms() - started), state[0] ? state : "-");
    }
    if (s.unmatched_sets.load()) {
        std::fprintf(stderr, "[rt] wait unmatched set calls at %s s:", label);
        for (unsigned api = 0; api < kMaxApiCounters && api < kApiCount; ++api)
            if (s.unmatched_by_api[api].load(std::memory_order_relaxed))
                std::fprintf(stderr, " %s=%lu", kApiNames[api],
                             s.unmatched_by_api[api].load(std::memory_order_relaxed));
        std::fprintf(stderr, "\n");
    }
    s.dumping.store(false);
}

// ---- start ------------------------------------------------------------------------------------------------------
inline unsigned parse_dump_times(const char* spec, double* times, unsigned capacity) {
    unsigned count = 0;
    if (!spec || !*spec) return 0;
    if (std::strcmp(spec, "1") == 0) {
        if (capacity >= 2) {
            times[0] = 60.0;
            times[1] = 120.0;
            return 2;
        }
        return 0;
    }
    const char* at = spec;
    while (at && *at && count < capacity) {
        char* end = nullptr;
        const double value = std::strtod(at, &end);
        if (end == at) break;
        if (value > 0.5 && value < 3600.0) times[count++] = value;
        at = *end == ',' ? end + 1 : end;
        if (*end == '\0') break;
    }
    return count;
}

// Installs the tracer: patches the mapped title image and every loaded host module, then runs the dump schedule and a
// bounded set of rescans for modules loaded after start. The title has not run a byte of code when this is called.
inline void start(std::uint64_t image_base, std::uint64_t image_size, const char* spec) {
    double times[kMaxDumps] = {};
    const unsigned time_count = parse_dump_times(spec, times, kMaxDumps);
    if (!time_count) return;
    State& s = state();
    if (s.started.exchange(true)) return;
    s.image_base = image_base;
    s.image_end = image_base + image_size;
    s.epoch_ms = GetTickCount64();
    patch_loaded_modules(true);
    std::thread([times, time_count] {
        for (unsigned i = 0; i < time_count; ++i) {
            const auto target = static_cast<unsigned long long>(times[i] * 1000.0);
            while (now_ms() < target) Sleep(10);
            char label[32];
            std::snprintf(label, sizeof label, "%.0f", times[i]);
            dump(label);
        }
    }).detach();
    std::thread([] {
        static const unsigned at_seconds[kMaxLateScans] = {1, 3, 10, 30, 60, 120};
        unsigned done = 0;
        for (unsigned i = 0; i < kMaxLateScans; ++i) {
            while (now_ms() < static_cast<unsigned long long>(at_seconds[i]) * 1000ull) Sleep(20);
            patch_loaded_modules(false);
            ++done;
        }
        (void)done;
    }).detach();
}

}  // namespace palmetto::wait_trace
