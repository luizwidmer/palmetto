// palmetto: native runtime for the target console build.
//
//   palmetto --dry-run <image> [--game-dir DIR] [--overlay DIR]
//       Maps the image and its bundled modules, applies relocations, binds every import, installs static TLS for
//       this thread and prints a report. No code from the mapped images
//       runs. TLS needs the runtime's own palmetto_tls_host.dll, which reserves the module TLS index; system DLLs are
//       loaded to resolve host imports, as before.
//
//   palmetto --init <image> [--game-dir DIR] [--overlay DIR]
//       The same mapping, then each loaded DLL's TLS callbacks and DllMain(DLL_PROCESS_ATTACH) are run.
//
//   palmetto --run <exe> [--game-dir DIR] [--overlay DIR] [--trace-d3d12] [--trace FILE] [-- game arguments...]
//       The same, then the EXE's own TLS callbacks and its entry point, on a game thread. The game's argv and
//       command line are <exe name> plus the arguments after "--"; exit/ExitProcess from the game end the run
//       (shims/process.cpp). palmetto prints the binding report first and returns the game's exit code.
//
// --game-dir is also where a title's GAME: paths read from; anything it writes or deletes goes to the overlay
// (--overlay, default <game-dir>/overlay) so the package is never modified (shims/file_system.cpp).
#include <windows.h>
#include <algorithm>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <chrono>
#include <intrin.h>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <system_error>
#include <vector>

#include "base_reservation.h"
#include "code_hooks.h"
#include "host_cpu.h"
#include "mods.h"
#include "pe_loader.h"
#include "wait_trace.h"
#include "../shims/d3d12_trace.h"
#include "../shims/d3d12_xs_device.h"
#include "../shims/file_system.h"
#include "../shims/process_shim.h"
#include "../shims/shims.h"
#include "../shims/startup_trace.h"
#include "../shims/xmem.h"

namespace {

const char* binding_name(rt::Binding b) {
    switch (b) {
        case rt::Binding::Shim: return "shim";
        case rt::Binding::Host: return "host";
        case rt::Binding::Module: return "module";
        default: return "UNIMPLEMENTED";
    }
}

int usage() {
    std::fprintf(stderr,
                 "usage: palmetto --dry-run <image> [--game-dir DIR] [--overlay DIR] [--mods DIR] [--archetype-lookup RVA[,ARG]] [--view-mode-getter RVA] [--trace FILE]\n"
                 "       palmetto --init <image> [--game-dir DIR] [--overlay DIR] [--mods DIR] [--archetype-lookup RVA[,ARG]] [--view-mode-getter RVA] [--trace-d3d12] [--trace FILE]\n"
                 "       palmetto --run <exe> [--game-dir DIR] [--overlay DIR] [--mods DIR] [--archetype-lookup RVA[,ARG]] [--view-mode-getter RVA] [--trace-d3d12] [--trace FILE] [-- game arguments...]\n");
    return 2;
}

}  // namespace

// The samplers' window times are absolute from this epoch (process start in practice), not from the sampler thread's
// own start, so a later window cannot drift when an earlier one runs long.
std::chrono::steady_clock::time_point sampler_epoch() {
    static const auto at = std::chrono::steady_clock::now();
    return at;
}

// ---- Wait-state sampler (PALMETTO_WAIT_SAMPLE) -----------------------------------------------------------------------
//
// Diagnostic, off unless the environment variable names comma-separated seconds ("5,10,20,40,60"; "1" means the
// default set). At each time it samples every other thread of the process: the instruction pointer, the four
// integer-argument registers, the thread's start address, and a shallow scan of the stack for return addresses that
// fall inside the mapped title image or a loaded module. A thread inside an ntdll/kernelbase wait stub is named with
// that wait API and, when the wait passes a HANDLE, with the object type that handle names. The output is a small
// per-sample site histogram plus one line per waiting thread, so one boot says which title function is blocked and on
// what. Counts, RVAs and handle values only; no game data.
namespace {

using NtQueryObjectFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);

struct WaitSampleApi {
    const char* name;
    std::uintptr_t address;
};

struct WaitSampleModules {
    std::uintptr_t image_base = 0, image_end = 0;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;  // host modules, for the stack filter
    bool in_code(std::uintptr_t address) const {
        if (address >= image_base && address < image_end) return true;
        for (const auto& range : ranges)
            if (address >= range.first && address < range.second) return true;
        return false;
    }
};

// Reads up to `cap` stack words at `rsp` and keeps the ones that look like code addresses. No C++ objects in this
// function, so __try is legal; each page is VirtualQuery'd before it is read (a guard page or an uncommitted page is
// skipped instead of faulted), so a bad stack end costs a skipped page, not a logged access violation.
unsigned scan_stack_words(std::uint64_t rsp, std::uint64_t* out, unsigned cap, const WaitSampleModules& modules) {
    unsigned found = 0;
    const std::uint64_t page = 0x1000;
    for (std::uint64_t at = rsp & ~(page - 1); at < rsp + 4096; at += page) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery((const void*)(std::uintptr_t)at, &info, sizeof(info)) != sizeof(info)) continue;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
        __try {
            const auto* words = reinterpret_cast<const std::uint64_t*>((std::uintptr_t)at);
            for (unsigned i = 0; i < page / 8 && found < cap; ++i) {
                const std::uint64_t value = words[i];
                if ((std::uintptr_t)value < rsp) continue;  // only the frames above this stack pointer
                if (modules.in_code((std::uintptr_t)value)) out[found++] = value;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return found;
        }
    }
    return found;
}

const char* wait_api_name(const WaitSampleApi* apis, unsigned count, std::uint64_t rip) {
    const WaitSampleApi* best = nullptr;
    for (unsigned i = 0; i < count; ++i) {
        const auto at = apis[i].address;
        if (rip >= at && rip < at + 0x40) {
            if (!best || apis[i].address > best->address) best = &apis[i];
        }
    }
    return best ? best->name : nullptr;
}

void resolve_wait_apis(std::vector<WaitSampleApi>& apis) {
    struct Request {
        const wchar_t* module;
        const char* name;
    };
    static const Request requests[] = {
        {L"ntdll.dll", "NtWaitForSingleObject"},
        {L"ntdll.dll", "NtWaitForMultipleObjects"},
        {L"ntdll.dll", "NtDelayExecution"},
        {L"ntdll.dll", "NtRemoveIoCompletion"},
        {L"ntdll.dll", "NtWaitForAlertByThreadId"},
        {L"ntdll.dll", "NtSignalAndWaitForSingleObject"},
        {L"kernelbase.dll", "WaitForSingleObjectEx"},
        {L"kernelbase.dll", "WaitForMultipleObjectsEx"},
        {L"kernelbase.dll", "SleepEx"},
        {L"kernelbase.dll", "GetOverlappedResult"},
        {L"kernelbase.dll", "GetQueuedCompletionStatus"},
        {L"kernelbase.dll", "GetQueuedCompletionStatusEx"},
        {L"kernelbase.dll", "SwitchToThread"},
        {L"kernel32.dll", "WaitForSingleObject"},
        {L"kernel32.dll", "WaitForMultipleObjects"},
        {L"kernel32.dll", "Sleep"},
    };
    for (const Request& request : requests) {
        const HMODULE module = GetModuleHandleW(request.module);
        if (!module) continue;
        if (FARPROC fn = GetProcAddress(module, request.name))
            apis.push_back({request.name, (std::uintptr_t)fn});
    }
}

// The handle-wait APIs whose first argument is a HANDLE (Sleep/SwitchToThread take a delay or nothing).
bool wait_takes_handle(const char* name) {
    if (!name) return false;
    return std::strstr(name, "NtWaitForSingleObject") || std::strstr(name, "NtSignalAndWaitForSingleObject") ||
           std::strstr(name, "WaitForSingleObject") || std::strstr(name, "WaitForMultipleObjects") ||
           std::strstr(name, "NtRemoveIoCompletion") || std::strstr(name, "GetQueuedCompletionStatus") ||
           std::strstr(name, "GetOverlappedResult");
}

// The object type a HANDLE names, from ntdll's ObjectTypeInformation: "Event", "Mutant", "Semaphore", "Thread",
// "IoCompletion", ... Empty when the handle is not ours to ask about.
void wait_object_type(HANDLE handle, char* out, std::size_t cap) {    out[0] = '\0';
    if (!handle) return;
    static NtQueryObjectFn query = reinterpret_cast<NtQueryObjectFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
    if (!query) return;
    struct TypeName {
        USHORT length;
        USHORT maximum;
        wchar_t* buffer;
    };
    unsigned char buffer[1024] = {};
    if (query(handle, 2 /*ObjectTypeInformation*/, buffer, sizeof(buffer), nullptr) < 0) return;
    TypeName name{};
    std::memcpy(&name, buffer, sizeof(name));
    if (!name.buffer || !name.length) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name.buffer, name.length / 2, out, static_cast<int>(cap) - 1,
                                          nullptr, nullptr);
    if (bytes > 0) out[bytes] = '\0';
}

struct WaitThreadSample {
    DWORD tid = 0;
    std::uint64_t rip = 0, rsp = 0, rcx = 0, rdx = 0, r8 = 0, r9 = 0, start = 0;
    unsigned stack_count = 0;
    std::uint64_t stack[8] = {};
};

// Reads up to `cap` handles from an array the wait call names. No C++ objects, so __try is legal; a bad pointer just
// yields no handles.
unsigned read_wait_handles(std::uint64_t pointer, HANDLE* out, unsigned cap) {
    if (!pointer) return 0;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery((const void*)(std::uintptr_t)pointer, &info, sizeof(info)) != sizeof(info)) return 0;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    unsigned found = 0;
    __try {
        const auto* handles = reinterpret_cast<const std::uint64_t*>((std::uintptr_t)pointer);
        for (unsigned i = 0; i < cap; ++i) out[found++] = (HANDLE)(std::uintptr_t)handles[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return found;
    }
    return found;
}

// One 64-bit word of the suspended thread's memory (a stack slot or a pointer argument). Page-checked, so a stale
// stack pointer is a skipped read, not a fault.
bool read_thread_word(std::uint64_t address, std::uint64_t& value) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery((const void*)(std::uintptr_t)address, &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    __try {
        value = *reinterpret_cast<const std::uint64_t*>((std::uintptr_t)address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Formats a relative NT timeout (100 ns units, negative) as milliseconds, an absolute one as "abs", a null pointer as
// "INF". Returns nullptr when the API does not carry one in a register this sampler can name.
const char* wait_timeout_text(const char* api, const WaitThreadSample& sample) {
    if (!api) return nullptr;
    static char buffer[32];
    auto from_large_integer = [](std::uint64_t pointer) -> const char* {
        if (!pointer) return "INF";
        std::uint64_t value = 0;
        if (!read_thread_word(pointer, value)) return "?";
        const auto amount = static_cast<long long>(value);
        if (amount < 0) {
            std::snprintf(buffer, sizeof buffer, "%lld", -amount / 10000);
            return buffer;
        }
        return "abs";
    };
    if (std::strstr(api, "NtWaitForSingleObject") || std::strstr(api, "NtWaitForAlertByThreadId") ||
        std::strstr(api, "NtDelayExecution"))
        return from_large_integer(sample.rdx);
    if (std::strstr(api, "NtWaitForMultipleObjects")) {
        std::uint64_t pointer = 0;  // the fifth argument sits at [rsp + 0x28] in the syscall stub
        if (!read_thread_word(sample.rsp + 0x28, pointer)) return "?";
        return from_large_integer(pointer);
    }
    unsigned long long ms = 0;
    bool found = true;
    if (std::strstr(api, "WaitForMultipleObjects"))
        ms = sample.r9;
    else if (std::strstr(api, "WaitForSingleObject") || std::strstr(api, "NtWaitForSingleObject"))
        ms = sample.rdx;
    else if (std::strstr(api, "SleepConditionVariableSRW"))
        ms = sample.r8;
    else if (std::strstr(api, "SleepEx") || std::strstr(api, "Sleep"))
        ms = sample.rcx;
    else if (std::strstr(api, "WaitOnAddress"))
        ms = sample.r9;
    else
        found = false;
    if (!found) return nullptr;
    if ((ms & 0xFFFFFFFFull) == 0xFFFFFFFFull) return "INF";
    std::snprintf(buffer, sizeof buffer, "%llu", ms & 0xFFFFFFFFull);
    return buffer;
}

void wait_state_sample_once(std::uintptr_t image_base, std::uintptr_t image_size,
                            const std::vector<WaitSampleApi>& apis, const char* at_seconds) {
    WaitSampleModules modules;
    modules.image_base = image_base;
    modules.image_end = image_base + image_size;
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Module32FirstW(snapshot, &entry))
                do {
                    modules.ranges.emplace_back((std::uintptr_t)entry.modBaseAddr,
                                                (std::uintptr_t)entry.modBaseAddr + entry.modBaseSize);
                } while (Module32NextW(snapshot, &entry));
            CloseHandle(snapshot);
        }
    }
    std::vector<DWORD> tids;
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD pid = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        if (Thread32First(snapshot, &entry))
            do {
                if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self && tids.size() < 256)
                    tids.push_back(entry.th32ThreadID);
            } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
    }
    static NtQueryInformationThreadFn query_thread = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    std::vector<WaitThreadSample> samples;
    samples.reserve(tids.size());  // allocated before any thread is suspended
    for (const DWORD tid : tids) {
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!thread) continue;
        if (SuspendThread(thread) == (DWORD)-1) {
            CloseHandle(thread);
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        WaitThreadSample sample;
        sample.tid = tid;
        if (GetThreadContext(thread, &context)) {
            sample.rip = context.Rip;
            sample.rsp = context.Rsp;
            sample.rcx = context.Rcx;
            sample.rdx = context.Rdx;
            sample.r8 = context.R8;
            sample.r9 = context.R9;
            if (query_thread) {
                void* start = nullptr;
                if (query_thread(thread, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) >= 0)
                    sample.start = (std::uint64_t)(std::uintptr_t)start;
            }
            sample.stack_count = scan_stack_words(sample.rsp, sample.stack, 8, modules);
        }
        ResumeThread(thread);
        CloseHandle(thread);
        if (sample.rip) samples.push_back(sample);
    }
    // Everything below runs with every thread resumed: module_offset() takes the registry lock.
    std::map<std::string, unsigned> sites;
    unsigned waiting = 0;
    std::string detail;
    for (const WaitThreadSample& sample : samples) {
        const std::string site = rt::shims::module_offset(sample.rip);
        ++sites[site];
        const char* api = wait_api_name(apis.data(), (unsigned)apis.size(), sample.rip);
        const bool interesting = api != nullptr || (sample.rip >= modules.image_base && sample.rip < modules.image_end);
        if (!interesting) continue;
        if (++waiting > 96) continue;  // bounded: the histogram above already names every site
        char type[64] = {};
        if (api && wait_takes_handle(api) && !std::strstr(api, "WaitForMultipleObjects"))
            wait_object_type((HANDLE)(std::uintptr_t)sample.rcx, type, sizeof(type));
        detail += "\n  tid " + std::to_string(sample.tid) + " rip " + site;
        if (api) detail += std::string(" wait ") + api;
        // The timeout the sampled call passes (registers survive in the ntdll stub), the address a wait-on-address wait
        // keys on, and the object's current state - so one sample says whether the wait is unbounded and what it is
        // waiting for, not just where it is blocked.
        const char* timeout = wait_timeout_text(api, sample);
        if (timeout) detail += std::string(" timeout ") + timeout;
        if (api && std::strstr(api, "NtWaitForAlertByThreadId")) {
            char key[32];
            std::snprintf(key, sizeof key, " key 0x%llx", (unsigned long long)sample.rcx);
            detail += key;
        }
        if (api && std::strstr(api, "WaitForMultipleObjects")) {
            char count[32];
            std::snprintf(count, sizeof count, " on %llu object(s)", (unsigned long long)sample.rcx);
            detail += count;
            HANDLE handles[4] = {};
            const unsigned found = read_wait_handles(sample.rdx, handles,
                                                     (unsigned)std::min<std::uint64_t>(sample.rcx, 4));
            for (unsigned i = 0; i < found; ++i) {
                char one[96];
                char kind[64] = {};
                wait_object_type(handles[i], kind, sizeof(kind));
                std::snprintf(one, sizeof one, " 0x%llx%s%s", (unsigned long long)(std::uintptr_t)handles[i],
                              kind[0] ? " (" : "", kind[0] ? kind : "");
                detail += one;
                if (kind[0]) detail += ")";
            }
        } else if (api && wait_takes_handle(api)) {
            char handle[32];
            std::snprintf(handle, sizeof handle, "0x%llx", (unsigned long long)sample.rcx);
            detail += std::string(" handle ") + handle;
            if (type[0]) detail += std::string(" (") + type + ")";
            char state[64] = {};
            palmetto::wait_trace::object_state_text((HANDLE)(std::uintptr_t)sample.rcx, type, state, sizeof(state));
            if (state[0]) detail += std::string(" state ") + state;
        }
        char start[48];
        std::snprintf(start, sizeof start, " start %s", rt::shims::module_offset(sample.start).c_str());
        detail += start;
        if (sample.stack_count) {
            detail += "\n         stack";
            for (unsigned i = 0; i < sample.stack_count; ++i)
                detail += " " + rt::shims::module_offset(sample.stack[i]);
        }
    }
    std::string histogram;
    unsigned shown = 0;
    for (const auto& [site, count] : sites) {
        if (shown++ == 12) break;
        histogram += "\n  " + std::to_string(count) + "x " + site;
    }
    std::fprintf(stderr,
                 "[rt] wait sample at %s s: %zu thread(s), %u in a wait/busy title site, %zu distinct RIP site(s)%s\n"
                 "[rt] wait sites at %s s:%s\n",
                 at_seconds, samples.size(), waiting, sites.size(),
                 detail.empty() ? "" : " (details below)", at_seconds, histogram.c_str());
    if (!detail.empty()) std::fprintf(stderr, "[rt] wait detail at %s s:%s\n", at_seconds, detail.c_str());
}

// Starts the sampler thread when PALMETTO_WAIT_SAMPLE names sample times. Detached: it ends with the process.
void start_wait_sampler(std::uintptr_t image_base, std::uintptr_t image_size, const char* spec) {
    std::vector<double> times;
    for (const char* at = spec; at && *at;) {
        char* end = nullptr;
        const double value = std::strtod(at, &end);
        if (end == at) break;
        if (value > 0.0 && value < 3600.0) times.push_back(value);
        at = *end == ',' ? end + 1 : end;
        if (*end == '\0') break;
    }
    if (times.empty()) {
        if (std::strcmp(spec, "1") == 0) times = {5.0, 10.0, 20.0, 40.0, 60.0};
        else return;
    }
    std::vector<WaitSampleApi> apis;
    resolve_wait_apis(apis);
    std::thread([image_base, image_size, apis, times] {
        const auto started = sampler_epoch();
        for (const double at : times) {
            std::this_thread::sleep_until(started + std::chrono::milliseconds(static_cast<long long>(at * 1000.0)));
            char label[32];
            std::snprintf(label, sizeof label, "%.0f", at);
            wait_state_sample_once(image_base, image_size, apis, label);
        }
    }).detach();
}

// ---- Full thread-stack sampler (PALMETTO_THREAD_STACKS) ---------------------------------------------------------------
// Diagnostic, off unless the environment variable names comma-separated seconds ("2.5,5,7.5,10,15,20"). At each time it
// suspends every other thread of the process, captures the instruction pointer, the thread's start address and the
// code-range words of its stack (bounded to eight pages and 32 frames), then prints one line per DISTINCT stack with the
// threads that share it. Unlike PALMETTO_WAIT_SAMPLE this does not filter to threads inside a named wait, so a title-side
// dispatcher that is never blocked in a wait API appears too. RVAs only; no game data.
struct ThreadStackSample {
    DWORD tid = 0;
    std::uint64_t rip = 0, rsp = 0, start = 0;
    unsigned count = 0;
    std::uint64_t stack[64] = {};
};

// Like scan_stack_words, but walks up to `pages` committed pages above rsp and keeps up to `cap` code-range words - the
// call chain a one-page scanner would cut off. Bounded and fault-tolerant the same way.
unsigned scan_stack_words_deep(std::uint64_t rsp, std::uint64_t* out, unsigned cap, const WaitSampleModules& modules,
                               unsigned pages) {
    unsigned found = 0;
    const std::uint64_t page = 0x1000;
    const std::uint64_t first = rsp & ~(page - 1);
    for (unsigned p = 0; p < pages; ++p) {
        const std::uint64_t at = first + std::uint64_t(p) * page;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery((const void*)(std::uintptr_t)at, &info, sizeof(info)) != sizeof(info)) continue;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
        __try {
            const auto* words = reinterpret_cast<const std::uint64_t*>((std::uintptr_t)at);
            for (unsigned i = 0; i < page / 8 && found < cap; ++i) {
                const std::uint64_t value = words[i];
                if ((std::uintptr_t)value < rsp) continue;
                if (modules.in_code((std::uintptr_t)value)) out[found++] = value;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return found;
        }
    }
    return found;
}

void thread_stacks_sample_once(std::uintptr_t image_base, std::uintptr_t image_size, const char* at_seconds) {
    WaitSampleModules modules;
    modules.image_base = image_base;
    modules.image_end = image_base + image_size;
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Module32FirstW(snapshot, &entry))
                do {
                    modules.ranges.emplace_back((std::uintptr_t)entry.modBaseAddr,
                                                (std::uintptr_t)entry.modBaseAddr + entry.modBaseSize);
                } while (Module32NextW(snapshot, &entry));
            CloseHandle(snapshot);
        }
    }
    std::vector<DWORD> tids;
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD pid = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        if (Thread32First(snapshot, &entry))
            do {
                if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self && tids.size() < 256)
                    tids.push_back(entry.th32ThreadID);
            } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
    }
    static NtQueryInformationThreadFn query_thread = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    std::vector<ThreadStackSample> samples;
    samples.reserve(tids.size());  // allocated before any thread is suspended
    for (const DWORD tid : tids) {
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!thread) continue;
        if (SuspendThread(thread) == (DWORD)-1) {
            CloseHandle(thread);
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        ThreadStackSample sample;
        sample.tid = tid;
        if (GetThreadContext(thread, &context)) {
            sample.rip = context.Rip;
            sample.rsp = context.Rsp;
            if (query_thread) {
                void* start = nullptr;
                if (query_thread(thread, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) >= 0)
                    sample.start = (std::uint64_t)(std::uintptr_t)start;
            }
            sample.count = scan_stack_words_deep(sample.rsp, sample.stack, 64, modules, 32);
        }
        ResumeThread(thread);
        CloseHandle(thread);
        if (sample.rip) samples.push_back(sample);
    }
    // Everything below runs with every thread resumed: module_offset() takes the registry lock.
    std::map<std::string, std::pair<unsigned, std::string>> distinct;  // stack signature -> threads
    for (const ThreadStackSample& sample : samples) {
        std::string signature = rt::shims::module_offset(sample.rip);
        for (unsigned i = 0; i < sample.count; ++i) signature += " " + rt::shims::module_offset(sample.stack[i]);
        auto& slot = distinct[signature];
        ++slot.first;
        if (slot.first <= 8) slot.second += (slot.second.empty() ? "" : ",") + std::to_string(sample.tid);
    }
    std::fprintf(stderr, "[rt] thread stacks at %s s: %zu thread(s), %zu distinct stack(s)\n", at_seconds,
                 samples.size(), distinct.size());
    unsigned shown = 0;
    for (const auto& [signature, slot] : distinct) {
        if (shown++ == 48) {
            std::fprintf(stderr, "[rt] thread stacks at %s s: %u more distinct stack(s) not shown\n", at_seconds,
                         static_cast<unsigned>(distinct.size() - shown + 1));
            break;
        }
        std::string detail = "[rt] thread stacks at " + std::string(at_seconds) + " s: " + std::to_string(slot.first) +
                             "x tid " + slot.second + "\n    " + signature;
        std::fprintf(stderr, "%s\n", detail.c_str());
    }
}

// Starts the full-stack sampler thread when PALMETTO_THREAD_STACKS names sample times. Detached: it ends with the process.
void start_thread_stack_sampler(std::uintptr_t image_base, std::uintptr_t image_size, const char* spec) {
    std::vector<double> times;
    for (const char* at = spec; at && *at;) {
        char* end = nullptr;
        const double value = std::strtod(at, &end);
        if (end == at) break;
        if (value > 0.0 && value < 3600.0) times.push_back(value);
        at = *end == ',' ? end + 1 : end;
        if (*end == '\0') break;
    }
    if (times.empty()) return;
    std::thread([image_base, image_size, times] {
        const auto started = sampler_epoch();
        for (const double at : times) {
            std::this_thread::sleep_until(started + std::chrono::milliseconds(static_cast<long long>(at * 1000.0)));
            char label[32];
            std::snprintf(label, sizeof label, "%.1f", at);
            thread_stacks_sample_once(image_base, image_size, label);
        }
    }).detach();
}

// ---- Loader write watch (PALMETTO_LOADER_WATCH): hardware data watchpoints on the loader object's fields -------------
// The loader object is [[game+0x79F6D70]] (the "dsms" job). Its armed flag at +0x78 rises while a load burst is in flight
// and its in-flight count at +0x64 follows the jobs. A polling sampler misses the writer; a hardware data watchpoint in
// every title thread catches the storing instruction itself, so the writer's RIP and the title stack around it name the
// code that arms/drops loading. Opt-in (PALMETTO_LOADER_WATCH=1); bounded records, RVAs only.
struct LoaderWatch {
    std::atomic<bool> ready{false};
    std::atomic<std::uintptr_t> armed_address{0};
    std::atomic<std::uintptr_t> inflight_address{0};
    std::atomic<std::uint32_t> armed_last{0xFFFFFFFFu};
    std::atomic<std::uint32_t> inflight_last{0xFFFFFFFFu};
    std::atomic<unsigned> records{0};
    std::atomic<unsigned> control_events{0};
    std::atomic<bool> control_changed{false};
};

// The hardware-watchpoint control: a port-owned slot a dedicated thread writes every 50 ms. If a data breakpoint on it
// never fires, debug registers are not honoured in this environment, so a silent loader watch is a method failure,
// not a measurement. Aligned and volatile only for the control; contains no title data.
alignas(8) volatile std::uint64_t loader_watch_control = 0;

LoaderWatch& loader_watch() {
    static LoaderWatch watch;
    return watch;
}

// The control writer and its report: a port-owned slot written every 50 ms, so a hardware watchpoint on it firing tells
// us debug registers are honoured here; silence tells us the loader-watch negative was a method failure.
void start_loader_watch_control() {
    std::thread([] {
        for (;;) {
            ++loader_watch_control;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
    std::thread([] {
        for (const double at : {3.0, 15.0, 60.0, 120.0}) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(at * 1000.0) - 0));
            std::fprintf(stderr, "[rt] loader watch control at %.0f s: %u hardware watchpoint event(s)\n", at,
                         loader_watch().control_events.load(std::memory_order_relaxed));
        }
    }).detach();
}

std::uintptr_t loader_watch_image_base = 0, loader_watch_image_size = 0;
std::atomic<std::uintptr_t> loader_begin_address{0}, loader_end_address{0};
std::atomic<int> loader_spinners{0};   // begin-minus-end nesting; >1 means two spinners overlap
std::atomic<unsigned> loader_spinner_depth{0};
std::atomic<std::uint64_t> loader_spinner_last_object{0};
std::atomic<unsigned long> loader_spinner_last_tid{0};
struct LoaderRaceEntry {
    std::uint64_t object = 0;
    unsigned long arm_tid = 0;
    unsigned begin = 0, end = 0, cross = 0;
    unsigned last_armed = 0;
};
LoaderRaceEntry g_loader_race[16];
std::mutex g_loader_race_mutex;
std::atomic<std::uintptr_t> loader_reinit_address{0};
std::atomic<std::uintptr_t> loader_poll_address{0};  // execution-breakpoint control: the DS poll loop
std::atomic<std::uintptr_t> loader_global_slot{0};  // [[game+0x79F6D70]]: the loader object pointer
std::mutex loader_objects_mutex;
std::map<std::uint64_t, std::pair<unsigned, unsigned>> loader_objects;  // object -> (begin hits, end hits)
std::map<std::uint64_t, unsigned> loader_reinits;                       // object installed -> re-init hits

// Title-image frames only, from the faulting thread's stack at the moment of the store. LoaderWatch values are atomics
// only; the walker is the same fault-tolerant page scan the samplers use. Called from the VEH, so it prints directly.
void loader_watch_record(const CONTEXT& context, const char* field, std::uint32_t old_value, std::uint32_t new_value,
                         const WaitSampleModules& modules) {
    if (loader_watch().records.fetch_add(1, std::memory_order_relaxed) >= 64) return;
    std::uint64_t frames[16] = {};
    const unsigned found = scan_stack_words_deep(context.Rsp, frames, 16, modules, 32);
    std::string title_frames;
    unsigned shown = 0;
    for (unsigned i = 0; i < found && shown < 12; ++i) {
        if (frames[i] < modules.image_base || frames[i] >= modules.image_end) continue;
        title_frames += " " + rt::shims::module_offset(frames[i]);
        ++shown;
    }
    char label[48];
    std::snprintf(label, sizeof label, "loader field %s", field);
    std::fprintf(stderr, "[rt] loader watch: %s 0x%x -> 0x%x, tid %lu, writer %s, title stack%s\n", label, old_value,
                 new_value, static_cast<unsigned long>(GetCurrentThreadId()), rt::shims::module_offset(context.Rip).c_str(),
                 title_frames.empty() ? " (none)" : "");
}

LONG CALLBACK loader_watch_exception(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    LoaderWatch& watch = loader_watch();
    if (!watch.ready.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
    const std::uintptr_t begin_address = loader_begin_address.load(std::memory_order_relaxed);
    const std::uintptr_t end_address = loader_end_address.load(std::memory_order_relaxed);
    const std::uintptr_t reinit_address = loader_reinit_address.load(std::memory_order_relaxed);
    const std::uintptr_t poll_address = loader_poll_address.load(std::memory_order_relaxed);
    if (poll_address && info->ContextRecord->Rip == poll_address) {
        static std::atomic<unsigned> poll_hits{0};
        const unsigned hit = poll_hits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hit <= 8)
            std::fprintf(stderr, "[rt] poll exec control: fired (%u), tid %lu (execution breakpoints work)\\n", hit,
                         static_cast<unsigned long>(GetCurrentThreadId()));
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (reinit_address && info->ContextRecord->Rip == reinit_address) {
        const std::uint64_t owner = info->ContextRecord->Rcx;
        const std::uintptr_t global_slot = loader_global_slot.load(std::memory_order_relaxed);
        const std::uint64_t installed = global_slot ? *reinterpret_cast<const std::uint64_t*>(global_slot) : 0;
        static std::atomic<unsigned> reinit_records{0};
        {
            std::lock_guard lock(loader_objects_mutex);
            ++loader_reinits[installed];
        }
        if (reinit_records.fetch_add(1, std::memory_order_relaxed) < 64)
            std::fprintf(stderr, "[rt] loader re-init: tid %lu owner 0x%llx, global now 0x%llx\n",
                         static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(owner),
                         static_cast<unsigned long long>(installed));
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (begin_address && (info->ContextRecord->Rip == begin_address || info->ContextRecord->Rip == end_address)) {
        const bool is_begin = info->ContextRecord->Rip == begin_address;
        const int depth = loader_spinners.fetch_add(is_begin ? 1 : -1, std::memory_order_relaxed) + (is_begin ? 1 : 0);
        static std::atomic<unsigned> begin_records{0};
        if (begin_records.fetch_add(1, std::memory_order_relaxed) < 256) {
            WaitSampleModules modules;
            modules.image_base = loader_watch_image_base;
            modules.image_end = loader_watch_image_base + loader_watch_image_size;
            std::uint64_t frames[16] = {};
            const unsigned found = scan_stack_words_deep(info->ContextRecord->Rsp, frames, 16, modules, 32);
            std::string title;
            unsigned shown = 0;
            for (unsigned i = 0; i < found && shown < 10; ++i) {
                if (frames[i] < modules.image_base || frames[i] >= modules.image_end) continue;
                title += " " + rt::shims::module_offset(frames[i]);
                ++shown;
            }
            const std::uint64_t object_argument = info->ContextRecord->Rdx;
            const std::uintptr_t global_slot = loader_global_slot.load(std::memory_order_relaxed);
            const std::uint64_t global_object = global_slot ? *reinterpret_cast<const std::uint64_t*>(global_slot) : 0;
            const auto state_of = [](std::uint64_t object, unsigned& armed, unsigned& inflight) {
                armed = inflight = 0xFFFFFFFFu;
                if (!object) return;
                armed = *reinterpret_cast<const std::uint8_t*>(object + 0x78);
                inflight = *reinterpret_cast<const std::uint32_t*>(object + 0x64);
            };
            unsigned argument_armed = 0, argument_inflight = 0, global_armed = 0, global_inflight = 0;
            state_of(object_argument, argument_armed, argument_inflight);
            state_of(global_object, global_armed, global_inflight);
            {
                std::lock_guard lock(loader_objects_mutex);
                auto& counts = loader_objects[object_argument];
                ++(is_begin ? counts.first : counts.second);
            }
            std::fprintf(stderr,
                         "[rt] loader begin/end: %s tid %lu depth %d%s, entry %s, object 0x%llx (armed %u in-flight %u), "
                         "global 0x%llx (armed %u in-flight %u)%s, title stack%s\n",
                         is_begin ? "begin" : "end", static_cast<unsigned long>(GetCurrentThreadId()), depth,
                         depth > 1 ? " (overlap)" : "", rt::shims::module_offset(info->ContextRecord->Rip).c_str(),
                         static_cast<unsigned long long>(object_argument), argument_armed, argument_inflight,
                         static_cast<unsigned long long>(global_object), global_armed, global_inflight,
                         object_argument != global_object ? " (OBJECT != GLOBAL)" : "",
                         title.empty() ? " (none)" : title.c_str());
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if ((info->ContextRecord->Dr6 & 0x4u) != 0) {  // DR2: the control slot
        LoaderWatch& watch = loader_watch();
        const unsigned events = watch.control_events.fetch_add(1, std::memory_order_relaxed) + 1;
        if (events <= 3) std::fprintf(stderr, "[rt] loader watch control: hardware watchpoint fired (%u)\n", events);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const std::uintptr_t armed = watch.armed_address.load(std::memory_order_relaxed);
    const std::uintptr_t inflight = watch.inflight_address.load(std::memory_order_relaxed);
    if (!armed && !inflight) return EXCEPTION_CONTINUE_SEARCH;
    WaitSampleModules modules;  // built on the faulting thread: cheap enough for a handful of transitions
    modules.image_base = loader_watch_image_base;
    modules.image_end = loader_watch_image_base + loader_watch_image_size;
    if (armed) {
        const std::uint32_t now = *reinterpret_cast<const std::uint8_t*>(armed);
        if (now != watch.armed_last.load(std::memory_order_relaxed)) {
            const std::uint32_t old_value = watch.armed_last.exchange(now, std::memory_order_relaxed);
            if (old_value != 0xFFFFFFFFu)
                loader_watch_record(*info->ContextRecord, "+0x78 armed", old_value, now, modules);
        }
    }
    if (inflight) {
        const std::uint32_t now = *reinterpret_cast<const std::uint32_t*>(inflight);
        if (now != watch.inflight_last.load(std::memory_order_relaxed)) {
            const std::uint32_t old_value = watch.inflight_last.exchange(now, std::memory_order_relaxed);
            if (old_value != 0xFFFFFFFFu)
                loader_watch_record(*info->ContextRecord, "+0x64 in flight", old_value, now, modules);
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;  // hardware data breakpoints do not consume the instruction
}


// Arms DR0 (the armed byte, exact 1 byte) and DR1 (the in-flight dword) write-only in every thread of the process, and
// keeps arming threads created later. Detached; ends with the process.
void start_loader_watch(std::uintptr_t image_base, std::size_t image_size, std::uint64_t loader_global_rva) {
    loader_watch_image_base = image_base;
    loader_watch_image_size = image_size;
    {
        char beginend_value[16] = {};
        if (GetEnvironmentVariableA("PALMETTO_LOADER_BEGINEND", beginend_value, sizeof(beginend_value)) > 0 &&
            beginend_value[0] != '\0' && beginend_value[0] != '0') {
            // Diagnostic (docs/frame_capture.md): the RVA constants the begin/end mode arms may be vtable entries or
            // thunks rather than function starts. Print the loader vtable's +0x240/+0x248 pointers and the first bytes
            // at each arm target, so a mismatch is visible in one boot.
            {
                const auto* vtable_base = reinterpret_cast<const std::uint64_t*>(image_base + 0x62FC550);
                const std::uint64_t slot_240 = vtable_base[0x240 / 8];
                const std::uint64_t slot_248 = vtable_base[0x248 / 8];
                const auto* begin_bytes = reinterpret_cast<const std::uint8_t*>(image_base + 0xE88F0);
                const auto* end_bytes = reinterpret_cast<const std::uint8_t*>(image_base + 0xE8960);
                const auto* reinit_bytes = reinterpret_cast<const std::uint8_t*>(image_base + 0x5CD3F50);
                std::fprintf(stderr,
                             "[rt] loader watch map: vtable+0x240 -> 0x%llx, vtable+0x248 -> 0x%llx; bytes at +0xE88F0"
                             " %02x %02x, at +0xE8960 %02x %02x, at +0x5CD3F50 %02x %02x\n",
                             static_cast<unsigned long long>(slot_240), static_cast<unsigned long long>(slot_248),
                             begin_bytes[0], begin_bytes[1], end_bytes[0], end_bytes[1], reinit_bytes[0],
                             reinit_bytes[1]);
            }
            loader_begin_address.store(image_base + 0xE88F0, std::memory_order_relaxed);            loader_reinit_address.store(image_base + 0x5CD3F50, std::memory_order_relaxed);
            { char poll_value[8] = {}; if (GetEnvironmentVariableA("PALMETTO_LOADER_POLLBP", poll_value, sizeof(poll_value)) > 0 && poll_value[0] != '0') loader_poll_address.store(image_base + 0x4F6584A, std::memory_order_relaxed); }
            loader_global_slot.store(image_base + 0x79F6D70, std::memory_order_relaxed);
            loader_end_address.store(image_base + 0xE8960, std::memory_order_relaxed);
        }
    }
    start_loader_watch_control();
    AddVectoredExceptionHandler(1, loader_watch_exception);
    std::thread([image_base, loader_global_rva] {
        const bool beginend = loader_begin_address.load(std::memory_order_relaxed) != 0;
        std::uintptr_t object = 0;
        if (!beginend) {
            const auto* global = reinterpret_cast<const std::uint64_t*>(image_base + loader_global_rva);
            for (unsigned wait = 0; wait < 2000 && !object; ++wait) {
                object = static_cast<std::uintptr_t>(*global);
                if (!object) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!object) {
                std::fprintf(stderr, "[rt] loader watch: the loader global stayed zero; not armed\n");
                return;
            }
        }
        const std::uintptr_t armed = beginend ? 0 : object + 0x78;
        const std::uintptr_t inflight = beginend ? 0 : object + 0x64;
        LoaderWatch& watch = loader_watch();
        watch.armed_address.store(armed, std::memory_order_relaxed);
        watch.inflight_address.store(inflight, std::memory_order_relaxed);
        if (!beginend) {
            watch.armed_last.store(*reinterpret_cast<const std::uint8_t*>(armed), std::memory_order_relaxed);
            watch.inflight_last.store(*reinterpret_cast<const std::uint32_t*>(inflight), std::memory_order_relaxed);
        }
        watch.ready.store(true, std::memory_order_relaxed);
        if (!beginend)
            std::fprintf(stderr,
                         "[rt] loader watch: armed on object %p, byte +0x78 (%p) and dword +0x64 (%p); initial 0x%x / 0x%x\n",
                         reinterpret_cast<void*>(object), reinterpret_cast<void*>(armed),
                         reinterpret_cast<void*>(inflight), *reinterpret_cast<const std::uint8_t*>(armed),
                         *reinterpret_cast<const std::uint32_t*>(inflight));
        else
            std::fprintf(stderr, "[rt] loader begin/end: execution breakpoints armed from boot-0 (no object wait)\n");
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(15));
                std::lock_guard lock(loader_objects_mutex);
                const std::uintptr_t global_slot = loader_global_slot.load(std::memory_order_relaxed);
                const std::uint64_t global_object = global_slot ? *reinterpret_cast<const std::uint64_t*>(global_slot) : 0;
                std::fprintf(stderr, "[rt] loader objects at 15 s: global 0x%llx; %zu object(s) seen\n",
                             static_cast<unsigned long long>(global_object), loader_objects.size());
                unsigned shown = 0;
                for (const auto& [object, counts] : loader_objects) {
                    if (shown++ == 8) break;
                    std::fprintf(stderr, "[rt]   object 0x%llx: begin %u, end %u%s\n",
                                 static_cast<unsigned long long>(object), counts.first, counts.second,
                                 object == global_object ? " (the current global)" : "");
                }
                for (const auto& [object, hits] : loader_reinits)
                    std::fprintf(stderr, "[rt]   re-init installed 0x%llx: %u hit(s)\n",
                                 static_cast<unsigned long long>(object), hits);
            }
        }).detach();
        std::set<DWORD> configured;
        for (;;) {
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{};
                entry.dwSize = sizeof(entry);
                const DWORD pid = GetCurrentProcessId();
                if (Thread32First(snapshot, &entry))
                    do {
                        if (entry.th32OwnerProcessID != pid || configured.count(entry.th32ThreadID)) continue;
                        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                                         FALSE, entry.th32ThreadID);
                        if (!thread) continue;
                        if (SuspendThread(thread) != (DWORD)-1) {
                            CONTEXT context{};
                            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                            if (GetThreadContext(thread, &context)) {
                                const bool beginend_mode = loader_begin_address.load(std::memory_order_relaxed) != 0;
                                context.Dr0 = beginend_mode ? loader_begin_address.load(std::memory_order_relaxed) : armed;
                                context.Dr1 = beginend_mode ? loader_end_address.load(std::memory_order_relaxed) : inflight;
                                context.Dr2 = reinterpret_cast<std::uintptr_t>(&loader_watch_control);
                                context.Dr3 = beginend_mode ? loader_reinit_address.load(std::memory_order_relaxed) : 0;
                                const std::uintptr_t poll_address = loader_poll_address.load(std::memory_order_relaxed);
                                if (beginend_mode && poll_address) context.Dr2 = poll_address;
                                context.Dr6 = 0;
                                if (beginend_mode)
                                    context.Dr7 = loader_poll_address.load(std::memory_order_relaxed)
                                                       ? (0x1u | (0x1u << 2) | (0x1u << 4) | (0x1u << 6))
                                                       : (0x1u | (0x1u << 2) | (0x1u << 4) | (0x1u << 6) | (0x1u << 24) | (0x3u << 26));
                                // L0 = 1, RW0 = 01 (write), LEN0 = 00 (1 byte); L1 = 1, RW1 = 01, LEN1 = 11 (4 bytes).
                                context.Dr7 = 0x1u | (0x1u << 2) | (0x1u << 4) | (0x1u << 16) | (0x1u << 20) | (0x3u << 22) | (0x1u << 24) | (0x3u << 26);
                                SetThreadContext(thread, &context);
                                configured.insert(entry.th32ThreadID);
                            }
                            ResumeThread(thread);
                        }
                        CloseHandle(thread);
                    } while (Thread32Next(snapshot, &entry));
                CloseHandle(snapshot);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }).detach();
}

// ---- RIP histogram (PALMETTO_RIP_HIST) ---------------------------------------------------------------------------------
// Diagnostic, off unless the variable names sample windows ("5,20" or "5,20;60,90": start,end seconds). In each window the
// sampler enumerates every other thread every 3 ms, suspends it, reads the instruction pointer and keeps the ones inside
// the title image; it then prints the top 40 title RVAs with hit counts and the threads they appeared on, and one stack
// (title frames only) for the top five. RVAs/counts only; no title data.
struct RipHistogramEntry {
    std::uint64_t hits = 0;
    std::set<DWORD> tids;
    std::string stack;  // title-image frames, captured once at first sight (bounded depth)
};

void rip_histogram_window(std::uintptr_t image_base, std::uintptr_t image_size, double begin, double end,
                          const char* label) {
    WaitSampleModules modules;
    modules.image_base = image_base;
    modules.image_end = image_base + image_size;
    std::map<std::uint64_t, RipHistogramEntry> hist;
    const auto started = sampler_epoch();
    std::this_thread::sleep_until(started + std::chrono::milliseconds(static_cast<long long>(begin * 1000.0)));
    const auto stop = started + std::chrono::milliseconds(static_cast<long long>(end * 1000.0));
    while (std::chrono::steady_clock::now() < stop) {
        std::vector<DWORD> tids;
        {
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{};
                entry.dwSize = sizeof(entry);
                const DWORD pid = GetCurrentProcessId();
                const DWORD self = GetCurrentThreadId();
                if (Thread32First(snapshot, &entry))
                    do {
                        if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self && tids.size() < 256)
                            tids.push_back(entry.th32ThreadID);
                    } while (Thread32Next(snapshot, &entry));
                CloseHandle(snapshot);
            }
        }
        for (const DWORD tid : tids) {
            const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
            if (!thread) continue;
            if (SuspendThread(thread) == (DWORD)-1) {
                CloseHandle(thread);
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(thread, &context) && context.Rip >= image_base &&
                context.Rip < image_base + image_size) {
                const std::uint64_t rva = context.Rip - image_base;
                RipHistogramEntry& entry = hist[rva];
                ++entry.hits;
                if (entry.tids.size() < 16) entry.tids.insert(tid);
                if (entry.stack.empty()) {
                    std::uint64_t frames[16] = {};
                    const unsigned found = scan_stack_words_deep(context.Rsp, frames, 16, modules, 32);
                    for (unsigned i = 0; i < found && i < 12; ++i)
                        entry.stack += " " + rt::shims::module_offset(frames[i]);
                }
            }
            ResumeThread(thread);
            CloseHandle(thread);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    std::vector<std::pair<std::uint64_t, RipHistogramEntry*>> rows;
    std::uint64_t total = 0;
    for (auto& [rva, entry] : hist) {
        rows.emplace_back(rva, &entry);
        total += entry.hits;
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second->hits > b.second->hits; });
    std::fprintf(stderr, "[rt] rip histogram %s: %llu title-image sample(s), %zu title site(s)\n", label,
                 static_cast<unsigned long long>(total), rows.size());
    const std::size_t shown = std::min<std::size_t>(rows.size(), 40);
    for (std::size_t i = 0; i < shown; ++i) {
        std::string tids;
        unsigned n = 0;
        for (const DWORD tid : rows[i].second->tids) {
            if (n++ == 8) break;
            tids += (tids.empty() ? "" : ",") + std::to_string(tid);
        }
        std::fprintf(stderr, "[rt] rip histogram %s: game+0x%llx hits %llu tid %s\n", label,
                     static_cast<unsigned long long>(rows[i].first),
                     static_cast<unsigned long long>(rows[i].second->hits), tids.c_str());
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), 5); ++i)
        std::fprintf(stderr, "[rt] rip histogram %s stack: game+0x%llx:%s\n", label,
                     static_cast<unsigned long long>(rows[i].first), rows[i].second->stack.c_str());
}

// Starts the histogram windows named by PALMETTO_RIP_HIST ("start,end" or "start,end;start,end"). Detached.
void start_rip_histogram(std::uintptr_t image_base, std::uintptr_t image_size, const char* spec) {
    std::vector<std::pair<double, double>> windows;
    for (const char* at = spec; at && *at;) {
        char* end = nullptr;
        const double begin = std::strtod(at, &end);
        if (end == at) break;
        double finish = begin + 15.0;
        if (*end == ',') finish = std::strtod(end + 1, &end);
        if (begin > 0.0 && finish > begin && finish < 3600.0) windows.emplace_back(begin, finish);
        at = *end == ';' ? end + 1 : end;
        if (*end == '\0') break;
    }
    if (windows.empty()) return;
    std::thread([image_base, image_size, windows] {
        unsigned index = 0;
        for (const auto& window : windows) {
            char label[48];
            std::snprintf(label, sizeof label, "%u (%.0f-%.0fs)", ++index, window.first, window.second);
            rip_histogram_window(image_base, image_size, window.first, window.second, label);
        }
    }).detach();
}

// ---- Loader entry detours (PALMETTO_LOADER_HOOK): inline hot-patch on begin/end/re-init ------------------------------
// Debug-register delivery is unreliable in this environment (measured), so the loader's begin (game+0xE88F0), end
// (game+0xE8960) and re-init (game+0x5CD3F50) are detoured by overwriting their first instruction(s) with a 5-byte E9 jump
// into a per-site trampoline: the trampoline logs the call (site, tid, object argument, loader global, armed/in-flight of
// both objects, top title frames of the caller's stack), executes the overwritten bytes and jumps back. Opt-in
// (PALMETTO_LOADER_HOOK=1); the prologue is verified before patching and a mismatch is logged, never patched. Bounded
// records, RVAs/counts only.
struct LoaderHookSite {
    const char* name;
    std::uint64_t rva;
    std::uintptr_t target = 0;
    unsigned char original[16] = {};
    unsigned length = 0;
    unsigned char* trampoline = nullptr;
    std::atomic<unsigned> calls{0};
};

LoaderHookSite g_loader_hooks[9] = {{"begin", 0xE88F0}, {"end", 0xE8960}, {"re-init", 0x5CD3F50}, {"completion", 0x4F65820}, {"spinner", 0x4F657A0}};

// Length of the instruction at `code` for the handful of prologue forms these functions use (push/REX/sub/mov/lea); 0 when
// it is not one this simple reader knows, which makes the site skipped rather than mis-patched.
unsigned prologue_instruction_length(const unsigned char* code) {
    unsigned at = 0;
    bool rex = false;
    if ((code[0] & 0xF0) == 0x40) {
        rex = true;
        at = 1;
    }
    const unsigned char op = code[at];
    if (op >= 0x50 && op <= 0x57) return at + 1;                         // push r64
    if (op == 0x55 || op == 0x53 || op == 0x56 || op == 0x57) return at + 1;
    if (op == 0x83) return at + 3;                                        // group 1, imm8
    if (op == 0x81) return at + 6;                                        // group 1, imm32
    if (op == 0x89 || op == 0x8B || op == 0x8D) {                         // mov/lea r/m
        const unsigned char modrm = code[at + 1];
        const unsigned rm = modrm & 7;
        unsigned extra = 0;
        if ((modrm >> 6) == 1) extra = 1;
        else if ((modrm >> 6) == 2) extra = 4;
        else if (rm == 5) extra = 4;
        return at + 2 + extra;
    }
    if (op == 0xEC || op == 0xE8) return at + 1;
    (void)rex;
    return 0;
}

void loader_hook_record(unsigned site, std::uintptr_t caller_rsp, std::uint64_t object_argument, std::uint64_t third) {
    LoaderHookSite& hook = g_loader_hooks[site];
    const unsigned call = hook.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (site == 3 || site == 2) {  // completion and re-init: pointers/counters only, never call the owner
        if (call > 20 && (call % 100) != 0) return;
        const std::uintptr_t global_slot = loader_global_slot.load(std::memory_order_relaxed);
        const std::uint64_t global_object = global_slot ? *reinterpret_cast<const std::uint64_t*>(global_slot) : 0;
        std::fprintf(stderr,
                     "[rt] loader hook: completion hit %u tid %lu, arg2 0x%llx arg3 0x%llx, caller %s, global 0x%llx\n",
                     call, static_cast<unsigned long>(GetCurrentThreadId()),
                     static_cast<unsigned long long>(object_argument), static_cast<unsigned long long>(third),
                     rt::shims::module_offset(*reinterpret_cast<const std::uint64_t*>(caller_rsp)).c_str(),
                     static_cast<unsigned long long>(global_object));
        return;
    }
    if (site == 4) {  // spinner entry: depth and last spinner record (first 40 hits)
        loader_spinner_depth.fetch_add(1, std::memory_order_relaxed);
        loader_spinner_last_object.store(object_argument, std::memory_order_relaxed);
        loader_spinner_last_tid.store(static_cast<unsigned long>(GetCurrentThreadId()), std::memory_order_relaxed);
        if (call <= 40)
            std::fprintf(stderr, "[rt] loader hook: spinner enter %u tid %lu, object 0x%llx\n", call,
                         static_cast<unsigned long>(GetCurrentThreadId()),
                         static_cast<unsigned long long>(object_argument));
        return;
    }
    {
        std::lock_guard lock(g_loader_race_mutex);
        LoaderRaceEntry* entry = nullptr;
        for (LoaderRaceEntry& candidate : g_loader_race)
            if (candidate.object == object_argument) { entry = &candidate; break; }
        if (!entry)
            for (LoaderRaceEntry& candidate : g_loader_race)
                if (!candidate.object) { candidate.object = object_argument; entry = &candidate; break; }
        if (entry) {
            if (site == 0) {
                entry->arm_tid = static_cast<unsigned long>(GetCurrentThreadId());
                ++entry->begin;
                entry->last_armed = 0;
            } else if (site == 1) {
                ++entry->end;
                if (entry->object >= 0x10000) entry->last_armed = *reinterpret_cast<const std::uint8_t*>(entry->object + 0x78);
                const unsigned long tid = static_cast<unsigned long>(GetCurrentThreadId());
                if (entry->last_armed && entry->arm_tid && tid != entry->arm_tid) {
                    ++entry->cross;
                    if (entry->cross <= 24)
                        std::fprintf(stderr,
                                     "[rt] loader cross-thread end: object 0x%llx, arm tid %lu, end tid %lu, "
                                     "armed %u in-flight %u, global %s, spinner depth %u\n",
                                     static_cast<unsigned long long>(entry->object), entry->arm_tid, tid,
                                     entry->last_armed, 0u,
                                     entry->object == loader_spinner_last_object.load(std::memory_order_relaxed)
                                         ? "this object"
                                         : "other",
                                     loader_spinner_depth.load(std::memory_order_relaxed));
                }
            }
        }
    }
    if (call > 200) return;
    const std::uintptr_t global_slot = loader_global_slot.load(std::memory_order_relaxed);
    const std::uint64_t global_object = global_slot ? *reinterpret_cast<const std::uint64_t*>(global_slot) : 0;
    const auto state_of = [](std::uint64_t object, unsigned& armed, unsigned& inflight) {
        armed = inflight = 0xFFFFFFFFu;
        if (!object) return;
        armed = *reinterpret_cast<const std::uint8_t*>(object + 0x78);
        inflight = *reinterpret_cast<const std::uint32_t*>(object + 0x64);
    };
    unsigned argument_armed = 0, argument_inflight = 0, global_armed = 0, global_inflight = 0;
    state_of(object_argument, argument_armed, argument_inflight);
    state_of(global_object, global_armed, global_inflight);
    WaitSampleModules modules;
    modules.image_base = loader_watch_image_base;
    modules.image_end = loader_watch_image_base + loader_watch_image_size;
    std::uint64_t frames[16] = {};
    const std::uint64_t return_address = caller_rsp ? *reinterpret_cast<const std::uint64_t*>(caller_rsp) : 0;
    if (return_address >= modules.image_base && return_address < modules.image_end)
        std::fprintf(stderr, "[rt] loader hook: %s caller return %s\n", hook.name,
                     rt::shims::module_offset(return_address).c_str());
    const unsigned found = scan_stack_words_deep(caller_rsp + 8, frames, 16, modules, 32);
    std::string title;
    unsigned shown = 0;
    for (unsigned i = 0; i < found && shown < 10; ++i) {
        if (frames[i] < modules.image_base || frames[i] >= modules.image_end) continue;
        title += " " + rt::shims::module_offset(frames[i]);
        ++shown;
    }
    std::fprintf(stderr,
                 "[rt] loader hook: %s tid %lu, object 0x%llx (armed %u in-flight %u), global 0x%llx (armed %u "
                 "in-flight %u)%s, third 0x%llx, caller stack%s\n",
                 hook.name, static_cast<unsigned long>(GetCurrentThreadId()),
                 static_cast<unsigned long long>(object_argument), argument_armed, argument_inflight,
                 static_cast<unsigned long long>(global_object), global_armed, global_inflight,
                 object_argument != global_object ? " (OBJECT != GLOBAL)" : "", static_cast<unsigned long long>(third),
                 title.empty() ? " (none)" : title.c_str());
}

// The trampoline's C side: rcx = site, rdx = caller rsp, r8 = second argument (object), r9 = third argument.
extern "C" void loader_hook_bridge(std::uint64_t site, std::uintptr_t caller_rsp, std::uint64_t object_argument,
                                   std::uint64_t third) {
    if (site < 3) loader_hook_record(static_cast<unsigned>(site), caller_rsp, object_argument, third);
}

// Writes a 64-bit absolute jump (mov rax,imm64; jmp rax) at `at`.
void write_absolute_jump(unsigned char* at, const void* target) {
    at[0] = 0x48;
    at[1] = 0xB8;
    std::memcpy(at + 2, &target, sizeof(target));
    at[10] = 0xFF;
    at[11] = 0xE0;
}

bool install_loader_hook(LoaderHookSite& hook, std::uintptr_t image_base) {
    hook.target = image_base + hook.rva;
    std::memcpy(hook.original, reinterpret_cast<const void*>(hook.target), sizeof(hook.original));
    unsigned length = 0;
    while (length < 5) {
        const unsigned step = prologue_instruction_length(hook.original + length);
        if (!step || length + step > sizeof(hook.original)) {
            std::fprintf(stderr,
                         "[rt] loader hook: %s prologue at +0x%llx is not a known form (bytes %02x %02x %02x %02x), "
                         "skipping the hook\n",
                         hook.name, static_cast<unsigned long long>(hook.rva), hook.original[0], hook.original[1],
                         hook.original[2], hook.original[3]);
            return false;
        }
        length += step;
    }
    hook.length = length;
    // Allocate the trampoline within +/-2 GiB of the target so the E9 relative jump reaches it.
    std::uintptr_t base = (hook.target & ~std::uintptr_t(0xFFFF)) - 0x10000;
    unsigned char* trampoline = nullptr;
    for (unsigned attempt = 0; attempt < 8192 && !trampoline; ++attempt, base -= 0x10000) {
        trampoline = static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(base), 0x1000,
                                                              MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        const std::int64_t distance = trampoline ? static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(trampoline)) - static_cast<std::int64_t>(hook.target) : 0;
    if (trampoline && (distance <= -2147483648ll || distance >= 2147483647ll)) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            trampoline = nullptr;
        }
    }
    if (!trampoline) {
        std::fprintf(stderr, "[rt] loader hook: %s trampoline allocation failed, skipping the hook\n", hook.name);
        return false;
    }
    hook.trampoline = trampoline;
    // Trampoline: save volatile registers, pass (site, caller rsp, object, third), call the bridge, restore, run the
    // overwritten bytes, jump back past them.
    unsigned char* at = trampoline;
    const unsigned char save[] = {0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53};
    std::memcpy(at, save, sizeof(save));
    at += sizeof(save);
    // rcx = site
    *at++ = 0x48;
    *at++ = 0xC7;
    *at++ = 0xC1;
    const std::uint32_t site = static_cast<std::uint32_t>(&hook - g_loader_hooks);
    std::memcpy(at, &site, sizeof(site));
    at += sizeof(site);
    // rdx = caller rsp = current rsp + (11 pushes + return address already accounted): after the 11 pushes rsp points at
    // the saved r11; the return address of the original caller sits at rsp + 88.
    *at++ = 0x48;
    *at++ = 0x8B;
    *at++ = 0xD4;  // mov rdx, rsp
    *at++ = 0x48;
    *at++ = 0x81;
    *at++ = 0xC2;
    const std::uint32_t stack_bias = 56;
    std::memcpy(at, &stack_bias, sizeof(stack_bias));
    at += sizeof(stack_bias);
    // r8 = saved original rdx (the object argument): [rsp + 16] (after rax,rcx pushes).
    *at++ = 0x4C;
    *at++ = 0x8B;
    *at++ = 0x44;
    *at++ = 0x24;
    *at++ = 0x20;  // mov r8, [rsp+0x20] (original rdx)
    *at++ = 0x4C;
    *at++ = 0x8B;
    *at++ = 0x4C;
    *at++ = 0x24;
    *at++ = 0x20;  // mov r9, [rsp+0x20]
    *at++ = 0x48;
    *at++ = 0x83;
    *at++ = 0xEC;
    *at++ = 0x20;  // sub rsp, 0x20
    *at++ = 0x48;
    *at++ = 0xB8;
    const void* bridge = reinterpret_cast<const void*>(&loader_hook_bridge);
    std::memcpy(at, &bridge, sizeof(bridge));
    at += sizeof(bridge);
    *at++ = 0xFF;
    *at++ = 0xD0;  // call rax
    *at++ = 0x48;
    *at++ = 0x83;
    *at++ = 0xC4;
    *at++ = 0x20;  // add rsp, 0x20
    const unsigned char restore[] = {0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58};
    std::memcpy(at, restore, sizeof(restore));
    at += sizeof(restore);
    std::memcpy(at, hook.original, length);
    at += length;
    // jmp target + length
    *at++ = 0xE9;
    const std::int32_t back = static_cast<std::int32_t>(hook.target + length - reinterpret_cast<std::uintptr_t>(at + 4));
    std::memcpy(at, &back, sizeof(back));
    at += sizeof(back);

    DWORD old_protection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(hook.target), 16, PAGE_EXECUTE_READWRITE, &old_protection)) {
        std::fprintf(stderr, "[rt] loader hook: %s target page is not writable, skipping the hook\n", hook.name);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        hook.trampoline = nullptr;
        return false;
    }
    std::uint8_t patch[16] = {};
    std::memcpy(patch, hook.original, 16);
    patch[0] = 0xE9;
    const std::int32_t forward = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(trampoline) - (hook.target + 5));
    std::memcpy(patch + 1, &forward, sizeof(forward));
    std::memcpy(reinterpret_cast<void*>(hook.target), patch, 16);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(hook.target), 16);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(hook.target), 16, old_protection, &ignored);
    return true;
}

void loader_race_summary(const char* when) {
    std::lock_guard lock(g_loader_race_mutex);
    std::fprintf(stderr, "[rt] loader race summary %s: spinner depth %u, last spinner tid %lu object 0x%llx\n", when,
                 loader_spinner_depth.load(std::memory_order_relaxed),
                 loader_spinner_last_tid.load(std::memory_order_relaxed),
                 static_cast<unsigned long long>(loader_spinner_last_object.load(std::memory_order_relaxed)));
    for (const LoaderRaceEntry& entry : g_loader_race) {
        if (!entry.object) continue;
        std::fprintf(stderr,
                     "[rt]   object 0x%llx: begin %u, end %u, cross-thread end %u, last armed %u, arm tid %lu\n",
                     static_cast<unsigned long long>(entry.object), entry.begin, entry.end, entry.cross,
                     entry.last_armed, entry.arm_tid);
    }
}

void loader_hook_verify(const char* when) {
    for (const LoaderHookSite& hook : g_loader_hooks) {
        if (!hook.target) continue;
        MEMORY_BASIC_INFORMATION info{};
        const bool queried = VirtualQuery(reinterpret_cast<const void*>(hook.target), &info, sizeof(info)) == sizeof(info);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(hook.target);
        char hex[64] = {};
        for (unsigned i = 0; i < 8; ++i) std::snprintf(hex + i * 3, 4, "%02x ", bytes[i]);
        std::fprintf(stderr,
                     "[rt] loader verify %s: %s at 0x%llx (rva +0x%llx) bytes %s, page %s/%s, jumper %s\n", when,
                     hook.name, static_cast<unsigned long long>(hook.target),
                     static_cast<unsigned long long>(hook.target - loader_watch_image_base), hex,
                     queried && (info.Protect & PAGE_EXECUTE) ? "exec" : "noexec",
                     queried && (info.Protect & PAGE_READWRITE) ? "writable" : "readonly",
                     bytes[0] == 0xE9 ? "E9 present" : "E9 MISSING");
    }
}

void loader_raw_rip_sample(std::uintptr_t image_base) {
    std::map<std::uint64_t, unsigned> hits;
    std::vector<DWORD> tids;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD pid = GetCurrentProcessId();
        if (Thread32First(snapshot, &entry))
            do {
                if (entry.th32OwnerProcessID == pid && tids.size() < 256) tids.push_back(entry.th32ThreadID);
            } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
    }
    for (unsigned round = 0; round < 300; ++round) {
        for (const DWORD tid : tids) {
            const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
            if (!thread) continue;
            if (SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); continue; }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(thread, &context) && context.Rip >= image_base &&
                context.Rip < image_base + 0x10000000ull)
                ++hits[context.Rip];
            ResumeThread(thread);
            CloseHandle(thread);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::vector<std::pair<std::uint64_t, unsigned>> rows(hits.begin(), hits.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "[rt] raw rip sample: loader base 0x%llx; %zu title RIP site(s)\n",
                 static_cast<unsigned long long>(image_base), rows.size());
    for (std::size_t i = 0; i < rows.size() && i < 12; ++i)
        std::fprintf(stderr, "[rt]   raw 0x%llx -> rva +0x%llx, hits %u\n",
                     static_cast<unsigned long long>(rows[i].first),
                     static_cast<unsigned long long>(rows[i].first - image_base), rows[i].second);
}

// Late vtable resolution (docs/frame_capture.md): the loader global is zero at process start, so the owner vtable is
// resolved by polling for it and the four method pointers are then hooked at their ACTUAL targets (the slots are left
// untouched; the target's entry jumps to the logging trampoline, so a copy would be logged too because the call goes
// through the slot's pointer). Prints the pointers with RVA and match/mismatch against the expected RVAs.
void start_loader_vtable_hooks(std::uintptr_t image_base) {
    std::thread([image_base] {
        std::uint64_t loader_object = 0;
        const std::uintptr_t global_slot = loader_watch_image_base + 0x79F6D70;
        for (unsigned wait = 0; wait < 2000 && !loader_object; ++wait) {
            loader_object = *reinterpret_cast<const std::uint64_t*>(global_slot);
            if (!loader_object) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!loader_object) {
            std::fprintf(stderr, "[rt] loader vtable: the global stayed zero; no vtable slots to resolve\n");
            return;
        }
        std::uint64_t owner = *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(loader_object) + 0x10);
        if (!owner) {
            std::fprintf(stderr, "[rt] loader vtable: owner sub-object is zero\n");
            return;
        }
        std::uint64_t vtable = *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(owner));
        std::fprintf(stderr, "[rt] loader vtable: object 0x%llx, owner 0x%llx, vtable 0x%llx\n",
                     static_cast<unsigned long long>(loader_object), static_cast<unsigned long long>(owner),
                     static_cast<unsigned long long>(vtable));
        const struct { unsigned site; std::uint64_t slot; std::uint64_t expected; } slots[] = {
            {5, 0x240, 0xE88F0}, {6, 0x248, 0xE8960}, {7, 0x260, 0x4F65820}, {8, 0x230, 0}};
        unsigned resolved = 0;
        for (const auto& entry : slots) {
            const std::uint64_t target =
                *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(vtable) + entry.slot);
            const std::uint64_t rva = target >= image_base ? target - image_base : 0;
            std::fprintf(stderr, "[rt] loader vtable slot +0x%llx -> 0x%llx (rva +0x%llx)%s\n", entry.slot,
                         static_cast<unsigned long long>(target), static_cast<unsigned long long>(rva),
                         entry.expected ? (rva == entry.expected ? " MATCHES expected" : " MISMATCH") : "");
            if (target >= image_base && target < image_base + 0x10000000ull) {
                LoaderHookSite& hook = g_loader_hooks[entry.site];
                hook.rva = rva;
                if (install_loader_hook(hook, image_base)) ++resolved;
            }
        }
        std::fprintf(stderr, "[rt] loader vtable: %u resolved method(s) hooked at their actual targets\n", resolved);
    }).detach();
}

void start_loader_hooks(std::uintptr_t image_base) {
    loader_watch_image_base = image_base;
    loader_watch_image_size = 0x10000000ull;  // the title image window the stack filter uses
    loader_global_slot.store(image_base + 0x79F6D70, std::memory_order_relaxed);
    // Vtable-slot resolution: read the loader global, its owner sub-object and the owner's vtable, compare the four
    // interesting slot targets with the expected RVAs and hook the actual method pointers.
    unsigned resolved = 0;
    std::uint64_t loader_object = 0;
    const std::uintptr_t global_slot = loader_global_slot.load(std::memory_order_relaxed);
    if (global_slot) loader_object = *reinterpret_cast<const std::uint64_t*>(global_slot);
    std::uint64_t owner = 0;
    std::uint64_t vtable = 0;
    if (loader_object) {
        owner = *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(loader_object) + 0x10);
        if (owner) vtable = *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(owner));
    }
    std::fprintf(stderr, "[rt] loader vtable: object 0x%llx, owner 0x%llx, vtable 0x%llx\n",
                 static_cast<unsigned long long>(loader_object), static_cast<unsigned long long>(owner),
                 static_cast<unsigned long long>(vtable));
    const struct { unsigned site; std::uint64_t slot; std::uint64_t expected; } vtable_slots[] = {
        {5, 0x240, 0xE88F0}, {6, 0x248, 0xE8960}, {7, 0x260, 0x4F65820}, {8, 0x230, 0}};
    for (const auto& entry : vtable_slots) {
        if (!vtable) break;
        const std::uint64_t target =
            *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(vtable) + entry.slot);
        const std::uint64_t rva = target >= image_base ? target - image_base : 0;
        std::fprintf(stderr, "[rt] loader vtable slot +0x%llx -> 0x%llx (rva +0x%llx)%s\n", entry.slot,
                     static_cast<unsigned long long>(target), static_cast<unsigned long long>(rva),
                     entry.expected ? (rva == entry.expected ? " MATCHES expected" : " MISMATCH") : "");
        if (target >= image_base && target < image_base + 0x10000000ull) {
            LoaderHookSite& hook = g_loader_hooks[entry.site];
            hook.rva = rva;
            ++resolved;
        }
    }
    unsigned installed = 0;
    for (LoaderHookSite& hook : g_loader_hooks)
        if (hook.rva && install_loader_hook(hook, image_base)) ++installed;
    std::fprintf(stderr, "[rt] loader hook: %u detour(s) installed (%u vtable slots resolved)\n", installed,
                 resolved);
    for (const LoaderHookSite& hook : g_loader_hooks) {
        if (!hook.target) continue;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(hook.target);
        char hex[64] = {};
        for (unsigned i = 0; i < 8; ++i) std::snprintf(hex + i * 3, 4, "%02x ", bytes[i]);
        std::fprintf(stderr, "[rt] loader install: %s at 0x%llx bytes %s\n", hook.name,
                     static_cast<unsigned long long>(hook.target), hex);
    }
    std::thread([image_base] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        loader_hook_verify("at 5 s");
        loader_raw_rip_sample(image_base);
        std::this_thread::sleep_for(std::chrono::seconds(55));
        loader_hook_verify("at 60 s");
    }).detach();
    start_loader_vtable_hooks(image_base);
    std::thread([] {
        for (const double at : {30.0, 60.0, 90.0, 120.0}) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(at * 1000.0)));
            char label[16];
            std::snprintf(label, sizeof label, "at %.0f s", at);
            loader_race_summary(label);
        }
    }).detach();
}

// ---- Title state trace (PALMETTO_STATE_TRACE) ----------------------------------------------------------------------
// The title's loading state machine keeps three zero-initialised .data words (image RVAs only, no game data):
//   0x7446510  game/init state. Its only writer is FUN_1453EC0C0 (called from the transition executor FUN_1453EBFE0,
//              which the message pump FUN_1453EBEA0/1453EBE50 runs on a queued transition). 1 = initial, 2 = init/load
//              stage, 3 = running; 4..6 are further stages.
//   0x7453ED0  loading state. Its only writer is FUN_1422572D0, which also notifies FUN_145B5D3D0(old, new). 0 = idle,
//              1 = subsystem initialised (set once by FUN_1422571E0), 2..5 = a load is running (FUN_141D67570 starts
//              one, FUN_141D5D040 stops it), 1 -> 2 is the poll in FUN_14011F740.
//   0x98F828C  session state. FUN_141D83960 dispatches state 0..16 (0/10 = nothing to do); the story-stage boot
//              FUN_1421B6CA0 runs the dispatcher once, then sets up the stage.
// Off unless PALMETTO_STATE_TRACE names "<interval>[,<seconds>]". Every interval it prints the three words with the
// title's own last-change tick, and (bounded) a census of the threads holding title-image frames: each thread's title
// wait site - its RIP when it is running title code, else its nearest title stack word - and the full title stack of
// the threads whose stack names one of the state-machine owners above. A polling probe cannot see a store between two
// samples; the writers are named by the static decompile and the probe measures the timeline plus which parked thread
// holds the machine. RVAs, values and counts only.
constexpr std::uint32_t kTitleInitStateRva = 0x7446510;
constexpr std::uint32_t kTitleInitStampRva = 0x7446508;   // title tick (ms) of the last init-state change
constexpr std::uint32_t kTitleLoadingStateRva = 0x7453ED0;
constexpr std::uint32_t kTitleLoadingFlagsRva = 0x7453ED4;  // byte flags beside the loading state
constexpr std::uint32_t kTitleLoadingTickRva = 0x7453ECC;   // tick updated by the loading poll/update
constexpr std::uint32_t kTitleLoadingStartedRva = 0x7453EBD;  // set by FUN_141D67570 when a load starts
constexpr std::uint32_t kTitleLoadingSinceRva = 0x7453EC0;    // title tick of the last load start
constexpr std::uint32_t kTitleSessionStateRva = 0x98F828C;
constexpr std::uint32_t kTitleSessionCompanionRva = 0x98F8290;
// The stage-2 boot wait: FUN_1421B8960 loops at RVA 0x21B8A3E calling the boot-media tick (0x313550) and the predicate
// at 0x5EA6930 ("byte 0x71B9048 == 2"), then continues at 0x21B8A58. The state machine object (global 0x71273C8, set by
// the stage-2 entry handler) keeps the current stage in a u16 at +0x10 and the requested next stage in a u16 at +0x12
// (fn_813660); the pump FUN_1453EBE50 executes the request when the two differ.
constexpr std::uint32_t kTitleBootMediaGateRva = 0x71B9048;   // byte: 2 = the boot-media stage the wait loop wants
constexpr std::uint32_t kTitleBootMediaActiveRva = 0x71B90AD;  // bytes the boot-media tick tests at 0x313560
constexpr std::uint32_t kTitleBootMediaFlagRva = 0x71B90AC;
constexpr std::uint32_t kTitleStateMachineRva = 0x71273C8;     // state machine object pointer (set at stage-2 entry)
constexpr std::uint32_t kTitlePlatformGateRva = 0xA210D81;     // byte; 1 makes FUN_1421B8960 take its 4 gated waits
constexpr std::uint32_t kTitleLoadingActiveRva = 0x7453EE0;    // byte set by the loading subsystem init

// RVAs of the state machine's own entry points: a thread whose RIP or stack names one of these is holding the machine.
constexpr std::uint32_t kTitleStateOwners[] = {
    0x53EBE50,  // queued-transition pump: runs one pending transition
    0x53EBEA0,  // state machine queue loop
    0x53EBFE0,  // transition executor: writes the init word, then runs the per-state entry/exit
    0x53EC0C0,  // the only writer of the init state word
    0x73C950,   // pump caller (UI module)
    0x810E20,   // pump caller (story module)
    0x1D66670,  // pump caller (event/handler registry)
    0x81B270,   // stage-2 entry handler: calls the boot init then requests stage 3 at 0x81B2EF
    0x21B8960,  // boot init: the wait loop at 0x21B8A3E gates stage 3
    0x22571E0,  // loading subsystem init: sets loading = 1
    0x1D5D040,  // loading stop: sets loading = 0
    0x1D67570,  // loading start: sets loading = mode (requires 0)
    0x11F740,   // loading poll: the only 1 -> 2 transition
    0x4FD1AD0,  // the registered loading task that calls the poll
    0x1D83960,  // session state dispatcher
};

bool title_address_readable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) return false;
    return info.State == MEM_COMMIT && (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

std::uint16_t title_read_u16_at(std::uintptr_t address, bool& ok) {
    ok = title_address_readable(address);
    std::uint16_t value = 0;
    if (ok) std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

bool title_word_readable(std::uintptr_t image_base, std::uint32_t rva) {
    return title_address_readable(image_base + rva);
}


std::uint32_t title_read_u32(std::uintptr_t image_base, std::uint32_t rva, bool& ok) {
    ok = title_word_readable(image_base, rva);
    std::uint32_t value = 0;
    if (ok) std::memcpy(&value, reinterpret_cast<const void*>(image_base + rva), sizeof(value));
    return value;
}

std::uint64_t title_read_u64(std::uintptr_t image_base, std::uint32_t rva, bool& ok) {
    ok = title_word_readable(image_base, rva);
    std::uint64_t value = 0;
    if (ok) std::memcpy(&value, reinterpret_cast<const void*>(image_base + rva), sizeof(value));
    return value;
}

bool title_rva_is_state_owner(std::uint32_t rva) {
    for (const std::uint32_t owner : kTitleStateOwners)
        if (rva == owner) return true;
    return false;
}

// One census: every other thread's title wait site and, for the state-machine owners, its title-side stack. Every
// object is allocated before any thread is suspended, and module_offset() (which takes the shim registry lock) is only
// called after every thread is resumed - the same rule the samplers above follow.
void state_trace_census(std::uintptr_t image_base, std::uintptr_t image_size, double at_seconds) {
    WaitSampleModules modules;
    modules.image_base = image_base;
    modules.image_end = image_base + image_size;
    std::vector<DWORD> tids;
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD pid = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        if (Thread32First(snapshot, &entry))
            do {
                if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self && tids.size() < 256)
                    tids.push_back(entry.th32ThreadID);
            } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
    }
    struct StateThreadRow {
        DWORD tid = 0;
        unsigned rip_rva = 0;   // RIP when the thread is executing title code
        unsigned site_rva = 0;  // RIP if in title code, else the nearest title stack frame
        unsigned frames = 0;
        bool owner = false;
        std::uint64_t stack[10] = {};
    };
    std::vector<StateThreadRow> rows;
    rows.reserve(tids.size());
    for (const DWORD tid : tids) {
        const HANDLE thread =
            OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!thread) continue;
        if (SuspendThread(thread) == (DWORD)-1) {
            CloseHandle(thread);
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        StateThreadRow row;
        row.tid = tid;
        if (GetThreadContext(thread, &context)) {
            if (context.Rip >= image_base && context.Rip < image_base + image_size)
                row.rip_rva = static_cast<unsigned>(context.Rip - image_base);
            row.frames = scan_stack_words_deep(context.Rsp, row.stack, 10, modules, 8);
            for (unsigned i = 0; i < row.frames; ++i) {
                const auto frame_rva = static_cast<unsigned>(row.stack[i] - image_base);
                if (title_rva_is_state_owner(frame_rva)) row.owner = true;
            }
        }
        ResumeThread(thread);
        CloseHandle(thread);
        row.site_rva = row.rip_rva ? row.rip_rva
                                   : (row.frames ? static_cast<unsigned>(row.stack[0] - image_base) : 0u);
        if (row.rip_rva && title_rva_is_state_owner(row.rip_rva)) row.owner = true;
        if (row.site_rva) rows.push_back(row);
    }
    std::map<unsigned, std::pair<unsigned, std::string>> sites;
    for (const StateThreadRow& row : rows) {
        auto& slot = sites[row.site_rva];
        ++slot.first;
        if (slot.second.size() < 48) slot.second += (slot.second.empty() ? "" : ",") + std::to_string(row.tid);
    }
    std::vector<std::pair<unsigned, std::pair<unsigned, std::string>*>> ordered;
    for (auto& [site, slot] : sites) ordered.emplace_back(site, &slot);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.second->first > b.second->first; });
    std::string top;
    const std::size_t shown = std::min<std::size_t>(ordered.size(), 8);
    for (std::size_t i = 0; i < shown; ++i)
        top += " " + rt::shims::module_offset(image_base + ordered[i].first) + " x" +
               std::to_string(ordered[i].second->first);
    std::fprintf(stderr, "[rt] state trace at %.1f s: %zu title-image thread(s), %zu site(s);%s\n", at_seconds,
                 rows.size(), ordered.size(), top.empty() ? " (none)" : top.c_str());
    unsigned owners = 0;
    for (const StateThreadRow& row : rows) {
        if (!row.owner || owners == 2) continue;
        std::string stack;
        for (unsigned i = 0; i < row.frames && i < 10; ++i)
            stack += " " + rt::shims::module_offset(static_cast<std::uintptr_t>(row.stack[i]));
        std::fprintf(stderr, "[rt] state trace at %.1f s: owner tid %lu at %s, title stack:%s\n", at_seconds,
                     static_cast<unsigned long>(row.tid), rt::shims::module_offset(image_base + row.site_rva).c_str(),
                     stack.empty() ? " (none)" : stack.c_str());
        ++owners;
    }
}

// ---- Stage-2 wait wake probe (PALMETTO_STAGE2_WAKE) -----------------------------------------------------------------
// The measured stage-2 stop: the boot sequence FUN_1421B8960 calls FUN_1421B8BD0 (RVA 0x21B8BD0) at 0x21B8A70, whose
// first call is FUN_14258DB70 (RVA 0x258DB70), and that function waits INFINITE at RVA 0x258DBA6 on the event the
// stage-2 tracker keeps at +0x610 when the armed dword at +0x608 is non-zero. The tracker is created by
// FUN_145BF0B10 (RVA 0x5BF0B10) and its pointer is the image .data qword at RVA 0x9844898 (measured: the pointer, the
// armed dword 1 before the wait and 0x80000001 inside it, an auto-reset event at +0x610 that no title code signals).
// This opt-in probe reads the tracker at the named second, logs those fields, and signals the armed event once - the
// smallest completion of the measured wait, so the boot's own next step (the stage-3 request at 0x81B2EF or the next
// stop) is observable under PALMETTO_STATE_TRACE. It signals no other handle and does nothing when the switch is
// unset; the default path never starts a thread for it.
constexpr std::uint32_t kTitleStage2TrackerRva = 0x9844898;

bool title_read_pointer_at(std::uintptr_t absolute, std::uintptr_t& value) {
    if (!title_address_readable(absolute)) return false;
    std::memcpy(&value, reinterpret_cast<const void*>(absolute), sizeof(value));
    return true;
}

void start_stage2_wake_probe(std::uintptr_t image_base, std::uintptr_t image_size, double at_seconds) {
    std::thread([image_base, image_size, at_seconds] {
        const auto started = sampler_epoch();
        auto elapsed = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        };
        std::this_thread::sleep_until(started + std::chrono::milliseconds(static_cast<long long>(at_seconds * 1000.0)));
        // The stage-2 handler's timing varies between boots, so wait for the tracker to appear with an armed event
        // rather than assuming it exists at the named second: poll every 250 ms for at most 60 s.
        std::uintptr_t tracker = 0;
        std::uint32_t armed = 0;
        std::uintptr_t event = 0;
        bool found = false;
        for (int poll = 0; poll < 240; ++poll) {
            tracker = 0;
            armed = 0;
            event = 0;
            if (title_read_pointer_at(image_base + kTitleStage2TrackerRva, tracker) && tracker != 0 &&
                (tracker & 7) == 0) {
                if (title_address_readable(tracker + 0x608))
                    std::memcpy(&armed, reinterpret_cast<const void*>(tracker + 0x608), sizeof(armed));
                title_read_pointer_at(tracker + 0x610, event);
                if (armed != 0 && event != 0) {
                    found = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        std::fprintf(stderr, "[rt] stage2 wake at %.1f s: tracker 0x%llx, armed 0x%x, event 0x%llx\n", elapsed(),
                     static_cast<unsigned long long>(tracker), armed, static_cast<unsigned long long>(event));
        if (!found) {
            std::fprintf(stderr, "[rt] stage2 wake: no armed tracker event within 60 s of the named time\n");
            return;
        }
        const DWORD before = WaitForSingleObject(reinterpret_cast<HANDLE>(event), 0);
        const BOOL hit = SetEvent(reinterpret_cast<HANDLE>(event));
        if (hit) {
            std::fprintf(stderr, "[rt] stage2 wake: object state before 0x%lx, SetEvent ok\n", before);
        } else {
            std::fprintf(stderr, "[rt] stage2 wake: object state before 0x%lx, SetEvent failed (error %lu)\n", before,
                         GetLastError());
        }
        // The wake is one event, not the producer; sample every thread's title-side stack right after it so the
        // title's own continuation is named even if it stops or faults within the second (the measured 41 s case).
        const double offsets[] = {0.15, 0.5, 1.0, 2.0, 4.0};
        double previous = 0.0;
        for (const double offset : offsets) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>((offset - previous) * 1000.0)));
            previous = offset;
            char label[48];
            std::snprintf(label, sizeof label, "wake+%.2f", offset);
            thread_stacks_sample_once(image_base, image_size, label);
            bool ok_init = false;
            const std::uint32_t init = title_read_u32(image_base, kTitleInitStateRva, ok_init);
            std::uint32_t armed_now = 0;
            std::uintptr_t event_now = 0;
            if (title_address_readable(tracker + 0x608))
                std::memcpy(&armed_now, reinterpret_cast<const void*>(tracker + 0x608), sizeof(armed_now));
            title_read_pointer_at(tracker + 0x610, event_now);
            std::fprintf(stderr, "[rt] stage2 wake +%.2f s: tracker 0x%llx armed 0x%x event 0x%llx, init %s%u\n",
                         offset, static_cast<unsigned long long>(tracker), armed_now,
                         static_cast<unsigned long long>(event_now), ok_init ? "" : "unreadable ", init);
        }
    }).detach();
}

// ---- Stage-2 tracker probe (PALMETTO_STAGE2_TRACKER) ----------------------------------------------------------------
// The stage-2 wait is armed by the batch body FUN_1421B8B20 on the tracker the boot sequence creates (global RVA
// 0x9844898). This probe measures the batch's own job list, which the factory FUN_145BF0B10 fills after construction
// (count dword at +0x3C and pointer array at +0x40; a record array of 4-dword records with count at +0x62C and pointer
// at +0x630) and the job fields the scheduler reads (state byte +0xE0, class masks +0xE1/+0xE2/+0xE4, dependency
// callback +0xE8, pending counter +0xF0, completion byte +0x74). It also counts the handles in THIS process that refer
// to the same kernel event object as the waited event at +0x610, so the "HandleCount 2" shape is measured instead of
// inferred from a debugger view. Every read is bounds-checked against the mapped title image / committed pages.
// Off when the switch is unset; the default path starts no thread for it.
constexpr std::uint32_t kTitleStage2TrackerJobLists[3][2] = {
    {0x3C, 0x40},  // batch job list: dword count, qword array pointer
    {0x0C, 0x10},  // second job list: dword count, qword array pointer
    {0x90, 0x88},  // dispatch list: u16 count at +0x90, qword array pointer at +0x88
};

struct Stage2HandleEntry {
    void* object;
    std::uint64_t process_id;
    std::uint64_t handle;
    std::uint32_t granted_access;
    std::uint16_t creator_backtrace;
    std::uint16_t object_type;
    std::uint32_t handle_attributes;
    std::uint32_t reserved;
};
struct Stage2HandleTable {
    std::uint64_t count;
    std::uint64_t reserved;
    Stage2HandleEntry entries[1];
};
struct Stage2ObjectBasicInformation {
    std::uint32_t granted_access;
    std::uint32_t handle_count;
    std::uint32_t pointer_count;
    std::uint32_t reserved[10];
};
using Stage2NtQuerySystemInformation = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using Stage2NtQueryObject = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

std::uint64_t stage2_read_qword(std::uintptr_t address) {
    std::uint64_t value = 0;
    if (title_address_readable(address)) std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

std::uint32_t stage2_read_u32(std::uintptr_t address) {
    std::uint32_t value = 0;
    if (title_address_readable(address)) std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

std::uint16_t stage2_read_u16(std::uintptr_t address) {
    std::uint16_t value = 0;
    if (title_address_readable(address)) std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

std::uint8_t stage2_read_u8(std::uintptr_t address) {
    std::uint8_t value = 0;
    if (title_address_readable(address)) std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

// Renders a qword as "image+0xRVA" when it points into the title image, else as a raw pointer.
std::string stage2_pointer_text(std::uintptr_t image_base, std::uintptr_t image_size, std::uint64_t value) {
    char text[48];
    if (value >= image_base && value < image_base + image_size) {
        std::snprintf(text, sizeof text, "image+0x%llx", static_cast<unsigned long long>(value - image_base));
    } else {
        std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(value));
    }
    return text;
}

void start_stage2_tracker_probe(std::uintptr_t image_base, std::uintptr_t image_size, double at_seconds) {
    std::thread([image_base, image_size, at_seconds] {
        const auto started = sampler_epoch();
        auto elapsed = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        };
        std::this_thread::sleep_until(started + std::chrono::milliseconds(static_cast<long long>(at_seconds * 1000.0)));
        std::uintptr_t tracker = 0;
        std::uint32_t armed = 0;
        std::uintptr_t event = 0;
        bool found = false;
        for (int poll = 0; poll < 1200; ++poll) {
            tracker = 0;
            armed = 0;
            event = 0;
            if (title_read_pointer_at(image_base + kTitleStage2TrackerRva, tracker) && tracker != 0 &&
                (tracker & 7) == 0) {
                armed = stage2_read_u32(tracker + 0x608);
                title_read_pointer_at(tracker + 0x610, event);
                if (armed != 0 || event != 0) {
                    found = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fprintf(stderr, "[rt] stage2 tracker at %.1f s: tracker 0x%llx armed 0x%x event 0x%llx\n", elapsed(),
                     static_cast<unsigned long long>(tracker), armed, static_cast<unsigned long long>(event));
        if (!found) {
            std::fprintf(stderr, "[rt] stage2 tracker: no tracker within 120 s of the named time\n");
            return;
        }
        std::fprintf(stderr,
                     "[rt] stage2 tracker head: vtbl %s armed 0x%x event 0x%llx ring 0x%llx idx/cap %u/%u/%u/%u\n",
                     stage2_pointer_text(image_base, image_size, stage2_read_qword(tracker)).c_str(),
                     stage2_read_u32(tracker + 0x608), static_cast<unsigned long long>(event),
                     static_cast<unsigned long long>(stage2_read_qword(tracker + 0x588)),
                     stage2_read_u16(tracker + 0x598), stage2_read_u16(tracker + 0x59A),
                     stage2_read_u16(tracker + 0x59C), stage2_read_u16(tracker + 0x59E));
        const std::uintptr_t context = stage2_read_qword(tracker + 0x618);
        std::fprintf(stderr,
                     "[rt] stage2 tracker tail: ctx 0x%llx self %s callbacks %s %s ctx+0x50 %s ctx+0x58 %s "
                     "ctx+0x68 %s ctx+0x80 %s ctx+0xD8 0x%x records %u/0x%llx\n",
                     static_cast<unsigned long long>(context),
                     stage2_pointer_text(image_base, image_size, stage2_read_qword(tracker + 0x620)).c_str(),
                     stage2_pointer_text(image_base, image_size, stage2_read_qword(tracker + 0x5F0)).c_str(),
                     stage2_pointer_text(image_base, image_size, stage2_read_qword(tracker + 0x600)).c_str(),
                     stage2_pointer_text(image_base, image_size, stage2_read_qword(context + 0x50)).c_str(),
                     stage2_pointer_text(image_base, image_size, stage2_read_qword(context + 0x58)).c_str(),
                     stage2_pointer_text(image_base, image_size, stage2_read_qword(context + 0x68)).c_str(),
                     stage2_pointer_text(image_base, image_size, stage2_read_qword(context + 0x80)).c_str(),
                     stage2_read_u32(context + 0xD8), stage2_read_u32(tracker + 0x62C),
                     static_cast<unsigned long long>(stage2_read_qword(tracker + 0x630)));
        // Dump each job list: the job object's own words (resolved to RVAs where they are image pointers) and the
        // scheduler-visible fields. Compact one line per job so a boot's stderr stays readable.
        auto dump_jobs = [&](const char* label) {
            for (int list = 0; list < 3; ++list) {
                const std::uint32_t count_offset = kTitleStage2TrackerJobLists[list][0];
                const std::uint32_t array_offset = kTitleStage2TrackerJobLists[list][1];
                const std::uint32_t count =
                    (list == 2) ? stage2_read_u16(tracker + count_offset) : stage2_read_u32(tracker + count_offset);
                const std::uintptr_t array = stage2_read_qword(tracker + array_offset);
                std::fprintf(stderr, "[rt] stage2 %s list %d: count %u array 0x%llx\n", label, list, count,
                             static_cast<unsigned long long>(array));
                if (count == 0 || count > 200 || !title_address_readable(array)) continue;
                const std::uint32_t shown = std::min<std::uint32_t>(count, 96);
                for (std::uint32_t i = 0; i < shown; ++i) {
                    const std::uintptr_t job = stage2_read_qword(array + static_cast<std::uintptr_t>(i) * 8);
                    if (job == 0 || !title_address_readable(job)) continue;
                    std::fprintf(stderr,
                                 "[rt] stage2 %s job %u ptr 0x%llx q0 %s q8 %s q10 %s q18 %s | state %u e1 0x%x "
                                 "e2 0x%x e4 0x%x e8 %s f0 %d f7 %u | f70 %u f72 %u f74 %u c8 %u e0 0x%x\n",
                                 label, i, static_cast<unsigned long long>(job),
                                 stage2_pointer_text(image_base, image_size, stage2_read_qword(job)).c_str(),
                                 stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 8)).c_str(),
                                 stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0x10)).c_str(),
                                 stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0x18)).c_str(),
                                 stage2_read_u8(job + 0xE0), stage2_read_u8(job + 0xE1),
                                 stage2_read_u8(job + 0xE2), stage2_read_u16(job + 0xE4),
                                 stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0xE8)).c_str(),
                                 static_cast<int>(stage2_read_u32(job + 0xF0)), stage2_read_u8(job + 0xF7),
                                 stage2_read_u16(job + 0x70), stage2_read_u8(job + 0x72), stage2_read_u8(job + 0x74),
                                 stage2_read_u8(job + 0xC8), stage2_read_u8(job + 0xE0));
                }
            }
        };
        dump_jobs("first");
        // The record array at +0x630: 4-dword records the builder writes a dword in; print the first 48.
        const std::uintptr_t records = stage2_read_qword(tracker + 0x630);
        const std::uint32_t record_count = stage2_read_u32(tracker + 0x62C);
        if (records != 0 && title_address_readable(records) && record_count <= 200) {
            std::string text;
            for (std::uint32_t i = 0; i < std::min<std::uint32_t>(record_count, 48); ++i) {
                const std::uintptr_t record = records + static_cast<std::uintptr_t>(i) * 16;
                char one[80];
                std::snprintf(one, sizeof one, " %u:%08x/%08x/%08x/%08x", i, stage2_read_u32(record),
                              stage2_read_u32(record + 4), stage2_read_u32(record + 8), stage2_read_u32(record + 12));
                text += one;
            }
            std::fprintf(stderr, "[rt] stage2 records%s\n", text.c_str());
        }
        // Handle census: which handles in this process refer to the same kernel event object as +0x610.
        if (event != 0) {
            HMODULE ntdll = GetModuleHandleA("ntdll.dll");
            auto query_system =
                reinterpret_cast<Stage2NtQuerySystemInformation>(ntdll ? GetProcAddress(ntdll, "NtQuerySystemInformation")
                                                                      : nullptr);
            if (query_system) {
                std::vector<unsigned char> buffer(1u << 22);
                ULONG needed = 0;
                LONG status = query_system(64, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
                if (status != 0 && needed > buffer.size() && needed < (64u << 20)) {
                    buffer.resize(needed + 0x1000);
                    status = query_system(64, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
                }
                if (status == 0) {
                    auto* table = reinterpret_cast<Stage2HandleTable*>(buffer.data());
                    const std::uint64_t pid = GetCurrentProcessId();
                    void* object = nullptr;
                    for (std::uint64_t i = 0; i < table->count; ++i) {
                        const Stage2HandleEntry& entry = table->entries[i];
                        if (entry.process_id == pid && entry.handle == event) {
                            object = entry.object;
                            break;
                        }
                    }
                    unsigned same_here = 0, same_all = 0;
                    std::string details;
                    if (object != nullptr) {
                        for (std::uint64_t i = 0; i < table->count; ++i) {
                            const Stage2HandleEntry& entry = table->entries[i];
                            if (entry.object != object) continue;
                            ++same_all;
                            if (entry.process_id != pid) continue;
                            ++same_here;
                            char one[64];
                            std::snprintf(one, sizeof one, " 0x%llx/access0x%x",
                                          static_cast<unsigned long long>(entry.handle), entry.granted_access);
                            details += one;
                        }
                    }
                    std::fprintf(stderr, "[rt] stage2 event object %p: %u handle(s) in this process, %u system-wide%s\n",
                                 object, same_here, same_all, details.c_str());
                } else {
                    std::fprintf(stderr, "[rt] stage2 event object: NtQuerySystemInformation status 0x%lx\n", status);
                }
            }
            auto query_object = reinterpret_cast<Stage2NtQueryObject>(
                ntdll ? GetProcAddress(ntdll, "NtQueryObject") : nullptr);
            if (query_object) {
                Stage2ObjectBasicInformation info{};
                ULONG length = 0;
                const LONG status = query_object(reinterpret_cast<HANDLE>(event), 0, &info, sizeof info, &length);
                std::fprintf(stderr,
                             "[rt] stage2 event basic info: status 0x%lx length %lu granted 0x%x handles %u "
                             "pointers %u\n",
                             status, length, info.granted_access, info.handle_count, info.pointer_count);
            }
        }
        // Second sample 3 s later: did any job state or the batch counter move while the wait is parked?
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::fprintf(stderr, "[rt] stage2 tracker +3 s: armed 0x%x\n", stage2_read_u32(tracker + 0x608));
        dump_jobs("plus3s");
    }).detach();
}

// Starts the sampler thread named by PALMETTO_STATE_TRACE ("<interval>[,<seconds>]"; "1" means 2 s to process end).
// Detached: it ends with the process. Off when the variable is unset, so the default path is unchanged.
void start_title_state_trace(std::uintptr_t image_base, std::uintptr_t image_size, const char* spec) {
    double interval = 2.0;
    double stop_after = 0.0;
    char* end = nullptr;
    const double first = std::strtod(spec, &end);
    if (end != spec) interval = first;
    if (end && *end == ',') {
        char* end2 = nullptr;
        const double second = std::strtod(end + 1, &end2);
        if (end2 != end + 1) stop_after = second;
    }
    if (interval < 0.2) interval = 0.2;
    if (interval > 60.0) interval = 60.0;
    std::thread([image_base, image_size, interval, stop_after] {
        const auto started = sampler_epoch();
        unsigned last_init = 0xFFFFFFFFu, last_loading = 0xFFFFFFFFu, last_session = 0xFFFFFFFFu;
        unsigned last_gate = 0xFFFFFFFFu, last_machine_next = 0xFFFFu;
        double init_changed = 0.0, loading_changed = 0.0, session_changed = 0.0;
        unsigned sample = 0, censuses = 0;
        std::fprintf(stderr,
                     "[rt] state trace: interval %.1f s%s; words init 0x%x, loading 0x%x, session 0x%x\n", interval,
                     stop_after > 0.0 ? (", until " + std::to_string(static_cast<int>(stop_after)) + " s").c_str()
                                      : "",
                     kTitleInitStateRva, kTitleLoadingStateRva, kTitleSessionStateRva);
        for (;;) {
            const double at =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (stop_after > 0.0 && at > stop_after) break;
            bool ok_init = false, ok_loading = false, ok_session = false, ok_tick = false, ok_started = false;
            const std::uint32_t init = title_read_u32(image_base, kTitleInitStateRva, ok_init);
            const std::uint32_t loading = title_read_u32(image_base, kTitleLoadingStateRva, ok_loading);
            const std::uint32_t session = title_read_u32(image_base, kTitleSessionStateRva, ok_session);
            bool ok_stamp = false, ok_since = false, ok_flags = false;
            const std::uint64_t init_stamp = title_read_u64(image_base, kTitleInitStampRva, ok_stamp);
            const std::uint32_t loading_tick = title_read_u32(image_base, kTitleLoadingTickRva, ok_tick);
            const std::uint32_t loading_started = title_read_u32(image_base, kTitleLoadingStartedRva, ok_started);
            const std::uint64_t loading_since = title_read_u64(image_base, kTitleLoadingSinceRva, ok_since);
            const std::uint32_t loading_flags = title_read_u32(image_base, kTitleLoadingFlagsRva, ok_flags);
            bool ok_gate = false, ok_media = false, ok_media_flag = false, ok_platform = false, ok_active = false,
                 ok_companion = false, ok_machine = false, ok_current = false, ok_next = false;
            const std::uint32_t boot_media_gate = title_read_u32(image_base, kTitleBootMediaGateRva, ok_gate);
            const std::uint32_t boot_media_active = title_read_u32(image_base, kTitleBootMediaActiveRva, ok_media);
            const std::uint32_t boot_media_flag = title_read_u32(image_base, kTitleBootMediaFlagRva, ok_media_flag);
            const std::uint32_t platform_gate = title_read_u32(image_base, kTitlePlatformGateRva, ok_platform);
            const std::uint32_t loading_active = title_read_u32(image_base, kTitleLoadingActiveRva, ok_active);
            const std::uint32_t session_companion = title_read_u32(image_base, kTitleSessionCompanionRva, ok_companion);
            const std::uint64_t state_machine = title_read_u64(image_base, kTitleStateMachineRva, ok_machine);
            std::uint16_t machine_current = 0, machine_next = 0;
            if (ok_machine && state_machine && (state_machine & 7) == 0) {
                machine_current = title_read_u16_at(static_cast<std::uintptr_t>(state_machine) + 0x10, ok_current);
                machine_next = title_read_u16_at(static_cast<std::uintptr_t>(state_machine) + 0x12, ok_next);
            }
            if (ok_init && init != last_init) {
                if (last_init != 0xFFFFFFFFu)
                    std::fprintf(stderr, "[rt] state change at %.1f s: init %u -> %u (title tick %llu)\n", at,
                                 last_init, init, static_cast<unsigned long long>(init_stamp));
                init_changed = at;
                last_init = init;
            }
            if (ok_loading && loading != last_loading) {
                if (last_loading != 0xFFFFFFFFu)
                    std::fprintf(stderr, "[rt] state change at %.1f s: loading %u -> %u (tick %u, started %u)\n", at,
                                 last_loading, loading, loading_tick, loading_started);
                loading_changed = at;
                last_loading = loading;
            }
            if (ok_session && session != last_session) {
                if (last_session != 0xFFFFFFFFu)
                    std::fprintf(stderr, "[rt] state change at %.1f s: session %u -> %u\n", at, last_session, session);
                session_changed = at;
                last_session = session;
            }
            if (ok_gate && (boot_media_gate & 0xFF) != (last_gate & 0xFF)) {
                if (last_gate != 0xFFFFFFFFu)
                    std::fprintf(stderr, "[rt] state change at %.1f s: boot-media gate 0x%x -> 0x%x\n", at,
                                 last_gate & 0xFF, boot_media_gate & 0xFF);
                last_gate = boot_media_gate;
            }
            if (ok_next && machine_next != last_machine_next) {
                if (last_machine_next != 0xFFFFu)
                    std::fprintf(stderr, "[rt] state change at %.1f s: machine pending stage %u -> %u (current %u)\n",
                                 at, last_machine_next, machine_next, machine_current);
                last_machine_next = machine_next;
            }
            std::fprintf(stderr,
                         "[rt] state trace at %.1f s: init %s%u (last change %.1f s, tick %llu), loading %s%u"
                         " (last change %.1f s, tick %u, started %u, since %llu, flags 0x%x), session %s%u"
                         " (last change %.1f s)\n",
                         at, ok_init ? "" : "?", init, init_changed, static_cast<unsigned long long>(init_stamp),
                         ok_loading ? "" : "?", loading, loading_changed, loading_tick, loading_started,
                         static_cast<unsigned long long>(loading_since), loading_flags, ok_session ? "" : "?", session,
                         session_changed);
            std::fprintf(stderr,
                         "[rt] state trace at %.1f s: boot-media gate %s0x%x (active %s0x%x flag %s0x%x), platform gate"
                         " %s0x%x, loading-active %s0x%x, session companion %s0x%x, machine 0x%llx current %s%u next"
                         " %s%u\n",
                         at, ok_gate ? "" : "?", boot_media_gate, ok_media ? "" : "?", boot_media_active,
                         ok_media_flag ? "" : "?", boot_media_flag, ok_platform ? "" : "?", platform_gate,
                         ok_active ? "" : "?", loading_active, ok_companion ? "" : "?", session_companion,
                         static_cast<unsigned long long>(state_machine), ok_current ? "" : "?", machine_current,
                         ok_next ? "" : "?", machine_next);
            ++sample;
            const bool census_now = censuses < 12 || (sample % 10 == 0 && censuses < 24);
            if (census_now) {
                state_trace_census(image_base, image_size, at);
                ++censuses;
            }
            std::this_thread::sleep_until(
                started + std::chrono::milliseconds(static_cast<long long>((sample + 1) * interval * 1000.0)));
        }
    }).detach();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path image, game_dir, overlay_dir, mods_dir;
    uint32_t archetype_lookup_rva = 0;  // --archetype-lookup RVA[,ARG]: the game's archetype lookup by hash
    int archetype_lookup_arg = 0;
    uint32_t view_mode_getter_rva = 0;  // --view-mode-getter RVA: the game's view mode of a camera context
    bool dry_run = false, init = false, run = false, trace_d3d12 = false;
    std::vector<std::wstring> game_args;  // wide: path::string() throws on a character the code page lacks (RT-84)
    std::wstring trace_file;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--") {
            for (++i; i < argc; ++i) game_args.push_back(argv[i]);
            break;
        }
        if (a == L"--dry-run") dry_run = true;
        else if (a == L"--init") init = true;
        else if (a == L"--run") run = true;
        else if (a == L"--trace-d3d12") trace_d3d12 = true;
        else if (a == L"--trace" && i + 1 < argc) trace_file = argv[++i];
        else if (a == L"--game-dir" && i + 1 < argc) game_dir = argv[++i];
        else if (a == L"--overlay" && i + 1 < argc) overlay_dir = argv[++i];
        else if (a == L"--mods" && i + 1 < argc) mods_dir = argv[++i];
        else if (a == L"--view-mode-getter" && i + 1 < argc) {
            wchar_t* end = nullptr;
            const unsigned long rva = std::wcstoul(argv[++i], &end, 16);
            if (!end || *end || rva == 0) return usage();
            view_mode_getter_rva = static_cast<uint32_t>(rva);
        }
        else if (a == L"--archetype-lookup" && i + 1 < argc) {
            const std::wstring v = argv[++i];
            const auto comma = v.find(L',');
            wchar_t* end = nullptr;
            const unsigned long rva = std::wcstoul(v.substr(0, comma).c_str(), &end, 16);
            if (!end || *end || rva == 0) return usage();
            archetype_lookup_rva = static_cast<uint32_t>(rva);
            if (comma != std::wstring::npos) archetype_lookup_arg = v.substr(comma + 1) == L"1" ? 1 : 0;
        }
        else if (image.empty()) image = a;
        else return usage();
    }
    if (image.empty()) return usage();
    // Exactly one mode: --dry-run --run would otherwise initialise the modules and call the entry point.
    if (dry_run + init + run != 1) return usage();
    // The game runs AVX code from its TLS callbacks on; without AVX that is a bare 0xC000001D (src/host_cpu.h).
    if ((init || run) && !rt::host_cpu::check_skipped())
        if (const auto missing = rt::host_cpu::missing_host_features(); !missing.empty()) {
            std::fprintf(stderr, "%s\n", rt::host_cpu::message(missing).c_str());
            return rt::host_cpu::kExitCode;
        }
    // The console folders are fixed once, as absolute paths. A bare image name (`palmetto --run game.exe` from the
    // game folder) has an empty parent path, which left GAME: with no root; and a relative --game-dir or --overlay
    // would follow the title's later SetCurrentDirectory. If absolute() fails the path stays as given.
    const auto absolute_or_same = [](const std::filesystem::path& p) {
        std::error_code ec;
        std::filesystem::path a = std::filesystem::absolute(p, ec);
        return ec ? p : a;
    };
    game_dir = game_dir.empty() ? absolute_or_same(image).parent_path() : absolute_or_same(game_dir);
    if (!overlay_dir.empty()) overlay_dir = absolute_or_same(overlay_dir);
    // PALMETTO_SHADERS too: the D3D12 device reads it at the title's first pipeline state, after it may have moved.
    if (const DWORD size = GetEnvironmentVariableW(L"PALMETTO_SHADERS", nullptr, 0)) {
        std::wstring shaders(size, L'\0');
        const DWORD n = GetEnvironmentVariableW(L"PALMETTO_SHADERS", shaders.data(), size);
        if (n && n < size) {
            shaders.resize(n);
            SetEnvironmentVariableW(L"PALMETTO_SHADERS", absolute_or_same(shaders).c_str());
        }
    }
    // An image without relocations whose range this process already uses (arm64 Wine on macOS) runs in a new
    // process that has the range reserved from the start (base_reservation.h); that process does not do this again.
    if (!rt::base_reservation::inherited())
        if (const auto range = rt::base_reservation::needed(image)) return rt::base_reservation::relaunch(*range);
    // GAME: paths read under --game-dir; whatever a title writes lands in the overlay, never in the package.
    if (overlay_dir.empty()) overlay_dir = game_dir / "overlay";
    rt::fs::configure(game_dir, overlay_dir);
    // Mods (docs/modding.md): <folder of palmetto.exe>\mods unless --mods names another. Loaded and reported here;
    // the archetype swaps take effect once the game's archetype lookup is hooked for this executable version.
    if (mods_dir.empty()) {
        std::wstring self(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        if (n && n < self.size()) mods_dir = std::filesystem::path(self.substr(0, n)).parent_path() / L"extensions";
    }
    {
        const palmetto::mods::LoadResult mods = palmetto::mods::load_mods(mods_dir);
        for (const std::string& e : mods.errors) std::fprintf(stderr, "[rt] mods: skipped %s\n", e.c_str());
        for (const auto& m : mods.mods) {
            std::fprintf(stderr, "[rt] mods: %s%s\n", m.name.c_str(), m.enabled ? "" : " (disabled)");
            for (const auto& sw : m.archetype_swaps)
                std::fprintf(stderr, "[rt] mods:   archetype %s (0x%08X) -> %s (0x%08X)\n", sw.from_name.c_str(),
                             sw.from, sw.to_name.c_str(), sw.to);
        }
        palmetto::mods::set_active(palmetto::mods::ArchetypeSwaps(mods.mods));
        const palmetto::mods::CameraView camera = palmetto::mods::camera_of(mods.mods);
        palmetto::mods::set_active_camera(camera);
        if (camera.first_person)
            std::fprintf(stderr, "[rt] mods: first-person view (value %u) for %s%s\n", camera.first_person_value,
                         camera.contexts.empty() ? "every context" : "the listed contexts",
                         view_mode_getter_rva ? "" : "; not applied: no --view-mode-getter RVA for this executable");
        if (palmetto::mods::active().size())
            std::fprintf(stderr, archetype_lookup_rva ? "[rt] mods: %zu archetype swap(s) loaded\n"
                                 : "[rt] mods: %zu archetype swap(s) loaded; not applied: no --archetype-lookup RVA for "
                                   "this executable (docs/modding.md)\n", palmetto::mods::active().size());
    }
    rt::d3d12_trace::set_enabled(trace_d3d12);
    // Slot 133 imports the title's pages; xmem commits them on first touch, so they are committed first.
    rt::d3d12_xs::set_placed_at_va_pages_hook(&rt::xmem::commit_physical_range);
    if (int trace_error = 0; !trace_file.empty() && !rt::startup_trace::open(trace_file.c_str(), &trace_error)) {
        std::fprintf(stderr, trace_error == EEXIST ? "error: the trace file already exists; --trace only creates a new file\n"
                                                   : "error: cannot create the trace file\n");
        return 2;
    }

    try {
        rt::Loader loader(game_dir);
        rt::Module& main_module = loader.load(image);  // maps, binds and installs TLS; runs no image code
        // The mods' archetype swaps (docs/modding.md): hooked here, before any image code runs, when the lookup's RVA
        // for this executable is given. The count of redirected calls shows whether the RVA is the real lookup.
        if (view_mode_getter_rva) {
            const auto r = palmetto::hooks::hook_image_results(main_module.base, view_mode_getter_rva,
                                                              &palmetto::mods::override_view_mode);
            if (r.ok)
                std::fprintf(stderr, "[rt] mods: view-mode getter at RVA 0x%X hooked: %zu direct call(s) redirected\n",
                             view_mode_getter_rva, r.redirected);
            else
                std::fprintf(stderr, "[rt] mods: view-mode getter at RVA 0x%X not hooked: %s\n", view_mode_getter_rva,
                             r.error);
        }
        if (archetype_lookup_rva) {
            const auto r = palmetto::hooks::hook_image_calls(
                main_module.base, archetype_lookup_rva,
                [](uint32_t hash) { return palmetto::mods::active().remap(hash); }, archetype_lookup_arg);
            if (r.ok)
                std::fprintf(stderr, "[rt] mods: archetype lookup at RVA 0x%X hooked (argument %d): %zu direct "
                                     "call(s) redirected\n", archetype_lookup_rva, archetype_lookup_arg, r.redirected);
            else
                std::fprintf(stderr, "[rt] mods: archetype lookup at RVA 0x%X not hooked: %s\n", archetype_lookup_rva,
                             r.error);
        }
        if (run) rt::process::set_command_line(image.filename().wstring(), game_args);
        std::exception_ptr init_failure;
        if (init || run) {
            try {
                loader.initialise();  // TLS callbacks, then DllMain(DLL_PROCESS_ATTACH) for DLLs
                std::printf("%zu module(s) initialised\n", loader.modules().size());
            } catch (const std::exception&) {
                // Keep the mapped modules available for the binding report on an attach failure.
                init_failure = std::current_exception();
            }
        }
        int unimplemented = 0, unimplemented_hresult = 0;
        for (auto* m : loader.modules()) {
            std::map<rt::Binding, int> counts;
            for (auto& r : m->imports) counts[r.binding]++;
            std::printf("%-36s base=%p%s size=%u entry=+%#x  shim=%d host=%d module=%d unimplemented=%d\n",
                        m->name.c_str(), m->base, m->relocated ? " (relocated)" : "", m->size, m->entry_rva,
                        counts[rt::Binding::Shim], counts[rt::Binding::Host], counts[rt::Binding::Module],
                        counts[rt::Binding::Unimplemented]);
            for (auto& r : m->imports)
                if (r.binding == rt::Binding::Unimplemented || r.binding == rt::Binding::Shim) {
                    const bool missing = r.binding == rt::Binding::Unimplemented;
                    const bool hresult =
                        missing && rt::shims::classify_import(r.dll, r.name) == rt::shims::ReturnClass::HResult;
                    std::printf("    %-13s %s!%s%s\n", binding_name(r.binding), r.dll.c_str(), r.name.c_str(),
                                hresult ? "  [returns HRESULT]" : "");
                    unimplemented += missing;
                    unimplemented_hresult += hresult;
                }
        }
        std::printf("%zu module(s) mapped, %d unimplemented import(s)\n", loader.modules().size(), unimplemented);
        std::printf("unimplemented by return policy: hresult=%d other=%d\n", unimplemented_hresult,
                    unimplemented - unimplemented_hresult);
        if (init_failure) {
            std::fflush(stdout);
            std::rethrow_exception(init_failure);
        }
        if (!run) {
            rt::startup_trace::event("exit", "\"code\":0");
            return 0;
        }
        if (!main_module.entry_rva) {
            std::fprintf(stderr, "error: %s has no entry point\n", main_module.name.c_str());
            return 1;
        }
        const std::wstring paths = L"console paths: GAME: -> " + game_dir.native() + L", writes -> " + overlay_dir.native();
        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;
        bool printed = false;
        if (GetConsoleMode(out, &mode)) {
            std::fflush(stdout);
            const std::wstring line = paths + L"\r\n";
            DWORD written;
            printed = WriteConsoleW(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != FALSE;
        }
        if (!printed) {
            const auto game = game_dir.u8string();
            const auto overlay = overlay_dir.u8string();
            std::printf("console paths: GAME: -> %s, writes -> %s\n",
                        reinterpret_cast<const char*>(game.c_str()), reinterpret_cast<const char*>(overlay.c_str()));
        }
        // PALMETTO_RESOLVE=<hex address>: say what lives at an address a call or a store names, so a target outside the
        // mapped modules is identified rather than guessed at. Prints the allocation's state, protection and, when it
        // belongs to a loaded module, that module's name.
        if (const DWORD size = GetEnvironmentVariableW(L"PALMETTO_RESOLVE", nullptr, 0); size > 1) {
            std::wstring value(size, L'\0');
            const DWORD n = GetEnvironmentVariableW(L"PALMETTO_RESOLVE", value.data(), size);
            if (n > 0 && n < size) value.resize(n);
            const auto address = std::wcstoull(value.c_str(), nullptr, 16);
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == sizeof(info)) {
                std::printf("[rt] resolve 0x%llx: state 0x%lx protect 0x%lx type 0x%lx allocation 0x%llx size 0x%llx\n",
                            static_cast<unsigned long long>(address), static_cast<unsigned long>(info.State),
                            static_cast<unsigned long>(info.Protect), static_cast<unsigned long>(info.Type),
                            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info.AllocationBase)),
                            static_cast<unsigned long long>(info.RegionSize));
            } else {
                std::printf("[rt] resolve 0x%llx: not committed\n", static_cast<unsigned long long>(address));
            }
            std::fflush(stdout);
        }
        // PALMETTO_SEED_GLOBAL=<rva>:<size> allocates a zeroed block and points a mapped image's 8-byte global at it
        // before the title's entry point runs. Diagnostic for a global whose initialiser exists but is never reached on
        // this run: it separates "the producer never runs" from "the consumer is wrong". Prints what it did.
        if (const DWORD size = GetEnvironmentVariableW(L"PALMETTO_SEED_GLOBAL", nullptr, 0); size > 1) {
            std::wstring value(size, L'\0');
            const DWORD n = GetEnvironmentVariableW(L"PALMETTO_SEED_GLOBAL", value.data(), size);
            if (n > 0 && n < size) value.resize(n);
            const std::wstring spec(value);
            const auto colon = spec.find(L':');
            const auto rva = std::wcstoull(spec.substr(0, colon).c_str(), nullptr, 16);
            const auto bytes = colon == std::wstring::npos ? 0x1000u
                                                           : static_cast<std::size_t>(
                                                                 std::wcstoull(spec.substr(colon + 1).c_str(), nullptr, 16));
            if (void* block = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
                *reinterpret_cast<void**>(main_module.base + rva) = block;
                std::fprintf(stderr, "[rt] seeded image+0x%llx with %zu zeroed bytes at %p (stand-in)\n",
                             static_cast<unsigned long long>(rva), bytes, block);
            } else {
                std::fprintf(stderr, "[rt] could not seed image+0x%llx (error %lu)\n",
                             static_cast<unsigned long long>(rva), GetLastError());
            }
        }
        // PALMETTO_TEB_CHECK=1 validates the TEB offsets the loader's TLS rewrite depends on, from host code: allocate a
        // TlsAlloc slot, store a marker in it, then read it back the way the rewritten guest code does. If the offsets do
        // not agree, every TLS read the rewrite produces is wrong, which no amount of guest-side logging can show.
        if (std::getenv("PALMETTO_TEB_CHECK")) {
            void* const marker = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1122334455667788ull));
            const DWORD slot = TlsAlloc();
            const bool stored = slot != TLS_OUT_OF_INDEXES && TlsSetValue(slot, marker) != FALSE;
            const auto array_pointer = static_cast<std::uintptr_t>(__readgsqword(0x58));
            const auto cell_58 = static_cast<std::uintptr_t>(__readgsqword(0x58 + 8 * slot));
            const auto cell_1480 = static_cast<std::uintptr_t>(__readgsqword(0x1480 + 8 * slot));
            const auto cell_1780 = static_cast<std::uintptr_t>(__readgsqword(0x1780 + 8 * slot));
            const auto via_tls_get = reinterpret_cast<std::uintptr_t>(TlsGetValue(slot));
            std::printf("[rt] TEB check: slot %lu marker %p; TlsGetValue %p; gs:[58] %p; gs:[58+8*slot] %p; "
                        "gs:[1480+8*slot] %p; gs:[1780+8*slot] %p\n",
                        static_cast<unsigned long>(slot), marker, reinterpret_cast<void*>(via_tls_get),
                        reinterpret_cast<void*>(array_pointer), reinterpret_cast<void*>(cell_58),
                        reinterpret_cast<void*>(cell_1480), reinterpret_cast<void*>(cell_1780));
            if (slot != TLS_OUT_OF_INDEXES) TlsFree(slot);
        }
        // PALMETTO_DUMP_BYTES=<hex rva>[,<hex rva>...]: print words of the mapped image at those RVAs, after the loader
        // has patched it. This is the only way to see what the guest will actually execute: the file on disk still holds
        // the unpatched instruction.
        if (const DWORD size = GetEnvironmentVariableW(L"PALMETTO_DUMP_BYTES", nullptr, 0); size > 1) {
            std::wstring value(size, L'\0');
            const DWORD n = GetEnvironmentVariableW(L"PALMETTO_DUMP_BYTES", value.data(), size);
            if (n > 0 && n < size) value.resize(n);
            size_t start = 0;
            while (start <= value.size()) {
                const size_t comma = value.find(L',', start);
                const std::wstring piece =
                    value.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                if (!piece.empty()) {
                    const auto rva = std::wcstoull(piece.c_str(), nullptr, 16);
                    const auto* at = reinterpret_cast<const std::uint8_t*>(main_module.base + rva);
                    std::printf("[rt] bytes at image+0x%llx:", static_cast<unsigned long long>(rva));
                    for (int i = 0; i < 32; ++i) std::printf(" %02x", at[i]);
                    std::printf("\n");
                }
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
            std::fflush(stdout);
        }
        if (const DWORD size = GetEnvironmentVariableW(L"PALMETTO_POKE_LEA", nullptr, 0); size > 1) {
            std::wstring value(size, L'\0');
            const DWORD n = GetEnvironmentVariableW(L"PALMETTO_POKE_LEA", value.data(), size);
            if (n > 0 && n < size) value.resize(n);
            const auto rva = std::wcstoull(value.c_str(), nullptr, 16);
            auto* const at = main_module.base + rva;
            DWORD old = 0;
            if (VirtualProtect(at, 7, PAGE_EXECUTE_READWRITE, &old)) {
                at[0] = 0xCC;
                for (int i = 1; i < 7; ++i) at[i] = 0x90;
                VirtualProtect(at, 7, old, &old);
                rt::startup_trace::set_poke_lea_address(at);
                std::printf("[rt] poked an int3 over the lea at image+0x%llx\n", static_cast<unsigned long long>(rva));
            }
        }
        if (const DWORD size = GetEnvironmentVariableW(L"PALMETTO_POKE_LOG", nullptr, 0); size > 1) {
            std::wstring value(size, L'\0');
            const DWORD n = GetEnvironmentVariableW(L"PALMETTO_POKE_LOG", value.data(), size);
            if (n > 0 && n < size) value.resize(n);
            std::uintptr_t addresses[4] = {};
            unsigned char originals[4] = {};
            unsigned count = 0;
            size_t start = 0;
            while (start <= value.size() && count < 4) {
                const size_t comma = value.find(L',', start);
                const std::wstring piece =
                    value.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                if (!piece.empty()) {
                    const auto rva = std::wcstoull(piece.c_str(), nullptr, 16);
                    auto* const at = main_module.base + rva;
                    DWORD old = 0;
                    if (VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) {
                        originals[count] = *at;
                        *at = 0xCC;
                        VirtualProtect(at, 1, old, &old);
                        addresses[count] = reinterpret_cast<std::uintptr_t>(at);
                        ++count;
                        std::printf("[rt] poked a register log at image+0x%llx (byte now %02x, was %02x)\n",
                                    static_cast<unsigned long long>(rva), *at, originals[count ? count - 1 : 0]);
                    }
                }
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
            rt::startup_trace::set_poke_log(addresses, originals, count);
            std::fflush(stdout);
        }
        // PALMETTO_BACK_IMAGES=1 commits zeroed memory over the console image-address range, which starts at 4 GiB. Our
        // descriptor code hands that range out as a texture's address on the strength of "nothing dereferences it" - but
        // the title has now been measured dereferencing exactly 0x100000000 as a pointer. With the range committed, the
        // read returns zero instead of faulting, which says whether the title treats that address as memory (and what it
        // does with what it finds there).
        if (std::getenv("PALMETTO_BACK_IMAGES")) {
            void* const want = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1) << 32);
            constexpr std::size_t bytes = 64ull << 20;
            void* const got = VirtualAlloc(want, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            std::printf("[rt] backed the console image range at %p (%zu MiB): %s\n", want, bytes >> 20,
                        got ? "committed" : "failed");
            std::fflush(stdout);
        }
        rt::startup_trace::set_crash_image(main_module.base, main_module.size, main_module.function_table,
                                           main_module.function_count);
        // PALMETTO_WAIT_SAMPLE=<seconds,...>: sample every thread's blocked site while the title runs (diagnostic; see
        // the wait-state sampler above). Off when unset.
        if (const char* spec = std::getenv("PALMETTO_WAIT_SAMPLE"); spec && *spec)
            start_wait_sampler(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size, spec);
        // PALMETTO_WAIT_TRACE=<seconds,...>: name every host wait the title reaches - API, caller site, object, timeout,
        // outcome - and dump the aggregates, the per-object set correlation and the still-outstanding waits at the given
        // times (diagnostic; see wait_trace.h). Off when unset.
        if (const char* spec = std::getenv("PALMETTO_WAIT_TRACE"); spec && *spec)
            palmetto::wait_trace::start(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size, spec);
        // PALMETTO_RIP_HIST=<start,end[;start,end]>: title-RIP histogram windows (diagnostic; see the RIP histogram
        // above). Off when unset.
        if (const char* spec = std::getenv("PALMETTO_RIP_HIST"); spec && *spec)
            start_rip_histogram(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size, spec);
        // PALMETTO_THREAD_STACKS=<seconds,...>: capture every thread's full title-side stack (diagnostic; see the
        // full thread-stack sampler above). Off when unset.
        if (const char* spec = std::getenv("PALMETTO_THREAD_STACKS"); spec && *spec)
            start_thread_stack_sampler(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size, spec);
        // PALMETTO_STATE_TRACE=<interval>[,<seconds>]: sample the title's init/loading/session state words and the
        // title-side wait site/stack of the threads holding the state machine (diagnostic; see the state trace above).
        // Off when unset.
        if (const char* spec = std::getenv("PALMETTO_STATE_TRACE"); spec && *spec)
            start_title_state_trace(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size, spec);
        // PALMETTO_STAGE2_WAKE=<seconds>: signal the measured stage-2 tracker event once at the named second and log the
        // tracker fields before/after (diagnostic; see the stage-2 wake probe above). Off when unset.
        if (const char* spec = std::getenv("PALMETTO_STAGE2_WAKE"); spec && *spec) {
            const double at_seconds = std::strtod(spec, nullptr);
            if (at_seconds > 0.0)
                start_stage2_wake_probe(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size,
                                        at_seconds);
        }
        // PALMETTO_STAGE2_TRACKER=<seconds>: dump the armed stage-2 tracker's job lists, job fields, context and the
        // process's handles to the waited event, then sample again 3 s later (diagnostic; see the tracker probe above).
        // Off when unset.
        if (const char* spec = std::getenv("PALMETTO_STAGE2_TRACKER"); spec && *spec) {
            const double at_seconds = std::strtod(spec, nullptr);
            if (at_seconds >= 0.0)
                start_stage2_tracker_probe(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size,
                                           at_seconds);
        }
        // PALMETTO_LOADER_HOOK=1: inline entry detours on the loader begin/end/re-init, with per-call records
        // (diagnostic; see the loader entry detours above). Off by default.
        if (const char* hook_switch = std::getenv("PALMETTO_LOADER_HOOK"); hook_switch && *hook_switch &&
            *hook_switch != '0')
            start_loader_hooks(reinterpret_cast<std::uintptr_t>(main_module.base));
        std::printf("running %s\n", main_module.name.c_str());
        std::fflush(stdout);
        // PALMETTO_WATCH_GLOBAL=<hex rva> logs every change of a mapped image's 8-byte global with the millisecond it
        // happened, so a value the title stores and later dereferences can be traced to the call that produced it
        // without a debugger. Polling is coarse but the log lines around it are ordered with it.
        if (const DWORD size = GetEnvironmentVariableW(L"PALMETTO_WATCH_GLOBAL", nullptr, 0); size > 1) {
            std::wstring value(size, L'\0');
            const DWORD n = GetEnvironmentVariableW(L"PALMETTO_WATCH_GLOBAL", value.data(), size);
            if (n > 0 && n < size) value.resize(n);
            const auto rva = std::wcstoull(value.c_str(), nullptr, 16);
            auto* const slot = reinterpret_cast<std::uint64_t*>(main_module.base + rva);
            std::fprintf(stderr, "[rt] watching image+0x%llx (%p)\n",
                         static_cast<unsigned long long>(rva), static_cast<void*>(slot));
            std::thread([slot] {
                std::uint64_t last = 0;
                for (;;) {
                    const std::uint64_t now = *slot;
                    if (now != last) {
                        std::fprintf(stderr, "[rt] watch: image global = 0x%llx\n",
                                     static_cast<unsigned long long>(now));
                        last = now;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }).detach();
        }
        // PALMETTO_LOADER_WATCH=1: hardware data watchpoints on the loader object's armed flag (+0x78) and in-flight
        // count (+0x64), so the storing title code and its stack are captured at each transition (diagnostic; see the
        // loader write watch above). Off by default.
        if (const char* watch = std::getenv("PALMETTO_LOADER_WATCH"); watch && *watch && *watch != '0')
            start_loader_watch(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size, 0x79F6D70ull);
        const int code = rt::process::run_entry(main_module.base + main_module.entry_rva);
        std::fflush(stdout);
        std::printf("game exited with code %d\n", code);
        rt::shims::report_called_unimplemented();
        rt::startup_trace::event("exit", "\"code\":" + std::to_string(code));
        std::fflush(stdout);
        std::fflush(stderr);
        // Not `return`: a normal host exit runs the CRT's atexit table, which the game shares with palmetto (its
        // handlers would run against a game that has stopped) and whose last word decides the exit status.
        // ExitProcess also skips atexit, so the TLS clone cleanup registered there is run here instead.
        rt::cleanup_tls_clones();
        ExitProcess(static_cast<UINT>(code));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        rt::startup_trace::event("error", "\"what\":" + rt::startup_trace::quote(e.what()));
        return 1;
    }
}
