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
#include <cwchar>
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
#include "title_class0a.h"
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

// ---- Renderer executor watch (PALMETTO_EXECUTOR_WATCH) ---------------------------------------------------------------
// docs/vertex_buffer_va.md section 10 left the renderer stop attributed to a title object whose memory is reused while
// the per-command executor (game+0x1335A20) still virtual-calls through its first qword; the writer was unnamed. This
// probe is a measurement, off by default, that names it in-process without a debugger:
//
//  * a hardware execution breakpoint on the executor entry (DR0). The job is RCX and the object the executor calls
//    through is *(*(job+0x28)+0x10); the probe reads both, remembers them, logs the first sight with the object's
//    vtable (image+RVA) and a bounded memory dump, and notices a first qword that changed between two appearances;
//  * a hardware execution breakpoint on the title's block free path (DR1, game+0x515ABB0; the block pointer is RDX).
//    A free of a remembered job/graph/object names the caller that released it, with tick and title stack;
//  * a write-only hardware data breakpoint on the tracked object's first qword (DR2), armed in every thread as soon as
//    the executor first names the object. The store that replaces the vtable raises the breakpoint after it executes, so
//    the log carries the writer's thread, instruction, registers and title stack - the writer itself;
//  * a write-only data breakpoint on this probe's own control slot (DR3), written every 50 ms. If that one never fires,
//    debug registers are not honoured here and a silent object watch is a method failure, not a measurement.
//
// Every line prints the steady_clock tick, the same value the runtime's PALMETTO_TIMELINE lines print, so a title-side
// write orders against the layer's ExecuteCommandLists/fence/deferred-compile returns. RVAs, field offsets, counts and
// register values only; no game data.
//
// PALMETTO_EXECUTOR_WATCH=1 uses the measured sites above; PALMETTO_EXECUTOR_WATCH=<exec_rva>[,<free_rva>] overrides
// them (hex, image-relative; a single RVA disables the free breakpoint). Off when unset or "0".
struct ExecutorLifetimeWatch {
    std::atomic<std::uintptr_t> executor_entry{0};  // DR0: execution breakpoint, the per-command executor's entry
    std::atomic<std::uintptr_t> free_base{0};       // DR1's site, once it is time to arm it (hot during loading)
    std::atomic<std::uintptr_t> free_entry{0};      // DR1: execution breakpoint, the title's block free path
    std::atomic<std::uintptr_t> object_qword{0};    // DR2: 8-byte write watch on the tracked object's first qword
    std::atomic<std::uintptr_t> object_base{0};     // the tracked object itself (the qword watch is aligned down)
    std::atomic<std::uint64_t> object_first{0};     // the tracked object's first qword as last seen at an entry
    std::atomic<std::uintptr_t> control_slot{0};    // DR3: 8-byte write watch on this probe's own control slot
    std::atomic<unsigned> target_version{0};        // bumped whenever the data watch target changes
    std::atomic<unsigned> executor_hits{0};
    std::atomic<unsigned> executor_logged{0};
    std::atomic<unsigned> free_hits{0};
    std::atomic<unsigned> free_logged{0};
    std::atomic<unsigned> object_hits{0};
    std::atomic<unsigned> control_hits{0};
};

ExecutorLifetimeWatch& executor_watch() {
    static ExecutorLifetimeWatch watch;
    return watch;
}

// The control slot the probe's own thread writes every 50 ms; nothing else writes it. Aligned and volatile only.
alignas(8) volatile std::uint64_t executor_watch_control_slot = 0;
std::uintptr_t executor_watch_image_base = 0, executor_watch_image_size = 0;
double executor_watch_free_arm_at = 0.0;  // absolute steady_clock second the delayed free breakpoint arms at

std::mutex& executor_watch_print_mutex() {
    static std::mutex mutex;
    return mutex;
}

void executor_watch_print(const std::string& text) {
    std::lock_guard lock(executor_watch_print_mutex());
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

std::string executor_watch_hex(std::uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

// A module-relative label for a value that points into the title image; the raw value otherwise.
std::string executor_watch_label(std::uint64_t value, std::uintptr_t image_base, std::uintptr_t image_size) {
    if (value >= image_base && value < image_base + image_size)
        return rt::shims::module_offset(static_cast<std::uintptr_t>(value));
    return executor_watch_hex(value);
}

double executor_watch_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string executor_watch_prefix(const char* what) {
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "[rt] executor watch t=%.3f %s", executor_watch_seconds(), what);
    return buffer;
}

bool executor_watch_readable(std::uintptr_t address, std::size_t bytes) {
    if (address < 0x10000) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info) != sizeof info) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address + bytes <= end;
}

// A guarded read of title memory: no C++ object is alive in this frame, so the __try is legal and a page that turns
// unreadable between the query and the copy costs a skipped read, not a second fault.
bool executor_watch_read_block(std::uintptr_t address, void* out, std::size_t bytes) {
    if (!executor_watch_readable(address, bytes)) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

std::uint64_t executor_watch_read_u64(std::uintptr_t address) {
    std::uint64_t value = 0;
    return executor_watch_read_block(address, &value, sizeof value) ? value : 0;
}

std::uint32_t executor_watch_read_u32(std::uintptr_t address) {
    std::uint32_t value = 0;
    return executor_watch_read_block(address, &value, sizeof value) ? value : 0;
}

// The title stack, title-image frames only, from the faulting/writing thread's RSP. Same fault-tolerant page scan the
// samplers use; module_offset() takes the shim registry lock after the walk.
std::string executor_watch_title_stack(std::uintptr_t rsp) {
    WaitSampleModules modules;
    modules.image_base = executor_watch_image_base;
    modules.image_end = executor_watch_image_base + executor_watch_image_size;
    std::uint64_t frames[32] = {};
    const unsigned found = scan_stack_words_deep(rsp, frames, 32, modules, 24);
    std::string text;
    unsigned shown = 0;
    for (unsigned i = 0; i < found && shown < 12; ++i) {
        if (frames[i] < modules.image_base || frames[i] >= modules.image_end) continue;
        text += " " + rt::shims::module_offset(static_cast<std::uintptr_t>(frames[i]));
        ++shown;
    }
    return text.empty() ? " (none)" : text;
}

std::string executor_watch_memory_dump(const char* name, std::uintptr_t address, unsigned bytes) {
    std::string text = "\n  ";
    text += name;
    text += " ";
    text += executor_watch_hex(address);
    for (unsigned offset = 0; offset + 8 <= bytes; offset += 8) {
        char word[24];
        std::snprintf(word, sizeof word, " %016llx",
                      static_cast<unsigned long long>(executor_watch_read_u64(address + offset)));
        text += word;
    }
    return text;
}

// The job/graph/object chain one executor entry runs: job state at +0x40 (the dword the job executor compares with 3
// and the submit/wait loop waits on), the graph struct the job names at +0x28, and the object it virtual-calls through.
struct ExecutorGraphView {
    std::uint64_t job = 0, graph = 0, object = 0, object_first = 0;
    std::uint32_t state = 0;
    bool object_readable = false;
};

ExecutorGraphView executor_watch_view(std::uintptr_t job) {
    ExecutorGraphView view;
    view.job = job;
    if (!job) return view;
    view.state = executor_watch_read_u32(job + 0x40);
    view.graph = executor_watch_read_u64(job + 0x28);
    if (!view.graph) return view;
    view.object = executor_watch_read_u64(static_cast<std::uintptr_t>(view.graph) + 0x10);
    if (!view.object) return view;
    view.object_readable = executor_watch_read_block(static_cast<std::uintptr_t>(view.object), &view.object_first, 8);
    return view;
}

std::string executor_watch_view_text(const ExecutorGraphView& view) {
    std::string text = "job " + executor_watch_hex(view.job) + " state " + std::to_string(view.state) + " graph " +
                       executor_watch_label(view.graph, executor_watch_image_base, executor_watch_image_size) +
                       " object " +
                       executor_watch_label(view.object, executor_watch_image_base, executor_watch_image_size) +
                       " object[0] " + executor_watch_hex(view.object_first);
    return text;
}

// Remember every pointer the probe learns, so the free breakpoint can name what it releases. Bounded; first name wins.
std::mutex& executor_watch_names_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::uint64_t, std::string>& executor_watch_names() {
    static std::map<std::uint64_t, std::string> names;
    return names;
}

void executor_watch_remember(std::uint64_t address, const char* name) {
    if (!address) return;
    std::lock_guard lock(executor_watch_names_mutex());
    auto& names = executor_watch_names();
    if (names.size() >= 256) return;
    if (names.find(address) == names.end()) names[address] = name;
}

std::string executor_watch_name_of(std::uint64_t address) {
    std::lock_guard lock(executor_watch_names_mutex());
    const auto it = executor_watch_names().find(address);
    return it == executor_watch_names().end() ? std::string() : it->second;
}

// Per-object first-qword history, so a change between two executor entries is visible even if the data breakpoint is
// armed after the first write (the free/reuse window a later boot can then watch from its first sight).
std::mutex& executor_watch_objects_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::uint64_t, std::uint64_t>& executor_watch_objects() {
    static std::map<std::uint64_t, std::uint64_t> objects;
    return objects;
}

// Arms the object first-qword write watch on the first object the executor names whose first qword looks like a vtable
// (a code or read-only-data pointer), and only once: a boot watches one object. Logs the arm with the tick. The
// steady state is allocation-free: 60k executor entries per boot hit this path, and the common case (the same object,
// the same first qword) is one atomic load and one memory read.
void executor_watch_track(const ExecutorGraphView& view, const char* source) {
    if (!view.object) return;
    ExecutorLifetimeWatch& watch = executor_watch();
    if (view.object == watch.object_base.load(std::memory_order_relaxed)) {
        const std::uint64_t previous = watch.object_first.load(std::memory_order_relaxed);
        if (view.object_first == previous) return;
        watch.object_first.store(view.object_first, std::memory_order_relaxed);
        executor_watch_print(executor_watch_prefix("object first qword changed between entries (") + source +
                             "): object " + executor_watch_hex(view.object) + " was " +
                             executor_watch_label(previous, executor_watch_image_base, executor_watch_image_size) +
                             " now " +
                             executor_watch_label(view.object_first, executor_watch_image_base, executor_watch_image_size) +
                             executor_watch_memory_dump("object", static_cast<std::uintptr_t>(view.object), 0x60) + "\n");
        return;
    }
    executor_watch_remember(view.job, "job");
    executor_watch_remember(view.graph, "graph");
    executor_watch_remember(view.object, "object");
    bool first = false, changed = false;
    std::uint64_t previous = 0;
    {
        std::lock_guard lock(executor_watch_objects_mutex());
        auto& objects = executor_watch_objects();
        const auto it = objects.find(view.object);
        if (it == objects.end()) {
            first = true;
            if (objects.size() < 512) objects[view.object] = view.object_first;
        } else if (it->second != view.object_first) {
            changed = true;
            previous = it->second;
            it->second = view.object_first;
        }
    }
    std::string text;
    if (first) {
        text = executor_watch_prefix("object first seen (") + source + "): " + executor_watch_view_text(view) +
               executor_watch_memory_dump("object", static_cast<std::uintptr_t>(view.object), 0x60) +
               executor_watch_memory_dump("graph", static_cast<std::uintptr_t>(view.graph), 0x40);
        const bool pointer_like = view.object_first >= 0x10000 && view.object_first < 0x7FFFFFFFFFFFull;
        if (watch.object_base.load() == 0 && pointer_like) {
            const auto qword = static_cast<std::uintptr_t>(view.object & ~static_cast<std::uint64_t>(7));
            watch.object_first.store(view.object_first, std::memory_order_relaxed);
            watch.object_base.store(static_cast<std::uintptr_t>(view.object));
            watch.object_qword.store(qword);
            watch.target_version.fetch_add(1);
            text += "\n  " + executor_watch_prefix("armed the object first-qword write watch at ") +
                    executor_watch_hex(qword) + " (object " + executor_watch_hex(view.object) + ", first qword " +
                    executor_watch_label(view.object_first, executor_watch_image_base, executor_watch_image_size) +
                    ")";
        }
    } else if (changed) {
        text = executor_watch_prefix("object first qword changed between entries (") + source +
               "): object " + executor_watch_hex(view.object) + " was " +
               executor_watch_label(previous, executor_watch_image_base, executor_watch_image_size) + " now " +
               executor_watch_label(view.object_first, executor_watch_image_base, executor_watch_image_size) +
               executor_watch_memory_dump("object", static_cast<std::uintptr_t>(view.object), 0x60);
    }
    if (!text.empty()) executor_watch_print(text + "\n");
}

// Names the free of a tracked block: the pointer, the block's first qword at release, and the caller's title stack.
void executor_watch_free(const CONTEXT& context) {
    ExecutorLifetimeWatch& watch = executor_watch();
    const unsigned hit = watch.free_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto pointer = static_cast<std::uint64_t>(context.Rdx);
    const std::string name = executor_watch_name_of(pointer);
    const auto watched_object = static_cast<std::uint64_t>(watch.object_qword.load());
    const bool tracked = !name.empty() || (watched_object && pointer == (watched_object & ~static_cast<std::uint64_t>(7)));
    const bool log_line = tracked || (hit % 4096 == 0);
    if (!log_line || watch.free_logged.fetch_add(1, std::memory_order_relaxed) >= (tracked ? 64u : 8u)) return;
    std::string text = executor_watch_prefix("free path hit: ") + "pointer " + executor_watch_hex(pointer);
    if (!name.empty()) text += " (" + name + ")";
    const std::uint64_t first = executor_watch_read_u64(static_cast<std::uintptr_t>(pointer));
    text += " first qword now " + executor_watch_label(first, executor_watch_image_base, executor_watch_image_size);
    text += "\n  " + executor_watch_prefix("free stack") + executor_watch_title_stack(context.Rsp);
    executor_watch_print(text + "\n");
}

// One executor entry: log the first few and periodic samples, remember the chain, and notice a first-qword change.
void executor_watch_entry(const CONTEXT& context) {
    ExecutorLifetimeWatch& watch = executor_watch();
    const unsigned hit = watch.executor_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto job = static_cast<std::uintptr_t>(context.Rcx);
    const ExecutorGraphView view = executor_watch_view(job);
    const std::uint64_t caller = executor_watch_read_u64(static_cast<std::uintptr_t>(context.Rsp));
    const bool sample = hit <= 12 || hit % 2048 == 0;
    if (sample && watch.executor_logged.fetch_add(1, std::memory_order_relaxed) < 128) {
        std::string text = executor_watch_prefix("executor entry:") + " hit " + std::to_string(hit) + " tid " +
                           std::to_string(GetCurrentThreadId()) + " caller " +
                           executor_watch_label(caller, executor_watch_image_base, executor_watch_image_size) + "\n  " +
                           executor_watch_view_text(view);
        // The deep stack scan costs 24 pages; only the first few entries and the periodic samples need it.
        if (hit <= 3 || hit % 2048 == 0)
            text += "\n  " + executor_watch_prefix("entry stack") + executor_watch_title_stack(context.Rsp);
        executor_watch_print(text + "\n");
    }
    executor_watch_track(view, "executor");
}

// A hardware data-breakpoint hit: DR2 is the tracked object's first qword (the writer the slice needs), DR3 the
// probe's own control slot. Called with the thread's context after the storing instruction executed.
void executor_watch_data(const CONTEXT& context) {
    ExecutorLifetimeWatch& watch = executor_watch();
    const DWORD dr6 = context.Dr6;
    if ((dr6 & 0x8u) != 0) {  // DR3: the control slot - proves the debug registers are honoured on this machine
        const unsigned hits = watch.control_hits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hits <= 3)
            executor_watch_print(executor_watch_prefix("control watch fired: the debug registers and the write watch "
                                                       "work") + " (" + std::to_string(hits) + ")\n");
    }
    if ((dr6 & 0x4u) == 0) return;  // DR2 is the object first qword; anything else is reported by its own path
    const unsigned hits = watch.object_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto qword = static_cast<std::uintptr_t>(watch.object_qword.load());
    std::uint64_t now = 0;
    executor_watch_read_block(qword, &now, 8);
    std::string text = executor_watch_prefix("OBJECT FIRST-QWORD WRITER") + " hit " + std::to_string(hits) + " tid " +
                       std::to_string(GetCurrentThreadId()) +
                       " writer " + executor_watch_hex(context.Rip) + " (" +
                       executor_watch_label(context.Rip, executor_watch_image_base, executor_watch_image_size) + ")" +
                       " value now " + executor_watch_label(now, executor_watch_image_base, executor_watch_image_size);
    // The bytes leading up to RIP name the store instruction without a disassembler (the breakpoint fires after it).
    unsigned char bytes[16] = {};
    if (context.Rip >= 16 && executor_watch_read_block(static_cast<std::uintptr_t>(context.Rip) - 16, bytes, 16)) {
        text += "\n  bytes before writer rip:";
        char byte[8];
        for (unsigned i = 0; i < 16; ++i) {
            std::snprintf(byte, sizeof byte, " %02x", bytes[i]);
            text += byte;
        }
    }
    char registers[320];
    std::snprintf(registers, sizeof registers,
                  "\n  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx r14 %llx r15 %llx",
                  static_cast<unsigned long long>(context.Rax), static_cast<unsigned long long>(context.Rbx),
                  static_cast<unsigned long long>(context.Rcx), static_cast<unsigned long long>(context.Rdx),
                  static_cast<unsigned long long>(context.Rsi), static_cast<unsigned long long>(context.Rdi),
                  static_cast<unsigned long long>(context.R8), static_cast<unsigned long long>(context.R9),
                  static_cast<unsigned long long>(context.R14), static_cast<unsigned long long>(context.R15));
    text += registers;
    text += "\n  " + executor_watch_prefix("writer stack") + executor_watch_title_stack(context.Rsp);
    text += executor_watch_memory_dump("object",
                                       static_cast<std::uintptr_t>(watch.object_base.load()), 0x80);
    if (hits <= 4) executor_watch_print(text + "\n");
}

LONG CALLBACK executor_watch_exception(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if ((context.EFlags & 0x100u) != 0) return EXCEPTION_CONTINUE_SEARCH;  // a trap the int3 probe machinery set
    ExecutorLifetimeWatch& watch = executor_watch();
    const auto at = static_cast<std::uintptr_t>(context.Rip);
    const auto entry = static_cast<std::uintptr_t>(watch.executor_entry.load());
    const auto free_entry = static_cast<std::uintptr_t>(watch.free_entry.load());
    if (entry && at == entry) {
        context.Dr6 = 0;
        executor_watch_entry(context);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (free_entry && at == free_entry) {
        context.Dr6 = 0;
        executor_watch_free(context);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if ((context.Dr6 & 0xCu) != 0) {
        executor_watch_data(context);
        context.Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Arms DR0/DR1 (execution) and DR2/DR3 (8-byte write) in one thread. Called with no lock the title could need: it
// suspends the thread for the length of one Get/SetThreadContext.
bool executor_watch_configure_thread(DWORD tid) {
    ExecutorLifetimeWatch& watch = executor_watch();
    const auto entry = static_cast<std::uintptr_t>(watch.executor_entry.load());
    const auto free_entry = static_cast<std::uintptr_t>(watch.free_entry.load());
    const auto object_qword = static_cast<std::uintptr_t>(watch.object_qword.load());
    const auto control = static_cast<std::uintptr_t>(watch.control_slot.load());
    DWORD dr7 = 0;
    if (entry) dr7 |= 0x1u;                                    // L0, RW0=00 LEN0=00: execute at DR0
    if (free_entry) dr7 |= 0x1u << 2;                          // L1: execute at DR1
    if (object_qword && (object_qword & 7u) == 0) {            // L2, RW2=01 write, LEN2=11 8 bytes
        dr7 |= 0x1u << 4;
        dr7 |= 0x1u << 24;
        dr7 |= 0x3u << 26;
    }
    if (control && (control & 7u) == 0) {                      // L3, RW3=01 write, LEN3=11 8 bytes
        dr7 |= 0x1u << 6;
        dr7 |= 0x1u << 28;
        dr7 |= 0x3u << 30;
    }
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!thread) return false;
    bool configured = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context)) {
            context.Dr0 = entry;
            context.Dr1 = free_entry;
            context.Dr2 = object_qword;
            context.Dr3 = control;
            context.Dr6 = 0;
            context.Dr7 = dr7;
            configured = SetThreadContext(thread, &context) != FALSE;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return configured;
}

void start_executor_watch_threads() {
    // The control writer: proves the data-watch machinery works where the object watch stays silent.
    std::thread([] {
        for (;;) {
            ++executor_watch_control_slot;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
    // The arming sweep: configures every thread once, reconfigures all of them when the object watch target appears,
    // and picks up threads created later. Fixed arrays only: a thread suspended inside its own heap allocation holds
    // the process heap lock, so the sweep must not allocate between SuspendThread and ResumeThread. A thread is only
    // configured after it has been alive across two scans: a thread suspended in the window between its creation and
    // its first instruction can end up with a suspend count its owner never balances, which stalls the loader.
    std::thread([] {
        static constexpr unsigned kMaxThreads = 4096, kPending = 512;
        static DWORD configured[kMaxThreads];
        static DWORD seen_tid[kMaxThreads];
        static ULONGLONG seen_at[kMaxThreads];
        unsigned configured_count = 0, seen_count = 0;
        unsigned configured_version = ~0u;
        unsigned long loops = 0;
        const auto find = [](const DWORD* table, unsigned count, DWORD tid) -> int {
            for (unsigned i = 0; i < count; ++i)
                if (table[i] == tid) return static_cast<int>(i);
            return -1;
        };
        for (;;) {
            ExecutorLifetimeWatch& watch = executor_watch();
            // Arm the hot free breakpoint only at its time: the allocator free path runs millions of times during
            // loading, and one trap per call would swamp the boot this probe is measuring.
            if (watch.free_entry.load(std::memory_order_relaxed) == 0 &&
                watch.free_base.load(std::memory_order_relaxed) != 0 &&
                executor_watch_seconds() >= executor_watch_free_arm_at) {
                watch.free_entry.store(watch.free_base.load(std::memory_order_relaxed));
                watch.target_version.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr, "[rt] executor watch: free path armed at %s (t=%.3f)\n",
                             executor_watch_hex(static_cast<std::uint64_t>(watch.free_entry.load())).c_str(),
                             executor_watch_seconds());
                std::fflush(stderr);
            }
            const unsigned version = watch.target_version.load(std::memory_order_relaxed);
            const bool changed = version != configured_version;
            if (changed) {
                configured_count = 0;
                configured_version = version;
                std::fprintf(stderr,
                             "[rt] executor watch: arming the debug registers (target version %u, object qword %p)\n",
                             version, reinterpret_cast<void*>(static_cast<std::uintptr_t>(watch.object_qword.load())));
                std::fflush(stderr);
            }
            const ULONGLONG now = GetTickCount64();
            DWORD pending[kPending];
            unsigned pending_count = 0, refused = 0;
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{};
                entry.dwSize = sizeof entry;
                const DWORD pid = GetCurrentProcessId();
                const DWORD self = GetCurrentThreadId();  // never suspend this sweep: a self-suspend never resumes
                if (Thread32First(snapshot, &entry))
                    do {
                        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
                        if (find(configured, configured_count, entry.th32ThreadID) >= 0) continue;
                        const int seat = find(seen_tid, seen_count, entry.th32ThreadID);
                        if (seat < 0) {
                            if (seen_count < kMaxThreads) {
                                seen_tid[seen_count] = entry.th32ThreadID;
                                seen_at[seen_count] = now;
                                ++seen_count;
                            }
                            continue;  // first sight: wait one scan before suspending it
                        }
                        if (now - seen_at[seat] < 1000) continue;
                        if (pending_count < kPending) pending[pending_count++] = entry.th32ThreadID;
                    } while (Thread32Next(snapshot, &entry));
                CloseHandle(snapshot);
            }
            for (unsigned i = 0; i < pending_count; ++i) {
                if (executor_watch_configure_thread(pending[i])) {
                    if (configured_count < kMaxThreads) configured[configured_count++] = pending[i];
                } else {
                    ++refused;
                }
            }
            // A full thread scan is only needed when the target just changed; afterwards it is enough to pick up
            // threads created later, and a slower scan keeps the probe's own cost out of the boot's timing. The
            // heartbeat proves the sweep is alive and shows how many threads carry the debug registers.
            if (++loops % 20 == 0) {
                std::fprintf(stderr,
                             "[rt] executor watch sweep: %u thread(s) configured, %u refused this pass, target version "
                             "%u\n",
                             configured_count, refused, configured_version);
                std::fflush(stderr);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(changed ? 25 : 250));
        }
    }).detach();
    // A bounded reporter, so a watch that never fires still shows the counters in the boot log.
    std::thread([] {
        for (int sample = 1; sample <= 120; ++sample) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            ExecutorLifetimeWatch& watch = executor_watch();
            std::fprintf(stderr,
                         "[rt] executor watch at %d s: executor entries %u (logged %u), free-path hits %u (logged %u), "
                         "object writer hits %u, control watch fired %u, object qword %p\n",
                         sample * 10, watch.executor_hits.load(std::memory_order_relaxed),
                         watch.executor_logged.load(std::memory_order_relaxed),
                         watch.free_hits.load(std::memory_order_relaxed),
                         watch.free_logged.load(std::memory_order_relaxed),
                         watch.object_hits.load(std::memory_order_relaxed),
                         watch.control_hits.load(std::memory_order_relaxed),
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(watch.object_qword.load())));
            std::fflush(stderr);
        }
    }).detach();
}

// Starts the renderer executor watch. Measurement only: it changes no layer call and no title call (the breakpoints
// trap and are continued); see the block comment above. The free path is optional and may carry an arm delay
// (`<exec_rva>,<free_rva>@<seconds>`): it is very hot while the title loads, so a boot arms it only for the window
// that matters.
void start_executor_watch(std::uintptr_t image_base, std::size_t image_size) {
    const char* spec = std::getenv("PALMETTO_EXECUTOR_WATCH");
    if (!spec || !*spec || *spec == '0') return;
    std::uint64_t exec_rva = 0x1335A20ull, free_rva = 0x515ABB0ull;
    double free_delay = 0.0;
    if (!(spec[0] == '1' && spec[1] == '\0')) {
        char* end = nullptr;
        const std::uint64_t first = std::strtoull(spec, &end, 16);
        if (end == spec) {
            std::fprintf(stderr, "[rt] executor watch: PALMETTO_EXECUTOR_WATCH=%s is not a hex RVA; not armed\n", spec);
            return;
        }
        exec_rva = first;
        free_rva = 0;
        if (*end == ',') {
            const char* second = end + 1;
            char* second_end = nullptr;
            const std::uint64_t value = std::strtoull(second, &second_end, 16);
            if (second_end != second && value) {
                free_rva = value;
                if (*second_end == '@') free_delay = std::strtod(second_end + 1, nullptr);
            }
        }
    }
    executor_watch_image_base = image_base;
    executor_watch_image_size = image_size;
    ExecutorLifetimeWatch& watch = executor_watch();
    watch.executor_entry.store(image_base + exec_rva);
    if (free_rva && free_delay > 0.0) {
        // Arm the hot free-path breakpoint only after the delay; the sweep sees the deadline cross.
        watch.free_base.store(image_base + free_rva);
        watch.free_entry.store(0);
        executor_watch_free_arm_at = executor_watch_seconds() + free_delay;
    } else {
        watch.free_entry.store(image_base + free_rva);  // 0 disables the free breakpoint
        executor_watch_free_arm_at = 0.0;
    }
    watch.control_slot.store(reinterpret_cast<std::uintptr_t>(const_cast<std::uint64_t*>(&executor_watch_control_slot)));
    AddVectoredExceptionHandler(1, executor_watch_exception);
    start_executor_watch_threads();
    unsigned char bytes[8] = {};
    executor_watch_read_block(image_base + exec_rva, bytes, sizeof bytes);
    std::fprintf(stderr, "[rt] executor watch: executor entry image+0x%llx, free path image+0x%llx, control slot %p\n",
                 static_cast<unsigned long long>(exec_rva), static_cast<unsigned long long>(free_rva),
                 static_cast<void*>(const_cast<std::uint64_t*>(&executor_watch_control_slot)));
    std::fprintf(stderr, "[rt] executor watch: bytes at the entry site: %02x %02x %02x %02x\n", bytes[0], bytes[1],
                 bytes[2], bytes[3]);
    std::fflush(stderr);
}

// ---- Title renderer-object write watch (PALMETTO_OBJECT_WRITE_WATCH) --------------------------------------
// docs/vertex_buffer_va.md sections 10-11. The renderer stop is an object the per-command executor
// (game+0x1335A20) virtual-calls through whose first qword reads two 1.0f floats: the block was freed and reused
// while the executor still held the pointer. The hardware-breakpoint probe above names the object but needs a
// suspended thread per configuration, and a boot with it did not get past loading (docs section 11 records that).
//
// This probe reaches the same measurement with no debug registers and no thread suspension:
//  * an int3 over the executor entry's first byte (re-written after each pass, so every pass is seen). From RCX (the
//    job) it derives the graph (job+0x28) and the object ([graph+0x10]), logs the first object of each class it meets
//    with the tick and the object's vtable (image+RVA), and keeps a first-qword history per object;
//  * on the first sight of an object whose first qword looks like a vtable, the 4 KiB page holding its first 0x100
//    bytes is made PAGE_READONLY. EVERY thread's store into it faults, so the writer is named by its thread, its
//    instruction and its title stack after the store executed, with no suspend/SetThreadContext in the path;
//  * a page owned by this probe is protected the same way and written every 50 ms: if its fault never fires, the
//    page-write method itself is unproven in this environment and a silent object watch is a method failure.
//
// Every line prints the steady_clock tick PALMETTO_TIMELINE lines print. PALMETTO_OBJECT_WRITE_WATCH=1 uses the
// measured executor entry (game+0x1335A20); =<exec_rva> overrides it. Off when unset or "0".
struct TitleWriteWatch {
    std::atomic<std::uintptr_t> executor_entry{0};
    std::atomic<unsigned char> original_byte{0};
    std::atomic<std::uintptr_t> pages[8];    // protected pages (object pages and the control page)
    std::atomic<std::uintptr_t> objects[8];  // object bases whose first 0x100 bytes are watched
    std::atomic<unsigned> page_count{0}, object_count{0};
    std::atomic<unsigned> executor_hits{0}, executor_logged{0};
    std::atomic<unsigned> write_faults{0}, tracked_writes{0}, control_faults{0}, page_faults_other{0};
    std::atomic<std::uintptr_t> control_page{0};
    std::atomic<unsigned> armed_at_tick{0};
};

TitleWriteWatch& title_write_watch() {
    static TitleWriteWatch watch;
    return watch;
}

thread_local std::uintptr_t title_write_repoke_address = 0;
thread_local std::uintptr_t title_write_pending_page = 0;
thread_local std::uintptr_t title_write_pending_object = 0;
thread_local std::uintptr_t title_write_pending_rip = 0;

// The bytes a protected-page write fault's instruction will execute next, so the store can be named without a
// disassembler: the fault is raised before the store, so RIP is the store instruction itself.
std::string title_write_store_bytes(std::uintptr_t rip) {
    unsigned char bytes[16] = {};
    if (!executor_watch_read_block(rip, bytes, sizeof bytes)) return "";
    std::string text;
    char byte[8];
    for (unsigned i = 0; i < 8; ++i) {
        std::snprintf(byte, sizeof byte, " %02x", bytes[i]);
        text += byte;
    }
    return text;
}

bool title_write_page_index(std::uintptr_t address, unsigned* index) {
    TitleWriteWatch& watch = title_write_watch();
    const unsigned count = watch.page_count.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < count; ++i) {
        if (watch.pages[i].load(std::memory_order_relaxed) == address) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

bool title_write_watched_object(std::uintptr_t address, std::uintptr_t* object) {
    TitleWriteWatch& watch = title_write_watch();
    const unsigned count = watch.object_count.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < count; ++i) {
        const std::uintptr_t candidate = watch.objects[i].load(std::memory_order_relaxed);
        if (address >= candidate && address < candidate + 0x100) {
            if (object) *object = candidate;
            return true;
        }
    }
    return false;
}

// Protects one 4 KiB page read-only and records it. Returns false when the page cannot be protected.
bool title_write_protect_page(std::uintptr_t page) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READONLY, &old)) return false;
    TitleWriteWatch& watch = title_write_watch();
    const unsigned index = watch.page_count.fetch_add(1, std::memory_order_relaxed);
    if (index < 8) watch.pages[index].store(page, std::memory_order_relaxed);
    return index < 8;
}

std::uintptr_t title_write_control_page() {
    TitleWriteWatch& watch = title_write_watch();
    if (watch.control_page.load(std::memory_order_relaxed)) return watch.control_page.load(std::memory_order_relaxed);
    void* const page = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    watch.control_page.store(reinterpret_cast<std::uintptr_t>(page), std::memory_order_relaxed);
    return reinterpret_cast<std::uintptr_t>(page);
}

// Names the writer of a store into a watched page: which object it hit, the storing instruction, the thread, the
// value after the store and the title stack. `first` is the first store of this fault (before the single-step).
void title_write_record(std::uintptr_t page, std::uintptr_t address, const CONTEXT& context, bool after) {
    TitleWriteWatch& watch = title_write_watch();
    std::uintptr_t object = 0;
    const bool tracked = title_write_watched_object(address, &object);
    const std::uintptr_t control_page = watch.control_page.load(std::memory_order_relaxed);
    if (page == control_page) {
        const unsigned hits = watch.control_faults.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hits <= 3)
            executor_watch_print(executor_watch_prefix("control page write watch fired: the page-write method and its "
                                                       "single-step arm work") + " (" + std::to_string(hits) + ")\n");
        return;
    }
    if (!after) {
        watch.write_faults.fetch_add(1, std::memory_order_relaxed);
        if (!tracked) {
            const unsigned other = watch.page_faults_other.fetch_add(1, std::memory_order_relaxed) + 1;
            if (other <= 4)
                executor_watch_print(executor_watch_prefix("write into a watched page outside the object window:") +
                                     " address " + executor_watch_hex(address) + " writer " +
                                     executor_watch_label(static_cast<std::uint64_t>(context.Rip), executor_watch_image_base,
                                                          executor_watch_image_size) + "\n");
        }
        return;
    }
    if (!tracked) return;
    const unsigned hits = watch.tracked_writes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hits > 6) return;
    std::uint64_t value = 0;
    executor_watch_read_block(address, &value, 8);
    const std::uintptr_t store_rip = title_write_pending_rip ? title_write_pending_rip
                                                             : static_cast<std::uintptr_t>(context.Rip);
    std::string text = executor_watch_prefix("OBJECT WRITE WATCH") + " hit " + std::to_string(hits) + " object " +
                       executor_watch_hex(object) + " offset " + executor_watch_hex(address - object) + " value " +
                       executor_watch_label(value, executor_watch_image_base, executor_watch_image_size) +
                       " writer " + executor_watch_label(static_cast<std::uint64_t>(store_rip), executor_watch_image_base,
                                                         executor_watch_image_size) +
                       " store bytes" + title_write_store_bytes(store_rip) +
                       " tid " + std::to_string(GetCurrentThreadId());
    text += "\n  " + executor_watch_prefix("writer stack") + executor_watch_title_stack(context.Rsp);
    text += executor_watch_memory_dump("object", object, 0x80);
    executor_watch_print(text + "\n");
}

void title_write_executor_entry(CONTEXT& context);

LONG CALLBACK title_write_exception(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    TitleWriteWatch& watch = title_write_watch();
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2 &&
        info->ExceptionRecord->ExceptionInformation[0] == 1) {
        const auto address = static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
        const auto page = address & ~static_cast<std::uintptr_t>(0xFFF);
        if (!title_write_page_index(page, nullptr)) return EXCEPTION_CONTINUE_SEARCH;
        title_write_record(page, address, context, false);
        title_write_pending_page = page;
        title_write_pending_object = address;
        title_write_pending_rip = static_cast<std::uintptr_t>(context.Rip);
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READWRITE, &old);
        context.EFlags |= 0x100;  // trap after the store, so the page can be protected again
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_BREAKPOINT) {
        const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        if (at != static_cast<std::uintptr_t>(watch.executor_entry.load())) return EXCEPTION_CONTINUE_SEARCH;
        title_write_executor_entry(context);
        DWORD old = 0;
        if (VirtualProtect(reinterpret_cast<void*>(at), 1, PAGE_EXECUTE_READWRITE, &old)) {
            *reinterpret_cast<unsigned char*>(at) = watch.original_byte.load();
            VirtualProtect(reinterpret_cast<void*>(at), 1, old, &old);
        }
        title_write_repoke_address = at;
        context.EFlags |= 0x100;  // trap after the restored instruction, then put the int3 back
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP) {
        if (title_write_pending_page) {
            const std::uintptr_t page = title_write_pending_page;
            const std::uintptr_t address = title_write_pending_object;
            title_write_pending_page = 0;
            title_write_pending_object = 0;
            context.EFlags &= ~static_cast<DWORD>(0x100);
            DWORD old = 0;
            VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READONLY, &old);
            title_write_record(page, address, context, true);
            title_write_pending_rip = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (title_write_repoke_address) {
            const std::uintptr_t address = title_write_repoke_address;
            title_write_repoke_address = 0;
            context.EFlags &= ~static_cast<DWORD>(0x100);
            DWORD old = 0;
            if (VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old)) {
                *reinterpret_cast<unsigned char*>(address) = 0xCC;
                VirtualProtect(reinterpret_cast<void*>(address), 1, old, &old);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// The executor entry's int3: logs the job/graph/object chain (bounded) and arms the page watch on a new object class.
void title_write_executor_entry(CONTEXT& context) {
    TitleWriteWatch& watch = title_write_watch();
    const unsigned hit = watch.executor_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto job = static_cast<std::uintptr_t>(context.Rcx);
    const ExecutorGraphView view = executor_watch_view(job);
    if (hit <= 12 || hit % 4096 == 0) {
        if (watch.executor_logged.fetch_add(1, std::memory_order_relaxed) < 64) {
            std::string text = executor_watch_prefix("executor entry:") + " hit " + std::to_string(hit) + " tid " +
                               std::to_string(GetCurrentThreadId()) + "\n  " + executor_watch_view_text(view);
            if (hit <= 3)
                text += "\n  " + executor_watch_prefix("entry stack") + executor_watch_title_stack(context.Rsp);
            executor_watch_print(text + "\n");
        }
    }
    if (!view.object || !view.object_readable) return;
    auto& objects_history = executor_watch_objects();
    bool first = false;
    std::uint64_t was = 0;
    {
        std::lock_guard lock(executor_watch_objects_mutex());
        const auto it = executor_watch_objects().find(view.object);
        if (it == executor_watch_objects().end()) {
            first = true;
            if (objects_history.size() < 512) objects_history[view.object] = view.object_first;
        } else if (it->second != view.object_first) {
            was = it->second;
            it->second = view.object_first;
        }
    }
    if (!first && !was) return;
    if (was) {
        executor_watch_print(executor_watch_prefix("object first qword changed between entries: object ") +
                             executor_watch_hex(view.object) + " was " +
                             executor_watch_label(was, executor_watch_image_base, executor_watch_image_size) + " now " +
                             executor_watch_label(view.object_first, executor_watch_image_base, executor_watch_image_size) +
                             executor_watch_memory_dump("object", static_cast<std::uintptr_t>(view.object), 0x80) + "\n");
        return;
    }
    executor_watch_remember(view.job, "job");
    executor_watch_remember(view.graph, "graph");
    executor_watch_remember(view.object, "object");
    std::string text = executor_watch_prefix("object first seen:") + " " + executor_watch_view_text(view) +
                       "\n  vtable " + executor_watch_label(view.object_first, executor_watch_image_base,
                                                             executor_watch_image_size) +
                       executor_watch_memory_dump("object", static_cast<std::uintptr_t>(view.object), 0x80) +
                       executor_watch_memory_dump("graph", static_cast<std::uintptr_t>(view.graph), 0x40);
    const unsigned index = watch.object_count.fetch_add(1, std::memory_order_relaxed);
    if (index < 8) {
        watch.objects[index].store(static_cast<std::uintptr_t>(view.object), std::memory_order_relaxed);
        const auto page = static_cast<std::uintptr_t>(view.object & ~static_cast<std::uint64_t>(0xFFF));
        text += "\n  " + executor_watch_prefix("armed the object page write watch on page ") + executor_watch_hex(page) +
                " (object " + executor_watch_hex(view.object) + ")";
        title_write_protect_page(page);
    }
    executor_watch_print(text + "\n");
}

// Puts an int3 over the executor entry and starts the probe's control writer and reporter. The exception handler is
// registered after the int3 is in place; every later pass is re-armed by the single-step handler.
void start_title_write_watch(std::uintptr_t image_base, std::size_t image_size) {
    const char* spec = std::getenv("PALMETTO_OBJECT_WRITE_WATCH");
    if (!spec || !*spec || *spec == '0') return;
    std::uint64_t exec_rva = 0x1335A20ull;
    if (!(spec[0] == '1' && spec[1] == '\0')) {
        char* end = nullptr;
        const std::uint64_t value = std::strtoull(spec, &end, 16);
        if (end == spec) {
            std::fprintf(stderr, "[rt] object write watch: PALMETTO_OBJECT_WRITE_WATCH=%s is not a hex RVA; not armed\n",
                         spec);
            return;
        }
        exec_rva = value;
    }
    executor_watch_image_base = image_base;
    executor_watch_image_size = image_size;
    TitleWriteWatch& watch = title_write_watch();
    const auto site = static_cast<std::uintptr_t>(image_base + exec_rva);
    watch.executor_entry.store(site);
    auto* const byte = reinterpret_cast<unsigned char*>(site);
    watch.original_byte.store(*byte);
    DWORD old = 0;
    if (!VirtualProtect(byte, 1, PAGE_EXECUTE_READWRITE, &old)) {
        std::fprintf(stderr, "[rt] object write watch: cannot make the executor entry writable; not armed\n");
        return;
    }
    *byte = 0xCC;
    VirtualProtect(byte, 1, old, &old);
    AddVectoredExceptionHandler(1, title_write_exception);
    // The control page: protect it, then write it every 50 ms; its first fault proves the page-write method works.
    const std::uintptr_t control = title_write_control_page();
    title_write_protect_page(control);
    std::thread([control] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (;;) {
            *reinterpret_cast<volatile std::uint64_t*>(control) += 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
    std::thread([] {
        for (int sample = 1; sample <= 120; ++sample) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            TitleWriteWatch& watch = title_write_watch();
            std::fprintf(stderr,
                         "[rt] object write watch at %d s: executor entries %u (logged %u), objects %u, page write "
                         "faults %u (other %u), tracked object writes %u, control watch fired %u, site byte %02x\n",
                         sample * 10, watch.executor_hits.load(std::memory_order_relaxed),
                         watch.executor_logged.load(std::memory_order_relaxed),
                         watch.object_count.load(std::memory_order_relaxed),
                         watch.write_faults.load(std::memory_order_relaxed),
                         watch.page_faults_other.load(std::memory_order_relaxed),
                         watch.tracked_writes.load(std::memory_order_relaxed),
                         watch.control_faults.load(std::memory_order_relaxed),
                         *reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(watch.executor_entry.load())));
            std::fflush(stderr);
        }
    }).detach();
    unsigned char bytes[8] = {};
    executor_watch_read_block(site + 1, bytes, 7);
    std::fprintf(stderr,
                 "[rt] object write watch: int3 over the executor entry image+0x%llx (original byte %02x, next bytes %02x "
                 "%02x %02x), control page %p\n",
                 static_cast<unsigned long long>(exec_rva), watch.original_byte.load(), bytes[0], bytes[1], bytes[2],
                 reinterpret_cast<void*>(control));
    std::fflush(stderr);
}

// ---- Renderer writer watch (PALMETTO_WRITER_WATCH) ---------------------------------------------------------------
// docs/vertex_buffer_va.md section 13. Section 12 armed four hardware write points from inside the int3 handler with
// GetThreadContext/SetThreadContext on the current thread and never saw a store - not even the job's +0x40 state
// dword that the job executor stores after a returning pass. Two measurements here correct that arm before its
// silence is read as "no writer":
//
//  * the points are armed in the exception's own CONTEXT record (Dr0..Dr3, Dr6 = 0, Dr7), which is the context
//    NtContinue restores when the handler returns EXCEPTION_CONTINUE_EXECUTION. That is the context the title's code
//    resumes with; a SetThreadContext on the current thread from inside the handler is a different object and section
//    12 never proved the two agree.
//  * the four points now include a SAME-PASS CONTROL on title heap memory the executor definitely writes: after the
//    entry's two virtual calls it calls the graph lock helper on [job+0x28]+0x18 (game+0x1335AE6 -> FUN_14515adb0
//    with lVar11 = graph+0x18), so DR1/DR2 watch graph+0x18 and graph+0x20 aligned down and their #DB proves the data
//    points are live in this thread, in this pass, in the same boot as the object-first-qword point. The same-pass
//    control is armed only when the machine stage reads 3 (the measured content window); before that the arm is the
//    section-12 one (object[0], graph field +0x28, state +0x40, referenced field +0x30).
//
// The writer search itself is unchanged in what it looks for and wider in where it looks:
//  * DR0 is the job first qword (the vtable the stop later calls through) and the pass's first-qword change is
//    printed with both values and the steady tick;
//  * when the machine word first reads 3, a sweep arms the job's first qword in EVERY thread of the process the
//    moment it sees it - no creation-window guard, no per-thread delay: a thread created after the first round is
//    armed on the round that first sees it, and every arm logs its thread id. Its first round is delayed 1500 ms
//    (PALMETTO_WRITER_SWEEP_DELAY_MS; PALMETTO_WRITER_SWEEP=0 disables it): the undelayed arm moved a stage-3 boot of
//    the same second from the object-vtable window into the allocator window, and the measured stop is 2-8 s after the
//    entry. The executor thread is skipped there because its four points are armed from the entry;
//  * the stage-3 entry also names the job page's mapping (VirtualQuery allocation/state/type/protect and whether a
//    file section maps there - never the file name, which is game data), the process's loaded modules with their bases
//    and base names, and starts a 1 ms poller that prints the tick at which the first qword changes. The writer line
//    resolves the storing RIP through rt::shims::module_offset, so the store that replaces the vtable is named with
//    its host module and offset, not only its raw address;
//  * the layer imports title pages as VK_EXT_external_memory_host buffers with write usage (d3d12_vulkan_device.cpp,
//    import_title_pages), so a GPU write into an imported page is the one non-CPU producer this probe cannot see with
//    a debug register. The page's mapping and the boot's own "title pages imported at first use" lines are the test:
//    a MEM_PRIVATE page no import names excludes that path in the doc, and a page an import names does not.
//
// Off unless PALMETTO_WRITER_WATCH is set; =<rva> overrides the entry site. PALMETTO_WRITER_SWEEP=0 disables the
// every-thread arm. RVAs, field offsets, counts and register values only; no game data.
constexpr std::uint32_t kWriterWatchStateMachineRva = 0x71273C8;  // kTitleStateMachineRva below: the same global
constexpr std::uint32_t kWriterWatchEntryRva = 0x1335A20;         // the per-command renderer executor

struct WriterWatch {
    std::atomic<std::uintptr_t> entry{0};
    std::atomic<unsigned char> original_byte{0};
    std::atomic<std::uintptr_t> job{0};          // the object the executor calls through (RCX at the entry)
    std::atomic<std::uintptr_t> armed_job{0};    // the job whose four points are armed in the executor thread
    std::atomic<std::uint64_t> job_first{0};     // the job's first qword as last seen at an entry
    std::atomic<std::uintptr_t> graph{0};        // *(job+0x28) at the stage-3 entry, the same-pass control's base
    // The four points as armed: the address (0 = slot unused) and what it means (kind: 0 object first qword,
    // 3 job state control, 4 the job+0x50 descriptor field the pass fills (section 14), 5 graph same-pass control).
    std::atomic<std::uintptr_t> point[4]{};
    std::atomic<unsigned> point_kind[4]{};
    std::atomic<unsigned> entries{0}, logged{0}, job_changes{0}, data_points_armed{0};
    std::atomic<unsigned> writes{0}, vtable_writes{0}, state_writes{0}, graph_control_writes{0}, other_writes{0};
    std::atomic<unsigned> descriptor_writes{0};  // kind 4: the job+0x50 descriptor field's writers (section 14)
    std::atomic<unsigned> same_pass_control_hits{0};
    std::atomic<unsigned> sweep_seen{0}, sweep_armed{0}, sweep_skipped{0}, sweep_failed{0};
    std::atomic<unsigned> poller_changes{0};
    std::atomic<unsigned> executor_tid{0};
    std::atomic<bool> stage3_entry{false};
};

WriterWatch& writer_watch() {
    static WriterWatch watch;
    return watch;
}

std::mutex& writer_sweep_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<DWORD>& writer_sweep_armed() {
    static std::vector<DWORD> tids;
    return tids;
}

thread_local std::uintptr_t writer_repoke_address = 0;

void writer_watch_start_poller(std::uintptr_t job, std::uint64_t first);
void writer_watch_start_sweep(std::uintptr_t job, std::uint64_t first);

std::string writer_watch_prefix(const char* what) {
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "[rt] writer watch t=%.3f %s", executor_watch_seconds(), what);
    return buffer;
}

// The state machine's current stage: the object at image+0x71273C8 holds it as a u16 at +0x10 (the same word the
// state trace prints as "machine current"). Returns 0xFFFF when the object is not there yet.
unsigned writer_watch_stage() {
    const std::uint64_t machine = executor_watch_read_u64(executor_watch_image_base + kWriterWatchStateMachineRva);
    if (!machine || (machine & 7) != 0) return 0xFFFF;
    std::uint16_t current = 0;
    if (!executor_watch_read_block(static_cast<std::uintptr_t>(machine) + 0x10, &current, sizeof current)) return 0xFFFF;
    return current;
}

std::string writer_watch_region(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info) != sizeof info) return "region ?";
    char text[200];
    std::snprintf(text, sizeof text, "region allocation %llx size %llx type 0x%lx protect 0x%lx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info.AllocationBase)),
                  static_cast<unsigned long long>(info.RegionSize), static_cast<unsigned long>(info.Type),
                  static_cast<unsigned long>(info.Protect));
    return text;
}

// The job page's mapping: state, type, protect and whether the pages are file/section-backed. The file name is never
// printed (it is a game path); "file-backed yes" is the whole claim a test of the non-CPU/aliased-writer hypothesis
// needs, because only a section or image mapping can alias the page at another VA or back it with non-private memory.
// The process's modules at the stage-3 entry, base and size and base name only (a file name is not game data; the full
// path is never printed). It lets the vtable writer's host address be resolved to a module+offset from the same boot:
// the writer of the object's first qword was measured in a host module, and which one is the mechanism question.
std::string writer_watch_modules() {
    std::string text = "modules:";
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return text + " ?";
    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    unsigned shown = 0;
    if (Module32First(snapshot, &entry))
        do {
            WCHAR name[MAX_MODULE_NAME32 + 1] = {};
            std::wcsncpy(name, entry.szModule, MAX_MODULE_NAME32);
            char utf8[MAX_MODULE_NAME32 * 3 + 1] = {};
            const int length = WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
            char one[320];
            std::snprintf(one, sizeof one, " %llx+%llx %s", static_cast<unsigned long long>(
                                                           reinterpret_cast<std::uintptr_t>(entry.modBaseAddr)),
                          static_cast<unsigned long long>(entry.modBaseSize), length > 1 ? utf8 : "?");
            text += one;
            if (++shown >= 48) break;
        } while (Module32Next(snapshot, &entry));
    CloseHandle(snapshot);
    return text;
}

std::string writer_watch_page_mapping(std::uintptr_t address) {
    const std::uintptr_t page = address & ~static_cast<std::uintptr_t>(0xFFF);
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(page), &info, sizeof info) != sizeof info) return "page ?";
    char text[240];
    std::snprintf(text, sizeof text,
                  "page %llx state 0x%lx type 0x%lx protect 0x%lx alloc %llx file-backed %s",
                  static_cast<unsigned long long>(page), static_cast<unsigned long>(info.State),
                  static_cast<unsigned long>(info.Type), static_cast<unsigned long>(info.Protect),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info.AllocationBase)),
                  info.Type == MEM_PRIVATE ? "no (private)" : "yes (a section/image can alias it)");
    return text;
}

// ---- The writer hit's payload: module-aware stack, destination and source (docs/vertex_buffer_va.md section 14) ------
// Section 13.8 left the copy's caller unnamed because the writer line used executor_watch_title_stack(): a raw stack
// scan that keeps only title-image frames, and at the store the storing frame is the host CRT, so it printed
// "(none)". This payload asks the OS for the unwind data instead (RtlLookupFunctionEntry/RtlVirtualUnwind, the walk
// the stage-3 pump probe already uses): the mapped title modules register their .pdata as dynamic function tables and
// host modules are in the loader's own table, so one walk crosses the host CRT, the title image, its mapped DLLs and
// the host; every frame is labelled with rt::shims::module_offset (a module+RVA, host:<dll>+offset, or the raw
// address). It also measures the copy itself: at the trap RCX is the trapped iteration's destination base, RDX its
// source and R8 the bytes still to copy, so the copy's real start and length fall out of the default-block pattern
// the earlier bytes hold. The default block is float4(1,1,1,0) repeated (section 13 called it "all-1.0f"; the dump
// at the store shows the 1.0f/0.0 lanes), so the pattern check is the exact alternating pair of qwords the copy
// writes. All reads are page checked; nothing is written into title memory. RVAs, module names and register values
// only; no game data. Off unless PALMETTO_WRITER_WATCH is set; PALMETTO_WRITER_STACK_LOG=<file> adds a file copy.
unsigned unwind_call_chain(CONTEXT context, std::uint64_t* frames, unsigned cap);  // the walker below

constexpr std::uint64_t kWriterDefaultBlockA = 0x3f8000003f800000ull;  // float4(1,1,1,0): lanes 1.0, 1.0
constexpr std::uint64_t kWriterDefaultBlockB = 0x000000003f800000ull;  // float4(1,1,1,0): lanes 0.0, 1.0

// The other qword of the two-qword float4(1,1,1,0) pattern, or 0 when the value is not one of them.
std::uint64_t writer_watch_default_other(std::uint64_t qword) {
    if (qword == kWriterDefaultBlockA) return kWriterDefaultBlockB;
    if (qword == kWriterDefaultBlockB) return kWriterDefaultBlockA;
    return 0;
}

// How many qwords of the alternating float4(1,1,1,0) block run from `address` in `step` direction (bounded). The copy
// writes front to back, so a backwards run from the trapped iteration's base measures how far the table's pattern runs;
// section 14's measurement shows it is the table's own extent (the builder fills only a prefix and leaves the default
// float4 in the rest), not the copy's start, which is why the payload derives the start from RAX instead.
unsigned writer_watch_default_run(std::uintptr_t address, int step, unsigned max_qwords) {
    std::uint64_t value = 0;
    if (!executor_watch_read_block(address, &value, sizeof value)) return 0;
    if (!writer_watch_default_other(value)) return 0;
    unsigned count = 0;
    while (count < max_qwords) {
        const std::uintptr_t next = step > 0 ? address + (count + 1) * 8 : address - (count + 1) * 8;
        std::uint64_t other = 0;
        if (!executor_watch_read_block(next, &other, sizeof other)) break;
        if (other != writer_watch_default_other(value)) break;
        value = other;
        ++count;
    }
    return count;
}

// A qword window with its address on each line, so the memory around a pointer is readable without a debugger.
std::string writer_watch_memory_lines(const char* name, std::uintptr_t address, unsigned bytes) {
    std::string text;
    for (unsigned offset = 0; offset < bytes; offset += 0x40) {
        char line[96];
        std::snprintf(line, sizeof line, "\n  %s+0x%x %s", name, offset, executor_watch_hex(address + offset).c_str());
        text += line;
        for (unsigned inner = 0; inner < 0x40; inner += 8) {
            char word[24];
            std::snprintf(word, sizeof word, " %016llx",
                          static_cast<unsigned long long>(executor_watch_read_u64(address + offset + inner)));
            text += word;
        }
    }
    return text;
}

// Every general-purpose register of the trap: the copy keeps its destination in RCX, its source in RDX and what is
// left in R8, and the title caller's frame pointer/callee-saved registers name its stack block.
std::string writer_watch_all_registers(const CONTEXT& context) {
    char buffer[560];
    std::snprintf(buffer, sizeof buffer,
                  "\n  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx rsp %llx"
                  "\n  r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx",
                  static_cast<unsigned long long>(context.Rax), static_cast<unsigned long long>(context.Rbx),
                  static_cast<unsigned long long>(context.Rcx), static_cast<unsigned long long>(context.Rdx),
                  static_cast<unsigned long long>(context.Rsi), static_cast<unsigned long long>(context.Rdi),
                  static_cast<unsigned long long>(context.Rbp), static_cast<unsigned long long>(context.Rsp),
                  static_cast<unsigned long long>(context.R8), static_cast<unsigned long long>(context.R9),
                  static_cast<unsigned long long>(context.R10), static_cast<unsigned long long>(context.R11),
                  static_cast<unsigned long long>(context.R12), static_cast<unsigned long long>(context.R13),
                  static_cast<unsigned long long>(context.R14), static_cast<unsigned long long>(context.R15));
    return buffer;
}

// The full call chain of the trapping thread, nearest frame first, each one labelled with its module (the host CRT's
// memcpy is frame 0, its caller is frame 1). Bounded and fault tolerant; the walker never modifies the CONTEXT.
std::string writer_watch_full_stack(const CONTEXT& context, unsigned max_frames = 24) {
    std::uint64_t frames[24] = {};
    const unsigned count = unwind_call_chain(context, frames, max_frames < 24 ? max_frames : 24);
    if (!count) return " (none)";
    std::string text;
    for (unsigned i = 0; i < count; ++i) {
        char line[48];
        std::snprintf(line, sizeof line, "\n  frame %u: ", i);
        text += line;
        text += rt::shims::module_offset(static_cast<std::uintptr_t>(frames[i]));
    }
    return text;
}

// The hit payload to stderr (the boot capture) and, when PALMETTO_WRITER_STACK_LOG names a file, to that file too:
// the boot's other threads interleave [rt] lines, so a copy the analysis can read on its own is worth one opt-in
// switch. The file gets the same text as the capture and nothing else.
void writer_watch_print(const std::string& text) {
    executor_watch_print(text);
    const char* path = std::getenv("PALMETTO_WRITER_STACK_LOG");
    if (!path || !*path) return;
    if (FILE* file = std::fopen(path, "ab")) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
}

// The DR7 value for up to four 8-byte write-only points (RWn = 01, LENn = 11, Ln = 1). A null or misaligned point
// leaves its slot off; the caller's point array is aligned down before it gets here.
std::uint32_t writer_watch_dr7(const std::uintptr_t points[4]) {
    std::uint32_t dr7 = 0;
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (!points[slot] || (points[slot] & 7u) != 0) continue;
        dr7 |= 1u << (slot * 2);
        dr7 |= 1u << (16 + slot * 4);
        dr7 |= 3u << (18 + slot * 4);
    }
    return dr7;
}

// Arms the four points in the exception context the handler is about to resume with. This is the method correction:
// the context record is what NtContinue restores, so these are the registers the title's code runs with. Returns how
// many points were armed (0 = the method failed on this host or every point was null).
unsigned writer_watch_arm_context(CONTEXT& context, const std::uintptr_t points[4]) {
    context.Dr0 = points[0];
    context.Dr1 = points[1];
    context.Dr2 = points[2];
    context.Dr3 = points[3];
    context.Dr6 = 0;
    context.Dr7 = writer_watch_dr7(points);
    unsigned armed = 0;
    for (unsigned slot = 0; slot < 4; ++slot)
        if (points[slot] && (points[slot] & 7u) == 0) ++armed;
    return armed;
}

// One data-point hit, after the storing instruction executed: which point, the writer's thread, instruction, the
// value the store left and the title stack. The same-pass control's hit is the method's proof and is logged first;
// the object-first-qword hit is the finding.
void writer_watch_data(CONTEXT& context) {
    WriterWatch& watch = writer_watch();
    const DWORD dr6 = context.Dr6;
    unsigned slot = 4;
    for (unsigned bit = 0; bit < 4; ++bit) {
        if ((dr6 & (1u << bit)) == 0) continue;
        slot = bit;
        break;
    }
    if (slot == 4) return;
    const std::uintptr_t address = watch.point[slot].load(std::memory_order_relaxed);
    const unsigned kind = watch.point_kind[slot].load(std::memory_order_relaxed);
    if (kind == 0) watch.vtable_writes.fetch_add(1, std::memory_order_relaxed);
    else if (kind == 3) watch.state_writes.fetch_add(1, std::memory_order_relaxed);
    else if (kind == 5) watch.same_pass_control_hits.fetch_add(1, std::memory_order_relaxed);
    else if (kind == 4) watch.descriptor_writes.fetch_add(1, std::memory_order_relaxed);
    else watch.graph_control_writes.fetch_add(1, std::memory_order_relaxed);
    const unsigned hits = watch.writes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (kind == 5) {
        // The same-pass control has to be cheap: the graph lock it watches is a hot word, so a trapped store there must
        // cost one terse line and then be disarmed, or the control itself perturbs the pass it is measuring. Two hits
        // are logged; after the second the control point is cleared in this context, so the hot word traps no more.
        const std::uint64_t control = watch.same_pass_control_hits.load(std::memory_order_relaxed);
        if (control > 1) {
            context.Dr1 = 0;
            context.Dr7 &= ~((1u << 2) | (0xFu << 20));
            return;
        }
        std::uint64_t control_value = 0;
        executor_watch_read_block(address, &control_value, sizeof control_value);
        std::fprintf(stderr,
                     "[rt] writer watch t=%.3f SAME-PASS CONTROL hit %llu: tid %lu writer %s value after %s at %s "
                     "machine stage %u\n",
                     executor_watch_seconds(), static_cast<unsigned long long>(control),
                     static_cast<unsigned long>(GetCurrentThreadId()),
                     executor_watch_label(static_cast<std::uint64_t>(context.Rip), executor_watch_image_base,
                                          executor_watch_image_size).c_str(),
                     executor_watch_hex(control_value).c_str(), executor_watch_hex(address).c_str(),
                     writer_watch_stage());
        std::fflush(stderr);
        // One hit is the proof; the control is disarmed at once so the hot lock word cannot become the measurement.
        // Only DR1 (the graph+0x18 word) is the control; DR2 is the provenance point on job+0x50 and stays armed.
        context.Dr1 = 0;
        context.Dr7 &= ~((1u << 2) | (0xFu << 20));
        return;
    }
    if (kind == 4) {
        // The provenance point. The executor reads job+0x50 before it calls the command-list object's slot 0x40, so who
        // writes that field would name the path that hands the command its descriptor. Measured (section 14): it stays
        // 0 for the whole stage-3 pass and the only hit here is the constant copy itself writing over the job, so the
        // pass took the other branch and built the descriptor on its stack from the resource object 0x1445594f0
        // returns. The point is kept as the arm's live control for that negative.
        const auto job = static_cast<std::uintptr_t>(watch.job.load(std::memory_order_relaxed));
        std::uint64_t value = 0;
        executor_watch_read_block(address, &value, sizeof value);
        std::string text = writer_watch_prefix("DESCRIPTOR FIELD WRITER (job+0x50)") + " tid " +
                           std::to_string(GetCurrentThreadId()) + " writer " +
                           rt::shims::module_offset(static_cast<std::uint64_t>(context.Rip)) + " value after " +
                           executor_watch_label(value, executor_watch_image_base, executor_watch_image_size) + " job " +
                           executor_watch_hex(job);
        text += writer_watch_all_registers(context);
        text += "\n  " + writer_watch_prefix("writer stack (module-aware)") + writer_watch_full_stack(context);
        if (value >= 0x10000) {
            const auto descriptor = static_cast<std::uintptr_t>(value);
            text += writer_watch_memory_lines("descriptor", descriptor, 0x60);
            const std::uint64_t entries = executor_watch_read_u64(descriptor + 0x18 + 0x10);
            if (entries >= 0x10000)
                text += writer_watch_memory_lines("entry0", static_cast<std::uintptr_t>(entries), 0x40);
        }
        writer_watch_print(text + "\n");
        return;
    }
    if (kind == 0) {
        if (hits > 24 && hits % 1024 != 0) return;
    } else if (kind == 3) {
        if (watch.state_writes.load(std::memory_order_relaxed) > 3) return;
    } else if (hits > 8) {
        return;
    }
    std::uint64_t value = 0;
    executor_watch_read_block(address, &value, sizeof value);
    const char* label = kind == 0   ? "OBJECT VTABLE WRITER"
                        : kind == 3 ? "job state writer (control)"
                        : kind == 5 ? "SAME-PASS CONTROL: graph lock writer"
                                    : "referenced field writer";
    const std::string writer = rt::shims::module_offset(static_cast<std::uint64_t>(context.Rip));
    std::string text = writer_watch_prefix(label) + " hit " + std::to_string(hits) + " slot " + std::to_string(slot) +
                       " tid " + std::to_string(GetCurrentThreadId()) + " writer " + writer + " value after " +
                       executor_watch_label(value, executor_watch_image_base, executor_watch_image_size) +
                       " machine stage " + std::to_string(writer_watch_stage()) + " job " +
                       executor_watch_hex(watch.job.load(std::memory_order_relaxed));
    // The breakpoint fires after the store, so the store is the instruction that ends at RIP: the bytes before RIP
    // name it without a disassembler.
    unsigned char bytes[16] = {};
    if (context.Rip >= 16 && executor_watch_read_block(static_cast<std::uintptr_t>(context.Rip) - 16, bytes,
                                                       sizeof bytes)) {
        text += "\n  bytes before writer rip:";
        char byte[8];
        for (unsigned i = 0; i < 16; ++i) {
            std::snprintf(byte, sizeof byte, " %02x", bytes[i]);
            text += byte;
        }
    }
    const auto job = static_cast<std::uintptr_t>(watch.job.load(std::memory_order_relaxed));
    std::string body;
    // The heavy payload is for the measured constant copy only: its storing frame is a host module (the CRT), while the
    // job-state control's store is title code. A title-code writer still gets the module-aware stack and the registers,
    // which is cheap; the copy's memory windows are skipped so a loading-phase control hit costs one line.
    const bool copy_hit = writer.rfind("host:", 0) == 0;
    if (copy_hit) {
        // The copy this hit is one store of. The CRT's memcpy keeps its original destination in RAX (the function opens
        // with `mov rax,rcx` and stores the first/last 32 bytes through it), and rounds the destination up to a 32-byte
        // boundary before its unrolled loop; RCX is the trapped iteration's chunk base, RDX its source and R8 the bytes
        // from RCX to the end. So the copy is RAX .. RCX+R8, and the number of completed 0x100 passes falls out of the
        // alignment offset. The default-block pattern around both pointers is the independent cross-check (section 13
        // called the block "all-1.0f"; it is float4(1,1,1,0) repeated, and only the block's tail is that pattern because
        // the rest was filled by the title's table builder).
        const auto dest = static_cast<std::uintptr_t>(context.Rcx);
        const auto source = static_cast<std::uintptr_t>(context.Rdx);
        const std::uint64_t left = static_cast<std::uint64_t>(context.R8);
        const auto saved_dst = static_cast<std::uintptr_t>(context.Rax);
        const unsigned before = dest > 8 ? writer_watch_default_run(dest - 8, -1, 8192) : 0;
        const unsigned source_before = source > 8 ? writer_watch_default_run(source - 8, -1, 8192) : 0;
        const unsigned source_after = writer_watch_default_run(source, 1, 8192);
        char copy[400];
        std::snprintf(copy, sizeof copy,
                      "\n  copy: memcpy dst (RAX) %s trapped chunk dest %s source %s left %s; default-block qwords"
                      " before the chunk dest %u (0x%llx bytes), before the source %u, from the source forward %u",
                      executor_watch_hex(saved_dst).c_str(), executor_watch_hex(dest).c_str(),
                      executor_watch_hex(source).c_str(), executor_watch_hex(left).c_str(), before,
                      static_cast<unsigned long long>(before) * 8, source_before, source_after);
        body += copy;
        if (saved_dst && dest > saved_dst) {
            // The unrolled loop starts at round_up(dst, 0x20) and each pass copies 0x100, so the pass count is exact.
            const std::uintptr_t aligned = saved_dst + (0x20 - (saved_dst & 0x1F));
            const std::uintptr_t offset = dest >= aligned ? dest - aligned : 0;
            char measured[256];
            std::snprintf(measured, sizeof measured,
                          "\n  copy size: dst %s first pass base %s completed pass(es) %llu size %s (end %s);"
                          " source end %s",
                          executor_watch_hex(saved_dst).c_str(), executor_watch_hex(aligned).c_str(),
                          static_cast<unsigned long long>(offset / 0x100),
                          executor_watch_hex(left + (offset / 0x100) * 0x100 +
                                             (static_cast<std::uint64_t>(aligned) - saved_dst)).c_str(),
                          executor_watch_hex(saved_dst + left + (offset / 0x100) * 0x100 +
                                             (static_cast<std::uint64_t>(aligned) - saved_dst)).c_str(),
                          executor_watch_hex(source + left).c_str());
            body += measured;
        }
        body += writer_watch_all_registers(context);
        body += "\n  " + writer_watch_prefix("writer stack (module-aware)") + writer_watch_full_stack(context);
        // The title function that calls the copy (0x550e2d0) selects the descriptor entry with 0x13367a0(index 0) into
        // its own frame at [rsp+0x80]; memcpy pushes nothing, so the caller's frame is the trap RSP + 8. The struct's
        // +0x10 is the entry's declared size (the allocation's size field, set by the allocator call in 0x1445594f0)
        // and +0x18 is the destination pointer 0x13367a0 returns: the copy length against that size is the mismatch
        // this measurement decides.
        const std::uintptr_t caller_frame = static_cast<std::uintptr_t>(context.Rsp) + 8;
        if (caller_frame > 0x1000)
            body += writer_watch_memory_lines("caller-frame", caller_frame + 0x40, 0x140);
        if (saved_dst > 0x100) body += writer_watch_memory_lines("copy-dst-before", saved_dst - 0x100, 0x100);
        if (saved_dst) body += writer_watch_memory_lines("copy-dst", saved_dst, 0x200);
        if (dest > 0x100) body += writer_watch_memory_lines("dest-before", dest - 0x100, 0x100);
        body += writer_watch_memory_lines("dest", dest, 0x300);
        if (source > 0x100) body += writer_watch_memory_lines("source-before", source - 0x100, 0x100);
        body += writer_watch_memory_lines("source", source, 0x300);
        if (saved_dst)
            body += "\n  " + writer_watch_region(saved_dst) + "\n  " + writer_watch_page_mapping(saved_dst);
    } else {
        body += writer_watch_all_registers(context);
        body += "\n  " + writer_watch_prefix("writer stack (module-aware)") + writer_watch_full_stack(context);
    }
    if (job) {
        // The job's own fields and sub-object identities. job+0x50 stays 0 through the pass (the kind-4 point's
        // negative), so the descriptor the copy's destination comes from is the stack-local one the pass builds from
        // the resource object 0x1445594f0 returns; the fields are printed anyway so the job's layout is in the record.
        body += executor_watch_memory_dump("job", job, 0x180);
        const std::uint64_t object30 = executor_watch_read_u64(job + 0x30);
        const std::uint64_t object50 = executor_watch_read_u64(job + 0x50);
        const std::uint64_t object58 = executor_watch_read_u64(job + 0x58);
        char classes[400];
        std::snprintf(classes, sizeof classes,
                      "\n  job+0x30 object %s vtable %s; job+0x50 descriptor %s vtbl %s; job+0x58 object %s vtable %s",
                      executor_watch_hex(object30).c_str(),
                      executor_watch_label(executor_watch_read_u64(static_cast<std::uintptr_t>(object30)),
                                           executor_watch_image_base, executor_watch_image_size).c_str(),
                      executor_watch_hex(object50).c_str(),
                      executor_watch_label(executor_watch_read_u64(static_cast<std::uintptr_t>(object50)),
                                           executor_watch_image_base, executor_watch_image_size).c_str(),
                      executor_watch_hex(object58).c_str(),
                      executor_watch_label(executor_watch_read_u64(static_cast<std::uintptr_t>(object58)),
                                           executor_watch_image_base, executor_watch_image_size).c_str());
        body += classes;
        if (object50 >= 0x10000) {
            const auto descriptor = static_cast<std::uintptr_t>(object50);
            body += writer_watch_memory_lines("descriptor", descriptor, 0x60);
            const std::uint64_t entries = executor_watch_read_u64(descriptor + 0x18 + 0x10);
            if (entries >= 0x10000) body += writer_watch_memory_lines("entry0", static_cast<std::uintptr_t>(entries), 0x40);
        }
    }
    writer_watch_print(text + body + "\n");
}

// One executor entry: RCX is the job/command object the executor calls through. Log a new job class with its chain
// and machine stage, notice a first qword that changed between passes, and arm the four points in the resume context.
void writer_watch_entry(CONTEXT& context) {
    WriterWatch& watch = writer_watch();
    const unsigned hit = watch.entries.fetch_add(1, std::memory_order_relaxed) + 1;
    // The int3 is at the entry's first byte, so the prologue has not run and RSP points at the return address. The
    // section-12 arm is kept for the loading phase; the stage-3 arm below replaces its two non-finding points.
    CONTEXT& resume = context;
    const auto job = static_cast<std::uintptr_t>(resume.Rcx);
    if (!job) return;
    std::uint64_t first = 0;
    if (!executor_watch_read_block(job, &first, sizeof first)) return;
    const unsigned stage = writer_watch_stage();
    const auto last_job = static_cast<std::uintptr_t>(watch.job.load(std::memory_order_relaxed));
    if (job != last_job) {
        // The pass that used the last job has returned; read its first qword again. A vtable replaced while the
        // object was between passes is logged here with both values and the tick.
        if (last_job) {
            std::uint64_t after = 0;
            const std::uint64_t before = watch.job_first.load(std::memory_order_relaxed);
            if (executor_watch_read_block(last_job, &after, sizeof after) && after != before) {
                std::fprintf(stderr,
                             "[rt] writer watch t=%.3f object first qword changed between passes: job %s was %s now %s"
                             " machine stage %u\n",
                             executor_watch_seconds(), executor_watch_hex(last_job).c_str(),
                             executor_watch_label(before, executor_watch_image_base, executor_watch_image_size).c_str(),
                             executor_watch_label(after, executor_watch_image_base, executor_watch_image_size).c_str(),
                             writer_watch_stage());
                std::fflush(stderr);
            }
        }
        watch.job.store(job);
        watch.job_first.store(first);
        const unsigned change = watch.job_changes.fetch_add(1, std::memory_order_relaxed) + 1;
        if (change <= 40 || change % 4096 == 0) {
            watch.logged.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t state = executor_watch_read_u32(job + 0x40);
            const std::uint64_t graph = executor_watch_read_u64(job + 0x28);
            std::string text = writer_watch_prefix("executor object") + " job " + executor_watch_hex(job) + " hit " +
                               std::to_string(hit) + " change " + std::to_string(change) + " tid " +
                               std::to_string(GetCurrentThreadId()) + " machine stage " + std::to_string(stage) +
                               " state " + std::to_string(state) + " graph " +
                               executor_watch_label(graph, executor_watch_image_base, executor_watch_image_size) +
                               " object[0] " +
                               executor_watch_label(first, executor_watch_image_base, executor_watch_image_size) +
                               "\n  " + writer_watch_region(job) + "\n  " + writer_watch_page_mapping(job) +
                               executor_watch_memory_dump("job", job, 0x80);
            executor_watch_print(text + "\n");
        }
    } else {
        const std::uint64_t before = watch.job_first.load(std::memory_order_relaxed);
        if (first != before) {
            watch.job_first.store(first);
            const unsigned hits = watch.vtable_writes.fetch_add(1, std::memory_order_relaxed) + 1;
            if (hits <= 24 || hits % 1024 == 0) {
                std::fprintf(stderr,
                             "[rt] writer watch t=%.3f object first qword changed between passes: job %s was %s now %s"
                             " machine stage %u\n",
                             executor_watch_seconds(), executor_watch_hex(job).c_str(),
                             executor_watch_label(before, executor_watch_image_base, executor_watch_image_size).c_str(),
                             executor_watch_label(first, executor_watch_image_base, executor_watch_image_size).c_str(),
                             writer_watch_stage());
                executor_watch_print(executor_watch_memory_dump("job", job, 0x80) + "\n");
            }
        }
    }
    // The four points. Qword addresses are aligned down: a DR7 8-byte watch requires alignment and the store to the
    // object's first qword overlaps the aligned word either way.
    std::uintptr_t points[4] = {job & ~static_cast<std::uintptr_t>(7),
                                job ? (job + 0x28) & ~static_cast<std::uintptr_t>(7) : 0,
                                job ? (job + 0x40) & ~static_cast<std::uintptr_t>(7) : 0,
                                job ? (job + 0x30) & ~static_cast<std::uintptr_t>(7) : 0};
    unsigned kinds[4] = {0, 1, 3, 2};
    const std::uint64_t graph = executor_watch_read_u64(job + 0x28);
    if (stage == 3 && graph && (graph & 7) == 0) {
        // The same-pass control: the executor's first job after the entry's virtual calls is the graph lock helper on
        // graph+0x18 (FUN_14515adb0, called from game+0x1335AE6). A write there is made by this thread in this pass.
        points[1] = (static_cast<std::uintptr_t>(graph) + 0x18) & ~static_cast<std::uintptr_t>(7);
        // Provenance point (section 14): the executor reads job+0x50 before it calls the command-list object's slot
        // 0x40, so a writer of this field would name the descriptor's path. The measurement shows it stays 0 and the
        // call is made with a stack-local descriptor instead; graph+0x20 was a duplicate of the graph+0x18 control
        // (one store word), so the control itself stays armed in DR1 and this point costs nothing.
        points[2] = (job + 0x50) & ~static_cast<std::uintptr_t>(7);
        points[3] = (job + 0x40) & ~static_cast<std::uintptr_t>(7);
        kinds[1] = 5;
        kinds[2] = 4;
        kinds[3] = 3;
        if (!watch.stage3_entry.exchange(true)) {            watch.graph.store(static_cast<std::uintptr_t>(graph));
            watch.executor_tid.store(GetCurrentThreadId());
            std::uint64_t lock_word = 0;
            executor_watch_read_block(points[1], &lock_word, sizeof lock_word);
            std::uint64_t descriptor_field = 0;
            executor_watch_read_block(points[2], &descriptor_field, sizeof descriptor_field);
            std::string text = writer_watch_prefix("STAGE 3 ENTRY") + " job " + executor_watch_hex(job) +
                               " tid " + std::to_string(GetCurrentThreadId()) + " graph " +
                               executor_watch_hex(graph) + " same-pass control at graph+0x18 " +
                               executor_watch_hex(points[1]) + " (now " + executor_watch_hex(lock_word) +
                               "), provenance point at job+0x50 " + executor_watch_hex(points[2]) + " (now " +
                               executor_watch_hex(descriptor_field) + ")" +
                               "\n  " + writer_watch_region(job) + "\n  " + writer_watch_page_mapping(job) +
                               "\n  " + writer_watch_page_mapping(points[1]) +
                               executor_watch_memory_dump("job", job, 0x80) +
                               executor_watch_memory_dump("graph", static_cast<std::uintptr_t>(graph), 0x60) +
                               "\n  " + writer_watch_modules();
            executor_watch_print(text + "\n");
            writer_watch_start_poller(job, first);
            writer_watch_start_sweep(job, first);
        }
    }
    for (unsigned slot = 0; slot < 4; ++slot) {
        watch.point[slot].store(points[slot], std::memory_order_relaxed);
        watch.point_kind[slot].store(kinds[slot], std::memory_order_relaxed);
    }
    const unsigned armed = writer_watch_arm_context(resume, points);
    if (!armed) {
        if (watch.data_points_armed.load(std::memory_order_relaxed) == 0)
            std::fprintf(stderr,
                         "[rt] writer watch: no write point could be armed in the resume context: the data watch is a "
                         "method failure on this host\n");
        return;
    }
    watch.armed_job.store(job);
    const unsigned total = watch.data_points_armed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (total <= 12 || total % 2048 == 0)
        executor_watch_print(writer_watch_prefix("armed the four write points in the resume context") + " for job " +
                             executor_watch_hex(job) + " at hit " + std::to_string(hit) + ", " + std::to_string(armed) +
                             " point(s) (object first qword, graph same-pass control, job state), " +
                             std::to_string(total) + " arm(s), machine stage " + std::to_string(stage) + "\n");
}

// Arms the object's first qword in a suspended thread. One 8-byte write point (DR0).
bool writer_watch_arm_thread(HANDLE thread, std::uintptr_t qword) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(thread, &context)) return false;
    context.Dr0 = qword;
    context.Dr1 = 0;
    context.Dr2 = 0;
    context.Dr3 = 0;
    context.Dr6 = 0;
    context.Dr7 = (qword && (qword & 7u) == 0) ? (0x1u | (0x1u << 16) | (3u << 18)) : 0;
    return SetThreadContext(thread, &context) != FALSE;
}

// The stage-3 every-thread sweep: no creation-window guard and no per-thread delay. Every thread id the process has
// when it is first seen is armed on that round (100 ms), and each arm logs its thread id; a thread created later is
// armed the round that first sees it. The executor thread is skipped by id: its four points are armed from the entry
// and a second SetThreadContext would replace them with the single sweep point.
//
// The sweep start is delayed (PALMETTO_WRITER_SWEEP_DELAY_MS, default 1500) because the measured arm without the
// delay moved the stop from the object-vtable window (game+0x13360A3, the same arm with no writer probe) into the
// allocator window: suspending 100+ threads at the stage-3 entry is a perturbation of the exact transition being
// measured. The delay is on the sweep, not on a thread: a thread seen after the sweep starts is armed immediately (the
// per-thread creation-window guard section 12 could not rule out is still gone). The measured stop is 2-8 s after the
// entry, so the sweep is armed for the window. PALMETTO_WRITER_SWEEP=0 disables the sweep.
unsigned writer_watch_sweep_delay_ms() {
    static const unsigned delay = [] {
        const char* value = std::getenv("PALMETTO_WRITER_SWEEP_DELAY_MS");
        if (!value || !*value) return 1500u;
        const long parsed = std::strtol(value, nullptr, 10);
        if (parsed < 0) return 1500u;
        return parsed > 60000 ? 60000u : static_cast<unsigned>(parsed);
    }();
    return delay;
}

bool writer_watch_sweep_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("PALMETTO_WRITER_SWEEP");
        return !(value && *value == '0');
    }();
    return enabled;
}

void writer_watch_start_sweep(std::uintptr_t job, std::uint64_t first) {
    if (!writer_watch_sweep_enabled()) {
        executor_watch_print(writer_watch_prefix("every-thread sweep disabled") + " by PALMETTO_WRITER_SWEEP=0\n");
        return;
    }
    const std::uintptr_t qword = job & ~static_cast<std::uintptr_t>(7);
    const unsigned delay_ms = writer_watch_sweep_delay_ms();
    std::thread([qword, first, delay_ms] {
        // The stage-3 entry is left unperturbed for the delay: the sweep's first suspension round is what moves the
        // stop out of the object-vtable window in the measured arms. The per-thread rule after it is unchanged.
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        const auto started = std::chrono::steady_clock::now();
        auto elapsed = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        };
        while (elapsed() < 240.0) {
            const DWORD self = GetCurrentThreadId();
            const DWORD executor = writer_watch().executor_tid.load(std::memory_order_relaxed);
            std::vector<DWORD> fresh;
            {
                const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (snapshot != INVALID_HANDLE_VALUE) {
                    THREADENTRY32 entry{};
                    entry.dwSize = sizeof(entry);
                    const DWORD pid = GetCurrentProcessId();
                    if (Thread32First(snapshot, &entry))
                        do {
                            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
                            if (entry.th32ThreadID == executor) continue;
                            bool known = false;
                            {
                                std::lock_guard lock(writer_sweep_mutex());
                                for (const DWORD tid : writer_sweep_armed())
                                    if (tid == entry.th32ThreadID) {
                                        known = true;
                                        break;
                                    }
                            }
                            if (!known && fresh.size() < 512) fresh.push_back(entry.th32ThreadID);
                        } while (Thread32Next(snapshot, &entry));
                    CloseHandle(snapshot);
                }
            }
            WriterWatch& watch = writer_watch();
            for (const DWORD tid : fresh) {
                watch.sweep_seen.fetch_add(1, std::memory_order_relaxed);
                const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                                     THREAD_QUERY_INFORMATION,
                                                 FALSE, tid);
                bool armed = false;
                if (thread) {
                    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                        armed = writer_watch_arm_thread(thread, qword);
                        ResumeThread(thread);
                    }
                    CloseHandle(thread);
                }
                {
                    std::lock_guard lock(writer_sweep_mutex());
                    if (writer_sweep_armed().size() < 1024) writer_sweep_armed().push_back(tid);
                }
                if (armed) {
                    const unsigned total = watch.sweep_armed.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (total <= 24 || total % 64 == 0)
                        executor_watch_print(writer_watch_prefix("swept a thread") + " tid " + std::to_string(tid) +
                                             " armed the object first qword " + executor_watch_hex(qword) +
                                             " (no creation-window guard), " + std::to_string(total) +
                                             " thread(s) armed, machine stage " + std::to_string(writer_watch_stage()) +
                                             "\n");
                } else {
                    watch.sweep_failed.fetch_add(1, std::memory_order_relaxed);
                    if (watch.sweep_failed.load(std::memory_order_relaxed) <= 3)
                        executor_watch_print(writer_watch_prefix("swept a thread") + " tid " + std::to_string(tid) +
                                             " could not be armed\n");
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
    executor_watch_print(writer_watch_prefix("every-thread sweep started") + " for the object first qword " +
                         executor_watch_hex(qword) + " (job " + executor_watch_hex(job) + ", first qword " +
                         executor_watch_label(first, executor_watch_image_base, executor_watch_image_size) +
                         "): no per-thread creation-window guard, executor thread skipped, first round after " +
                         std::to_string(delay_ms) + " ms\n");
}

// Dates the store: a 1 ms poll of the object's first qword from the stage-3 entry, printed on every change with the
// steady tick the title probes and PALMETTO_TIMELINE share.
void writer_watch_start_poller(std::uintptr_t job, std::uint64_t first) {
    std::thread([job, first] {
        std::uint64_t last = first;
        std::uint64_t previous_tick = executor_watch_seconds();
        for (int sample = 0; sample < 240000; ++sample) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::uint64_t now = 0;
            if (!executor_watch_read_block(job, &now, sizeof now)) {
                executor_watch_print(writer_watch_prefix("poller") + " job " + executor_watch_hex(job) +
                                     " is not readable any more at t=" + executor_watch_hex(0) + "\n");
                return;
            }
            if (now == last) continue;
            const std::uint64_t tick = static_cast<std::uint64_t>(executor_watch_seconds());
            WriterWatch& watch = writer_watch();
            const unsigned change = watch.poller_changes.fetch_add(1, std::memory_order_relaxed) + 1;
            executor_watch_print(
                writer_watch_prefix("POLLER: object first qword changed") + " job " + executor_watch_hex(job) +
                " was " + executor_watch_label(last, executor_watch_image_base, executor_watch_image_size) + " now " +
                executor_watch_label(now, executor_watch_image_base, executor_watch_image_size) + " at t=" +
                executor_watch_hex(tick) + " (previous sample " + executor_watch_hex(previous_tick) + "), change " +
                std::to_string(change) + ", machine stage " + std::to_string(writer_watch_stage()) + "\n  " +
                writer_watch_page_mapping(job) + executor_watch_memory_dump("job", job, 0x80) + "\n");
            last = now;
            previous_tick = tick;
            if (change >= 12) return;
        }
    }).detach();
}

LONG CALLBACK writer_watch_exception(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    WriterWatch& watch = writer_watch();
    if (code == EXCEPTION_BREAKPOINT) {
        const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        if (at != static_cast<std::uintptr_t>(watch.entry.load())) return EXCEPTION_CONTINUE_SEARCH;
        writer_watch_entry(context);
        DWORD old = 0;
        if (VirtualProtect(reinterpret_cast<void*>(at), 1, PAGE_EXECUTE_READWRITE, &old)) {
            *reinterpret_cast<unsigned char*>(at) = watch.original_byte.load();
            VirtualProtect(reinterpret_cast<void*>(at), 1, old, &old);
        }
        writer_repoke_address = at;
        context.EFlags |= 0x100;  // trap after the restored instruction, then put the int3 back
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP) {
        // A data hit and the trap-flag single-step of the restored instruction can be the same exception. Handle the
        // data hit first, then the re-poke, and clear Dr6 either way.
        const bool data_hit = (context.Dr6 & 0xFu) != 0;
        if (data_hit) writer_watch_data(context);
        if (writer_repoke_address) {
            const std::uintptr_t address = writer_repoke_address;
            writer_repoke_address = 0;
            context.EFlags &= ~static_cast<DWORD>(0x100);
            DWORD old = 0;
            if (VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old)) {
                *reinterpret_cast<unsigned char*>(address) = 0xCC;
                VirtualProtect(reinterpret_cast<void*>(address), 1, old, &old);
            }
            context.Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (data_hit) {
            context.Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Puts the int3 over the executor entry and starts the reporter. The write points are armed in the exception's own
// CONTEXT record, and the stage-3 pass adds the every-thread sweep and the change poller.
void start_writer_watch(std::uintptr_t image_base, std::size_t image_size) {
    const char* spec = std::getenv("PALMETTO_WRITER_WATCH");
    if (!spec || !*spec || *spec == '0') return;
    std::uint64_t entry_rva = kWriterWatchEntryRva;
    if (!(spec[0] == '1' && spec[1] == '\0')) {
        char* end = nullptr;
        const std::uint64_t value = std::strtoull(spec, &end, 16);
        if (end == spec) {
            std::fprintf(stderr, "[rt] writer watch: PALMETTO_WRITER_WATCH=%s is not a hex RVA; not armed\n", spec);
            return;
        }
        entry_rva = value;
    }
    executor_watch_image_base = image_base;
    executor_watch_image_size = image_size;
    WriterWatch& watch = writer_watch();
    const auto site = static_cast<std::uintptr_t>(image_base + entry_rva);
    watch.entry.store(site);
    auto* const byte = reinterpret_cast<unsigned char*>(site);
    watch.original_byte.store(*byte);
    DWORD old = 0;
    if (!VirtualProtect(byte, 1, PAGE_EXECUTE_READWRITE, &old)) {
        std::fprintf(stderr, "[rt] writer watch: cannot make the executor entry writable; not armed\n");
        return;
    }
    *byte = 0xCC;
    VirtualProtect(byte, 1, old, &old);
    AddVectoredExceptionHandler(1, writer_watch_exception);
    std::thread([] {
        for (int sample = 1; sample <= 120; ++sample) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            WriterWatch& watch = writer_watch();
            std::fprintf(stderr,
                         "[rt] writer watch at %d s: executor entries %u (job logs %u), job changes %u (data arm(s) "
                         "%u), data hits %u (vtable %u, same-pass control %u, state %u, descriptor field %u, graph "
                         "control %u), same-pass "
                         "control hits %u, sweep seen %u armed %u failed %u, poller changes %u, machine stage %u, "
                         "site byte %02x\n",
                         sample * 10, watch.entries.load(std::memory_order_relaxed),
                         watch.logged.load(std::memory_order_relaxed),
                         watch.job_changes.load(std::memory_order_relaxed),
                         watch.data_points_armed.load(std::memory_order_relaxed),
                         watch.writes.load(std::memory_order_relaxed),
                         watch.vtable_writes.load(std::memory_order_relaxed),
                         watch.same_pass_control_hits.load(std::memory_order_relaxed),
                         watch.state_writes.load(std::memory_order_relaxed),
                         watch.descriptor_writes.load(std::memory_order_relaxed),
                         watch.graph_control_writes.load(std::memory_order_relaxed),
                         watch.same_pass_control_hits.load(std::memory_order_relaxed),
                         watch.sweep_seen.load(std::memory_order_relaxed),
                         watch.sweep_armed.load(std::memory_order_relaxed),
                         watch.sweep_failed.load(std::memory_order_relaxed),
                         watch.poller_changes.load(std::memory_order_relaxed), writer_watch_stage(),
                         *reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(watch.entry.load())));
            std::fflush(stderr);
        }
    }).detach();
    unsigned char bytes[8] = {};
    executor_watch_read_block(site + 1, bytes, 7);
    std::fprintf(stderr,
                 "[rt] writer watch: int3 over the executor entry image+0x%llx (original byte %02x, next bytes %02x "
                 "%02x %02x), no debug register armed at the site; write points are armed in the exception context\n",
                 static_cast<unsigned long long>(entry_rva), watch.original_byte.load(), bytes[0], bytes[1], bytes[2]);
    std::fflush(stderr);
}

// ---- Title allocation trace (PALMETTO_ALLOC_WATCH) -----------------------------------------------------------------
// docs/vertex_buffer_va.md section 15. Section 14 measured the stage-3 copy: the renderer executor's slot-0x40 table
// builder copies a 0x1000-byte float4 table from the executor thread's stack into a block the title's own allocator
// returns, and in every catch that block's range contains the live command object (the job) the same pass is running.
// Two title-side mechanisms fit - a block allocated smaller than the copy, or an allocator range that still holds the
// live object - and this probe decides between them at the allocation, before the copy runs:
//
//  * game+0x1335A20 (executor entry): reads machine stage 3 and records the job the executor runs (RCX). Its first
//    stage-3 pass is when the two allocation points below are armed, so no loading-phase traffic is trapped.
//  * game+0x184F0C (after the flush's 0x70-slot allocation of the 0xa0-byte job): RAX is the returned job, RDI the
//    object it was created from, so the command object's allocation is named with its address and page.
//  * game+0x455964C (after the constant helper's 0x50-slot allocation of the table block): RAX is the returned block,
//    R12 the requested size, RBP/R13 the table's width/height and [RSP+0x28] the descriptor object being built. It
//    reports the block, the request, the job the executor is running, and whether the two ranges overlap.
//  * game+0x1847A1 (after the command object's slot-0x20 getter returns, armed at the stage-3 poll before the pass that
//    crashes): RAX is the format key the flush uses as the job's format, RCX the command object whose slot 0x20 is
//    game+0x23E550 (`mov eax,0xa; ret`) and RSI the object whose slot 0x10 creates the job. This is the class input at
//    its source, measured before the job exists; docs/vertex_buffer_va.md section 16.
//  * game+0x45594F0 (the constant helper's entry: class, plane count, dims): besides those inputs, the first four
//    entries print the class-origin chain - the job's +0x28/+0x30/+0x58 objects with the vtable and the slots the
//    decompiled code calls (job+0x58 slot 8 is the class, job+0x30 slot 0x20 is the format key the job was built
//    with, slot 0x40 the table builder), plus the format object's own bytes. The stage-3 entry prints the same chain
//    once. This is docs/vertex_buffer_va.md section 16.
//  * game+0x515ABB0 (the title's block free entry, RDX = pointer - the site PALMETTO_EXECUTOR_WATCH names), off unless
//    PALMETTO_ALLOC_WATCH_FREE=1, armed at the stage-3 entry and printed only for a pointer within 4 KiB of the job or
//    the block, so a free that could have put the live job on the free list is dated against the block.
//
// All four are int3 sites with the writer watch's own restore/trap/re-poke discipline: the original byte runs, the
// single-step puts the int3 back. The executor site is skipped when PALMETTO_WRITER_WATCH is on (both would patch the
// same byte), so an allocation arm runs PALMETTO_ALLOC_WATCH alone. Bounded: 24 lines per site class, then counters.
// RVAs, sizes, register values, field offsets and module+offset labels only; no game data.
constexpr std::uint32_t kAllocWatchExecutorRva = 0x1335A20;        // the per-command renderer executor entry
constexpr std::uint32_t kAllocWatchJobAllocRva = 0x184F0C;         // after the flush's 0xa0-byte job allocation
constexpr std::uint32_t kAllocWatchBlockAllocRva = 0x455964C;      // after the constant helper's block allocation
constexpr std::uint32_t kAllocWatchFreeRva = 0x515ABB0;            // the title allocator's block free entry
constexpr std::uint32_t kAllocWatchHelperEntryRva = 0x45594F0;     // the constant helper: class, plane count, dims
constexpr std::uint32_t kAllocWatchEntryWrittenRva = 0x4559684;    // the 0x20-byte descriptor entry just written
constexpr std::uint32_t kAllocWatchFormatKeyRva = 0x1847A1;        // after the command object's format-getter call
constexpr std::uint32_t kAllocWatchStateMachineRva = 0x71273C8;    // the state-machine object pointer, stage at +0x10
constexpr std::uint32_t kAllocWatchAllocatorGlobalRva = 0xABCC310; // the title's default allocator object pointer
constexpr unsigned kAllocWatchSites = 7;
constexpr unsigned kAllocWatchLines = 24;

enum AllocWatchSite : unsigned { alloc_watch_executor = 0, alloc_watch_job = 1, alloc_watch_block = 2,
                                 alloc_watch_free = 3, alloc_watch_helper = 4, alloc_watch_entry = 5,
                                 alloc_watch_formatkey = 6 };

struct AllocWatch {
    std::uintptr_t image_base = 0;
    std::atomic<std::uintptr_t> site[kAllocWatchSites]{};
    std::atomic<unsigned char> original[kAllocWatchSites]{};
    std::atomic<unsigned> hits[kAllocWatchSites]{}, logged[kAllocWatchSites]{};
    std::atomic<unsigned> stage{0xFFFF};
    std::atomic<bool> stage3_entry{false}, identity_logged{false}, free_requested{false}, armed{false};
    std::atomic<std::uintptr_t> executor_job{0}, executor_tid{0};
    std::atomic<std::uintptr_t> last_job{0}, last_job_tid{0}, block{0}, block_size{0};
    std::atomic<unsigned> executor_entries{0}, job_allocs{0}, job_matches{0}, overlaps{0}, free_matches{0};
    std::mutex recent_mutex;
    std::uintptr_t recent_jobs[8]{};
    unsigned recent_count = 0;
};

AllocWatch& alloc_watch() {
    static AllocWatch watch;
    return watch;
}

// The site whose original byte is about to be re-poked after the trap flag single-step (slot + 1; 0 is "none").
thread_local unsigned alloc_watch_repoke_slot = 0;

// The state machine's current stage: the object at image+0x71273C8 holds it as a u16 at +0x10 (the word the state
// trace prints as "machine current"). Same source the writer watch uses. 0xFFFF when the object is not there yet.
unsigned alloc_watch_stage() {
    const AllocWatch& watch = alloc_watch();
    if (!watch.image_base) return 0xFFFF;
    const std::uint64_t machine = executor_watch_read_u64(watch.image_base + kAllocWatchStateMachineRva);
    if (!machine || (machine & 7) != 0) return 0xFFFF;
    std::uint16_t current = 0;
    if (!executor_watch_read_block(static_cast<std::uintptr_t>(machine) + 0x10, &current, sizeof current)) return 0xFFFF;
    return current;
}

// The title's default allocator object, its vtable and the three slots this measurement uses, labelled by module.
std::string alloc_watch_allocator_text() {
    const AllocWatch& watch = alloc_watch();
    const auto image_base = watch.image_base;
    if (!image_base) return "allocator image base unknown";
    const auto allocator = static_cast<std::uintptr_t>(executor_watch_read_u64(image_base + kAllocWatchAllocatorGlobalRva));
    if (!allocator) return "allocator image+0xabcc310 -> 0";
    const auto vtable = static_cast<std::uintptr_t>(executor_watch_read_u64(allocator));
    std::string text = "allocator image+0xabcc310 -> " + executor_watch_hex(allocator) + " vtable " +
                       executor_watch_hex(vtable) + " (" + rt::shims::module_offset(vtable) + ")";
    struct Slot {
        const char* name;
        std::uint32_t offset;
    };
    const Slot slots[3] = {{"alloc", 0x50}, {"free", 0x60}, {"alloc-classed", 0x70}};
    for (const Slot& slot : slots) {
        const auto target = static_cast<std::uintptr_t>(executor_watch_read_u64(vtable + slot.offset));
        text += "\n  vtable+0x";
        char offset[8];
        std::snprintf(offset, sizeof offset, "%x", slot.offset);
        text += offset;
        text += std::string(" ") + slot.name + " " + executor_watch_hex(target) + " (" +
                rt::shims::module_offset(target) + ")";
    }
    return text;
}

// Replace first byte with int3; remember the original. The same order the writer watch uses: the page is made
// writable first, then the byte is read and replaced, so a page the loader has not committed yet fails here by name.
bool alloc_watch_install(AllocWatch& watch, unsigned slot, std::uintptr_t site) {
    auto* const byte = reinterpret_cast<unsigned char*>(site);
    DWORD old = 0;
    if (!VirtualProtect(byte, 1, PAGE_EXECUTE_READWRITE, &old)) {
        MEMORY_BASIC_INFORMATION info{};
        const SIZE_T queried = VirtualQuery(byte, &info, sizeof info);
        std::fprintf(stderr,
                     "[rt] alloc watch: VirtualProtect failed at image+0x%llx (error %lu; query %llu state 0x%lx protect "
                     "0x%lx type 0x%lx)\n",
                     static_cast<unsigned long long>(site - watch.image_base), GetLastError(),
                     static_cast<unsigned long long>(queried), queried == sizeof info ? info.State : 0,
                     queried == sizeof info ? info.Protect : 0, queried == sizeof info ? info.Type : 0);
        std::fflush(stderr);
        return false;
    }
    watch.original[slot].store(*byte);
    *byte = 0xCC;
    VirtualProtect(byte, 1, old, &old);
    watch.site[slot].store(site);
    return true;
}

// The original byte back (so the trapped instruction can execute), or the int3 back (after its single step).
void alloc_watch_write_byte(unsigned slot, unsigned char value) {
    AllocWatch& watch = alloc_watch();
    const auto site = static_cast<std::uintptr_t>(watch.site[slot].load());
    if (!site) return;
    DWORD old = 0;
    auto* const byte = reinterpret_cast<unsigned char*>(site);
    if (!VirtualProtect(byte, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    *byte = value;
    VirtualProtect(byte, 1, old, &old);
}

// One traced allocation of the constant table's block, at the helper's own return site before the descriptor entry is
// written: the block and its requested size are RAX/R12, and the executor's job is whatever the last stage-3 entry read.
void alloc_watch_block_hit(CONTEXT& context) {
    AllocWatch& watch = alloc_watch();
    const unsigned hit = watch.hits[alloc_watch_block].fetch_add(1, std::memory_order_relaxed) + 1;
    const auto block = static_cast<std::uintptr_t>(context.Rax);
    const auto size = static_cast<std::uintptr_t>(context.R12);
    const auto rsp = static_cast<std::uintptr_t>(context.Rsp);
    const auto descriptor = static_cast<std::uintptr_t>(executor_watch_read_u64(rsp + 0x28));
    const auto param3 = static_cast<std::uintptr_t>(executor_watch_read_u64(rsp + 0x40));
    const auto caller = static_cast<std::uintptr_t>(executor_watch_read_u64(rsp + 0x88));  // 8 pushes + 0x48
    const auto job = static_cast<std::uintptr_t>(watch.executor_job.load(std::memory_order_relaxed));
    const auto job_tid = static_cast<unsigned long>(watch.executor_tid.load(std::memory_order_relaxed));
    const auto width = static_cast<unsigned>(context.Rbp & 0xFFFFFFFFu);
    const auto height = static_cast<unsigned>(context.R13 & 0xFFFFFFFFu);
    if (block) {
        watch.block.store(block);
        watch.block_size.store(size);
    }
    const std::uintptr_t end = block + (size ? size : 0x1000);
    const bool overlap = block && job && block <= job && job < end;
    if (overlap) watch.overlaps.fetch_add(1, std::memory_order_relaxed);
    if (watch.logged[alloc_watch_block].fetch_add(1, std::memory_order_relaxed) >= kAllocWatchLines) return;
    char line[420];
    std::snprintf(line, sizeof line,
                  "[rt] alloc watch t=%.3f BLOCK ALLOC hit %u tid %lu block %s size %s width %u height %u caller %s "
                  "job %s job-block 0x%llx overlap %s",
                  executor_watch_seconds(), hit, static_cast<unsigned long>(GetCurrentThreadId()),
                  executor_watch_hex(block).c_str(), executor_watch_hex(size).c_str(), width, height,
                  rt::shims::module_offset(caller).c_str(), executor_watch_hex(job).c_str(),
                  static_cast<unsigned long long>(job > block ? job - block : 0), overlap ? "YES" : "no");
    std::string text = line;
    text += "\n  " + alloc_watch_allocator_text();
    text += "\n  block: " + writer_watch_region(block) + "\n    " + writer_watch_page_mapping(block);
    if (job) text += "\n  job: " + writer_watch_page_mapping(job) + " (executor tid " + std::to_string(job_tid) + ")";
    // Where the arena's bytes came from: an XMemVirtualAlloc answer this shim served, or memory the title's own host
    // calls created. xmem.cpp fills the registry only while this probe switch is set (docs/vertex_buffer_va.md 15).
    const auto served_text = [](const char* what, std::uintptr_t address) {
        std::uint64_t served_caller = 0;
        std::uint8_t served_kind = 0;
        if (rt::xmem::served_range(reinterpret_cast<const void*>(address), &served_caller, &served_kind))
            return std::string("\n  ") + what + " " + executor_watch_hex(address) +
                   " served by XMemVirtualAlloc (kind " + std::to_string(served_kind) + "), title caller " +
                   rt::shims::module_offset(served_caller);
        return std::string("\n  ") + what + " " + executor_watch_hex(address) + " not in a recorded XMem answer";
    };
    text += served_text("block", block);
    if (job) text += served_text("job", job);
    text += served_text("allocator", static_cast<std::uintptr_t>(
                                          executor_watch_read_u64(watch.image_base + kAllocWatchAllocatorGlobalRva)));
    if (block >= 0x80) text += writer_watch_memory_lines("block-before", block - 0x80, 0x80);
    if (block) text += writer_watch_memory_lines("block", block, 0x100);
    if (job) text += writer_watch_memory_lines("job", job, 0x100);
    // The descriptor is freshly allocated (0x68 bytes) and the helper has not written this entry yet, so its bytes are
    // whatever the allocator handed back: recycled bytes here are the allocator-reuse cross-check section 14 lacked.
    if (descriptor) {
        text += writer_watch_memory_lines("descriptor", descriptor, 0x68);
        const auto entries = static_cast<std::uintptr_t>(executor_watch_read_u64(descriptor + 0x38));
        if (entries) text += writer_watch_memory_lines("entry0-before", entries, 0x40);
    }
    (void)param3;
    executor_watch_print(text + "\n");
}

// One 0xa0-byte job allocation from the flush's 0x70-slot call: RAX the returned job, RDI the object it was created
// from. Recorded in the recent-jobs ring so the executor entry can say whether the job it runs came from here.
void alloc_watch_job_hit(CONTEXT& context) {
    AllocWatch& watch = alloc_watch();
    const auto job = static_cast<std::uintptr_t>(context.Rax);
    const auto tag = static_cast<std::uintptr_t>(context.Rdi);
    const auto caller = static_cast<std::uintptr_t>(executor_watch_read_u64(static_cast<std::uintptr_t>(context.Rsp) + 0x78));  // 7 pushes + 0x40
    const unsigned stage = alloc_watch_stage();
    if (job) {
        watch.last_job.store(job);
        watch.last_job_tid.store(GetCurrentThreadId());
    }
    {
        std::lock_guard lock(watch.recent_mutex);
        if (watch.recent_count < 8) {
            watch.recent_jobs[watch.recent_count++] = job;
        } else {
            for (unsigned i = 1; i < 8; ++i) watch.recent_jobs[i - 1] = watch.recent_jobs[i];
            watch.recent_jobs[7] = job;
        }
    }
    const unsigned hit = watch.job_allocs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!job || stage != 3) return;
    if (watch.logged[alloc_watch_job].fetch_add(1, std::memory_order_relaxed) >= kAllocWatchLines) return;
    char line[320];
    std::snprintf(line, sizeof line,
                  "[rt] alloc watch t=%.3f JOB ALLOC hit %u tid %lu job %s size 0xa0 tag %s caller %s stage %u",
                  executor_watch_seconds(), hit, static_cast<unsigned long>(GetCurrentThreadId()),
                  executor_watch_hex(job).c_str(), executor_watch_hex(tag).c_str(),
                  rt::shims::module_offset(caller).c_str(), stage);
    std::string text = line;
    text += "\n  " + alloc_watch_allocator_text();
    text += "\n  job: " + writer_watch_region(job) + "\n    " + writer_watch_page_mapping(job);
    text += writer_watch_memory_lines("job", job, 0x100);
    executor_watch_print(text + "\n");
}

std::string alloc_watch_class_chain(const char* what, std::uintptr_t job);

// One executor entry. Before stage 3 it is only a counter; at the first stage-3 pass it names the job and arms the
// job and block points (and the free point when asked), then logs the job whenever it changes.
void alloc_watch_executor_hit(CONTEXT& context) {
    AllocWatch& watch = alloc_watch();
    const unsigned hit = watch.executor_entries.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto job = static_cast<std::uintptr_t>(context.Rcx);
    const auto caller = static_cast<std::uintptr_t>(executor_watch_read_u64(static_cast<std::uintptr_t>(context.Rsp)));
    const unsigned stage = alloc_watch_stage();
    watch.stage.store(stage);
    if (stage != 3) {
        if (hit <= 3)
            executor_watch_print("[rt] alloc watch: executor entry hit " + std::to_string(hit) + " caller " +
                                 rt::shims::module_offset(caller) + " job " + executor_watch_hex(job) + " stage " +
                                 std::to_string(stage) + " (allocation points not armed)\n");
        return;
    }
    const auto previous = static_cast<std::uintptr_t>(watch.executor_job.load(std::memory_order_relaxed));
    watch.executor_job.store(job);
    watch.executor_tid.store(GetCurrentThreadId());
    const bool first = !watch.stage3_entry.exchange(true);
    bool recent = false;
    {
        std::lock_guard lock(watch.recent_mutex);
        for (unsigned i = 0; i < watch.recent_count; ++i)
            if (watch.recent_jobs[i] == job) recent = true;
    }
    if (recent) watch.job_matches.fetch_add(1, std::memory_order_relaxed);
    if (first) {
        std::string text = "[rt] alloc watch t=" + std::to_string(executor_watch_seconds()) + " STAGE 3 ENTRY hit " +
                           std::to_string(hit) + " tid " + std::to_string(GetCurrentThreadId()) + " job " +
                           executor_watch_hex(job) + " from " + rt::shims::module_offset(caller) + " stage 3";
        text += "\n  " + alloc_watch_allocator_text();
        if (job) {
            text += "\n  job: " + writer_watch_region(job) + "\n    " + writer_watch_page_mapping(job);
            text += writer_watch_memory_lines("job", job, 0x100);
            text += alloc_watch_class_chain("stage-3 entry", job);
        }
        if (job && recent) text += "\n  this job is one of the recently allocated flush jobs";
        const auto allocator = static_cast<std::uintptr_t>(
            executor_watch_read_u64(watch.image_base + kAllocWatchAllocatorGlobalRva));
        text += "\n  allocator page: " + writer_watch_page_mapping(allocator);
        if (watch.free_requested.load(std::memory_order_relaxed)) {
            const bool armed = alloc_watch_install(watch, alloc_watch_free, watch.image_base + kAllocWatchFreeRva);
            text += std::string("\n  free point ") + (armed ? "armed at image+0x515abb0" : "could not be armed");
        }
        const bool job_armed = alloc_watch_install(watch, alloc_watch_job, watch.image_base + kAllocWatchJobAllocRva);
        const bool block_armed = alloc_watch_install(watch, alloc_watch_block, watch.image_base + kAllocWatchBlockAllocRva);
        const bool helper_armed = alloc_watch_install(watch, alloc_watch_helper, watch.image_base + kAllocWatchHelperEntryRva);
        const bool entry_armed = alloc_watch_install(watch, alloc_watch_entry, watch.image_base + kAllocWatchEntryWrittenRva);
        text += std::string("\n  job point ") + (job_armed ? "armed at image+0x184f0c" : "FAILED") + ", block point " +
                (block_armed ? "armed at image+0x455964c" : "FAILED");
        text += std::string("\n  helper entry point ") + (helper_armed ? "armed at image+0x45594f0" : "FAILED") +
                ", entry-written point " + (entry_armed ? "armed at image+0x4559684" : "FAILED");
        executor_watch_print(text + "\n");
        return;
    }
    if (job == previous) return;
    const auto allocator = static_cast<std::uintptr_t>(
        executor_watch_read_u64(watch.image_base + kAllocWatchAllocatorGlobalRva));
    if (watch.logged[alloc_watch_executor].fetch_add(1, std::memory_order_relaxed) >= kAllocWatchLines) return;
    executor_watch_print("[rt] alloc watch t=" + std::to_string(executor_watch_seconds()) + " executor job changed was " +
                         executor_watch_hex(previous) + " now " + executor_watch_hex(job) + " tid " +
                         std::to_string(GetCurrentThreadId()) + " recent flush job " + (recent ? "yes" : "no") +
                         " allocator " + executor_watch_hex(allocator) + " stage " + std::to_string(stage) + "\n");
}

// One free at the title allocator's free entry: only a pointer within 4 KiB of the job or the tracked block can be the
// free that would explain an overlap, so every other call costs the trap and one comparison.
void alloc_watch_free_hit(CONTEXT& context) {
    AllocWatch& watch = alloc_watch();
    const auto pointer = static_cast<std::uintptr_t>(context.Rdx);
    const auto job = static_cast<std::uintptr_t>(watch.executor_job.load(std::memory_order_relaxed));
    const auto block = static_cast<std::uintptr_t>(watch.block.load(std::memory_order_relaxed));
    const bool near_job = job && pointer + 0x1000 >= job && pointer <= job + 0x1000;
    const bool near_block = block && pointer + 0x1000 >= block && pointer <= block + 0x1000;
    if (!near_job && !near_block) return;
    watch.free_matches.fetch_add(1, std::memory_order_relaxed);
    if (watch.logged[alloc_watch_free].fetch_add(1, std::memory_order_relaxed) >= kAllocWatchLines) return;
    std::string text = "[rt] alloc watch t=" + std::to_string(executor_watch_seconds()) + " FREE near " +
                       (near_job ? "job" : "") + (near_job && near_block ? "+" : "") + (near_block ? "block" : "") +
                       ": pointer " + executor_watch_hex(pointer) + " job " + executor_watch_hex(job) + " block " +
                       executor_watch_hex(block) + " tid " + std::to_string(GetCurrentThreadId());
    text += "\n  " + alloc_watch_allocator_text();
    text += "\n  " + writer_watch_page_mapping(pointer);
    text += "\n  " + writer_watch_full_stack(context);
    executor_watch_print(text + "\n");
}

// The class-origin chain, read at the helper's entry or at a stage-3 job: the class ECX is the job's slot-0x28 method
// (game+0x6fecc0), which returns (*(job+0x58))->slot 8; job+0x58 is the format object the job constructor looked up in
// the command object's format cache, keyed by the format the command object returns from its slot 0x20. job+0x30 is
// that command object (its slot 0x40 is the table builder), and job+0x28 is the object the flush stored from its own
// `this+0x78`. Printing each pointer with its vtable, and the vtable slots the decompiled code calls, lets one boot name
// every object of the chain against the static reading; the format object's own bytes are dumped because its slot 8
// is the class the helper receives.
std::string alloc_watch_class_chain(const char* what, std::uintptr_t job) {
    const auto qword = [](std::uintptr_t object, unsigned offset) -> std::uintptr_t {
        return object ? static_cast<std::uintptr_t>(executor_watch_read_u64(object + offset)) : 0;
    };
    const auto object = [](std::uintptr_t value) -> std::uintptr_t { return value; };
    const auto labelled = [](std::uintptr_t address, const char* missing) -> std::string {
        if (!address) return missing;
        return executor_watch_hex(address) + " (" + rt::shims::module_offset(address) + ")";
    };
    const std::uintptr_t job_28 = qword(job, 0x28), job_30 = qword(job, 0x30), job_58 = qword(job, 0x58);
    const std::uintptr_t vt_28 = qword(job_28, 0), vt_30 = qword(job_30, 0), vt_58 = qword(job_58, 0);
    std::string text = std::string("\n  class chain (") + what + "): job " + labelled(job, "0");
    text += "\n    job+0x28 " + labelled(object(job_28), "0") + " vtable " + labelled(vt_28, "0");
    text += "\n    job+0x30 " + labelled(object(job_30), "0") + " vtable " + labelled(vt_30, "0") +
            " slot0x20 " + labelled(vt_30 ? qword(vt_30, 0x20) : 0, "0") +
            " slot0x40 " + labelled(vt_30 ? qword(vt_30, 0x40) : 0, "0");
    text += "\n    job+0x58 " + labelled(object(job_58), "0") + " vtable " + labelled(vt_58, "0") +
            " slot8 " + labelled(vt_58 ? qword(vt_58, 0x8) : 0, "0") +
            " slot0x10 " + labelled(vt_58 ? qword(vt_58, 0x10) : 0, "0");
    if (job_58) text += writer_watch_memory_lines("format-object", job_58, 0x40);
    if (job_30) text += writer_watch_memory_lines("command-object", job_30, 0x80);
    return text;
}

// The constant helper's entry: the class (ECX, the value the job's slot-0x28 method returns), the plane count (EDX),
// the dimensions pointer (R8) and the allocator argument (stack). This is the input side of the size computation whose
// result the block point measures, so the zero size is attributed to either the plane count or the class stride. The
// first four entries also print the class-origin chain (alloc_watch_class_chain) for the executor's current job.
void alloc_watch_helper_hit(CONTEXT& context) {
    AllocWatch& watch = alloc_watch();
    const unsigned hit = watch.hits[alloc_watch_helper].fetch_add(1, std::memory_order_relaxed) + 1;
    const unsigned logged = watch.logged[alloc_watch_helper].fetch_add(1, std::memory_order_relaxed);
    if (logged >= kAllocWatchLines) return;
    const auto rsp = static_cast<std::uintptr_t>(context.Rsp);
    const auto allocator_arg = static_cast<std::uintptr_t>(executor_watch_read_u64(rsp + 0x28));
    const auto dims = static_cast<std::uintptr_t>(context.R8);
    char line[400];
    std::snprintf(line, sizeof line,
                  "[rt] alloc watch t=%.3f HELPER ENTRY hit %u tid %lu class 0x%llx plane-count %llu dims-ptr %s "
                  "allocator-arg %s",
                  executor_watch_seconds(), hit, static_cast<unsigned long>(GetCurrentThreadId()),
                  static_cast<unsigned long long>(context.Rcx & 0xFFFFFFFFu),
                  static_cast<unsigned long long>(context.Rdx & 0xFFFFFFFFu), executor_watch_hex(dims).c_str(),
                  executor_watch_hex(allocator_arg).c_str());
    std::string text = line;
    if (dims && dims < 0x7FFFFFFFFFFFull) {
        text += "\n  dims at " + executor_watch_hex(dims) + ": width " + std::to_string(executor_watch_read_u32(dims)) +
                " height " + std::to_string(executor_watch_read_u32(dims + 4));
    }
    if (logged < 4) {
        const auto job = static_cast<std::uintptr_t>(watch.executor_job.load(std::memory_order_relaxed));
        if (job) text += alloc_watch_class_chain("helper entry", job);
    }
    executor_watch_print(text + "\n");
}

// The instruction after the helper stores its 0x20-byte descriptor entry. RCX is the entry array, RSI the entry offset,
// RBP/R13D the width/height, RAX the row stride, R12 the declared size and RDI the block pointer, so the entry the copy
// later follows is read as written: {width, height, stride, size, address}.
void alloc_watch_entry_hit(CONTEXT& context) {
    AllocWatch& watch = alloc_watch();
    const unsigned hit = watch.hits[alloc_watch_entry].fetch_add(1, std::memory_order_relaxed) + 1;
    const auto entries = static_cast<std::uintptr_t>(context.Rcx);
    const auto offset = static_cast<std::uintptr_t>(context.Rsi);
    const auto stride = static_cast<std::uintptr_t>(context.Rax);
    const auto declared = static_cast<std::uintptr_t>(context.R12);
    const auto block = static_cast<std::uintptr_t>(context.Rdi);
    const auto width = static_cast<unsigned>(context.Rbp & 0xFFFFFFFFu);
    const auto height = static_cast<unsigned>(context.R13 & 0xFFFFFFFFu);
    if (watch.logged[alloc_watch_entry].fetch_add(1, std::memory_order_relaxed) >= kAllocWatchLines) return;
    char line[400];
    std::snprintf(line, sizeof line,
                  "[rt] alloc watch t=%.3f ENTRY WRITTEN hit %u tid %lu entry %s width %u height %u stride %s size %s "
                  "block %s",
                  executor_watch_seconds(), hit, static_cast<unsigned long>(GetCurrentThreadId()),
                  executor_watch_hex(entries + offset).c_str(), width, height, executor_watch_hex(stride).c_str(),
                  executor_watch_hex(declared).c_str(), executor_watch_hex(block).c_str());
    std::string text = line;
    text += "\n  " + alloc_watch_allocator_text();
    executor_watch_print(text + "\n");
}

// The format key at its source: the instruction after the command object's slot-0x20 getter returns (game+0x1847A1).
// RAX is the key the flush then uses as the job's format, RCX the command object the getter ran on (that vtable's slot
// 0x20 is the decompiled `mov eax,0xa; ret` at game+0x23E550) and RSI the object whose slot 0x10 creates the job. This
// is the one value the zero-byte request's class is computed from, measured before the job exists; the command object's
// own +0x38 dims are printed with it so the same boot ties key and 256x1 table together.
void alloc_watch_formatkey_hit(CONTEXT& context) {
    AllocWatch& watch = alloc_watch();
    const unsigned hit = watch.hits[alloc_watch_formatkey].fetch_add(1, std::memory_order_relaxed) + 1;
    const unsigned logged = watch.logged[alloc_watch_formatkey].fetch_add(1, std::memory_order_relaxed);
    if (logged >= kAllocWatchLines) return;
    const auto key = static_cast<unsigned>(context.Rax & 0xFFFFFFFFu);
    const auto command = static_cast<std::uintptr_t>(context.Rcx);
    const auto creator = static_cast<std::uintptr_t>(context.Rsi);
    const auto caller = static_cast<std::uintptr_t>(
        executor_watch_read_u64(static_cast<std::uintptr_t>(context.Rsp) + 0x58));  // 3 pushes + 0x40
    const auto command_vtable = static_cast<std::uintptr_t>(executor_watch_read_u64(command));
    const auto creator_vtable = static_cast<std::uintptr_t>(executor_watch_read_u64(creator));
    char line[400];
    std::snprintf(line, sizeof line,
                  "[rt] alloc watch t=%.3f FORMAT KEY hit %u tid %lu key 0x%x command %s vtable %s creator %s "
                  "vtable %s caller %s",
                  executor_watch_seconds(), hit, static_cast<unsigned long>(GetCurrentThreadId()), key,
                  executor_watch_hex(command).c_str(), rt::shims::module_offset(command_vtable).c_str(),
                  executor_watch_hex(creator).c_str(), rt::shims::module_offset(creator_vtable).c_str(),
                  rt::shims::module_offset(caller).c_str());
    std::string text = line;
    if (command_vtable) {
        const auto slot_20 = static_cast<std::uintptr_t>(executor_watch_read_u64(command_vtable + 0x20));
        text += "\n  command vtable+0x20 " + executor_watch_hex(slot_20) + " (" + rt::shims::module_offset(slot_20) + ")";
    }
    text += "\n  command+0x38 dims width " + std::to_string(executor_watch_read_u32(command + 0x38)) + " height " +
            std::to_string(executor_watch_read_u32(command + 0x3c));
    executor_watch_print(text + "\n");
}

LONG CALLBACK alloc_watch_exception(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    AllocWatch& watch = alloc_watch();
    if (code == EXCEPTION_BREAKPOINT) {
        const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        unsigned slot = kAllocWatchSites;
        for (unsigned i = 0; i < kAllocWatchSites; ++i)
            if (watch.site[i].load(std::memory_order_relaxed) == at) {
                slot = i;
                break;
            }
        if (slot == kAllocWatchSites) return EXCEPTION_CONTINUE_SEARCH;
        if (slot == alloc_watch_executor) alloc_watch_executor_hit(context);
        else if (slot == alloc_watch_job) alloc_watch_job_hit(context);
        else if (slot == alloc_watch_block) alloc_watch_block_hit(context);
        else if (slot == alloc_watch_free) alloc_watch_free_hit(context);
        else if (slot == alloc_watch_helper) alloc_watch_helper_hit(context);
        else if (slot == alloc_watch_entry) alloc_watch_entry_hit(context);
        else alloc_watch_formatkey_hit(context);
        // The original instruction runs, then the trap flag brings us back here to put the int3 back.
        alloc_watch_write_byte(slot, watch.original[slot].load());
        alloc_watch_repoke_slot = slot + 1;
        context.EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP) {
        if (!alloc_watch_repoke_slot) return EXCEPTION_CONTINUE_SEARCH;
        const unsigned slot = alloc_watch_repoke_slot - 1;
        alloc_watch_repoke_slot = 0;
        alloc_watch_write_byte(slot, 0xCC);
        context.EFlags &= ~static_cast<DWORD>(0x100);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Puts the int3 over the executor entry once the machine reads stage 3, then the two allocation points are armed by
// that entry. The patch is deliberately late: the loading phase never traps, so the probe cannot move whether the boot
// reaches stage 3 - the reach is the environmental race sections 10-14 record, and the arm's own reach is reported.
void start_alloc_watch(std::uintptr_t image_base, std::size_t image_size) {
    const char* spec = std::getenv("PALMETTO_ALLOC_WATCH");
    if (!spec || !*spec || *spec == '0') return;
    (void)image_size;
    const char* writer = std::getenv("PALMETTO_WRITER_WATCH");
    if (writer && *writer && *writer != '0') {
        std::fprintf(stderr,
                     "[rt] alloc watch: not armed - PALMETTO_WRITER_WATCH owns the executor entry this probe gates on; "
                     "run the allocation arm with the writer watch off\n");
        std::fflush(stderr);
        return;
    }
    AllocWatch& watch = alloc_watch();
    watch.image_base = image_base;
    const char* free_switch = std::getenv("PALMETTO_ALLOC_WATCH_FREE");
    watch.free_requested.store(free_switch && *free_switch && *free_switch != '0');
    AddVectoredExceptionHandler(1, alloc_watch_exception);
    // The stage-3 gate. A 20 ms poll is fine: stage 3 stays set until the stop, and the first executor entry after the
    // patch arms the job and block points, so the stage-3 pass that crashes is still covered.
    std::thread([] {
        AllocWatch& watch = alloc_watch();
        for (unsigned attempt = 0; attempt < 60000; ++attempt) {
            if (alloc_watch_stage() == 3) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (alloc_watch_stage() != 3) {
            std::fprintf(stderr, "[rt] alloc watch: stage 3 never read; the arm did not reach it, so no site was armed\n");
            std::fflush(stderr);
            return;
        }
        if (watch.armed.exchange(true)) return;
        const bool key_armed = alloc_watch_install(watch, alloc_watch_formatkey, watch.image_base + kAllocWatchFormatKeyRva);
        const bool armed = alloc_watch_install(watch, alloc_watch_executor, watch.image_base + kAllocWatchExecutorRva);
        std::fprintf(stderr,
                     "[rt] alloc watch t=%.3f stage 3 read: %s the format-getter return image+0x%x; %s the executor "
                     "entry image+0x%x; job point image+0x%x, block point image+0x%x, helper entry image+0x%x and "
                     "entry-written image+0x%x arm at its first entry, free point image+0x%x %s\n",
                     executor_watch_seconds(), key_armed ? "int3 over" : "FAILED to arm", kAllocWatchFormatKeyRva,
                     armed ? "int3 over" : "FAILED to arm", kAllocWatchExecutorRva, kAllocWatchJobAllocRva,
                     kAllocWatchBlockAllocRva, kAllocWatchHelperEntryRva, kAllocWatchEntryWrittenRva, kAllocWatchFreeRva,
                     watch.free_requested.load() ? "requested" : "off (PALMETTO_ALLOC_WATCH_FREE unset)");
        std::fflush(stderr);
    }).detach();
    std::fprintf(stderr,
                 "[rt] alloc watch: waiting for machine stage 3 (no int3 before it; stage-gated format-getter return "
                 "point image+0x%x, executor point image+0x%x, then job image+0x%x, block image+0x%x, helper entry "
                 "image+0x%x, entry-written image+0x%x, free image+0x%x %s)\n",
                 kAllocWatchFormatKeyRva, kAllocWatchExecutorRva, kAllocWatchJobAllocRva, kAllocWatchBlockAllocRva,
                 kAllocWatchHelperEntryRva, kAllocWatchEntryWrittenRva, kAllocWatchFreeRva,
                 watch.free_requested.load() ? "requested" : "off (PALMETTO_ALLOC_WATCH_FREE unset)");
    std::fflush(stderr);
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
// The media/UI present body 0x22185A0 signals semaphore A (0x71B9078) at step 5 and semaphore B (0x71B9088) at
// step 10 of the measured sequence; both handles are created by the kernel-object helper 0x5A91C40. Sampling them
// beside the gate/active/flag words says whether the handles exist and whether the body has ever run
// (docs/decomp_first_ui.md section 3 and section 11).
constexpr std::uint32_t kTitleBootMediaSemaphoreARva = 0x71B9078;
constexpr std::uint32_t kTitleBootMediaSemaphoreBRva = 0x71B9088;
constexpr std::uint32_t kTitleStateMachineRva = 0x71273C8;     // state machine object pointer (set at stage-2 entry)
constexpr std::uint32_t kTitlePlatformGateRva = 0xA210D81;     // byte; 1 makes FUN_1421B8960 take its 4 gated waits
constexpr std::uint32_t kTitleLoadingActiveRva = 0x7453EE0;    // byte set by the loading subsystem init
// The movie/Bink chain (docs/decomp_first_ui.md section 13). The middleware module init FUN_14577F500(2) runs from the
// stage-2 boot sequence (call site 0x21B8A64; it sets the ready byte 0x91874B2 = 1 and the player-slot count
// 0x9187418 = 58). The engine frame FUN_142257700 calls the component update FUN_143A1AEB0 while the movie-active guard
// byte 0x91AC9FA reads 0 (it is cleared by the frame init FUN_1422571E0). That update loops the 58 player slots and
// FUN_143A18620 returns from each slot at its first two compares: slot+8 (the entity) must be non-null and slot+0x694
// must be 1, which only the game-side play request FUN_143A16260 -> FUN_143A167E0 binds. Sampling these words says
// whether the module is armed and whether any playback request was ever bound, without an int3 on the per-frame path.
constexpr std::uint32_t kTitleMovieReadyRva = 0x91874B2;        // byte: 1 after the module init, 0 after the stop
constexpr std::uint32_t kTitleMovieUpdateGuardRva = 0x91AC9FA;  // byte: 0 = the frame calls the component update
constexpr std::uint32_t kTitleMoviePlayerCountRva = 0x9187418;  // dword: 58 after the module init
constexpr std::uint32_t kTitleMovieSlot0EntityRva = 0x9165BA0;  // qword: player slot 0 + 8, the request's entity
constexpr std::uint32_t kTitleMovieSlot0ActiveRva = 0x916622C;  // byte: player slot 0 + 0x694, set by the request
constexpr std::uint32_t kTitleMovieSlot0ArmedRva = 0x916618A;   // byte: player slot 0 + 0x5f2, per-frame armed flag
constexpr std::uint32_t kTitleMovieSlot0MediaIdRva = 0x9166060; // dword: player slot 0 + 0x4c8, the request's media id
// The player slot's base RVA (the entity qword above is base+8) and the slot words the four play steps gate on
// (docs/decomp_first_ui.md section 13.1's conditions). The entity's own state byte is at entity+0x1c4: the movie
// middleware's player state machine, 1 created, 2/4 queued, 3 probe retry, 5 metadata read, 6 playback open due,
// 7 Bink open done, 8 frame buffers configuring, 9 playing, 0xa transfer, 0xb error with the code at +0x240.
constexpr std::uint32_t kTitleMovieSlot0BaseRva = 0x9165B98;
// The front-end manager the movie/UI request path runs through (docs/decomp_first_ui.md section 14). The title's
// front-end module init FUN_1422082D0(2) calls FUN_143DA1560 -> FUN_143DA1A80, which creates the manager singleton
// FUN_1419DB790() (0x98 bytes, vtable 0x654CF30) and stores it in the image global 0x98C0F20. The singleton's vtable
// slots 0x28..0x58 are seven service getters returning the seven 8-byte service objects the singleton's slot 0
// (FUN_145AF53B0) allocates at manager+0x60..+0x90; each service has a vtable whose slot +8 is its start. The per-frame
// path FUN_142208B90 -> FUN_140267FD0 calls FUN_1403DB3F0, which calls the manager's slot 0x10 and then every service's
// slot 0x18 (the render step). Sampling the manager and its seven service pointers/vtable words says whether the
// manager was created, whether a service holder is freed (the destroy FUN_143DADD00 zeroes each field after calling the
// service's destructor) or overwritten, without an int3 on the per-frame path.
constexpr std::uint32_t kTitleFrontEndManagerRva = 0x98C0F20;   // qword: the manager singleton pointer
constexpr std::uint32_t kTitleFrontEndReadyRva = 0x98C0E98;     // byte: 1 after the manager's service checks pass
constexpr std::uint32_t kTitleFrontEndContextRva = 0x98C0EA0;   // qword: the context the init was given
constexpr std::uint32_t kTitleFrontEndRunningRva = 0x7453DB8;   // byte: 1 while the front-end frame tick runs
constexpr std::uint32_t kTitleFrontEndService0Offset = 0x60;    // the seven service getters return manager+0x60..+0x90
constexpr unsigned kTitleFrontEndServiceCount = 7;              // +0x60, +0x68, +0x70, +0x78, +0x80, +0x88, +0x90

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

std::uint32_t title_read_u32_at(std::uintptr_t address, bool& ok) {
    ok = title_address_readable(address);
    std::uint32_t value = 0;
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

// The same read for a VA (the heartbeat below is above 4 GiB: image+0x1471B6338 does not fit a 32-bit RVA).
std::uint64_t title_read_u64_at(std::uintptr_t address, bool& ok) {
    ok = title_address_readable(address);
    std::uint64_t value = 0;
    if (ok) std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
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

// ---- Stage-3 pump probe (PALMETTO_STAGE3_PUMP) ---------------------------------------------------------------------
// docs/3d_textures.md section 10.6: with the placed-address fix the boot requests and enters stage 3 and about 29 s
// later the title's own hang watchdog aborts at game+0x2215C35 because its heartbeat at image+0x1471B6338 has not
// advanced through 0x1E one-second passes. The per-frame pump FUN_1453EBE50 runs the transition executor FUN_1453EBFE0,
// whose stage-3 entry calls FUN_140812620: if one of those calls does not return, the thread that pumps the state machine
// never reaches the per-frame tick that pets the heartbeat. The stage-3 second is not stable between boots (66.0 s and
// 106.0 s measured), so this probe watches the state machine's current-stage word (the object at [[image+0x71273C8]],
// current u16 at +0x10) and samples at the named offsets after it first reads 3. The sample unwinds every other
// thread's REAL call chain with RtlVirtualUnwind: the stack-word scans of PALMETTO_WAIT_SAMPLE and
// PALMETTO_THREAD_STACKS pick stale return addresses out of a suspended title thread's stack, so they could not name the
// pump. It also prints the heartbeat's observed change count and age, so "the pump is not ticking" is a measurement, not
// an inference. Off unless the switch names offsets ("2,5,10,20,40"; "1" means that set); a diagnostic only.
// The hang watchdog's heartbeat. Section 10.6 wrote it as `image+0x1471B6338`, which is the Ghidra address of the
// instruction's RIP-relative operand (the image is based at 0x140000000): the image RVA is 0x71B6338, the same
// correction the watchdog's other globals need (the state machine pointer reads 0x1471273C8 = image+0x71273C8). A read
// of image+0x1471B6338 is past the image's mapping, which is how the first probe boot measured the wrong address.
constexpr std::uint64_t kTitleHeartbeatRva = 0x71B6338ull;

// The functions a stage-3 sample tags by name: the pump's entries, the transition executor and the per-stage processors,
// the stage entries, the heartbeat's pet function and the two measured per-frame call sites, and the hang watchdog. The
// ranges are the .pdata extents of those functions.
struct Stage3Tag {
    std::uint32_t begin, end;
    const char* name;
};
constexpr Stage3Tag kStage3Tags[] = {
    {0x53EBE50, 0x53EBE98, "pump"},          {0x53EBEA0, 0x53EBFD3, "pump-loop"},
    {0x53EBFE0, 0x53EC0B4, "transition"},    {0x53EC0F0, 0x53EC1BE, "state-processor"},
    {0x812620, 0x812764, "stage3-entry"},    {0x81B270, 0x81B2FB, "stage2-entry"},
    {0x22159F0, 0x2215A13, "heartbeat-pet"}, {0x120790, 0x1209A8, "frame-tick"},
    {0xC9E350, 0xC9E5D6, "progress-tick"},   {0x2215A60, 0x2215E09, "hang-watchdog"},
    // The stage-3 post-entry path the pump takes (section 11) and the frame/loading path whose wait the probe names:
    // the runtime frame update, its frame pump, the per-frame tick dispatcher and the state-machine wrapper.
    {0x81AFB0, 0x81AFED, "stage3-post"},      {0x1D83960, 0x1D839FA, "session-dispatch"},
    {0x21B7070, 0x21B719A, "session3-poll"},  {0x56A1D54, 0x56A1EC5, "frame-update"},
    {0x5A91BD0, 0x5A91C24, "frame-pump"},     {0x1D109B0, 0x1D109DD, "tick-dispatch"},
    {0x661E0, 0x661FB, "machine-wrapper"},    {0x11FD60, 0x11FDCF, "load-monitor-tick"},
    {0x4FD1B20, 0x4FD227C, "loading-update"}, {0x4FD1AD0, 0x4FD1AF0, "loading-task"},
    {0x23CEFE0, 0x23CF20A, "loading-worker"}, {0x23CF210, 0x23CF3D7, "task-executor"},
};

struct UnwoundThread {
    DWORD tid = 0;
    std::uint64_t start = 0;
    unsigned count = 0;
    std::uint64_t rip = 0, rsp = 0, rcx = 0, rdx = 0, r8 = 0;
    std::uint64_t frames[40] = {};
};

// The call chain a suspended thread is in, from the .pdata unwind info (RtlLookupFunctionEntry/RtlVirtualUnwind). A leaf
// function has no entry, so its return address is the word at RSP. Bounded and fault-tolerant: a bad stack or a frame
// without unwind info ends the walk instead of faulting the process. No C++ objects here, so __try is legal.
unsigned unwind_call_chain(CONTEXT context, std::uint64_t* frames, unsigned cap) {
    unsigned count = 0;
    while (count < cap) {
        if (context.Rip == 0) break;
        __try {
            frames[count++] = context.Rip;
            std::uint64_t function_image_base = 0;
            const PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry((DWORD64)context.Rip, &function_image_base, nullptr);
            if (!entry) {
                std::uint64_t return_address = 0;
                if (!read_thread_word(context.Rsp, return_address)) break;
                context.Rip = return_address;
                context.Rsp += 8;
                continue;
            }
            PVOID handler_data = nullptr;
            DWORD64 establisher_frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, function_image_base, (DWORD64)context.Rip, entry, &context,
                             &handler_data, &establisher_frame, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return count;
}

// The tag names a chain reaches, space-separated (empty when it reaches none).
std::string stage3_chain_tags(const UnwoundThread& thread, std::uintptr_t image_base) {
    std::string tags;
    for (unsigned i = 0; i < thread.count; ++i) {
        if (thread.frames[i] < image_base) continue;
        const auto rva = static_cast<std::uint32_t>(thread.frames[i] - image_base);
        for (const Stage3Tag& tag : kStage3Tags)
            if (rva >= tag.begin && rva < tag.end && tags.find(tag.name) == std::string::npos)
                tags += (tags.empty() ? "" : " ") + std::string(tag.name);
    }
    return tags;
}

// The wait a thread is in, when its deepest frame is a host wait API: the API, the timeout it passes, and the object
// the handle names (type and current state). A chain otherwise says only where it stopped; this says what it waits for,
// which is what separates "parked worker on its own event" from "waiting for a completion nobody owes".
std::string stage3_wait_text(const UnwoundThread& thread, const std::vector<WaitSampleApi>& apis) {
    const char* api = wait_api_name(apis.data(), (unsigned)apis.size(), thread.rip);
    if (!api) return {};
    WaitThreadSample sample;
    sample.tid = thread.tid;
    sample.rip = thread.rip;
    sample.rsp = thread.rsp;
    sample.rcx = thread.rcx;
    sample.rdx = thread.rdx;
    sample.r8 = thread.r8;
    std::string text = std::string(" wait ") + api;
    if (const char* timeout = wait_timeout_text(api, sample)) text += std::string(" timeout ") + timeout;
    if (wait_takes_handle(api) && !std::strstr(api, "WaitForMultipleObjects")) {
        char handle[32], type[64] = {}, state[64] = {};
        std::snprintf(handle, sizeof handle, "0x%llx", static_cast<unsigned long long>(thread.rcx));
        text += std::string(" handle ") + handle;
        wait_object_type((HANDLE)(std::uintptr_t)thread.rcx, type, sizeof(type));
        if (type[0]) text += std::string(" (") + type + ")";
        palmetto::wait_trace::object_state_text((HANDLE)(std::uintptr_t)thread.rcx, type, state, sizeof(state));
        if (state[0]) text += std::string(" state ") + state;
    }
    return text;
}

// One stage-3 sample: every other thread's suspended context unwound into its real call chain, grouped by chain, with
// the machine words and the heartbeat at that instant. The machine fields are current stage +0x10, requested stage
// +0x12, pending request +0x14 and flags +0x16.
void stage3_pump_sample_once(std::uintptr_t image_base, const char* label, double stage3_at,
                             unsigned heartbeat_changes_at_stage3, unsigned heartbeat_changes, double heartbeat_changed_at) {
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
    std::vector<UnwoundThread> threads;
    threads.reserve(tids.size());  // allocated before any thread is suspended
    for (const DWORD tid : tids) {
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!thread) continue;
        if (SuspendThread(thread) == (DWORD)-1) {
            CloseHandle(thread);
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        UnwoundThread sample;
        sample.tid = tid;
        if (GetThreadContext(thread, &context)) {
            if (query_thread) {
                void* start = nullptr;
                if (query_thread(thread, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) >= 0)
                    sample.start = (std::uint64_t)(std::uintptr_t)start;
            }
            sample.rip = context.Rip;
            sample.rsp = context.Rsp;
            sample.rcx = context.Rcx;
            sample.rdx = context.Rdx;
            sample.r8 = context.R8;
            sample.count = unwind_call_chain(context, sample.frames, 40);
        }
        ResumeThread(thread);
        CloseHandle(thread);
        if (sample.count) threads.push_back(sample);
    }
    // Everything below runs with every thread resumed: module_offset() takes the shim registry lock.
    bool ok_machine = false;
    const std::uint64_t machine = title_read_u64(image_base, kTitleStateMachineRva, ok_machine);
    bool ok_current = false, ok_next = false, ok_pending = false, ok_flags = false, ok_heartbeat = false;
    std::uint16_t current = 0, next = 0, pending = 0, flags = 0;
    if (ok_machine && machine && (machine & 7) == 0) {
        current = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x10, ok_current);
        next = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x12, ok_next);
        pending = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x14, ok_pending);
        flags = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x16, ok_flags);
    }
    const std::uint64_t heartbeat = title_read_u64_at(image_base + static_cast<std::uintptr_t>(kTitleHeartbeatRva), ok_heartbeat);
    bool ok_init = false, ok_loading = false, ok_session = false;
    const std::uint32_t init = title_read_u32(image_base, kTitleInitStateRva, ok_init);
    const std::uint32_t loading = title_read_u32(image_base, kTitleLoadingStateRva, ok_loading);
    const std::uint32_t session = title_read_u32(image_base, kTitleSessionStateRva, ok_session);
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - sampler_epoch()).count();
    std::fprintf(stderr,
                 "[rt] stage3 pump at %s (stage 3 at %.1f s, this sample %+.1f s): %zu thread(s); machine current %s%u"
                 " next %s%u pending %s0x%x flags %s0x%x; heartbeat %s0x%llx advances %u (%u since stage 3, last"
                 " %.1f s ago); init %s%u loading %s%u session %s%u\n",
                 label, stage3_at, now - stage3_at, threads.size(), ok_current ? "" : "?", current, ok_next ? "" : "?",
                 next, ok_pending ? "" : "?", pending, ok_flags ? "" : "?", flags, ok_heartbeat ? "" : "?", heartbeat,
                 heartbeat_changes, heartbeat_changes - heartbeat_changes_at_stage3,
                 heartbeat_changed_at > 0.0 ? now - heartbeat_changed_at : -1.0, ok_init ? "" : "?", init,
                 ok_loading ? "" : "?", loading, ok_session ? "" : "?", session);
    std::map<std::string, std::pair<unsigned, std::string>> distinct;
    std::vector<WaitSampleApi> wait_apis;
    resolve_wait_apis(wait_apis);
    std::map<std::string, std::string> distinct_wait;  // the first thread's wait in each chain
    for (const UnwoundThread& thread : threads) {
        std::string signature;
        for (unsigned i = 0; i < thread.count; ++i)
            signature += " " + rt::shims::module_offset(static_cast<std::uintptr_t>(thread.frames[i]));
        auto& slot = distinct[signature];
        ++slot.first;
        if (slot.first <= 8) slot.second += (slot.second.empty() ? "" : ",") + std::to_string(thread.tid);
        if (slot.first == 1) distinct_wait[signature] = stage3_wait_text(thread, wait_apis);
    }
    unsigned shown = 0;
    for (const auto& [signature, slot] : distinct) {
        if (shown++ == 120) {
            std::fprintf(stderr, "[rt] stage3 pump at %s: %u more distinct chain(s) not shown\n", label,
                         static_cast<unsigned>(distinct.size() - shown + 1));
            break;
        }
        std::fprintf(stderr, "[rt] stage3 pump at %s: %ux tid %s%s\n    %s\n", label, slot.first, slot.second.c_str(),
                     distinct_wait[signature].c_str(), signature.c_str());
    }
    // The chains that reach the pump or a per-frame tick are the dissection: name them again with the thread's start
    // address and tag, so a chain is never confused with a task-pool worker that merely shares its stack shape.
    for (const UnwoundThread& thread : threads) {
        const std::string tags = stage3_chain_tags(thread, image_base);
        if (tags.empty()) continue;
        std::string chain;
        for (unsigned i = 0; i < thread.count; ++i)
            chain += " " + rt::shims::module_offset(static_cast<std::uintptr_t>(thread.frames[i]));
        const std::string wait = stage3_wait_text(thread, wait_apis);
        std::fprintf(stderr, "[rt] stage3 pump at %s: tid %lu start %s reaches [%s]%s\n    %s\n", label,
                     static_cast<unsigned long>(thread.tid), rt::shims::module_offset(thread.start).c_str(),
                     tags.c_str(), wait.c_str(), chain.c_str());
    }
}

// Starts the stage-3 pump probe when PALMETTO_STAGE3_PUMP names offsets after the machine word first reads 3.
void start_stage3_pump_probe(std::uintptr_t image_base, const char* spec) {
    std::vector<double> offsets;
    for (const char* at = spec; at && *at;) {
        char* end = nullptr;
        const double value = std::strtod(at, &end);
        if (end == at) break;
        if (value > 0.0 && value < 600.0) offsets.push_back(value);
        at = *end == ',' ? end + 1 : end;
        if (*end == '\0') break;
    }
    if (offsets.empty()) {
        if (std::strcmp(spec, "1") == 0) offsets = {2.0, 5.0, 10.0, 20.0, 40.0};
        else return;
    }
    std::thread([image_base, offsets] {
        const auto started = sampler_epoch();
        auto elapsed = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        };
        // The heartbeat is petted by a per-frame tick; watching it from here dates its stop without a debugger. The
        // poll is 25 ms, so a change is seen within one frame of the store.
        bool heartbeat_seen = false;
        std::uint64_t heartbeat_last = 0;
        unsigned heartbeat_changes = 0;
        double heartbeat_changed_at = 0.0;
        double stage3_at = -1.0;
        unsigned heartbeat_changes_at_stage3 = 0;
        std::size_t next_offset = 0;
        while (elapsed() < 600.0) {
            bool ok_heartbeat = false;
            const std::uint64_t heartbeat = title_read_u64_at(image_base + static_cast<std::uintptr_t>(kTitleHeartbeatRva), ok_heartbeat);
            if (ok_heartbeat && (!heartbeat_seen || heartbeat != heartbeat_last)) {
                if (heartbeat_seen) ++heartbeat_changes;
                heartbeat_last = heartbeat;
                heartbeat_seen = true;
                heartbeat_changed_at = elapsed();
            }
            if (stage3_at < 0.0) {
                bool ok_machine = false;
                const std::uint64_t machine = title_read_u64(image_base, kTitleStateMachineRva, ok_machine);
                bool ok_current = false;
                const std::uint16_t current =
                    (ok_machine && machine && (machine & 7) == 0)
                        ? title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x10, ok_current)
                        : 0;
                if (ok_current && current == 3) {
                    stage3_at = elapsed();
                    heartbeat_changes_at_stage3 = heartbeat_changes;
                    bool ok_next = false, ok_init = false, ok_loading = false, ok_session = false;
                    const std::uint16_t next = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x12, ok_next);
                    const std::uint32_t init = title_read_u32(image_base, kTitleInitStateRva, ok_init);
                    const std::uint32_t loading = title_read_u32(image_base, kTitleLoadingStateRva, ok_loading);
                    const std::uint32_t session = title_read_u32(image_base, kTitleSessionStateRva, ok_session);
                    std::fprintf(stderr,
                                 "[rt] stage3 pump: machine current 3 at %.1f s (next %s%u, init %s%u, loading %s%u,"
                                 " session %s%u, heartbeat 0x%llx after %u advances); sampling at %zu offset(s)\n",
                                 stage3_at, ok_next ? "" : "?", next, ok_init ? "" : "?", init, ok_loading ? "" : "?",
                                 loading, ok_session ? "" : "?", session,
                                 static_cast<unsigned long long>(heartbeat_last), heartbeat_changes, offsets.size());
                }
            } else if (next_offset < offsets.size() && elapsed() - stage3_at >= offsets[next_offset]) {
                char label[48];
                std::snprintf(label, sizeof label, "stage3+%.1f", offsets[next_offset]);
                stage3_pump_sample_once(image_base, label, stage3_at, heartbeat_changes_at_stage3,
                                        heartbeat_changes, heartbeat_changed_at);
                ++next_offset;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }).detach();
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

// ---- Stage-2 dispatch split probe (PALMETTO_STAGE2_DISPATCH) ---------------------------------------------------------
// Section 11 of docs/stage2_blocker.md measured the batch body's dispatch: FUN_1421B8B20 arms tracker+0x608 and tail-calls
// FUN_14509B3E0, which walks the dispatch list at tracker+0x88 (u16 count at +0x90) through FUN_14509B890. That function
// tests each job's class masks and dependency callback, writes job+0xF0 = 2 (the "queued" value the tracker probe saw) and
// appends the job to a run state on the stack; FUN_14509B3E0 then calls FUN_1400560B0 with that run state. FUN_1400560B0
// is the scheduler pump: it claims the lock at queue+0x308 (queue = tracker+0xB8), runs the current work item through
// FUN_144E53470, enqueues a command packet into the queue's 64-slot ring (head dword +0x0, tail dword +0x4, payload
// qwords from +0x108) and drains the ring itself until the queue reports empty (FUN_1455ADA30). The ready set lives in the
// same queue object: capacity +0x1860, count +0x1864, id array +0x1868, bitmask +0x1870, min/max/count at +0x1A90/92/94.
// The registered jobs are the tracker hash table at +0x628 (capacity) / +0x62C (count) / +0x630 (array of {id dword, job
// qword}); the executing job's state byte is +0xE0 and its scheduler counter +0xF0.
//
// This probe dates the split between "queued", "submitted to the queue ring" and "executing". It is off by default; when
// PALMETTO_STAGE2_DISPATCH names a start second it starts one detached thread that samples the tracker global at 1 ms
// from that second, logs every observed change of the armed dword, queue ring head/tail, ready-set count and lock, logs a
// full job census at creation/arming/the wait, and captures every thread's stack once when the wait is entered so the
// threads that should pick the batch up are named. It only reads committed memory (bounds-checked) and never writes title
// state. RVAs, field values and counts only; no game data.
constexpr std::uint32_t kTitleJobPoolGlobalRva = 0x764A8E8;  // global scheduler pool object pointer (FUN_1400560B0)
constexpr std::uint32_t kTitleStage2TrackerJobTableCapacity = 0x628;
constexpr std::uint32_t kTitleStage2TrackerJobTableCount = 0x62C;
constexpr std::uint32_t kTitleStage2TrackerJobTable = 0x630;

struct Stage2QueueView {
    std::uintptr_t pointer = 0;
    std::uint32_t head = 0;
    std::uint32_t tail = 0;
    std::uint32_t lock = 0;
    std::uint32_t ready_capacity = 0;
    std::uint32_t ready_count = 0;
    std::uint32_t ready_min = 0;
    std::uint32_t ready_max = 0;
    std::uint32_t ready_bits = 0;
    std::uint32_t ring_0 = 0;
    std::uint32_t ring_1 = 0;
};

Stage2QueueView stage2_queue_view(std::uintptr_t tracker) {
    Stage2QueueView view;
    view.pointer = stage2_read_qword(tracker + 0xB8);
    if (view.pointer == 0 || (view.pointer & 7) != 0 || !title_address_readable(view.pointer + 0x1870)) return view;
    view.head = stage2_read_u32(view.pointer);
    view.tail = stage2_read_u32(view.pointer + 4);
    view.lock = stage2_read_u32(view.pointer + 0x308);
    view.ready_capacity = stage2_read_u32(view.pointer + 0x1860);
    view.ready_count = stage2_read_u32(view.pointer + 0x1864);
    view.ready_min = stage2_read_u16(view.pointer + 0x1A90);
    view.ready_max = stage2_read_u16(view.pointer + 0x1A92);
    view.ready_bits = stage2_read_u16(view.pointer + 0x1A94);
    for (unsigned slot = 0; slot < 2 && view.tail + slot < view.head; ++slot) {  // 64-slot ring, payload from +0x108
        const std::uint32_t index = (view.tail + slot) & 0x3F;
        const std::uintptr_t payload = view.pointer + 0x108 + static_cast<std::uintptr_t>(index) * 8;
        if (!title_address_readable(payload)) break;
        (slot == 0 ? view.ring_0 : view.ring_1) = stage2_read_u32(payload);
    }
    return view;
}

// One line: the registered jobs' state byte (+0xE0), scheduler counter (+0xF0) and the queue-linked flags (+0xF4/+0xF5/
// +0xF7/+0xF8), aggregated. `started` counts jobs whose state byte is no longer 0.
std::string stage2_job_census(std::uintptr_t tracker) {
    const std::uint32_t capacity = stage2_read_u32(tracker + kTitleStage2TrackerJobTableCapacity);
    const std::uint32_t count = stage2_read_u32(tracker + kTitleStage2TrackerJobTableCount);
    const std::uintptr_t records = stage2_read_qword(tracker + kTitleStage2TrackerJobTable);
    if (count == 0 || count > 128 || capacity == 0 || capacity > 128 || records == 0 ||
        !title_address_readable(records)) {
        char text[64];
        std::snprintf(text, sizeof text, "job table unreadable (cap %u count %u)", capacity, count);
        return text;
    }
    unsigned states[4] = {}, counters[5] = {};
    unsigned started = 0, f4 = 0, f5 = 0, f7 = 0, f8 = 0, visited = 0, unreadable = 0;
    // The hash table's count is the number of used slots, not their prefix: empty slots can sit anywhere in the
    // capacity, so the census walks all of it (the measured bootstrap boot read only a third of the jobs without this).
    for (std::uint32_t i = 0; i < capacity; ++i) {
        const std::uintptr_t job = stage2_read_qword(records + static_cast<std::uintptr_t>(i) * 16 + 8);
        if (job == 0 || (job & 7) != 0 || !title_address_readable(job + 0xF8)) {
            ++unreadable;
            continue;
        }
        const std::uint32_t state = stage2_read_u8(job + 0xE0);
        if (state < 4) ++states[state];
        if (state != 0) ++started;
        const std::int32_t counter = static_cast<std::int32_t>(stage2_read_u32(job + 0xF0));
        if (counter >= 0 && counter < 4) ++counters[counter];
        else ++counters[4];
        if (stage2_read_u8(job + 0xF4) != 0) ++f4;
        if (stage2_read_u8(job + 0xF5) != 0) ++f5;
        if (stage2_read_u8(job + 0xF7) != 0) ++f7;
        if (stage2_read_u8(job + 0xF8) != 0) ++f8;
        if (stage2_read_u8(job + 0x74) != 0) ++visited;
    }
    char text[320];
    std::snprintf(text, sizeof text,
                  "jobs %u: e0[0]=%u e0[1]=%u e0[2]=%u e0[3]=%u started=%u f0[0]=%u f0[1]=%u f0[2]=%u "
                  "f0[3]=%u f0[big]=%u f4=%u f5=%u f7=%u f8=%u +74=%u unreadable=%u",
                  count, states[0], states[1], states[2], states[3], started, counters[0],
                  counters[1], counters[2], counters[3], counters[4], f4, f5, f7, f8, visited, unreadable);
    return text;
}

// Starts the dispatch split probe when PALMETTO_STAGE2_DISPATCH names a start second. Detached; off when unset.
void start_stage2_dispatch_probe(std::uintptr_t image_base, std::uintptr_t image_size, double at_seconds) {
    std::thread([image_base, image_size, at_seconds] {
        const auto started = sampler_epoch();
        auto elapsed = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        };
        std::this_thread::sleep_until(started + std::chrono::milliseconds(static_cast<long long>(at_seconds * 1000.0)));
        std::uintptr_t tracker = 0;
        bool found = false;
        for (int poll = 0; poll < 120000; ++poll) {
            if (title_read_pointer_at(image_base + kTitleStage2TrackerRva, tracker) && tracker != 0 &&
                (tracker & 7) == 0) {
                found = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double created_at = elapsed();
        std::fprintf(stderr, "[rt] stage2 dispatch: tracker %s at %.3f s\n", found ? "created" : "not seen",
                     created_at);
        if (!found) return;
        auto dump_tracker = [&](const char* label) {
            const std::uintptr_t queue = stage2_read_qword(tracker + 0xB8);
            std::fprintf(stderr,
                         "[rt] stage2 dispatch %s +%.3f s: tracker 0x%llx queue 0x%llx vtbl %s b0 0x%x b8 0x%llx "
                         "5e 0x%x 70 0x%x 48 0x%x 50 0x%llx c1 %u armed 0x%x event 0x%llx pool 0x%llx\n",
                         label, elapsed() - created_at, static_cast<unsigned long long>(tracker),
                         static_cast<unsigned long long>(queue),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(tracker)).c_str(),
                         stage2_read_u32(tracker + 0xB0), static_cast<unsigned long long>(queue),
                         stage2_read_u16(tracker + 0x5E), stage2_read_u8(tracker + 0x70),
                         stage2_read_u32(tracker + 0x48), static_cast<unsigned long long>(stage2_read_qword(tracker + 0x50)),
                         stage2_read_u8(tracker + 0xC1), stage2_read_u32(tracker + 0x608),
                         static_cast<unsigned long long>(stage2_read_qword(tracker + 0x610)),
                         static_cast<unsigned long long>(stage2_read_qword(image_base + kTitleJobPoolGlobalRva)));
            const Stage2QueueView view = stage2_queue_view(tracker);
            std::fprintf(stderr,
                         "[rt] stage2 dispatch %s queue: ptr 0x%llx head %u tail %u lock %u ready %u/%u bits %u "
                         "min %u max %u\n",
                         label, static_cast<unsigned long long>(view.pointer), view.head, view.tail, view.lock,
                         view.ready_count, view.ready_capacity, view.ready_bits, view.ready_min, view.ready_max);
            std::fprintf(stderr, "[rt] stage2 dispatch %s census: %s\n", label, stage2_job_census(tracker).c_str());
            if (view.head != view.tail) {
                std::string packets;
                for (unsigned slot = 0; slot < 4 && view.tail + slot < view.head; ++slot) {
                    const std::uint32_t index = (view.tail + slot) & 0x3F;
                    const std::uintptr_t packet = stage2_read_qword(view.pointer + 0x108 +
                                                                    static_cast<std::uintptr_t>(index) * 8);
                    char one[96];
                    std::snprintf(one, sizeof one, " [%u]0x%llx jobs=%u", slot,
                                  static_cast<unsigned long long>(packet), packet ? stage2_read_u32(packet + 4) : 0);
                    packets += one;
                }
                std::fprintf(stderr, "[rt] stage2 dispatch %s ring payload:%s\n", label, packets.c_str());
            }
        };
        dump_tracker("create");
        Stage2QueueView previous = stage2_queue_view(tracker);
        std::uint32_t previous_armed = stage2_read_u32(tracker + 0x608);
        bool armed_seen = previous_armed != 0;
        bool wait_seen = false;
        unsigned tick = 0;
        for (;;) {
            if (!title_address_readable(tracker + 0x608)) {
                std::fprintf(stderr, "[rt] stage2 dispatch: tracker went away at +%.3f s\n", elapsed() - created_at);
                return;
            }
            const std::uint32_t armed = stage2_read_u32(tracker + 0x608);
            const Stage2QueueView view = stage2_queue_view(tracker);
            const bool changed = armed != previous_armed || view.head != previous.head || view.tail != previous.tail ||
                                 view.lock != previous.lock || view.ready_count != previous.ready_count;
            if (changed) {
                std::fprintf(stderr,
                             "[rt] stage2 dispatch +%.3f ms: armed 0x%x ring %u/%u lock %u ready %u/%u bits %u "
                             "min %u max %u\n",
                             (elapsed() - created_at) * 1000.0, armed, view.tail, view.head, view.lock,
                             view.ready_count, view.ready_capacity, view.ready_bits, view.ready_min, view.ready_max);
                previous_armed = armed;
                previous = view;
            }
            if (!armed_seen && armed != 0) {
                armed_seen = true;
                dump_tracker("armed");
            }
            // The wait helper turns the armed 1 into 0x80000001 with a locked add before it blocks (section 2.2).
            if (armed == 0x80000001 || armed == 0x80000000) {
                wait_seen = true;
                dump_tracker("wait");
                const std::uintptr_t event = stage2_read_qword(tracker + 0x610);
                if (event != 0)
                    std::fprintf(stderr,
                                 "[rt] stage2 dispatch wait: event 0x%llx object state before 0x%lx\n",
                                 static_cast<unsigned long long>(event),
                                 static_cast<unsigned long>(WaitForSingleObject(reinterpret_cast<HANDLE>(event), 0)));
                char label[64];
                std::snprintf(label, sizeof label, "dispatch-wait+%.1f", elapsed());
                thread_stacks_sample_once(image_base, image_size, label);
                break;
            }
            if (++tick % 100 == 0 && elapsed() - created_at > 60.0) {
                std::fprintf(stderr, "[rt] stage2 dispatch: no armed dword 60 s after creation\n");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!wait_seen) return;
        // Post-wait: sample the same fields every 20 ms for 3 s to prove whether the ring/ready set/jobs move at all.
        double last_report = 0.0;
        const double wait_at = elapsed();
        for (;;) {
            const double now = elapsed();
            if (!title_address_readable(tracker + 0x608)) break;
            const Stage2QueueView view = stage2_queue_view(tracker);
            if (now - last_report >= 1.0) {
                last_report = now;
                dump_tracker("post");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        if (title_address_readable(tracker + 0x608)) dump_tracker("post2");
        std::fprintf(stderr, "[rt] stage2 dispatch: wait held %.3f s (probe end)\n", elapsed() - wait_at);
    }).detach();
}

// ---- Stage-2 ready-set class-lane probe (PALMETTO_STAGE2_READY) -----------------------------------------------
// Section 12 of docs/stage2_blocker.md measured four job ids ready in the scheduler's ready bitmask that no pump
// dispatches. This probe resolves each ready id through the queue's id->job table (queue+0x1AB0) and prints the
// job's class-lane fields (lane +0x73, class +0x74/+0xC8), dependency state (+0xCA..+0xCD, dependents +0x90/+0x98,
// wait objects +0x72/+0x78), work function (+0x50), executor (+0x68) and the queue's class-lane masks: the
// bit-sliced accounting at queue+0x310..0x11F0 (see the dispatcher FUN_144E53EF0), the published filter at
// +0x1410, the pending aggregate +0x1630 that the ready mark FUN_14003F470 builds, and the per-id request mask at
// queue+0x1AA0 + id*0x220. It also dumps the tracker's registered job table (capacity +0x628, count +0x62C,
// 16-byte records at +0x630: hash, job pointer) so ids can be named by their work functions. It only reads
// committed memory and never writes title state; RVAs, ids, field values and counts only, no game data.
struct Stage2ReadyView {
    std::uintptr_t queue = 0;
    std::uintptr_t job_array = 0;
    std::uintptr_t mask_array = 0;
    std::uint32_t flags_1850 = 0;
    std::uint32_t default_class = 0;
    std::uint32_t ready_capacity = 0;
    std::uint32_t ready_count = 0;
    std::uint32_t ready_min = 0;
    std::uint32_t ready_max = 0;
    std::uint32_t ready_bits = 0;
};

Stage2ReadyView stage2_ready_view(std::uintptr_t queue) {
    Stage2ReadyView view;
    view.queue = queue;
    if (queue == 0 || (queue & 7) != 0 || !title_address_readable(queue + 0x1B38)) return view;
    view.job_array = stage2_read_qword(queue + 0x1AB0);
    view.mask_array = stage2_read_qword(queue + 0x1AA0);
    view.flags_1850 = stage2_read_u16(queue + 0x1850);
    view.default_class = stage2_read_u8(queue + 0x1B38);
    view.ready_capacity = stage2_read_u32(queue + 0x1860);
    view.ready_count = stage2_read_u32(queue + 0x1864);
    view.ready_min = stage2_read_u16(queue + 0x1A90);
    view.ready_max = stage2_read_u16(queue + 0x1A92);
    view.ready_bits = stage2_read_u16(queue + 0x1A94);
    return view;
}

bool stage2_mask_bit(std::uintptr_t base, std::uint32_t id) {
    const std::uintptr_t at = base + static_cast<std::uintptr_t>(id >> 6) * 8;
    return title_address_readable(at) && ((stage2_read_qword(at) >> (id & 0x3F)) & 1) != 0;
}

// Nonzero qwords of one 0x220-byte mask; enough to compare the published filter against the pending aggregate.
void stage2_mask_dump(const char* label, std::uintptr_t base, std::uint32_t bytes) {
    std::string text;
    unsigned nonzero = 0;
    for (std::uint32_t off = 0; off + 8 <= bytes; off += 8) {
        if (!title_address_readable(base + off)) break;
        const std::uint64_t value = stage2_read_qword(base + off);
        if (value == 0) continue;
        ++nonzero;
        if (nonzero <= 40) {
            char one[40];
            std::snprintf(one, sizeof one, " +0x%x=%016llx", off, static_cast<unsigned long long>(value));
            text += one;
        }
    }
    std::fprintf(stderr, "[rt] stage2 ready mask %s: nz %u%s\n", label, nonzero, text.c_str());
}

std::string stage2_ready_ids(std::uintptr_t queue) {
    std::string text;
    unsigned count = 0;
    for (std::uint32_t qword = 0; qword < 64; ++qword) {
        if (!title_address_readable(queue + 0x1870 + qword * 8)) break;
        std::uint64_t bits = stage2_read_qword(queue + 0x1870 + qword * 8);
        while (bits) {
            unsigned long index = 0;
            _BitScanForward64(&index, bits);
            bits &= bits - 1;
            const std::uint32_t id = qword * 64 + index;
            char one[16];
            std::snprintf(one, sizeof one, " %u", id);
            text += one;
            if (++count >= 64) return text;
        }
    }
    return text;
}

void stage2_ready_probe_dump(std::uintptr_t image_base, std::uintptr_t image_size, std::uintptr_t tracker,
                             const char* label, double at_seconds, bool with_table) {
    const std::uintptr_t queue = stage2_read_qword(tracker + 0xB8);
    const Stage2ReadyView view = stage2_ready_view(queue);
    std::fprintf(stderr,
                 "[rt] stage2 ready %s +%.3f s: tracker 0x%llx queue 0x%llx b0 0x%x b4 0x%x 5e 0x%x 70 0x%x "
                 "armed 0x%x flags1850 0x%x dfltclass 0x%x jobs 0x%llx masks 0x%llx ready %u/%u bits %u min %u max %u "
                 "ids%s\n",
                 label, at_seconds, static_cast<unsigned long long>(tracker),
                 static_cast<unsigned long long>(queue), stage2_read_u32(tracker + 0xB0),
                 stage2_read_u32(tracker + 0xB4), stage2_read_u16(tracker + 0x5E), stage2_read_u8(tracker + 0x70),
                 stage2_read_u32(tracker + 0x608), view.flags_1850, view.default_class,
                 static_cast<unsigned long long>(view.job_array),
                 static_cast<unsigned long long>(view.mask_array), view.ready_count, view.ready_capacity,
                 view.ready_bits, view.ready_min, view.ready_max, stage2_ready_ids(queue).c_str());
    if (queue == 0 || view.job_array == 0 || (view.job_array & 7) != 0) return;
    const std::uintptr_t filter = queue + 0x1410;
    const std::uintptr_t aggregate = queue + 0x11F0;
    const std::uintptr_t pending = queue + 0x1630;
    stage2_mask_dump("aggregate(11f0)", aggregate, 0x220);
    stage2_mask_dump("filter(1410)", filter, 0x220);
    stage2_mask_dump("pending(1630)", pending, 0x220);
    for (std::uint32_t lane = 0; lane < 7; ++lane) {
        char name[32];
        std::snprintf(name, sizeof name, "plane%u(%x)", lane, 0x310 + lane * 0x220);
        stage2_mask_dump(name, queue + 0x310 + lane * 0x220, 0x220);
    }
    // Resolve the ready ids: bit position -> job pointer -> class-lane fields and the per-id request mask.
    for (std::uint32_t qword = 0; qword < 64; ++qword) {
        if (!title_address_readable(queue + 0x1870 + qword * 8)) break;
        std::uint64_t bits = stage2_read_qword(queue + 0x1870 + qword * 8);
        while (bits) {
            unsigned long index = 0;
            _BitScanForward64(&index, bits);
            bits &= bits - 1;
            const std::uint32_t id = qword * 64 + index;
            const std::uintptr_t slot = view.job_array + static_cast<std::uintptr_t>(id) * 8;
            if (!title_address_readable(slot)) continue;
            const std::uintptr_t job = stage2_read_qword(slot);
            if (job == 0 || (job & 7) != 0 || !title_address_readable(job + 0xF8)) {
                std::fprintf(stderr, "[rt] stage2 ready id %u: job slot 0x%llx unreadable\n", id,
                             static_cast<unsigned long long>(job));
                continue;
            }
            auto u8 = [&](std::uint32_t rva) { return static_cast<unsigned>(stage2_read_u8(job + rva)); };
            auto u16 = [&](std::uint32_t rva) { return static_cast<unsigned>(stage2_read_u16(job + rva)); };
            auto u32 = [&](std::uint32_t rva) { return static_cast<unsigned long long>(stage2_read_u32(job + rva)); };
            std::fprintf(stderr,
                         "[rt] stage2 ready id %u job 0x%llx hash 0x%x jid %u lane %u class %u c8 %u e0 %u "
                         "e1 %u e2 %u e4 0x%x e8 %s d8 0x%x f0 %d f4 %u f5 %u f7 %u f8 %u "
                         "ca %u cb %u cc %u cd %u work %s item %s exec %s tracker %s deps %u/%s ctx %s %s %s\n",
                         id, static_cast<unsigned long long>(job), stage2_read_u32(job + 0xDC),
                         stage2_read_u16(job + 0xCE), u8(0x73), u8(0x74), u8(0xC8), u8(0xE0), u8(0xE1), u8(0xE2),
                         u16(0xE4),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0xE8)).c_str(),
                         u32(0xD8), static_cast<int>(stage2_read_u32(job + 0xF0)), u8(0xF4), u8(0xF5), u8(0xF7),
                         u8(0xF8), u8(0xCA), u8(0xCB), u8(0xCC), u8(0xCD),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0x50)).c_str(),
                         stage2_pointer_text(image_base, image_size, job + 0x68).c_str(),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0x68)).c_str(),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0x80)).c_str(),
                         u16(0x98),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0x90)).c_str(),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(job)).c_str(),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 8)).c_str(),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0x10)).c_str());
            std::fprintf(stderr,
                         "[rt] stage2 ready id %u bits: ready %u filter %u aggregate %u pending %u plane0 %u "
                         "plane1 %u plane2 %u plane3 %u plane4 %u plane5 %u plane6 %u\n",
                         id, stage2_mask_bit(queue + 0x1870, id), stage2_mask_bit(filter, id),
                         stage2_mask_bit(aggregate, id), stage2_mask_bit(pending, id),
                         stage2_mask_bit(queue + 0x310, id), stage2_mask_bit(queue + 0x530, id),
                         stage2_mask_bit(queue + 0x750, id), stage2_mask_bit(queue + 0x970, id),
                         stage2_mask_bit(queue + 0xB90, id), stage2_mask_bit(queue + 0xDB0, id),
                         stage2_mask_bit(queue + 0xFD0, id));
            // The id's own request mask (what the dispatcher would subtract from the lane counters when it runs).
            if (view.mask_array != 0 && (view.mask_array & 7) == 0) {
                const std::uintptr_t block =
                    view.mask_array + static_cast<std::uintptr_t>(id) * 0x220;
                if (title_address_readable(block + 0x21F)) stage2_mask_dump("id_mask", block, 0x220);
                else std::fprintf(stderr, "[rt] stage2 ready id %u: request mask unreadable at 0x%llx\n", id,
                                  static_cast<unsigned long long>(block));
            }
            // Dependent jobs and wait objects, by id, so the ready set's own graph is visible.
            const std::uint32_t dependents = u16(0x98);
            const std::uintptr_t dependents_array = stage2_read_qword(job + 0x90);
            std::string ids;
            for (std::uint32_t i = 0; i < dependents && i < 32; ++i) {
                const std::uintptr_t at_ptr = dependents_array + static_cast<std::uintptr_t>(i) * 8;
                if (dependents_array == 0 || !title_address_readable(at_ptr)) break;
                const std::uintptr_t dependent = stage2_read_qword(at_ptr);
                if (dependent == 0 || (dependent & 7) != 0 || !title_address_readable(dependent + 0xCE)) continue;
                char one[64];
                std::snprintf(one, sizeof one, " %u(lane%u,e0%u)", stage2_read_u16(dependent + 0xCE),
                              static_cast<unsigned>(stage2_read_u8(dependent + 0x73)),
                              static_cast<unsigned>(stage2_read_u8(dependent + 0xE0)));
                ids += one;
            }
            const std::uint32_t wait_objects = u8(0x72);
            const std::uintptr_t wait_array = stage2_read_qword(job + 0x78);
            std::string waits;
            for (std::uint32_t i = 0; i < wait_objects && i < 8; ++i) {
                const std::uintptr_t at_ptr = wait_array + static_cast<std::uintptr_t>(i) * 8;
                if (wait_array == 0 || !title_address_readable(at_ptr)) break;
                const std::uintptr_t object = stage2_read_qword(at_ptr);
                if (object == 0 || !title_address_readable(object + 0x10)) continue;
                char one[64];
                std::snprintf(one, sizeof one, " 0x%llx(+8 %u)", static_cast<unsigned long long>(object),
                              static_cast<unsigned>(stage2_read_u16(object + 8)));
                waits += one;
            }
            std::fprintf(stderr, "[rt] stage2 ready id %u graph: dependents %u%s waitobjs %u%s\n", id, dependents,
                         ids.c_str(), wait_objects, waits.c_str());
        }
    }
    // The registered job table: hash and pointer per slot. The hash names the factory definition; the work
    // function names the subsystem.
    if (!with_table) return;
    // Which job's claim mask accounts for the aggregate? If exactly one job's mask equals the aggregate, that job was
    // dispatched but its claim was never released (FUN_144E53AD0 runs only at the start of the next run item). Dump
    // the holder's work-item header and children, and the class-lane task streams (the pool the pump posts to:
    // stream object at image+0x71BFD00 + lane*0x40, ring queue pointer at +8, wake counters at +0x14/+0x1C, the
    // queue's class ring at queue + class*0x6008 with 0x800 slots and 16-byte payloads from +0x4010) so a lost item
    // is visible as an occupied ring slot.
    if (view.mask_array != 0 && (view.mask_array & 7) == 0) {
        constexpr std::uint32_t kMaskBytes = 0x220;
        for (std::uint32_t id = 0; id < 256; ++id) {
            const std::uintptr_t block = view.mask_array + static_cast<std::uintptr_t>(id) * kMaskBytes;
            if (!title_address_readable(block + kMaskBytes - 1)) continue;
            bool equal = true, any = false;
            for (std::uint32_t off = 0; off + 8 <= kMaskBytes && equal; off += 8) {
                const std::uint64_t mine = stage2_read_qword(block + off);
                if (mine != 0) any = true;
                if (mine != stage2_read_qword(queue + 0x11F0 + off)) equal = false;
            }
            if (!equal || !any) continue;
            const std::uintptr_t holder = stage2_read_qword(view.job_array + static_cast<std::uintptr_t>(id) * 8);
            if (holder == 0 || !title_address_readable(holder + 0xF8)) continue;
            const std::uintptr_t item = holder + 0x68;
            const std::uint32_t children = stage2_read_u8(item + 0xA);
            std::fprintf(stderr,
                         "[rt] stage2 ready holder id %u job 0x%llx hash 0x%x item exec %s flags8 %u children %u "
                         "lane %u class %u weight %u e0 %u f0 %d\n",
                         id, static_cast<unsigned long long>(holder), stage2_read_u32(holder + 0xDC),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(item)).c_str(),
                         static_cast<unsigned>(stage2_read_u16(item + 8)), children,
                         static_cast<unsigned>(stage2_read_u8(item + 0xB)),
                         static_cast<unsigned>(stage2_read_u8(item + 0xC)),
                         static_cast<unsigned>(stage2_read_u8(item + 0xD)),
                         static_cast<unsigned>(stage2_read_u8(holder + 0xE0)),
                         static_cast<int>(stage2_read_u32(holder + 0xF0)));
            std::string kid_text;
            for (std::uint32_t i = 0; i < children && i < 32; ++i) {
                const std::uintptr_t child = stage2_read_qword(item + 0x10 + static_cast<std::uintptr_t>(i) * 8);
                if (child == 0 || !title_address_readable(child + 0x10)) continue;
                const std::uintptr_t child_job = child - 0x68;
                char one[96];
                if (child_job > 0x1000 && title_address_readable(child_job + 0xE8))
                    std::snprintf(one, sizeof one, " %u(lane%u,class%u,ref%u,weight%u)", stage2_read_u16(child_job + 0xCE),
                                  static_cast<unsigned>(stage2_read_u8(child + 0xB)),
                                  static_cast<unsigned>(stage2_read_u8(child + 0xC)),
                                  static_cast<unsigned>(stage2_read_u16(child + 8)),
                                  static_cast<unsigned>(stage2_read_u8(child + 0xD)));
                else
                    std::snprintf(one, sizeof one, " 0x%llx", static_cast<unsigned long long>(child));
                kid_text += one;
            }
            std::fprintf(stderr, "[rt] stage2 ready holder id %u children:%s\n", id, kid_text.c_str());
        }
        for (std::uint32_t lane = 0; lane < 7; ++lane) {
            const std::uintptr_t stream = image_base + 0x71BFD00 + static_cast<std::uintptr_t>(lane) * 0x40;
            if (!title_address_readable(stream + 0x30)) break;
            const std::uintptr_t ring = stage2_read_qword(stream + 8);
            std::fprintf(stderr,
                         "[rt] stage2 ready pool lane %u: stream 0x%llx ring 0x%llx counts %u/%u/%u/%u pool 0x%llx "
                         "workers %u\n",
                         lane, static_cast<unsigned long long>(stream), static_cast<unsigned long long>(ring),
                         stage2_read_u32(stream + 0x10), stage2_read_u32(stream + 0x14),
                         stage2_read_u32(stream + 0x18), stage2_read_u32(stream + 0x1C),
                         static_cast<unsigned long long>(stage2_read_qword(stream + 0x28)),
                         ring ? stage2_read_u32(ring + 0x18) : 0);
            if (ring == 0 || (ring & 7) != 0 || !title_address_readable(ring + 0x18)) continue;
            std::string classes;
            for (std::uint32_t cls = 0; cls < 8; ++cls) {
                const std::uintptr_t sub = ring + static_cast<std::uintptr_t>(cls) * 0x6008;
                if (!title_address_readable(sub + 0x4010)) break;
                const std::uint32_t head = stage2_read_u32(sub);
                const std::uint32_t slot =
                    stage2_read_u32(sub + 8 + static_cast<std::uintptr_t>(head & 0x7FF) * 4);
                const bool empty = slot == head;
                char one[96];
                std::snprintf(one, sizeof one, " c%u head %u %s", cls, head, empty ? "empty" : "BUSY");
                classes += one;
                if (!empty) {
                    for (std::uint32_t back = 1; back <= 4; ++back) {
                        const std::uint32_t index = (head - back) & 0x7FF;
                        const std::uintptr_t payload =
                            sub + 0x4010 + static_cast<std::uintptr_t>(index) * 16;
                        if (!title_address_readable(payload + 8)) break;
                        const std::uintptr_t queued = stage2_read_qword(payload);
                        if (queued == 0 || !title_address_readable(queued + 0x10)) continue;
                        const std::uintptr_t queued_job = queued - 0x68;
                        if (queued_job > 0x1000 && title_address_readable(queued_job + 0xE8))
                            std::snprintf(one, sizeof one, " item id %u lane %u class %u weight %u",
                                          stage2_read_u16(queued_job + 0xCE), static_cast<unsigned>(stage2_read_u8(queued + 0xB)),
                                          static_cast<unsigned>(stage2_read_u8(queued + 0xC)),
                                          static_cast<unsigned>(stage2_read_u8(queued + 0xD)));
                        else
                            std::snprintf(one, sizeof one, " item 0x%llx", static_cast<unsigned long long>(queued));
                        classes += one;
                    }
                }
            }
            std::fprintf(stderr, "[rt] stage2 ready pool lane %u classes:%s\n", lane, classes.c_str());
        }
    }
    // Print every job's mask bit count and its XOR difference against the aggregate: the closest job is the holder.
    if (view.mask_array != 0 && (view.mask_array & 7) == 0 && view.job_array != 0) {
        constexpr std::uint32_t kMaskBytes = 0x220;
        auto mask_count = [](std::uintptr_t base) {
            unsigned bits = 0;
            for (std::uint32_t off = 0; off + 8 <= kMaskBytes; off += 8) {
                if (!title_address_readable(base + off)) break;
                std::uint64_t value = stage2_read_qword(base + off);
                while (value) {
                    value &= value - 1;
                    ++bits;
                }
            }
            return bits;
        };
        for (std::uint32_t id = 0; id < 256; ++id) {
            const std::uintptr_t slot = view.job_array + static_cast<std::uintptr_t>(id) * 8;
            if (!title_address_readable(slot)) break;
            const std::uintptr_t job = stage2_read_qword(slot);
            if (job == 0 || (job & 7) != 0 || !title_address_readable(job + 0xF8)) continue;
            const std::uintptr_t block = view.mask_array + static_cast<std::uintptr_t>(id) * kMaskBytes;
            unsigned over = 0, under = 0;
            std::string diff;
            if (title_address_readable(block + kMaskBytes - 1)) {
                for (std::uint32_t off = 0; off + 8 <= kMaskBytes; off += 8) {
                    const std::uint64_t mine = stage2_read_qword(block + off);
                    const std::uint64_t aggr = stage2_read_qword(queue + 0x11F0 + off);
                    std::uint64_t extra = mine & ~aggr;
                    std::uint64_t missing = aggr & ~mine;
                    while (extra) {
                        unsigned long bit = 0;
                        _BitScanForward64(&bit, extra);
                        extra &= extra - 1;
                        if (++over <= 24) {
                            char one[16];
                            std::snprintf(one, sizeof one, " +%u", off * 8 + bit);
                            diff += one;
                        }
                    }
                    while (missing) {
                        unsigned long bit = 0;
                        _BitScanForward64(&bit, missing);
                        missing &= missing - 1;
                        ++under;
                    }
                }
            }
            std::fprintf(stderr,
                         "[rt] stage2 ready cand id %u job 0x%llx hash 0x%x lane %u class %u e0 %u f0 %d "
                         "f4 %u f5 %u f7 %u f8 %u maskbits %u xorAggr over %u under %u%s work %s\n",
                         id, static_cast<unsigned long long>(job), stage2_read_u32(job + 0xDC),
                         static_cast<unsigned>(stage2_read_u8(job + 0x73)),
                         static_cast<unsigned>(stage2_read_u8(job + 0x74)),
                         static_cast<unsigned>(stage2_read_u8(job + 0xE0)),
                         static_cast<int>(stage2_read_u32(job + 0xF0)),
                         static_cast<unsigned>(stage2_read_u8(job + 0xF4)),
                         static_cast<unsigned>(stage2_read_u8(job + 0xF5)),
                         static_cast<unsigned>(stage2_read_u8(job + 0xF7)),
                         static_cast<unsigned>(stage2_read_u8(job + 0xF8)),
                         mask_count(block), over, under, diff.c_str(),
                         stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0x50)).c_str());
        }
    }
    const std::uint32_t capacity = stage2_read_u32(tracker + kTitleStage2TrackerJobTableCapacity);
    const std::uint32_t count = stage2_read_u32(tracker + kTitleStage2TrackerJobTableCount);
    const std::uintptr_t records = stage2_read_qword(tracker + kTitleStage2TrackerJobTable);
    std::fprintf(stderr, "[rt] stage2 ready table: cap %u count %u records 0x%llx\n", capacity, count,
                 static_cast<unsigned long long>(records));
    if (records == 0 || capacity > 1024 || !title_address_readable(records)) return;
    for (std::uint32_t i = 0; i < capacity; ++i) {
        const std::uintptr_t record = records + static_cast<std::uintptr_t>(i) * 16;
        if (!title_address_readable(record + 8)) break;
        const std::uint32_t hash = stage2_read_u32(record);
        const std::uintptr_t job = stage2_read_qword(record + 8);
        if (hash == 0 || job == 0 || (job & 7) != 0 || !title_address_readable(job + 0xF8)) continue;
        const std::uint32_t id = stage2_read_u16(job + 0xCE);
        std::fprintf(stderr,
                     "[rt] stage2 ready rec %u hash 0x%x job 0x%llx id %u lane %u class %u c8 %u e0 %u f0 %d "
                     "f8 %u ready %u work %s\n",
                     i, hash, static_cast<unsigned long long>(job), id,
                     static_cast<unsigned>(stage2_read_u8(job + 0x73)),
                     static_cast<unsigned>(stage2_read_u8(job + 0x74)),
                     static_cast<unsigned>(stage2_read_u8(job + 0xC8)),
                     static_cast<unsigned>(stage2_read_u8(job + 0xE0)),
                     static_cast<int>(stage2_read_u32(job + 0xF0)),
                     static_cast<unsigned>(stage2_read_u8(job + 0xF8)), stage2_mask_bit(queue + 0x1870, id),
                     stage2_pointer_text(image_base, image_size, stage2_read_qword(job + 0x50)).c_str());
    }
}

// Starts the ready-set class-lane probe when PALMETTO_STAGE2_READY names a start second. Detached; off when unset.
void start_stage2_ready_probe(std::uintptr_t image_base, std::uintptr_t image_size, double at_seconds) {
    std::thread([image_base, image_size, at_seconds] {
        const auto started = sampler_epoch();
        auto elapsed = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        };
        std::this_thread::sleep_until(started +
                                      std::chrono::milliseconds(static_cast<long long>(at_seconds * 1000.0)));
        std::uintptr_t tracker = 0;
        bool found = false;
        for (int poll = 0; poll < 120000; ++poll) {
            if (title_read_pointer_at(image_base + kTitleStage2TrackerRva, tracker) && tracker != 0 &&
                (tracker & 7) == 0) {
                found = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!found) {
            std::fprintf(stderr, "[rt] stage2 ready: no tracker within 120 s of %.1f s\n", at_seconds);
            return;
        }
        // Section 12's steady state is reached after the dispatch burst and the section-2.1 wait: wait for the armed
        // dword to turn into 0x80000001, then sample the ready set immediately and twice more while it is frozen.
        bool wait_seen = false;
        for (int poll = 0; poll < 180000; ++poll) {
            if (!title_address_readable(tracker + 0x608)) break;
            const std::uint32_t armed = stage2_read_u32(tracker + 0x608);
            if (armed == 0x80000001 || armed == 0x80000000) {
                wait_seen = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!wait_seen)
            std::fprintf(stderr, "[rt] stage2 ready: the wait state was not seen; sampling anyway at %.3f s\n",
                         elapsed());
        stage2_ready_probe_dump(image_base, image_size, tracker, "wait", elapsed(), false);
        std::this_thread::sleep_for(std::chrono::seconds(6));
        if (title_address_readable(tracker + 0xB8)) {
            stage2_ready_probe_dump(image_base, image_size, tracker, "steady", elapsed(), true);
            std::fprintf(stderr, "[rt] stage2 ready: census %s\n", stage2_job_census(tracker).c_str());
        }
        std::this_thread::sleep_for(std::chrono::seconds(12));
        if (title_address_readable(tracker + 0xB8)) {
            stage2_ready_probe_dump(image_base, image_size, tracker, "steady2", elapsed(), false);
            std::fprintf(stderr, "[rt] stage2 ready: census %s\n", stage2_job_census(tracker).c_str());
        }
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
                 ok_companion = false, ok_machine = false, ok_current = false, ok_next = false, ok_sem_a = false,
                 ok_sem_b = false;
            const std::uint32_t boot_media_gate = title_read_u32(image_base, kTitleBootMediaGateRva, ok_gate);
            const std::uint32_t boot_media_active = title_read_u32(image_base, kTitleBootMediaActiveRva, ok_media);
            const std::uint32_t boot_media_flag = title_read_u32(image_base, kTitleBootMediaFlagRva, ok_media_flag);
            const std::uint64_t boot_media_sem_a =
                title_read_u64(image_base, kTitleBootMediaSemaphoreARva, ok_sem_a);
            const std::uint64_t boot_media_sem_b =
                title_read_u64(image_base, kTitleBootMediaSemaphoreBRva, ok_sem_b);
            const std::uint32_t platform_gate = title_read_u32(image_base, kTitlePlatformGateRva, ok_platform);
            const std::uint32_t loading_active = title_read_u32(image_base, kTitleLoadingActiveRva, ok_active);
            const std::uint32_t session_companion = title_read_u32(image_base, kTitleSessionCompanionRva, ok_companion);
            bool ok_movie_ready = false, ok_movie_guard = false, ok_movie_players = false, ok_movie_entity = false,
                 ok_movie_slot_active = false, ok_movie_slot_armed = false, ok_movie_media_id = false;
            const std::uint32_t movie_ready = title_read_u32(image_base, kTitleMovieReadyRva, ok_movie_ready);
            const std::uint32_t movie_guard = title_read_u32(image_base, kTitleMovieUpdateGuardRva, ok_movie_guard);
            const std::uint32_t movie_players = title_read_u32(image_base, kTitleMoviePlayerCountRva, ok_movie_players);
            const std::uint64_t movie_entity = title_read_u64(image_base, kTitleMovieSlot0EntityRva, ok_movie_entity);
            const std::uint32_t movie_slot_active =
                title_read_u32(image_base, kTitleMovieSlot0ActiveRva, ok_movie_slot_active);
            const std::uint32_t movie_slot_armed =
                title_read_u32(image_base, kTitleMovieSlot0ArmedRva, ok_movie_slot_armed);
            const std::uint32_t movie_media_id =
                title_read_u32(image_base, kTitleMovieSlot0MediaIdRva, ok_movie_media_id);
            // The movie player's own state machine and the player-slot gates (docs/decomp_first_ui.md section 15):
            // entity+0x1c4 is the state byte every play step gates on, entity+0x50 the Bink player handle, the
            // +0x1b8/+0x23d/+0x23e/+0x240 bytes the per-frame decode's gate, the metadata probe's result, the
            // playback open's result and the error code the error transition stores; the slot words are the rest
            // of section 13.1's conditions.
            std::uint32_t movie_state = 0, movie_next_state = 0, movie_buffers = 0, movie_probe = 0, movie_open = 0,
                          movie_error = 0;
            std::uint64_t movie_player_handle = 0;
            bool ok_movie_state = false, ok_movie_next_state = false, ok_movie_buffers = false, ok_movie_probe = false,
                 ok_movie_open = false, ok_movie_error = false, ok_movie_player_handle = false;
            if (ok_movie_entity && movie_entity) {
                const auto movie_entity_va = static_cast<std::uintptr_t>(movie_entity);
                movie_state = title_read_u32_at(movie_entity_va + 0x1c4, ok_movie_state) & 0xFF;
                movie_next_state = title_read_u32_at(movie_entity_va + 0x1c5, ok_movie_next_state) & 0xFF;
                movie_buffers = title_read_u32_at(movie_entity_va + 0x1b8, ok_movie_buffers) & 0xFF;
                movie_probe = title_read_u32_at(movie_entity_va + 0x23d, ok_movie_probe) & 0xFF;
                movie_open = title_read_u32_at(movie_entity_va + 0x23e, ok_movie_open) & 0xFF;
                movie_error = title_read_u32_at(movie_entity_va + 0x240, ok_movie_error) & 0xFF;
                movie_player_handle = title_read_u64_at(movie_entity_va + 0x50, ok_movie_player_handle);
            }
            const std::uint32_t kMovieSlotGateRva[6] = {0x5f0, 0x5f1, 0x5fb, 0x515, 0x8ae, 0x5c0};
            std::uint32_t movie_slot_gate[6] = {};
            bool ok_movie_slot_gate[6] = {};
            for (unsigned i = 0; i < 6; ++i)
                movie_slot_gate[i] =
                    title_read_u32(image_base, kTitleMovieSlot0BaseRva + kMovieSlotGateRva[i], ok_movie_slot_gate[i]);
            bool ok_fe_manager = false, ok_fe_ready = false, ok_fe_context = false, ok_fe_running = false;
            const std::uint64_t fe_manager = title_read_u64(image_base, kTitleFrontEndManagerRva, ok_fe_manager);
            const std::uint32_t fe_ready = title_read_u32(image_base, kTitleFrontEndReadyRva, ok_fe_ready);
            const std::uint64_t fe_context = title_read_u64(image_base, kTitleFrontEndContextRva, ok_fe_context);
            const std::uint32_t fe_running = title_read_u32(image_base, kTitleFrontEndRunningRva, ok_fe_running);
            std::uint64_t fe_service[kTitleFrontEndServiceCount] = {};
            std::uint64_t fe_service_vtable[kTitleFrontEndServiceCount] = {};
            for (unsigned i = 0; i < kTitleFrontEndServiceCount; ++i) {
                bool ok_service = false;
                fe_service[i] = title_read_u64_at(
                    static_cast<std::uintptr_t>(fe_manager) + kTitleFrontEndService0Offset + i * 8, ok_service);
                if (fe_service[i]) fe_service_vtable[i] = title_read_u64_at(fe_service[i], ok_service);
            }
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
                         " %s%u, media semaphores %s0x%llx/%s0x%llx\n",
                         at, ok_gate ? "" : "?", boot_media_gate, ok_media ? "" : "?", boot_media_active,
                         ok_media_flag ? "" : "?", boot_media_flag, ok_platform ? "" : "?", platform_gate,
                         ok_active ? "" : "?", loading_active, ok_companion ? "" : "?", session_companion,
                         static_cast<unsigned long long>(state_machine), ok_current ? "" : "?", machine_current,
                         ok_next ? "" : "?", machine_next, ok_sem_a ? "" : "?",
                         static_cast<unsigned long long>(boot_media_sem_a), ok_sem_b ? "" : "?",
                         static_cast<unsigned long long>(boot_media_sem_b));
            // The movie/Bink chain (docs/decomp_first_ui.md section 13): ready byte and slot count prove the middleware
            // module init ran; the update guard proves the engine frame still calls the component update; the slot-0
            // entity/active/armed words prove whether a game-side play request ever bound playback to a slot.
            std::fprintf(stderr,
                         "[rt] state trace at %.1f s: movie ready %s0x%x, update guard %s0x%x, players %s%u,"
                         " slot0 entity %s0x%llx active %s0x%x armed %s0x%x media-id %s0x%x\n",
                         at, ok_movie_ready ? "" : "?", movie_ready & 0xFF, ok_movie_guard ? "" : "?", movie_guard & 0xFF,
                         ok_movie_players ? "" : "?", movie_players, ok_movie_entity ? "" : "?",
                         static_cast<unsigned long long>(movie_entity), ok_movie_slot_active ? "" : "?",
                         movie_slot_active & 0xFF, ok_movie_slot_armed ? "" : "?", movie_slot_armed & 0xFF,
                         ok_movie_media_id ? "" : "?", movie_media_id);
            std::fprintf(stderr,
                         "[rt] state trace at %.1f s: movie player state %s0x%x next %s0x%x handle %s0x%llx"
                         " buffers %s0x%x probe %s0x%x open %s0x%x error %s0x%x\n",
                         at, ok_movie_state ? "" : "?", movie_state, ok_movie_next_state ? "" : "?", movie_next_state,
                         ok_movie_player_handle ? "" : "?",
                         static_cast<unsigned long long>(movie_player_handle), ok_movie_buffers ? "" : "?", movie_buffers,
                         ok_movie_probe ? "" : "?", movie_probe, ok_movie_open ? "" : "?", movie_open,
                         ok_movie_error ? "" : "?", movie_error);
            std::fprintf(stderr,
                         "[rt] state trace at %.1f s: movie slot gates 0x5f0 %s0x%x 0x5f1 %s0x%x 0x5fb %s0x%x"
                         " 0x515 %s0x%x 0x8ae %s0x%x 0x5c0 %s0x%x\n",
                         at, ok_movie_slot_gate[0] ? "" : "?", movie_slot_gate[0],
                         ok_movie_slot_gate[1] ? "" : "?", movie_slot_gate[1],
                         ok_movie_slot_gate[2] ? "" : "?", movie_slot_gate[2],
                         ok_movie_slot_gate[3] ? "" : "?", movie_slot_gate[3],
                         ok_movie_slot_gate[4] ? "" : "?", movie_slot_gate[4],
                         ok_movie_slot_gate[5] ? "" : "?", movie_slot_gate[5]);
            ++sample;
            // The front-end manager the movie/UI request path runs through (docs/decomp_first_ui.md section 14): the
            // singleton pointer proves FUN_143DA1A80 ran; the ready/context/running words prove its service checks and the
            // frame tick; the seven service pointers and their vtable words prove a holder is live, freed (the destroy
            // zeroes the field) or overwritten (its first qword is not the service vtable the constructor wrote).
            std::fprintf(stderr,
                         "[rt] state trace at %.1f s: front-end manager %s0x%llx ready %s0x%x context %s0x%llx running"
                         " %s0x%x services",
                         at, ok_fe_manager ? "" : "?", static_cast<unsigned long long>(fe_manager),
                         ok_fe_ready ? "" : "?", fe_ready & 0xFF, ok_fe_context ? "" : "?",
                         static_cast<unsigned long long>(fe_context), ok_fe_running ? "" : "?", fe_running & 0xFF);
            for (unsigned i = 0; i < kTitleFrontEndServiceCount; ++i)
                std::fprintf(stderr, " %u:0x%llx/0x%llx", i,
                             static_cast<unsigned long long>(fe_service[i]),
                             static_cast<unsigned long long>(fe_service_vtable[i]));
            std::fputc('\n', stderr);
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

// ---- Session-3 loading-record writer watch (PALMETTO_SLOT_WATCH) ----------------------------------------------------
// docs/decomp_stage.md section 10 named this slice as the falsifier for its negative: the session-3 poll 0x21B7070 tests
// the completion dword of the loading record 0x78AC9C8 slot 4 at image+0x78ACB20 (slot field +0x18), that dword stayed
// zero in every sampled window of six boots, and the store that would write it has no static call site because it goes
// through a register. A polling sampler cannot name it; a hardware write watch raises a debug exception after the storing
// instruction executed and the exception context carries the writing thread and its stack.
//
// Windows debug registers are per-thread, so the probe arms every thread of the process (CreateToolhelp32Snapshot +
// SuspendThread/GetThreadContext/SetThreadContext) and keeps arming threads created later. A thread is only armed after
// it has been seen across two scans, the rule the executor watch documents: suspending a thread between its creation and
// its first instruction can leave a suspend count its owner never balances and stall the loader. Four data breakpoints
// exist and all four are used, all as aligned 8-byte write watches:
//
//   DR0  the target completion dword (image+0x78AC9C8 + slot*0x50 + 0x18) - the measurement;
//   DR1  slot 0's tick dword (image+0x78AC9C8), the progress write section 10.3 measured moving during the loading pass:
//        the same-boot live control, proving the watch catches title stores into this record at all;
//   DR2  the target slot's tick dword (image+0x78AC9C8 + slot*0x50): the session-3 poll 0x41E8C0 writes its own tick
//        there, so a fire dates the first poll pass without sampling the state word;
//   DR3  a port-owned control slot written every 50 ms by a probe thread: if that never fires, the debug registers are
//        not honoured in this environment and a silent DR0 is a method failure, not a measurement.
//
// Every line prints the steady clock (the PALMETTO_TIMELINE clock) and the process-relative second (the state trace's
// clock), the three state words with the machine's current/requested pair, the loader object's armed/in-flight fields,
// all five slots' tick/completion/state dwords and the poll index, the bytes leading up to the writer's RIP (the
// breakpoint fires after the store), the registers and the call chain from RtlVirtualUnwind (image frames as game+RVA,
// other modules by name). Bounded: a per-watch log cap, a 96-line total cap and a 10 s counter reporter, so a negative
// boot still shows what the watch saw. RVAs, offsets, values and counts only; no game data.
//
// PALMETTO_SLOT_WATCH=1 watches slot 4 (the poll's slot); =<slot> (2, 3 or 4) overrides it. Off when unset or "0".
struct SlotWatch {
    std::atomic<bool> ready{false};
    std::atomic<std::uintptr_t> completion{0};      // DR0: 8-byte write watch, slot +0x18
    std::atomic<std::uintptr_t> live_control{0};    // DR1: 8-byte write watch, slot 0 tick
    std::atomic<std::uintptr_t> poll_tick{0};       // DR2: 8-byte write watch, the target slot's tick
    std::atomic<std::uintptr_t> method_control{0};  // DR3: 8-byte write watch, the probe's own control slot
    std::atomic<std::uint64_t> completion_last{0}, live_control_last{0}, poll_tick_last{0};
    std::atomic<unsigned> completion_hits{0}, live_control_hits{0}, poll_tick_hits{0}, method_control_hits{0};
    std::atomic<unsigned> lines{0};
    std::atomic<unsigned> configured{0};
};

SlotWatch& slot_watch() {
    static SlotWatch watch;
    return watch;
}

// The method control: a port-owned slot a probe thread writes every 50 ms. If DR3 never fires, data breakpoints are not
// honoured on this machine. Aligned and volatile only; contains no title data.
alignas(8) volatile std::uint64_t slot_watch_control_slot = 0;
std::uintptr_t slot_watch_image_base = 0, slot_watch_image_size = 0;
// The loading/streaming record the session-3 poll reads (docs/decomp_stage.md section 10.1): 0x50-byte slots, entry +0x18
// is the completion dword, entry +0x0 the poll's tick. The poll index at the record's +0x1D8 selects the slot.
constexpr std::uint32_t kTitleLoadingRecordRva = 0x78AC9C8;
constexpr std::uint32_t kTitleLoadingRecordIndexRva = 0x78ACBA0;
constexpr std::uint32_t kTitleLoaderObjectRva = 0x79F6D70;

std::mutex& slot_watch_print_mutex() {
    static std::mutex mutex;
    return mutex;
}

void slot_watch_print(const char* text) {
    std::lock_guard lock(slot_watch_print_mutex());
    std::fwrite(text, 1, std::strlen(text), stderr);
    std::fflush(stderr);
}

const char* slot_watch_kind(unsigned index) {
    switch (index) {
        case 0: return "COMPLETION";
        case 1: return "LIVE-CONTROL";
        case 2: return "POLL-TICK";
        default: return "METHOD-CONTROL";
    }
}

std::uintptr_t slot_watch_address_of(unsigned index) {
    SlotWatch& watch = slot_watch();
    switch (index) {
        case 0: return watch.completion.load(std::memory_order_relaxed);
        case 1: return watch.live_control.load(std::memory_order_relaxed);
        case 2: return watch.poll_tick.load(std::memory_order_relaxed);
        default: return watch.method_control.load(std::memory_order_relaxed);
    }
}

std::atomic<std::uint64_t>& slot_watch_last_of(unsigned index) {
    SlotWatch& watch = slot_watch();
    switch (index) {
        case 0: return watch.completion_last;
        case 1: return watch.live_control_last;
        case 2: return watch.poll_tick_last;
        default: return watch.completion_last;  // the method control is not compared
    }
}

std::atomic<unsigned>& slot_watch_hits_of(unsigned index) {
    SlotWatch& watch = slot_watch();
    switch (index) {
        case 0: return watch.completion_hits;
        case 1: return watch.live_control_hits;
        case 2: return watch.poll_tick_hits;
        default: return watch.method_control_hits;
    }
}

// A label for one code address: image addresses as game+RVA without taking the shim registry lock (the common case in
// this handler), other modules through module_offset() so a host-runtime frame is named rather than guessed.
void slot_watch_label_into(char* out, std::size_t cap, std::uint64_t address) {
    if (address >= slot_watch_image_base && address < slot_watch_image_base + slot_watch_image_size) {
        std::snprintf(out, cap, "game+0x%x", static_cast<unsigned>(address - slot_watch_image_base));
        return;
    }
    const std::string label = rt::shims::module_offset(address);
    std::snprintf(out, cap, "%s", label.c_str());
}

// The writer's real call chain from the .pdata unwind info (RtlLookupFunctionEntry/RtlVirtualUnwind), the same walk the
// stage-3 pump probe uses on suspended threads. Bounded and fault-tolerant inside unwind_call_chain; the frame text is a
// fixed buffer so the handler stays allocation-light.
std::string slot_watch_chain(const CONTEXT& context) {
    std::uint64_t frames[40] = {};
    const unsigned found = unwind_call_chain(context, frames, 40);
    std::string text;
    text.reserve(768);
    unsigned shown = 0;
    for (unsigned i = 0; i < found && shown < 24; ++i) {
        char label[96];
        slot_watch_label_into(label, sizeof label, frames[i]);
        text += ' ';
        text += label;
        ++shown;
    }
    return text.empty() ? std::string(" (none)") : text;
}

// One hardware write-watch hit: the writer's RIP and bytes before it (the breakpoint fires after the store), the value
// now and before, the state words and the record at that instant, and the unwound writer chain.
void slot_watch_hit(unsigned index, CONTEXT& context) {
    static constexpr unsigned kLogCap[4] = {48, 8, 12, 3};
    SlotWatch& watch = slot_watch();
    const std::uintptr_t address = slot_watch_address_of(index);
    const unsigned hit = slot_watch_hits_of(index).fetch_add(1, std::memory_order_relaxed) + 1;
    std::uint64_t now = 0;
    const bool read_ok = executor_watch_read_block(address, &now, 8);
    std::uint64_t previous = 0;
    if (index != 3) previous = slot_watch_last_of(index).exchange(now, std::memory_order_relaxed);
    const bool log = hit <= kLogCap[index] || (hit % 4096 == 0);
    if (!log || watch.lines.fetch_add(1, std::memory_order_relaxed) >= 96) return;

    const double absolute = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double relative =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - sampler_epoch()).count();
    char writer[96];
    slot_watch_label_into(writer, sizeof writer, context.Rip);

    // The state words the record and the session machine read, and the loader object the storage path arms.
    bool ok_init = false, ok_loading = false, ok_session = false, ok_loading_tick = false, ok_machine = false;
    const std::uint32_t init = title_read_u32(slot_watch_image_base, kTitleInitStateRva, ok_init);
    const std::uint32_t loading = title_read_u32(slot_watch_image_base, kTitleLoadingStateRva, ok_loading);
    const std::uint32_t loading_tick = title_read_u32(slot_watch_image_base, kTitleLoadingTickRva, ok_loading_tick);
    const std::uint32_t session = title_read_u32(slot_watch_image_base, kTitleSessionStateRva, ok_session);
    const std::uint64_t machine = title_read_u64(slot_watch_image_base, kTitleStateMachineRva, ok_machine);
    bool ok_current = false, ok_next = false;
    std::uint16_t machine_current = 0, machine_next = 0;
    if (ok_machine && machine && (machine & 7) == 0) {
        machine_current = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x10, ok_current);
        machine_next = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x12, ok_next);
    }
    bool ok_poll_index = false;
    const std::uint32_t poll_index = title_read_u32(slot_watch_image_base, kTitleLoadingRecordIndexRva, ok_poll_index);
    bool ok_loader = false;
    const std::uint64_t loader = title_read_u64(slot_watch_image_base, kTitleLoaderObjectRva, ok_loader);
    std::uint32_t loader_armed = 0, loader_inflight = 0;
    if (ok_loader && loader) {
        loader_armed = executor_watch_read_u32(static_cast<std::uintptr_t>(loader) + 0x78);
        loader_inflight = executor_watch_read_u32(static_cast<std::uintptr_t>(loader) + 0x64);
    }
    unsigned tick[5] = {}, completion[5] = {}, state[5] = {};
    bool slots_ok = true;
    for (unsigned slot = 0; slot < 5; ++slot) {
        bool ok_tick = false, ok_completion = false, ok_state = false;
        const auto base = static_cast<std::uint32_t>(kTitleLoadingRecordRva + slot * 0x50u);
        tick[slot] = title_read_u32(slot_watch_image_base, base, ok_tick);
        completion[slot] = title_read_u32(slot_watch_image_base, base + 0x18, ok_completion);
        state[slot] = title_read_u32(slot_watch_image_base, base + 0x20, ok_state);
        slots_ok = slots_ok && ok_tick && ok_completion && ok_state;
    }

    // The bytes leading up to the writer's RIP name the store instruction without a disassembler.
    char store_bytes[80] = " (unreadable)";
    unsigned char bytes[16] = {};
    if (context.Rip >= 16 && executor_watch_read_block(static_cast<std::uintptr_t>(context.Rip) - 16, bytes, 16)) {
        int at = 0;
        for (unsigned i = 0; i < 16 && at < static_cast<int>(sizeof store_bytes) - 4; ++i)
            at += std::snprintf(store_bytes + at, sizeof store_bytes - at, " %02x", bytes[i]);
    }
    const std::string chain = slot_watch_chain(context);

    char line[4096];
    std::snprintf(line, sizeof line,
                  "[rt] slot watch t=%.3f (%.1f s) %s hit %u tid %lu writer %s value %s0x%llx (was 0x%llx)\n"
                  "  state: init %s%u machine %s%u/%s%u loading %s%u (tick %s%u) session %s%u; poll index %s%u; "
                  "loader 0x%llx armed %u in-flight %u\n"
                  "  record: slots 0..4 tick/comp/state 0:%u/%u/%u 1:%u/%u/%u 2:%u/%u/%u 3:%u/%u/%u 4:%u/%u/%u%s\n"
                  "  bytes before writer rip:%s\n"
                  "  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx r14 %llx r15 %llx\n"
                  "  stack (RtlVirtualUnwind):%s\n",
                  absolute, relative, slot_watch_kind(index), hit, static_cast<unsigned long>(GetCurrentThreadId()),
                  writer, read_ok ? "" : "UNREADABLE ", static_cast<unsigned long long>(now),
                  static_cast<unsigned long long>(previous), ok_init ? "" : "?", init, ok_current ? "" : "?",
                  machine_current, ok_next ? "" : "?", machine_next, ok_loading ? "" : "?", loading,
                  ok_loading_tick ? "" : "?", loading_tick, ok_session ? "" : "?", session, ok_poll_index ? "" : "?",
                  poll_index, static_cast<unsigned long long>(loader), loader_armed, loader_inflight, tick[0],
                  completion[0], state[0], tick[1], completion[1], state[1], tick[2], completion[2], state[2], tick[3],
                  completion[3], state[3], tick[4], completion[4], state[4], slots_ok ? "" : " (partial)",
                  store_bytes, static_cast<unsigned long long>(context.Rax),
                  static_cast<unsigned long long>(context.Rbx), static_cast<unsigned long long>(context.Rcx),
                  static_cast<unsigned long long>(context.Rdx), static_cast<unsigned long long>(context.Rsi),
                  static_cast<unsigned long long>(context.Rdi), static_cast<unsigned long long>(context.R8),
                  static_cast<unsigned long long>(context.R9), static_cast<unsigned long long>(context.R14),
                  static_cast<unsigned long long>(context.R15), chain.c_str());
    slot_watch_print(line);
}

LONG CALLBACK slot_watch_exception(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if ((context.EFlags & 0x100u) != 0) return EXCEPTION_CONTINUE_SEARCH;  // a trap another probe's machinery set
    SlotWatch& watch = slot_watch();
    if (!watch.ready.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD dr6 = context.Dr6;
    if ((dr6 & 0xFu) == 0) return EXCEPTION_CONTINUE_SEARCH;
    context.Dr6 = 0;
    for (unsigned index = 0; index < 4; ++index)
        if ((dr6 & (1u << index)) != 0) slot_watch_hit(index, context);
    return EXCEPTION_CONTINUE_EXECUTION;  // a data breakpoint does not consume the instruction
}

// DR7: four local data breakpoints, RW=01 (write), LEN=11 (8 bytes). An address that is not 8-byte aligned cannot take an
// 8-byte watch and its slot is left disabled (the arm log names it).
DWORD slot_watch_dr7(const std::uintptr_t points[4]) {
    DWORD dr7 = 0;
    for (unsigned index = 0; index < 4; ++index) {
        if (!points[index] || (points[index] & 7u) != 0) continue;
        dr7 |= 0x1u << (2 * index);           // L_i
        dr7 |= 0x1u << (16 + 4 * index);      // RW_i = 01, write
        dr7 |= 0x3u << (18 + 4 * index);      // LEN_i = 11, 8 bytes
    }
    return dr7;
}

// Arms the four data breakpoints in one thread. Called with no title lock held: it suspends the thread for the length of
// one Get/SetThreadContext only.
bool slot_watch_configure_thread(DWORD tid, const std::uintptr_t points[4], DWORD dr7) {
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!thread) return false;
    bool configured = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context)) {
            context.Dr0 = points[0];
            context.Dr1 = points[1];
            context.Dr2 = points[2];
            context.Dr3 = points[3];
            context.Dr6 = 0;
            context.Dr7 = dr7;
            configured = SetThreadContext(thread, &context) != FALSE;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return configured;
}

// The arming sweep: every thread is configured once, after it has been alive across two scans, and threads created
// later are picked up. Fixed arrays only, and no allocation between SuspendThread and ResumeThread (a thread suspended
// inside its own heap allocation holds the process heap lock).
void start_slot_watch_threads() {
    std::thread([] {
        for (;;) {
            ++slot_watch_control_slot;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
    // A bounded reporter, so a watch that never fires still shows the counters in the boot log.
    std::thread([] {
        for (int sample = 1; sample <= 30; ++sample) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            SlotWatch& watch = slot_watch();
            std::fprintf(stderr,
                         "[rt] slot watch at %d s: %u thread(s) armed; completion hits %u (logged), live-control hits %u, "
                         "poll-tick hits %u, method-control hits %u\n",
                         sample * 10, watch.configured.load(std::memory_order_relaxed),
                         watch.completion_hits.load(std::memory_order_relaxed),
                         watch.live_control_hits.load(std::memory_order_relaxed),
                         watch.poll_tick_hits.load(std::memory_order_relaxed),
                         watch.method_control_hits.load(std::memory_order_relaxed));
            std::fflush(stderr);
        }
    }).detach();
    std::thread([] {
        static constexpr unsigned kMaxThreads = 4096, kPending = 512;
        static DWORD configured[kMaxThreads];
        static DWORD seen_tid[kMaxThreads];
        static ULONGLONG seen_at[kMaxThreads];
        unsigned configured_count = 0, seen_count = 0;
        unsigned long loops = 0;
        const auto find = [](const DWORD* table, unsigned count, DWORD tid) -> int {
            for (unsigned i = 0; i < count; ++i)
                if (table[i] == tid) return static_cast<int>(i);
            return -1;
        };
        for (;;) {
            SlotWatch& watch = slot_watch();
            const std::uintptr_t points[4] = {watch.completion.load(std::memory_order_relaxed),
                                              watch.live_control.load(std::memory_order_relaxed),
                                              watch.poll_tick.load(std::memory_order_relaxed),
                                              watch.method_control.load(std::memory_order_relaxed)};
            const DWORD dr7 = slot_watch_dr7(points);
            const ULONGLONG now = GetTickCount64();
            DWORD pending[kPending];
            unsigned pending_count = 0, refused = 0;
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{};
                entry.dwSize = sizeof entry;
                const DWORD pid = GetCurrentProcessId();
                const DWORD self = GetCurrentThreadId();  // never suspend this sweep: a self-suspend never resumes
                if (Thread32First(snapshot, &entry))
                    do {
                        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
                        if (find(configured, configured_count, entry.th32ThreadID) >= 0) continue;
                        const int seat = find(seen_tid, seen_count, entry.th32ThreadID);
                        if (seat < 0) {
                            if (seen_count < kMaxThreads) {
                                seen_tid[seen_count] = entry.th32ThreadID;
                                seen_at[seen_count] = now;
                                ++seen_count;
                            }
                            continue;  // first sight: wait one scan before suspending it
                        }
                        if (now - seen_at[seat] < 1000) continue;
                        if (pending_count < kPending) pending[pending_count++] = entry.th32ThreadID;
                    } while (Thread32Next(snapshot, &entry));
                CloseHandle(snapshot);
            }
            for (unsigned i = 0; i < pending_count; ++i) {
                if (slot_watch_configure_thread(pending[i], points, dr7)) {
                    if (configured_count < kMaxThreads) configured[configured_count++] = pending[i];
                } else {
                    ++refused;
                }
            }
            watch.configured.store(configured_count, std::memory_order_relaxed);
            if (++loops % 600 == 0)
                std::fprintf(stderr, "[rt] slot watch sweep: %u thread(s) armed, %u refused this pass\n",
                             configured_count, refused);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }).detach();
}

// Starts the loading-record writer watch. Measurement only: the breakpoints trap and are continued, and no layer or
// title call changes. `spec` is the PALMETTO_SLOT_WATCH value: "1" is slot 4, 2/3/4 select another slot.
void start_slot_watch(std::uintptr_t image_base, std::size_t image_size, const char* spec) {
    unsigned slot = 4;
    if (!(spec && spec[0] == '1' && spec[1] == '\0')) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(spec ? spec : "", &end, 0);
        if (!spec || end == spec || *end != '\0') {
            std::fprintf(stderr, "[rt] slot watch: PALMETTO_SLOT_WATCH=%s is not a slot index; not armed\n",
                         spec ? spec : "");
            return;
        }
        if (parsed < 2 || parsed > 4) {
            std::fprintf(stderr, "[rt] slot watch: slot %lu is not one of 2..4 (1 means slot 4); not armed\n", parsed);
            return;
        }
        slot = static_cast<unsigned>(parsed);
    }
    const std::uintptr_t record = image_base + kTitleLoadingRecordRva;
    const std::uintptr_t completion = record + static_cast<std::uintptr_t>(slot) * 0x50 + 0x18;
    const std::uintptr_t live_control = record;  // slot 0's tick dword: the loading pass's own progress write
    const std::uintptr_t poll_tick = record + static_cast<std::uintptr_t>(slot) * 0x50;
    const std::uintptr_t method_control = reinterpret_cast<std::uintptr_t>(&slot_watch_control_slot);
    if (!executor_watch_readable(completion, 8) || !executor_watch_readable(live_control, 8) ||
        !executor_watch_readable(poll_tick, 8)) {
        std::fprintf(stderr,
                     "[rt] slot watch: the loading record at game+0x%x is not readable; not armed (image base %p, size "
                     "0x%zx)\n",
                     static_cast<unsigned>(kTitleLoadingRecordRva), reinterpret_cast<void*>(image_base), image_size);
        return;
    }
    SlotWatch& watch = slot_watch();
    slot_watch_image_base = image_base;
    slot_watch_image_size = image_size;
    watch.completion.store(completion, std::memory_order_relaxed);
    watch.live_control.store(live_control, std::memory_order_relaxed);
    watch.poll_tick.store(poll_tick, std::memory_order_relaxed);
    watch.method_control.store(method_control, std::memory_order_relaxed);
    watch.completion_last.store(executor_watch_read_u64(completion), std::memory_order_relaxed);
    watch.live_control_last.store(executor_watch_read_u64(live_control), std::memory_order_relaxed);
    watch.poll_tick_last.store(executor_watch_read_u64(poll_tick), std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[rt] slot watch: slot %u; completion game+0x%x -> 0x%llx (initial 0x%llx), live control game+0x%x "
                 "(initial 0x%llx), poll tick game+0x%x (initial 0x%llx), method control 0x%llx; arming every thread\n",
                 slot, static_cast<unsigned>(completion - image_base),
                 static_cast<unsigned long long>(completion), static_cast<unsigned long long>(watch.completion_last.load()),
                 static_cast<unsigned>(live_control - image_base),
                 static_cast<unsigned long long>(watch.live_control_last.load()),
                 static_cast<unsigned>(poll_tick - image_base),
                 static_cast<unsigned long long>(watch.poll_tick_last.load()),
                 static_cast<unsigned long long>(method_control));
    std::fflush(stderr);
    start_slot_watch_threads();
    AddVectoredExceptionHandler(1, slot_watch_exception);
    watch.ready.store(true, std::memory_order_relaxed);
}

// ---- Session-3 completion dispatch probe (PALMETTO_S3DISPATCH) ------------------------------------------------------
// docs/decomp_stage.md section 12 measured the recording side of the session-3 wall: the loading record's completion
// dword game+0x78ACB20 is written by game+0x5862370, and the only two callers that pass this record are the streaming
// subscriber's completion method game+0x2355D40 (vtable +0x60) and the task callback game+0x27ED3A0, and neither ran in
// two 300 s arms. Section 12's next falsifier is exactly this probe: an execution probe on the vtable +0x60 dispatch,
// on the task callback, on the loading event queue consumer and on the job the subscriber start builds, so one boot
// separates "the awaited streaming job finished and nobody dispatched the completion" from "the job never finished".
//
// The measured static shape this probe reads at run time (docs/decomp_stage.md section 12 plus this slice's decompile):
//   0x2355DD0  subscriber start (vtable +0x68), runs once at the stage-3 transition through the loading update;
//              builds a command block from the event payload's +0x18, calls 0x5BB4DE0, then registers the entry at
//              subscriber+0x80 on the global event bus for event type 0xe (the bus object is *image+0x7F00518);
//   0x5BB4DE0  the start's command execution; 0x1DBC0F0 dispatches the command and submits the async operation whose
//              handle it stores at subscriber+0x58 (the handle's +0x30 is the job-finished marker, 0x5DE07F0's test);
//   0x2355D40  the subscriber completion method (vtable +0x60): would write the record completion dword, unregister the
//              0xe entry and call the media state object's +0x70;
//   0x27ED3A0  the task callback that also writes this record's completion dword (a virtual method, slot +0x48 of its
//              class vtable at game+0x62EC448);
//   0x224CD60  the global event bus raise/post: the event id is *(u32*)(event+8), and the subscriber's registered
//              0x2355EF0 handler is what an id-0xe delivery would run;
//   0x5862370  the completion writer itself, logged with the record and index it is called for (control: section 12
//              measured it live for two other loading records).
//
// Everything is opt-in and bounded: the sites are int3 traps that restore the original byte, single-step it and re-arm
// (the allocation watch's discipline), the raise site logs at most three events per id, the whole log has a line cap,
// and a 2 s sampler prints the subscriber/async/record/state/bus fields so a boot without any hit still shows whether
// the job reached its finished marker. RVAs, offsets, values and counts only; no game data.
constexpr std::uint32_t kS3BusGlobalRva = 0x7F00518;             // qword: the global event bus object
constexpr std::uint32_t kS3WriterRva = 0x5862370;                // the record-completion writer
constexpr unsigned kS3DispatchSiteCount = 12;
constexpr unsigned kS3TotalLines = 600;
constexpr unsigned kS3SamplerLines = 170;
constexpr unsigned kS3FineLines = 150;

struct S3DispatchSite {
    std::uint32_t rva;
    const char* name;
    unsigned cap;  // logged hits per site (the raise site's cap is per event id)
};

constexpr S3DispatchSite kS3DispatchSites[kS3DispatchSiteCount] = {
    {0x2355DD0, "subscriber-start", 4},
    {0x5BB4DE0, "job-exec", 4},
    {0x1DBC0F0, "async-submit", 4},
    {0x2355D40, "subscriber-completion", 8},
    {0x27ED3A0, "task-callback", 8},
    {0x224CD60, "event-bus-raise", 3},
    {0x2355EF0, "subscriber-bus-handler", 8},
    {kS3WriterRva, "completion-writer", 12},
    {0x4043300, "async-request-consume", 8},   // the SDK routine the completion method runs first (0x22FD000 tail-jumps here)
    {0x402F0F0, "owner-attach-request", 4},    // the stream object's pending-request registration at submit
    {0x402F110, "owner-attach-notify", 4},     // its notification step
    {0x1D1B8D0, "async-attach", 4},            // the attach check FUN_145A9FD90 runs
};

struct S3Dispatch {
    std::atomic<bool> ready{false};
    std::atomic<std::uintptr_t> address[kS3DispatchSiteCount]{};
    std::atomic<unsigned> original[kS3DispatchSiteCount]{};
    std::atomic<unsigned> hits[kS3DispatchSiteCount]{};
    std::atomic<unsigned> logged[kS3DispatchSiteCount]{};
    std::atomic<unsigned> lines{0};
    std::atomic<std::uintptr_t> subscriber{0};
    std::atomic<unsigned> raise_total{0};
    std::atomic<unsigned> raise_other{0};
    std::atomic<unsigned> raise_count[0x100]{};
    std::atomic<unsigned> raise_logged[0x100]{};
    std::atomic<unsigned> sampler_lines{0};
    std::atomic<unsigned> fine_lines{0};
};

S3Dispatch& s3_dispatch() {
    static S3Dispatch watch;
    return watch;
}

std::uintptr_t s3_dispatch_image_base = 0;
std::mutex& s3_dispatch_print_mutex() {
    static std::mutex mutex;
    return mutex;
}

void s3_dispatch_print(const std::string& text) {
    std::lock_guard lock(s3_dispatch_print_mutex());
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

// A code/data label for a value: image addresses as game+RVA, other modules by name, anything else in hex.
std::string s3_dispatch_label(std::uint64_t value) {
    if (value >= s3_dispatch_image_base && value < s3_dispatch_image_base + 0x18000000ull) {
        const auto rva = static_cast<std::uint32_t>(value - s3_dispatch_image_base);
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "game+0x%x", rva);
        return buffer;
    }
    return rt::shims::module_offset(static_cast<std::uintptr_t>(value));
}

std::string s3_dispatch_qwords(const char* what, std::uintptr_t address, unsigned count) {
    std::string text = std::string(" ") + what + " " + executor_watch_hex(address) + ":";
    for (unsigned i = 0; i < count; ++i) text += " " + executor_watch_hex(executor_watch_read_u64(address + i * 8));
    return text;
}

std::string s3_dispatch_event_text(std::uintptr_t event) {
    if (!event) return " event 0";
    const std::uint32_t id = executor_watch_read_u32(event + 8);
    const std::uintptr_t payload = static_cast<std::uintptr_t>(executor_watch_read_u64(event + 0x18));
    std::string text = " event " + executor_watch_hex(event) + " vtable " +
                       s3_dispatch_label(executor_watch_read_u64(event)) + " id 0x" +
                       executor_watch_hex(id).substr(2) + " payload " + executor_watch_hex(payload);
    if (payload) {
        text += " payload+8 0x" + executor_watch_hex(executor_watch_read_u32(payload + 8)).substr(2);
        text += s3_dispatch_qwords("payload qwords", payload, 4);
    }
    return text;
}

std::string s3_dispatch_record_text() {
    bool ok = false;
    const std::uint32_t index = title_read_u32(s3_dispatch_image_base, kTitleLoadingRecordIndexRva, ok);
    std::string text = " record index " + std::to_string(index) + " header +198 0x" +
                       executor_watch_hex(title_read_u32(s3_dispatch_image_base, kTitleLoadingRecordRva + 0x198, ok)).substr(2) +
                       " +1a0 " + executor_watch_hex(title_read_u64(s3_dispatch_image_base, kTitleLoadingRecordRva + 0x1a0, ok)) +
                       " +1a8 " + executor_watch_hex(title_read_u64(s3_dispatch_image_base, kTitleLoadingRecordRva + 0x1a8, ok)) +
                       " +1c0 " + executor_watch_hex(title_read_u64(s3_dispatch_image_base, kTitleLoadingRecordRva + 0x1c0, ok));
    text += " slots tick/comp:";
    for (unsigned slot = 0; slot < 5; ++slot) {
        const auto base = static_cast<std::uint32_t>(kTitleLoadingRecordRva + slot * 0x50u);
        text += " " + std::to_string(slot) + " " +
                std::to_string(title_read_u32(s3_dispatch_image_base, base, ok)) + "/" +
                std::to_string(title_read_u32(s3_dispatch_image_base, base + 0x18, ok));
    }
    return text;
}

// The subscriber object the start is called on, and the async handle it submits: the handle's +0x30 is the async
// library's own finished marker (0x5DE07F0 returns *(u64*)(handle+0x30) == 0 ? not-ready : ready), so the sampler can
// tell "the job finished" from "the job never finished" without a hit on any dispatch path.
std::string s3_dispatch_subscriber_text(std::uintptr_t subscriber) {
    if (!subscriber) return " subscriber 0";
    std::string text = " subscriber " + executor_watch_hex(subscriber) + " vtable " +
                       s3_dispatch_label(executor_watch_read_u64(subscriber));
    text += s3_dispatch_qwords("+0x18", subscriber + 0x18, 3);
    text += s3_dispatch_qwords("+0x50", subscriber + 0x50, 2);
    const std::uintptr_t stream = static_cast<std::uintptr_t>(executor_watch_read_u64(subscriber + 0x50));
    const std::uintptr_t handle = static_cast<std::uintptr_t>(executor_watch_read_u64(subscriber + 0x58));
    if (stream) {
        text += " stream vtable " + s3_dispatch_label(executor_watch_read_u64(stream));
        text += s3_dispatch_qwords("stream +0x108", stream + 0x108, 2);
        text += s3_dispatch_qwords("stream pending +0x120", stream + 0x120, 1);
        text += " stream +0xfc 0x" + executor_watch_hex(executor_watch_read_u32(stream + 0xfc)).substr(2) + " +0x100 " +
                executor_watch_hex(executor_watch_read_u64(stream + 0x100));
    }
    text += " handle " + executor_watch_hex(handle);
    if (handle) {
        text += " vtable " + s3_dispatch_label(executor_watch_read_u64(handle)) + " +0x18 0x" +
                executor_watch_hex(executor_watch_read_u32(handle + 0x18)).substr(2) + " +0x30 owner " +
                executor_watch_hex(executor_watch_read_u64(handle + 0x30)) + " callbacks " +
                executor_watch_hex(executor_watch_read_u64(handle + 0x20)) + "/" +
                executor_watch_hex(executor_watch_read_u32(handle + 0x28)) + " key 0x" +
                executor_watch_hex(executor_watch_read_u32(handle + 0x38)).substr(2) + " flag 0x" +
                executor_watch_hex(executor_watch_read_u32(handle + 0x3c)).substr(2);
    }
    text += s3_dispatch_qwords("+0x80 bus-entry", subscriber + 0x80, 1);
    text += s3_dispatch_qwords("+0x88 handler", subscriber + 0x88, 1);
    text += s3_dispatch_qwords("+0x90 links", subscriber + 0x90, 3);
    text += " progress +a8 " + executor_watch_hex(executor_watch_read_u64(subscriber + 0xa8)) + " +ac 0x" +
            executor_watch_hex(executor_watch_read_u32(subscriber + 0xac)).substr(2) + " +b1 0x" +
            executor_watch_hex(executor_watch_read_u32(subscriber + 0xb1)).substr(2);
    return text;
}

std::string s3_dispatch_state_text() {
    bool ok = false;
    const std::uint64_t machine = title_read_u64(s3_dispatch_image_base, kTitleStateMachineRva, ok);
    std::uint16_t current = 0, next = 0;
    if (machine && (machine & 7) == 0) {
        current = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x10, ok);
        next = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x12, ok);
    }
    std::string text = " init " + std::to_string(title_read_u32(s3_dispatch_image_base, kTitleInitStateRva, ok)) +
                       " loading " + std::to_string(title_read_u32(s3_dispatch_image_base, kTitleLoadingStateRva, ok)) +
                       " session " + std::to_string(title_read_u32(s3_dispatch_image_base, kTitleSessionStateRva, ok)) +
                       " machine " + std::to_string(current) + "/" + std::to_string(next);
    const std::uintptr_t bus = static_cast<std::uintptr_t>(title_read_u64(s3_dispatch_image_base, kS3BusGlobalRva, ok));
    text += " bus " + executor_watch_hex(bus);
    if (bus) {
        text += " bus+0x10 " + executor_watch_hex(executor_watch_read_u64(bus + 0x10)) + " bus+0x28 " +
                executor_watch_hex(executor_watch_read_u32(bus + 0x28));
    }
    const std::uintptr_t entity = static_cast<std::uintptr_t>(
        title_read_u64(s3_dispatch_image_base, kTitleMovieSlot0EntityRva, ok));
    text += " movie entity " + executor_watch_hex(entity);
    if (entity) {
        unsigned char state = 0, probe = 0, open = 0;
        executor_watch_read_block(entity + 0x1c4, &state, 1);
        executor_watch_read_block(entity + 0x23d, &probe, 1);
        executor_watch_read_block(entity + 0x23e, &open, 1);
        text += " state 0x" + executor_watch_hex(state).substr(2) + " handle " +
                executor_watch_hex(executor_watch_read_u64(entity + 0x50)) + " buffers " +
                std::to_string(executor_watch_read_u32(entity + 0x1b8)) + " probe 0x" +
                executor_watch_hex(probe).substr(2) + " open 0x" + executor_watch_hex(open).substr(2) + " error 0x" +
                executor_watch_hex(executor_watch_read_u32(entity + 0x240)).substr(2);
    }
    return text;
}

std::string s3_dispatch_raise_summary() {
    S3Dispatch& watch = s3_dispatch();
    std::string text = " raises " + std::to_string(watch.raise_total.load(std::memory_order_relaxed));
    unsigned shown = 0;
    for (unsigned id = 0; id < 0x100 && shown < 12; ++id) {
        const unsigned count = watch.raise_count[id].load(std::memory_order_relaxed);
        if (!count) continue;
        text += " 0x" + executor_watch_hex(id).substr(2) + ":" + std::to_string(count);
        ++shown;
    }
    const unsigned other = watch.raise_other.load(std::memory_order_relaxed);
    if (other) text += " other:" + std::to_string(other);
    return text;
}

std::string s3_dispatch_hits_summary() {
    S3Dispatch& watch = s3_dispatch();
    std::string text = " hits";
    for (unsigned i = 0; i < kS3DispatchSiteCount; ++i)
        text += std::string(" ") + kS3DispatchSites[i].name + " " +
                std::to_string(watch.hits[i].load(std::memory_order_relaxed));
    return text;
}

// One logged hit; the site-specific body reads the arguments the measured call shape passes.
void s3_dispatch_log_hit(unsigned slot, CONTEXT& context) {
    S3Dispatch& watch = s3_dispatch();
    if (watch.lines.fetch_add(1, std::memory_order_relaxed) >= kS3TotalLines) return;
    const std::uintptr_t rcx = static_cast<std::uintptr_t>(context.Rcx);
    const std::uintptr_t rdx = static_cast<std::uintptr_t>(context.Rdx);
    const std::uintptr_t r8 = static_cast<std::uintptr_t>(context.R8);
    char head[192];
    std::snprintf(head, sizeof head, "[rt] s3dispatch t=%.3f site %s hit %u tid %lu rcx 0x%llx rdx 0x%llx r8 0x%llx",
                  executor_watch_seconds(), kS3DispatchSites[slot].name,
                  watch.hits[slot].load(std::memory_order_relaxed), static_cast<unsigned long>(GetCurrentThreadId()),
                  static_cast<unsigned long long>(rcx), static_cast<unsigned long long>(rdx),
                  static_cast<unsigned long long>(r8));
    std::string text = head;
    switch (slot) {
        case 0:  // subscriber start (vtable +0x68): the subscriber object and the event payload it consumed
            watch.subscriber.store(rcx, std::memory_order_relaxed);
            text += s3_dispatch_subscriber_text(rcx) + s3_dispatch_event_text(rdx) + s3_dispatch_record_text();
            break;
        case 1:  // job-exec 0x5BB4DE0: subscriber object + the command block the start built
            text += " subscriber 0x" + executor_watch_hex(rcx).substr(2);
            if (rcx) text += " handle " + executor_watch_hex(executor_watch_read_u64(rcx + 0x58));
            if (rdx) {
                text += s3_dispatch_qwords("block", rdx, 6);
                text += " command-id 0x" + executor_watch_hex(executor_watch_read_u32(rdx + 0x18)).substr(2);
            }
            break;
        case 2:  // async-submit 0x1DBC0F0: command id in edx, flag in r8b, the handle before/after the submission
            text += " subscriber 0x" + executor_watch_hex(rcx).substr(2);
            if (rcx) {
                const std::uintptr_t handle = static_cast<std::uintptr_t>(executor_watch_read_u64(rcx + 0x58));
                text += " stream-object " + executor_watch_hex(executor_watch_read_u64(rcx + 0x50)) + " handle " +
                        executor_watch_hex(handle);
                if (handle)
                    text += " finished " + executor_watch_hex(executor_watch_read_u64(handle + 0x30));
            }
            text += " command-id 0x" + executor_watch_hex(context.Rdx & 0xFFFFFFFFu).substr(2);
            break;
        case 3:  // subscriber completion (vtable +0x60): the method that would write the record's completion dword
            watch.subscriber.store(rcx, std::memory_order_relaxed);
            text += s3_dispatch_subscriber_text(rcx) + s3_dispatch_record_text();
            break;
        case 4:  // task callback 0x27ED3A0: its object and the record completion it would write
            text += " object 0x" + executor_watch_hex(rcx).substr(2);
            if (rcx) text += s3_dispatch_qwords("object", rcx, 8);
            text += s3_dispatch_record_text();
            break;
        case 5: {  // event bus raise (logged by the handler itself; this body is a fallback)
            const std::uintptr_t caller = static_cast<std::uintptr_t>(executor_watch_read_u64(context.Rsp));
            text += s3_dispatch_event_text(rdx) + " caller " + s3_dispatch_label(caller);
            break;
        }
        case 6:  // subscriber's registered 0xe handler: the delivery path, when the bus dispatches an event to it
            text += s3_dispatch_subscriber_text(rcx) + s3_dispatch_event_text(rdx);
            break;
        case 7: {  // the completion writer: record + poll index + the slot dword it would store
            const std::uint32_t index = rdx ? executor_watch_read_u32(rdx) : 0xFFFFFFFFu;
            const std::uint32_t tick = executor_watch_read_u32(rcx + static_cast<std::uintptr_t>(index) * 0x50);
            const std::uint32_t completion =
                executor_watch_read_u32(rcx + 0x18 + static_cast<std::uintptr_t>(index) * 0x50);
            text += " record 0x" + executor_watch_hex(rcx).substr(2) + " index " + std::to_string(index) +
                    " slot-tick " + std::to_string(tick) + " slot-completion " + std::to_string(completion) +
                    " ours " + ((rcx == s3_dispatch_image_base + kTitleLoadingRecordRva) ? "yes" : "no");
            break;
        }
        case 8:  // the SDK async-request consume routine the completion method runs first
            text += " request 0x" + executor_watch_hex(rcx).substr(2) + " object2 0x" + executor_watch_hex(rdx).substr(2);
            if (rcx) {
                text += s3_dispatch_qwords("request", rcx, 7);
                text += " owner " + executor_watch_hex(executor_watch_read_u64(rcx + 0x30)) + " callbacks " +
                        executor_watch_hex(executor_watch_read_u64(rcx + 0x20)) + "/" +
                        executor_watch_hex(executor_watch_read_u32(rcx + 0x28));
            }
            break;
        case 9:  // the stream object's pending-request registration (attach): owner + request + key
            text += " owner 0x" + executor_watch_hex(rcx).substr(2) + " request 0x" +
                    executor_watch_hex(rdx).substr(2) + " key 0x" +
                    executor_watch_hex(r8 & 0xFFFFFFFFu).substr(2);
            if (rcx) text += s3_dispatch_qwords("owner +0xf8", rcx + 0xf8, 6);
            break;
        case 10:  // the attach notification step
            text += " owner 0x" + executor_watch_hex(rcx).substr(2) + " arg 0x" + executor_watch_hex(rdx).substr(2);
            break;
        case 11:  // the attach check inside FUN_145A9FD90
            text += " request 0x" + executor_watch_hex(rcx).substr(2) + " owner 0x" +
                    executor_watch_hex(rdx).substr(2);
            if (rcx) {
                text += s3_dispatch_qwords("request", rcx, 7);
                text += " owner-slot " + executor_watch_hex(executor_watch_read_u64(rcx + 0x30));
            }
            break;
        default:
            break;
    }
    text += "\n  chain:" + slot_watch_chain(context) + "\n";
    s3_dispatch_print(text);
}

void s3_dispatch_write_byte(unsigned slot, unsigned char value) {
    S3Dispatch& watch = s3_dispatch();
    const auto address = watch.address[slot].load(std::memory_order_relaxed);
    if (!address) return;
    DWORD old_protect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *reinterpret_cast<unsigned char*>(address) = value;
        VirtualProtect(reinterpret_cast<void*>(address), 1, old_protect, &old_protect);
    }
}

thread_local unsigned s3_dispatch_repoke_slot = 0;

LONG CALLBACK s3_dispatch_exception(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    S3Dispatch& watch = s3_dispatch();
    if (!watch.ready.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        unsigned slot = kS3DispatchSiteCount;
        for (unsigned i = 0; i < kS3DispatchSiteCount; ++i)
            if (watch.address[i].load(std::memory_order_relaxed) == at) {
                slot = i;
                break;
            }
        if (slot == kS3DispatchSiteCount) return EXCEPTION_CONTINUE_SEARCH;
        const unsigned hit = watch.hits[slot].fetch_add(1, std::memory_order_relaxed) + 1;
        const bool log_this_hit =
            slot == 5 ? (hit <= 512) : (hit <= kS3DispatchSites[slot].cap || (hit % 4096 == 0));
        if (log_this_hit && watch.logged[slot].fetch_add(1, std::memory_order_relaxed) < 512) {
            if (slot != 5) {
                s3_dispatch_log_hit(slot, context);
            } else {
                // The raise site is shared by every event on the bus: three logged lines per id, then counts only.
                const std::uintptr_t event = static_cast<std::uintptr_t>(context.Rdx);
                const std::uint32_t id = executor_watch_read_u32(event + 8);
                const unsigned total = watch.raise_total.fetch_add(1, std::memory_order_relaxed) + 1;
                bool log_raise = false;
                if (id < 0x100) {
                    watch.raise_count[id].fetch_add(1, std::memory_order_relaxed);
                    log_raise = watch.raise_logged[id].fetch_add(1, std::memory_order_relaxed) < 3;
                } else {
                    log_raise = watch.raise_other.fetch_add(1, std::memory_order_relaxed) < 8;
                }
                if (log_raise) {
                    char head[192];
                    std::snprintf(head, sizeof head,
                                  "[rt] s3dispatch t=%.3f site event-bus-raise hit %u (total %u) tid %lu rcx "
                                  "0x%llx rdx 0x%llx r8 0x%llx",
                                  executor_watch_seconds(), hit, total,
                                  static_cast<unsigned long>(GetCurrentThreadId()),
                                  static_cast<unsigned long long>(context.Rcx),
                                  static_cast<unsigned long long>(context.Rdx),
                                  static_cast<unsigned long long>(context.R8));
                    const std::uintptr_t caller = static_cast<std::uintptr_t>(executor_watch_read_u64(context.Rsp));
                    std::string text = head;
                    text += s3_dispatch_event_text(event) + " caller " + s3_dispatch_label(caller) + "\n";
                    s3_dispatch_print(text);
                }
            }
        }
        s3_dispatch_write_byte(slot, static_cast<unsigned char>(watch.original[slot].load(std::memory_order_relaxed)));
        s3_dispatch_repoke_slot = slot + 1;
        context.EFlags |= 0x100;  // trap after the original instruction, then put the int3 back
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        // The trap flag is only raised by this probe's own restore-and-single-step, and the pending slot is
        // thread-local; the saved context does not reliably keep TF set for the trap, so the slot is the test.
        if (!s3_dispatch_repoke_slot) return EXCEPTION_CONTINUE_SEARCH;
        const unsigned slot = s3_dispatch_repoke_slot - 1;
        s3_dispatch_repoke_slot = 0;
        s3_dispatch_write_byte(slot, 0xCC);
        context.EFlags &= ~static_cast<DWORD>(0x100);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Starts the probe: the int3 sites, a 2 s sampler (subscriber/async/record/state/bus) and a 10 s counter reporter.
void start_s3_dispatch_probe(std::uintptr_t image_base, std::size_t image_size) {
    (void)image_size;
    S3Dispatch& watch = s3_dispatch();
    s3_dispatch_image_base = image_base;
    unsigned armed = 0;
    AddVectoredExceptionHandler(1, s3_dispatch_exception);
    watch.ready.store(true, std::memory_order_relaxed);
    std::string list;
    for (unsigned i = 0; i < kS3DispatchSiteCount; ++i) {
        auto* const at = reinterpret_cast<unsigned char*>(image_base + kS3DispatchSites[i].rva);
        DWORD old_protect = 0;
        if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
            list += std::string(" ") + kS3DispatchSites[i].name + "=FAILED";
            continue;
        }
        watch.original[i].store(*at, std::memory_order_relaxed);
        watch.address[i].store(reinterpret_cast<std::uintptr_t>(at), std::memory_order_relaxed);
        *at = 0xCC;
        VirtualProtect(at, 1, old_protect, &old_protect);
        ++armed;
        char item[64];
        std::snprintf(item, sizeof item, " %s=game+0x%x", kS3DispatchSites[i].name, kS3DispatchSites[i].rva);
        list += item;
    }
    std::fprintf(stderr,
                 "[rt] s3dispatch: %u/%u int3 site(s) armed:%s; sampling the subscriber/async record every 2 s\n",
                 armed, kS3DispatchSiteCount, list.c_str());
    std::fflush(stderr);
    std::thread([] {
        for (unsigned tick = 1; tick <= 320; ++tick) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (tick % 2 != 0) continue;
            S3Dispatch& watch = s3_dispatch();
            const std::uintptr_t subscriber = watch.subscriber.load(std::memory_order_relaxed);
            if (!subscriber) continue;
            if (watch.sampler_lines.fetch_add(1, std::memory_order_relaxed) >= kS3SamplerLines) return;
            char head[96];
            std::snprintf(head, sizeof head, "[rt] s3dispatch at %u s:", tick);
            std::string text = head;
            text += s3_dispatch_subscriber_text(subscriber) + s3_dispatch_record_text() + s3_dispatch_state_text() +
                    s3_dispatch_raise_summary() + s3_dispatch_hits_summary() + "\n";
            s3_dispatch_print(text);
        }
    }).detach();
    std::thread([] {
        for (unsigned sample = 1; sample <= 31; ++sample) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            S3Dispatch& watch = s3_dispatch();
            std::fprintf(stderr, "[rt] s3dispatch counters at %u s:%s%s\n", sample * 10,
                         s3_dispatch_raise_summary().c_str(), s3_dispatch_hits_summary().c_str());
            std::fflush(stderr);
            if (watch.raise_total.load(std::memory_order_relaxed) > 100000) return;
        }
    }).detach();
    // The fine change log: after the start captured a subscriber, poll the async request's first 0x40 bytes (the owner
    // slot, the sub-operation count, the callback array/count, the key and the flag) and the stream object's pending
    // slot every 100 ms, and print one line per change. A request that never changes is a job whose completion record
    // never appeared; a request whose fields move is a job that is progressing.
    std::thread([] {
        S3Dispatch& watch = s3_dispatch();
        std::uint64_t last[7] = {};
        std::uintptr_t last_handle = 0, last_stream = 0;
        bool have = false;
        for (unsigned tick = 0; tick < 3100; ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::uintptr_t subscriber = watch.subscriber.load(std::memory_order_relaxed);
            if (!subscriber) continue;
            const std::uintptr_t handle = static_cast<std::uintptr_t>(executor_watch_read_u64(subscriber + 0x58));
            const std::uintptr_t stream = static_cast<std::uintptr_t>(executor_watch_read_u64(subscriber + 0x50));
            if (!handle) continue;
            std::uint64_t now[7] = {};
            for (unsigned i = 0; i < 7; ++i) now[i] = executor_watch_read_u64(handle + i * 8);
            bool changed = !have || handle != last_handle || stream != last_stream;
            for (unsigned i = 0; i < 7; ++i)
                if (now[i] != last[i]) changed = true;
            if (!changed) continue;
            if (watch.fine_lines.fetch_add(1, std::memory_order_relaxed) >= kS3FineLines) return;
            char head[128];
            std::snprintf(head, sizeof head, "[rt] s3dispatch t=%.3f fine-change request 0x%llx stream 0x%llx",
                          executor_watch_seconds(), static_cast<unsigned long long>(handle),
                          static_cast<unsigned long long>(stream));
            std::string text = head;
            text += s3_dispatch_qwords("request", handle, 7);
            text += " owner " + executor_watch_hex(executor_watch_read_u64(handle + 0x30)) + " callbacks " +
                    executor_watch_hex(executor_watch_read_u64(handle + 0x20)) + "/" +
                    executor_watch_hex(executor_watch_read_u32(handle + 0x28));
            if (stream) {
                text += " stream-vtable " + s3_dispatch_label(executor_watch_read_u64(stream));
                text += s3_dispatch_qwords("stream +0x100", stream + 0x100, 5);
            }
            text += "\n";
            s3_dispatch_print(text);
            for (unsigned i = 0; i < 7; ++i) last[i] = now[i];
            last_handle = handle;
            last_stream = stream;
            have = true;
        }
    }).detach();
}

// ---- Streaming request completion-producer probe (PALMETTO_STREAMREAP) ----------------------------------------------
// docs/decomp_stage.md section 13 measured the session-3 wall down to the request: the loading subscriber's start
// submits the streaming subsystem's async request (class vtable game+0x658DE18, key 0x9837FB2E), attaches it to the
// subscriber's stream object (vtable game+0x658DE48) whose pending slot +0x120 holds it, and from then on the request's
// completion fields and the stream's +0x100..+0x120 block never move; the consumer 0x4043300, the subscriber completion
// 0x2355D40, the task callback and the event bus all stay at zero hits. Section 13's next falsifier is exactly this
// probe: the request-class and stream-class vtable slots, the streaming manager's reaping family, the manager global
// game+0x7455F40, and a write watch on the completion fields, with a same-boot control.
//
// The static shape this probe measures, all named from the image's own bytes (RVAs, offsets and values only):
//   0x2355DD0  loading subscriber's start (its vtable +0x68): resets the loading record, runs the event's command
//              (0x5BB4DE0) and registers its id-0xe bus entry; subscriber+0x50 is its stream object, +0x58 its request;
//   0x1DBC0F0  the dispatch inside that start: requires the stream "ready", starts the stream command (0x402F120),
//              asks the subscriber's vtable +0xF0 for the command key, inserts/looks the key up in the global request
//              cache (0x4043570, cache object *game+0x999ABE0) and stores the attached request at subscriber+0x58;
//   0x402F0F0  the attach's pending registration on the stream: stream+0x1c = command id, stream+0x30 = request key,
//              stream+0x120 = request (the measured pending slot);
//   0x402F120  the stream command start: resolves the command handler into stream+0xe8, requires the stream's
//              +0xfc/+0x100 state to be open, then raises the command-start event on the stream queue at +0x128;
//   0x4043390  the async cache-entry completion: checks the entry's owner (+0x30), resolves it, requires the stream
//              state open, builds the completion record and raises the completion event on the stream queue +0x128.
//              Its four callers below are the only paths that can complete a cache entry, so the request finishes iff
//              one of them reaches this function for it;
//   0x4042D560/0x4042D580  the cache key builder and the hash lookup (buckets at cache+0x348, count +0x350, entries
//              +0x358); the node layout is {key +0x0, request +0x8, next +0x10};
//   0x234A870  the streaming manager's job reap (class vtable game+0x61B4170 slot +0x40): looks the job's async handle
//              (+0x40) up in the request cache and completes the entry with the job's +0x48 status;
//   0x131C480, 0x2329110, 0x230BB90  the other three cache-entry completion callers (command, queue and manager event
//              paths of the same async framework);
//   0x143065AF0/0x140864100  the stream event-queue raises used by the completion and by the command start;
//   0x4043300  the request consume (walks callbacks +0x20/+0x28, decrements the +0x18 sub-op count, clears the owner
//              +0x30 and the +0x3c flag): the measured-zero consumer of section 13;
//   0x2355D40  the subscriber completion that would write the loading record's completion dword;
//   0x2306070  the streaming stop, which would explain a request being released instead of completed.
// The write watch (PALMETTO_STREAMREAP_WATCH=1) arms when the attach registers the request: DR0 request+0x18, DR1
// request+0x20, DR2 stream+0x120, and DR3 the loading record's slot-4 tick game+0x78ACB08, which the session-3 poll
// 0x41E8C0 writes every 33 ms - the same-boot live control that proves the debug registers catch title stores here.
// Everything is opt-in and bounded (per-site caps, a total line cap, a 100 ms fine-change log and 10 s counters), and
// with the switch unset there is one cached getenv, no handler, no thread and no patch. RVAs, offsets, values only.
constexpr std::uint32_t kSrManagerGlobalRva = 0x7455F40;       // streaming manager object base (vtable pointer at +0)
constexpr std::uint32_t kSrRequestCacheGlobalRva = 0x999ABE0;  // qword: the async request-cache object
constexpr std::uint32_t kSrRecordSlot4TickRva = 0x78ACB08;     // live control: the session-3 poll's own tick store
constexpr unsigned kSrSiteCount = 20;
constexpr unsigned kSrTotalLines = 900;
constexpr unsigned kSrSamplerLines = 150;
constexpr unsigned kSrFineLines = 140;
constexpr unsigned kSrWatchLines = 120;

struct SrSite {
    std::uint32_t rva;
    const char* name;
    unsigned cap;
};

constexpr SrSite kSrSites[kSrSiteCount] = {
    {0x2355DD0, "subscriber-start", 4},
    {0x1DBC0F0, "stream-submit", 4},
    {0x402F0F0, "stream-pending-register", 4},
    {0x402F120, "stream-command-start", 6},
    {0x4043390, "cache-complete", 10},
    {0x402D580, "cache-lookup", 10},
    {0x402D560, "cache-key", 6},
    {0x234A870, "job-reap", 10},
    {0x131C480, "async-cmd-handler", 6},
    {0x2329110, "async-complete-handler", 6},
    {0x230BB90, "manager-complete", 6},
    {0x3065AF0, "stream-event-raise", 10},
    {0x864100, "stream-start-event", 6},
    {0x4043300, "request-consume", 8},
    {0x2355D40, "subscriber-completion", 8},
    {0x2306070, "streaming-stop", 6},
    {0x45E570, "stream-pump", 6},
    {0x402E260, "stream-step", 6},
    {0x402E340, "async-engine-step", 8},
    {0x45E630, "stream-open-check", 8},
};

struct StreamReap {
    std::atomic<bool> ready{false};
    std::atomic<std::uintptr_t> address[kSrSiteCount]{};
    std::atomic<unsigned> original[kSrSiteCount]{};
    std::atomic<unsigned> hits[kSrSiteCount]{};
    std::atomic<unsigned> logged[kSrSiteCount]{};
    std::atomic<unsigned> lines{0};
    std::atomic<std::uintptr_t> subscriber{0};
    std::atomic<std::uintptr_t> request{0};
    std::atomic<std::uintptr_t> stream{0};
    std::atomic<std::uint32_t> request_key{0};
    std::atomic<unsigned> sampler_lines{0};
    std::atomic<unsigned> fine_lines{0};
    // The hardware write watch on the measured request/stream, armed by the attach registration.
    std::atomic<bool> watch_enabled{false};
    std::atomic<bool> watch_ready{false};
    std::atomic<std::uintptr_t> points[4]{};
    std::atomic<std::uint64_t> point_last[3]{};
    std::atomic<unsigned> watch_hits[4]{};
    std::atomic<unsigned> watch_lines{0};
    std::atomic<unsigned> watch_armed{0};
};

StreamReap& stream_reap() {
    static StreamReap watch;
    return watch;
}

std::uintptr_t sr_image_base = 0;
std::mutex& sr_print_mutex() {
    static std::mutex mutex;
    return mutex;
}

void sr_print(const std::string& text) {
    std::lock_guard lock(sr_print_mutex());
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

// A label for a value: image RVAs as game+RVA, other modules by name, anything else in hex.
std::string sr_label(std::uint64_t value) {
    if (value >= sr_image_base && value < sr_image_base + 0x18000000ull) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "game+0x%x", static_cast<unsigned>(value - sr_image_base));
        return buffer;
    }
    return value ? rt::shims::module_offset(static_cast<std::uintptr_t>(value)) : std::string("0x0");
}

std::string sr_qwords(const char* what, std::uintptr_t address, unsigned count) {
    std::string text = std::string(" ") + what + ":";
    for (unsigned i = 0; i < count; ++i) text += " " + executor_watch_hex(executor_watch_read_u64(address + i * 8));
    return text;
}

// The measured request layout (section 13.4): vtable, refcount at +8, sub-op count +0x18, callback array +0x20 with
// count +0x28, owner +0x30, key +0x38, flag +0x3c.
std::string sr_request_text(std::uintptr_t request) {
    if (!request) return " request 0";
    std::string text = " request " + executor_watch_hex(request) + " vtable " +
                       sr_label(executor_watch_read_u64(request));
    text += " subops " + std::to_string(executor_watch_read_u32(request + 0x18)) + " callbacks " +
            executor_watch_hex(executor_watch_read_u64(request + 0x20)) + "/" +
            std::to_string(executor_watch_read_u32(request + 0x28)) + " owner " +
            executor_watch_hex(executor_watch_read_u64(request + 0x30)) + " key 0x" +
            executor_watch_hex(executor_watch_read_u32(request + 0x38)).substr(2) + " flag 0x" +
            executor_watch_hex(executor_watch_read_u32(request + 0x3c)).substr(2) + " qwords" +
            sr_qwords("", request, 8);
    return text;
}

// The stream object the request is attached to (vtable game+0x658DE48): command id +0x1c, request key +0x30, the
// +0xd8..+0xe8 command state, the +0xfc/+0x100 identity pair, the pending slot +0x120 and the event queue +0x128.
std::string sr_stream_text(std::uintptr_t stream) {
    if (!stream) return " stream 0";
    std::string text = " stream " + executor_watch_hex(stream) + " vtable " +
                       sr_label(executor_watch_read_u64(stream)) + " cmd-id 0x" +
                       executor_watch_hex(executor_watch_read_u32(stream + 0x1c)).substr(2) + " key 0x" +
                       executor_watch_hex(executor_watch_read_u32(stream + 0x30)).substr(2);
    text += sr_qwords(" +0x18", stream + 0x18, 5);
    text += sr_qwords(" +0xd8", stream + 0xd8, 3);
    text += " +0xfc 0x" + executor_watch_hex(executor_watch_read_u32(stream + 0xfc)).substr(2) + " +0x100 " +
            executor_watch_hex(executor_watch_read_u64(stream + 0x100));
    text += sr_qwords(" +0x108", stream + 0x108, 5);
    return text;
}


// The stream's manager-table gate, read the way 0x45e630 reads it: the id at stream+0xfc, the table at stream+0x100,
// the entry qword at table+0x18+(id-1)*0xc0 and the entry's state dword at +0xac (2 = the state the pump requires).
// Printing it every sample says whether the per-frame stream pump is skipping its command loop because the stream is
// not "open" in the manager's own table.
std::string sr_gate_text(std::uintptr_t stream) {
    if (!stream) return " gate node";
    const std::uint32_t id = executor_watch_read_u32(stream + 0xfc);
    const std::uintptr_t table = static_cast<std::uintptr_t>(executor_watch_read_u64(stream + 0x100));
    std::string text = " gate id " + std::to_string(id) + " table " + executor_watch_hex(table);
    if (!table || id == 0 || id > 0x100) return text;
    const std::uintptr_t entry = static_cast<std::uintptr_t>(
        executor_watch_read_u64(table + 0x18 + static_cast<std::uintptr_t>(id - 1) * 0xc0));
    text += " entry " + executor_watch_hex(entry);
    if (entry) {
        text += " state " + std::to_string(executor_watch_read_u32(entry + 0xac));
        text += " +0x58 " + std::to_string(executor_watch_read_u32(entry + 0x58));
    }
    return text;
}

std::string sr_subscriber_text(std::uintptr_t subscriber) {
    if (!subscriber) return " subscriber 0";
    std::string text = " subscriber " + executor_watch_hex(subscriber) + " vtable " +
                       sr_label(executor_watch_read_u64(subscriber));
    text += " stream " + executor_watch_hex(executor_watch_read_u64(subscriber + 0x50)) + " request " +
            executor_watch_hex(executor_watch_read_u64(subscriber + 0x58));
    text += sr_qwords(" +0x80", subscriber + 0x80, 2);
    text += " +0xa8 " + executor_watch_hex(executor_watch_read_u64(subscriber + 0xa8)) + " +0xac 0x" +
            executor_watch_hex(executor_watch_read_u32(subscriber + 0xac)).substr(2) + " +0xb4 0x" +
            executor_watch_hex(executor_watch_read_u32(subscriber + 0xb4)).substr(2);
    return text;
}

// The global request cache at *game+0x999ABE0: buckets +0x348, bucket count +0x350, entry count +0x358, and the node
// layout {key +0x0, request +0x8, next +0x10}. Bounded walk: at most 4096 buckets, 64 nodes per chain and 20000 nodes
// per sample; it reports whether the measured request and its key are still in the cache, which is the cache's own
// statement about whether the request was completed, reaped or never left.
std::string sr_cache_text(std::uintptr_t ours, std::uint32_t our_key) {
    const std::uintptr_t cache = static_cast<std::uintptr_t>(
        executor_watch_read_u64(sr_image_base + kSrRequestCacheGlobalRva));
    if (!cache) return " cache 0";
    const std::uintptr_t buckets = static_cast<std::uintptr_t>(executor_watch_read_u64(cache + 0x348));
    const std::uint32_t bucket_count = executor_watch_read_u32(cache + 0x350);
    const std::uint32_t entries = executor_watch_read_u32(cache + 0x358);
    const std::uint32_t init_word = executor_watch_read_u32(cache + 0x35c);
    std::string text = " cache " + executor_watch_hex(cache) + " buckets " + std::to_string(bucket_count) +
                       " entries " + std::to_string(entries) + " init-word 0x" +
                       executor_watch_hex(init_word).substr(2);
    if (!buckets || bucket_count == 0) return text;
    unsigned nodes = 0, scanned = 0, our_key_nodes = 0;
    bool found_ours = false;
    unsigned buckets_scanned = bucket_count < 4096 ? bucket_count : 4096;
    for (unsigned b = 0; b < buckets_scanned && nodes < 20000; ++b) {
        std::uintptr_t node = static_cast<std::uintptr_t>(executor_watch_read_u64(buckets + b * 8ull));
        unsigned depth = 0;
        ++scanned;
        while (node && depth < 64) {
            const std::uint32_t key = executor_watch_read_u32(node);
            if (key == our_key) ++our_key_nodes;
            if (executor_watch_read_u64(node + 8) == ours && ours) found_ours = true;
            node = static_cast<std::uintptr_t>(executor_watch_read_u64(node + 0x10));
            ++nodes;
            ++depth;
        }
    }
    text += " scanned " + std::to_string(scanned) + " buckets " + std::to_string(nodes) + " node(s); request-in-cache " +
            (found_ours ? "yes" : "no") + " key-nodes " + std::to_string(our_key_nodes);
    return text;
}

std::string sr_manager_text() {
    const std::uintptr_t manager = sr_image_base + kSrManagerGlobalRva;
    std::string text = " manager " + executor_watch_hex(manager) + " vtable " +
                       sr_label(executor_watch_read_u64(manager));
    text += sr_qwords(" +0x8", manager + 0x8, 8);
    text += sr_qwords(" +0x48", manager + 0x48, 4);
    return text;
}

std::string sr_state_text() {
    bool ok = false;
    const std::uint64_t machine = title_read_u64(sr_image_base, kTitleStateMachineRva, ok);
    std::uint16_t current = 0, next = 0;
    if (machine && (machine & 7) == 0) {
        current = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x10, ok);
        next = title_read_u16_at(static_cast<std::uintptr_t>(machine) + 0x12, ok);
    }
    std::string text = " init " + std::to_string(title_read_u32(sr_image_base, kTitleInitStateRva, ok)) +
                       " loading " + std::to_string(title_read_u32(sr_image_base, kTitleLoadingStateRva, ok)) +
                       " session " + std::to_string(title_read_u32(sr_image_base, kTitleSessionStateRva, ok)) +
                       " machine " + std::to_string(current) + "/" + std::to_string(next) +
                       " loading-tick " + std::to_string(title_read_u32(sr_image_base, kTitleLoadingTickRva, ok)) +
                       " record-slot4-tick " + std::to_string(title_read_u32(sr_image_base, kSrRecordSlot4TickRva, ok)) +
                       " record-slot4-comp " + std::to_string(title_read_u32(sr_image_base, kTitleLoadingRecordRva + 4 * 0x50 + 0x18, ok));
    const std::uintptr_t entity = static_cast<std::uintptr_t>(
        title_read_u64(sr_image_base, kTitleMovieSlot0EntityRva, ok));
    if (entity) {
        unsigned char state = 0, open = 0;
        executor_watch_read_block(entity + 0x1c4, &state, 1);
        executor_watch_read_block(entity + 0x23e, &open, 1);
        text += " movie state 0x" + executor_watch_hex(state).substr(2) + " buffers " +
                std::to_string(executor_watch_read_u32(entity + 0x1b8)) + " open 0x" +
                executor_watch_hex(open).substr(2);
    } else {
        text += " movie entity 0";
    }
    return text;
}

std::string sr_chain(const CONTEXT& context) {
    std::uint64_t frames[40] = {};
    const unsigned found = unwind_call_chain(context, frames, 40);
    std::string text;
    text.reserve(768);
    unsigned shown = 0;
    for (unsigned i = 0; i < found && shown < 20; ++i) {
        text += ' ';
        text += sr_label(frames[i]);
        ++shown;
    }
    return text.empty() ? std::string(" (none)") : text;
}

std::string sr_hits_summary() {
    StreamReap& watch = stream_reap();
    std::string text = " hits";
    for (unsigned i = 0; i < kSrSiteCount; ++i)
        text += std::string(" ") + kSrSites[i].name + " " + std::to_string(watch.hits[i].load(std::memory_order_relaxed));
    text += " watch";
    for (unsigned i = 0; i < 4; ++i)
        text += " " + std::to_string(watch.watch_hits[i].load(std::memory_order_relaxed));
    return text;
}

// One logged execution-probe hit. The per-site body reads the argument registers the measured call shape passes.
void sr_log_site(unsigned slot, CONTEXT& context) {
    StreamReap& watch = stream_reap();
    if (watch.lines.fetch_add(1, std::memory_order_relaxed) >= kSrTotalLines) return;
    const std::uintptr_t rcx = static_cast<std::uintptr_t>(context.Rcx);
    const std::uintptr_t rdx = static_cast<std::uintptr_t>(context.Rdx);
    const std::uintptr_t r8 = static_cast<std::uintptr_t>(context.R8);
    const std::uintptr_t r9 = static_cast<std::uintptr_t>(context.R9);
    char head[224];
    std::snprintf(head, sizeof head,
                  "[rt] streamreap t=%.3f (%.1f s) site %s hit %u tid %lu rcx 0x%llx rdx 0x%llx r8 0x%llx r9 0x%llx",
                  executor_watch_seconds(),
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - sampler_epoch()).count(),
                  kSrSites[slot].name, watch.hits[slot].load(std::memory_order_relaxed),
                  static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(rcx),
                  static_cast<unsigned long long>(rdx), static_cast<unsigned long long>(r8),
                  static_cast<unsigned long long>(r9));
    std::string text = head;
    const std::uintptr_t ours = watch.request.load(std::memory_order_relaxed);
    switch (slot) {
        case 0:  // subscriber start: the measured subscriber object and the event payload it consumed
            if (rcx) watch.subscriber.store(rcx, std::memory_order_relaxed);
            text += sr_subscriber_text(rcx);
            if (rdx) text += sr_qwords(" payload", rdx, 4);
            break;
        case 1: {  // the command dispatch: subscriber, command id and the stream/handle it is about to submit
            const std::uintptr_t stream = rcx ? static_cast<std::uintptr_t>(executor_watch_read_u64(rcx + 0x50)) : 0;
            const std::uintptr_t request = rcx ? static_cast<std::uintptr_t>(executor_watch_read_u64(rcx + 0x58)) : 0;
            text += " subscriber 0x" + executor_watch_hex(rcx).substr(2) + " command-id 0x" +
                    executor_watch_hex(rdx & 0xFFFFFFFFu).substr(2) + " flag " + std::to_string(context.R8 & 0xFF) +
                    " stream " + executor_watch_hex(stream) + " request-before " + executor_watch_hex(request);
            break;
        }
        case 2:  // the attach's pending registration: stream in rcx, request in rdx, command id in r8d
            if (rcx) watch.stream.store(rcx, std::memory_order_relaxed);
            if (rdx) {
                watch.request.store(rdx, std::memory_order_relaxed);
                watch.request_key.store(executor_watch_read_u32(rdx + 0x38), std::memory_order_relaxed);
                watch.watch_ready.store(true, std::memory_order_relaxed);
            }
            text += sr_stream_text(rcx) + sr_request_text(rdx) + " command-id 0x" +
                    executor_watch_hex(context.R8 & 0xFFFFFFFFu).substr(2);
            break;
        case 3:  // the stream command start: stream and command id
            text += sr_stream_text(rcx) + " command-id 0x" +
                    executor_watch_hex(rdx & 0xFFFFFFFFu).substr(2);
            break;
        case 4: {  // the async cache-entry completion: entry in rcx, status in rdx; the producer's last step
            text += " entry 0x" + executor_watch_hex(rcx).substr(2);
            const std::uintptr_t owner = rcx ? static_cast<std::uintptr_t>(executor_watch_read_u64(rcx + 0x30)) : 0;
            text += sr_request_text(rcx) + " status 0x" + executor_watch_hex(rdx & 0xFFFFFFFFu).substr(2);
            text += " owner " + sr_label(owner);
            if (ours && rcx == ours) text += " OURS";
            if (owner && owner == watch.stream.load(std::memory_order_relaxed)) text += " OWNER-IS-STREAM";
            if (owner) text += sr_stream_text(owner);
            break;
        }
        case 5:  // the cache hash lookup: table rcx, count rdx, out r8, key r9d
            text += " lookup key 0x" + executor_watch_hex(context.R9 & 0xFFFFFFFFu).substr(2) + " buckets " +
                    executor_watch_hex(rcx) + " count " + std::to_string(rdx & 0xFFFFFFFFu) + " out " +
                    executor_watch_hex(r8);
            if (ours && (context.R9 & 0xFFFFFFFFu) == executor_watch_read_u32(ours + 0x38)) text += " OURS-KEY";
            break;
        case 6:  // the cache key builder: out rcx, handle rdx, flag r8d
            text += " handle " + executor_watch_hex(rdx) + " out " + executor_watch_hex(rcx) + " flag " +
                    std::to_string(context.R8 & 0xFF);
            break;
        case 7:  // the streaming manager's job reap: the job object rcx (handle +0x40, status +0x48), context rdx
            text += " job 0x" + executor_watch_hex(rcx).substr(2) + " vtable " +
                    sr_label(rcx ? executor_watch_read_u64(rcx) : 0) + " handle " +
                    executor_watch_hex(rcx ? executor_watch_read_u64(rcx + 0x40) : 0) + " status 0x" +
                    executor_watch_hex(rcx ? executor_watch_read_u32(rcx + 0x48) : 0).substr(2) + " context " +
                    executor_watch_hex(rdx) + " context-vtable " + sr_label(rdx ? executor_watch_read_u64(rdx) : 0);
            break;
        case 8:  // the async command handler: object rcx (its +0x58 async request), payload rdx
            text += " object 0x" + executor_watch_hex(rcx).substr(2);
            if (ours && rcx) {
                const std::uintptr_t handle = static_cast<std::uintptr_t>(executor_watch_read_u64(rcx + 0x58));
                text += " object+0x58 " + executor_watch_hex(handle);
                if (handle == ours) text += " OURS";
            }
            text += sr_qwords(" object", rcx, 8) + sr_qwords(" payload", rdx, 4);
            break;
        case 9:  // the async completion handler: object rcx, handle/payload rdx, flags r8
            text += sr_qwords(" object", rcx, 8) + sr_qwords(" payload", rdx, 4) + " flags 0x" +
                    executor_watch_hex(context.R8).substr(2);
            break;
        case 10:  // the streaming manager's completion entry: status rcx, payload rdx, flags r8
            text += sr_qwords(" status", rcx, 4) + sr_qwords(" payload", rdx, 4) + " flags 0x" +
                    executor_watch_hex(context.R8).substr(2);
            break;
        case 11:  // the stream event-queue raise: queue rcx (stream+0x128), value rdx
            text += " queue " + executor_watch_hex(rcx) + " value " + executor_watch_hex(rdx) + " queue-vtable " +
                    sr_label(rcx ? executor_watch_read_u64(rcx) : 0);
            if (ours) text += " stream-queue " +
                              sr_label(executor_watch_read_u64(ours + 0x30) + 0x128);
            break;
        case 12:  // the stream command-start event raise: queue rcx, command id rdx
            text += " queue " + executor_watch_hex(rcx) + " command-id 0x" +
                    executor_watch_hex(rdx & 0xFFFFFFFFu).substr(2) + " queue-vtable " +
                    sr_label(rcx ? executor_watch_read_u64(rcx) : 0);
            break;
        case 13:  // the request consume: request rcx; the measured-zero consumer of section 13
            text += sr_request_text(rcx);
            if (ours && rcx == ours) text += " OURS";
            break;
        case 14:  // the subscriber completion: subscriber rcx
            text += sr_subscriber_text(rcx);
            break;
        case 15:  // the streaming stop: its argument and the manager
            text += " arg " + std::to_string(context.Rcx & 0xFFFFFFFFu) + sr_manager_text();
            break;
        case 16:  // the per-frame stream pump: stream in rcx, frame delta in rdx (float bits)
            text += sr_stream_text(rcx) + sr_gate_text(rcx);
            break;
        case 17:  // the async command step the pump runs with the pending request: stream in rcx
            text += sr_stream_text(rcx) + sr_gate_text(rcx);
            break;
        case 18:  // the async engine step: queue stream+0xe8 in rcx, delta rdx, out r8, pending request r9
            text += " queue 0x" + executor_watch_hex(rcx).substr(2) + " out " + executor_watch_hex(r8) +
                    " request " + executor_watch_hex(r9);
            if (rcx) text += sr_qwords(" queue", rcx, 6);
            if (ours && r9 == ours) text += " OURS";
            break;
        case 19: {  // the manager-table open check: id in rcx, table in rdx; print the entry and state it tests
            const std::uint32_t id = static_cast<std::uint32_t>(rcx);
            const std::uintptr_t table = rdx;
            text += " check id " + std::to_string(id) + " table " + executor_watch_hex(table);
            if (table && id && id <= 0x100) {
                const std::uintptr_t entry = static_cast<std::uintptr_t>(
                    executor_watch_read_u64(table + 0x18 + static_cast<std::uintptr_t>(id - 1) * 0xc0));
                text += " entry " + executor_watch_hex(entry);
                if (entry) {
                    const std::uint32_t state = executor_watch_read_u32(entry + 0xac);
                    text += " state " + std::to_string(state) + " open " + (state == 2 ? "yes" : "no");
                }
            }
            break;
        }
        default:
            break;
    }
    text += "\n  chain:" + sr_chain(context) + "\n";
    sr_print(text);
}

void sr_write_byte(unsigned slot, unsigned char value) {
    StreamReap& watch = stream_reap();
    const auto address = watch.address[slot].load(std::memory_order_relaxed);
    if (!address) return;
    DWORD old_protect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *reinterpret_cast<unsigned char*>(address) = value;
        VirtualProtect(reinterpret_cast<void*>(address), 1, old_protect, &old_protect);
    }
}

thread_local unsigned sr_repoke_slot = 0;

// The four hardware write breakpoints: DR0 request+0x18 (sub-op count), DR1 request+0x20 (callback array), DR2 the
// stream's pending slot +0x120, DR3 the loading record's slot-4 tick as the same-boot title-store live control.
DWORD sr_watch_dr7(const std::uintptr_t points[4]) {
    DWORD dr7 = 0;
    for (unsigned index = 0; index < 4; ++index) {
        if (!points[index] || (points[index] & 7u) != 0) continue;
        dr7 |= 0x1u << (2 * index);           // L_i
        dr7 |= 0x1u << (16 + 4 * index);      // RW_i = 01, write
        dr7 |= 0x3u << (18 + 4 * index);      // LEN_i = 11, 8 bytes
    }
    return dr7;
}

bool sr_watch_configure_thread(DWORD tid, const std::uintptr_t points[4], DWORD dr7) {
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!thread) return false;
    bool configured = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context)) {
            context.Dr0 = points[0];
            context.Dr1 = points[1];
            context.Dr2 = points[2];
            context.Dr3 = points[3];
            context.Dr6 = 0;
            context.Dr7 = dr7;
            configured = SetThreadContext(thread, &context) != FALSE;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return configured;
}

// One hardware write-watch hit: writer RIP and the bytes before it (the trap fires after the store), the value now and
// before, the request/stream state at that instant and the unwound writer chain.
void sr_watch_hit(unsigned index, CONTEXT& context) {
    StreamReap& watch = stream_reap();
    std::atomic<unsigned>& hits = watch.watch_hits[index];
    const unsigned hit = hits.fetch_add(1, std::memory_order_relaxed) + 1;
    static constexpr unsigned kCap[4] = {24, 12, 12, 3};
    const bool log = hit <= kCap[index] || (hit % 4096 == 0);
    if (!log) return;
    if (watch.watch_lines.fetch_add(1, std::memory_order_relaxed) >= kSrWatchLines) return;
    const std::uintptr_t address = watch.points[index].load(std::memory_order_relaxed);
    std::uint64_t now = 0;
    const bool read_ok = executor_watch_read_block(address, &now, 8);
    std::uint64_t previous = 0;
    if (index < 3) previous = watch.point_last[index].exchange(now, std::memory_order_relaxed);
    char writer[96];
    const std::uint64_t rip = context.Rip;
    snprintf(writer, sizeof writer, "%s", sr_label(rip).c_str());
    unsigned char bytes[16] = {};
    char store_bytes[96] = " (unreadable)";
    if (rip >= 16 && executor_watch_read_block(static_cast<std::uintptr_t>(rip) - 16, bytes, 16)) {
        int at = 0;
        for (unsigned i = 0; i < 16 && at + 4 < static_cast<int>(sizeof store_bytes); ++i)
            at += std::snprintf(store_bytes + at, sizeof store_bytes - static_cast<std::size_t>(at), " %02x", bytes[i]);
    }
    static const char* const kNames[4] = {"REQUEST +0x18", "REQUEST +0x20", "STREAM +0x120", "LIVE-CONTROL"};
    char head[320];
    std::snprintf(head, sizeof head,
                  "[rt] streamreap watch t=%.3f (%.1f s) %s hit %u tid %lu writer %s value %s0x%llx (was 0x%llx)",
                  executor_watch_seconds(),
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - sampler_epoch()).count(),
                  kNames[index], hit, static_cast<unsigned long>(GetCurrentThreadId()), writer,
                  read_ok ? "" : "UNREADABLE ", static_cast<unsigned long long>(now),
                  static_cast<unsigned long long>(previous));
    std::string text = head;
    if (index != 3) {
        text += sr_request_text(watch.request.load(std::memory_order_relaxed));
        const std::uintptr_t watched_stream = watch.stream.load(std::memory_order_relaxed);
        text += sr_stream_text(watched_stream);
        text += sr_gate_text(watched_stream);
        text += sr_state_text();
        text += sr_cache_text(watch.request.load(std::memory_order_relaxed),
                              watch.request_key.load(std::memory_order_relaxed));
    }
    text += "\n  bytes before writer rip:" + std::string(store_bytes) +
            "\n  rax " + executor_watch_hex(context.Rax) + " rbx " + executor_watch_hex(context.Rbx) + " rcx " +
            executor_watch_hex(context.Rcx) + " rdx " + executor_watch_hex(context.Rdx) + " rsi " +
            executor_watch_hex(context.Rsi) + " rdi " + executor_watch_hex(context.Rdi) +
            "\n  chain:" + sr_chain(context) + "\n";
    sr_print(text);
}

LONG CALLBACK sr_exception(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    StreamReap& watch = stream_reap();
    if (!watch.ready.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        unsigned slot = kSrSiteCount;
        for (unsigned i = 0; i < kSrSiteCount; ++i)
            if (watch.address[i].load(std::memory_order_relaxed) == at) {
                slot = i;
                break;
            }
        if (slot == kSrSiteCount) return EXCEPTION_CONTINUE_SEARCH;
        const unsigned hit = watch.hits[slot].fetch_add(1, std::memory_order_relaxed) + 1;
        if ((hit <= kSrSites[slot].cap || hit % 4096 == 0) &&
            watch.logged[slot].fetch_add(1, std::memory_order_relaxed) < 256)
            sr_log_site(slot, context);
        sr_write_byte(slot, static_cast<unsigned char>(watch.original[slot].load(std::memory_order_relaxed)));
        sr_repoke_slot = slot + 1;
        context.EFlags |= 0x100;  // trap after the original instruction, then put the int3 back
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        // The int3 restore-and-single-step first: its pending slot is thread-local. A debug-register trap has no TF.
        if (sr_repoke_slot) {
            const unsigned slot = sr_repoke_slot - 1;
            sr_repoke_slot = 0;
            sr_write_byte(slot, 0xCC);
            context.EFlags &= ~static_cast<DWORD>(0x100);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        const DWORD dr6 = context.Dr6;
        if ((dr6 & 0xFu) == 0) return EXCEPTION_CONTINUE_SEARCH;
        context.Dr6 = 0;
        for (unsigned index = 0; index < 4; ++index)
            if ((dr6 & (1u << index)) != 0) sr_watch_hit(index, context);
        return EXCEPTION_CONTINUE_EXECUTION;  // a data breakpoint does not consume the instruction
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// The arming sweep for the request/stream watch: starts when the attach has registered the request, configures every
// thread that has been alive across two scans, and keeps picking up threads created later.
void sr_start_watch_sweep() {
    std::thread([] {
        StreamReap& watch = stream_reap();
        for (unsigned tick = 0; tick < 12000; ++tick) {
            if (watch.request.load(std::memory_order_relaxed) && watch.stream.load(std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!watch.request.load(std::memory_order_relaxed) || !watch.stream.load(std::memory_order_relaxed)) return;
        static constexpr unsigned kMaxThreads = 4096, kPending = 512;
        static DWORD configured[kMaxThreads];
        static DWORD seen_tid[kMaxThreads];
        static ULONGLONG seen_at[kMaxThreads];
        unsigned configured_count = 0, seen_count = 0;
        std::uintptr_t last_request = 0, last_stream = 0;
        const auto find = [](const DWORD* table, unsigned count, DWORD tid) -> int {
            for (unsigned i = 0; i < count; ++i)
                if (table[i] == tid) return static_cast<int>(i);
            return -1;
        };
        std::uintptr_t points[4] = {};
        DWORD dr7 = 0;
        for (;;) {
            // The attach names the request and the stream; re-arm every thread when the measured pair changes (a later
            // loading channel's attach would otherwise leave the watch on a stale object).
            const std::uintptr_t request = watch.request.load(std::memory_order_relaxed);
            const std::uintptr_t stream = watch.stream.load(std::memory_order_relaxed);
            if (request && stream && (request != last_request || stream != last_stream)) {
                points[0] = request + 0x18;
                points[1] = request + 0x20;
                points[2] = stream + 0x120;
                points[3] = sr_image_base + kSrRecordSlot4TickRva;
                for (unsigned i = 0; i < 4; ++i) watch.points[i].store(points[i], std::memory_order_relaxed);
                for (unsigned i = 0; i < 3; ++i) watch.point_last[i].store(executor_watch_read_u64(points[i]));
                dr7 = sr_watch_dr7(points);
                configured_count = 0;
                last_request = request;
                last_stream = stream;
                char log[480];
                std::snprintf(log, sizeof log,
                              "[rt] streamreap watch: request 0x%llx stream 0x%llx; DR0 game+0x%x DR1 game+0x%x "
                              "DR2 game+0x%x DR3 game+0x%x (live control); dr7 0x%lx; arming every thread\n",
                              static_cast<unsigned long long>(request), static_cast<unsigned long long>(stream),
                              static_cast<unsigned>(points[0] - sr_image_base),
                              static_cast<unsigned>(points[1] - sr_image_base),
                              static_cast<unsigned>(points[2] - sr_image_base),
                              static_cast<unsigned>(points[3] - sr_image_base), static_cast<unsigned long>(dr7));
                std::lock_guard lock(sr_print_mutex());
                std::fwrite(log, 1, std::strlen(log), stderr);
                std::fflush(stderr);
            }
            const ULONGLONG now = GetTickCount64();
            DWORD pending[kPending];
            unsigned pending_count = 0, refused = 0;
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{};
                entry.dwSize = sizeof entry;
                const DWORD pid = GetCurrentProcessId();
                const DWORD self = GetCurrentThreadId();
                if (Thread32First(snapshot, &entry))
                    do {
                        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
                        if (find(configured, configured_count, entry.th32ThreadID) >= 0) continue;
                        const int seat = find(seen_tid, seen_count, entry.th32ThreadID);
                        if (seat < 0) {
                            if (seen_count < kMaxThreads) {
                                seen_tid[seen_count] = entry.th32ThreadID;
                                seen_at[seen_count] = now;
                                ++seen_count;
                            }
                            continue;  // first sight: wait one scan before suspending it
                        }
                        if (now - seen_at[seat] < 1000) continue;
                        if (pending_count < kPending) pending[pending_count++] = entry.th32ThreadID;
                    } while (Thread32Next(snapshot, &entry));
                CloseHandle(snapshot);
            }
            for (unsigned i = 0; i < pending_count; ++i)
                if (dr7 && sr_watch_configure_thread(pending[i], points, dr7)) {
                    if (configured_count < kMaxThreads) configured[configured_count++] = pending[i];
                } else {
                    ++refused;
                }
            watch.watch_armed.store(configured_count, std::memory_order_relaxed);
            (void)refused;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }).detach();
}

void start_stream_reap_probe(std::uintptr_t image_base, std::size_t image_size, bool watch_enabled) {
    (void)image_size;
    StreamReap& watch = stream_reap();
    sr_image_base = image_base;
    watch.watch_enabled.store(watch_enabled, std::memory_order_relaxed);
    unsigned armed = 0;
    std::string list;
    for (unsigned i = 0; i < kSrSiteCount; ++i) {
        auto* const at = reinterpret_cast<unsigned char*>(image_base + kSrSites[i].rva);
        DWORD old_protect = 0;
        if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
            list += std::string(" ") + kSrSites[i].name + "=FAILED";
            continue;
        }
        watch.original[i].store(*at, std::memory_order_relaxed);
        watch.address[i].store(reinterpret_cast<std::uintptr_t>(at), std::memory_order_relaxed);
        *at = 0xCC;
        VirtualProtect(at, 1, old_protect, &old_protect);
        ++armed;
        char item[64];
        std::snprintf(item, sizeof item, " %s=game+0x%x", kSrSites[i].name,
                      static_cast<unsigned>(kSrSites[i].rva));
        list += item;
    }
    AddVectoredExceptionHandler(1, sr_exception);
    watch.ready.store(true, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[rt] streamreap: %u/%u int3 site(s) armed:%s; manager game+0x%x, request cache *game+0x%x, watch %s\n",
                 armed, kSrSiteCount, list.c_str(), static_cast<unsigned>(kSrManagerGlobalRva),
                 static_cast<unsigned>(kSrRequestCacheGlobalRva), watch_enabled ? "on" : "off");
    std::fflush(stderr);
    if (watch_enabled) sr_start_watch_sweep();
    // The 2 s sampler: state words, manager, request cache, the measured request and stream, and the hit counters.
    std::thread([] {
        for (unsigned tick = 1; tick <= 320; ++tick) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (tick % 2 != 0) continue;
            StreamReap& watch = stream_reap();
            const std::uintptr_t request = watch.request.load(std::memory_order_relaxed);
            if (!request && !watch.subscriber.load(std::memory_order_relaxed)) continue;
            if (watch.sampler_lines.fetch_add(1, std::memory_order_relaxed) >= kSrSamplerLines) return;
            char head[96];
            std::snprintf(head, sizeof head, "[rt] streamreap at %u s:", tick);
            std::string text = head;
            text += sr_state_text();
            text += sr_manager_text();
            text += sr_cache_text(request, watch.request_key.load(std::memory_order_relaxed));
            if (request) text += sr_request_text(request);
            const std::uintptr_t sampled_stream = watch.stream.load(std::memory_order_relaxed);
            text += sr_stream_text(sampled_stream);
            text += sr_gate_text(sampled_stream);
            text += sr_hits_summary();
            text += "\n";
            sr_print(text);
        }
    }).detach();
    // The 10 s counter reporter: a boot without a hit still shows what the probe saw.
    std::thread([] {
        for (unsigned sample = 1; sample <= 30; ++sample) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            StreamReap& watch = stream_reap();
            std::fprintf(stderr, "[rt] streamreap counters at %u s:%s watch-armed %u\n", sample * 10,
                         sr_hits_summary().c_str(), watch.watch_armed.load(std::memory_order_relaxed));
            std::fflush(stderr);
        }
    }).detach();
    // The 100 ms fine change log: the request's first 0x40 bytes, the stream's +0xfc..+0x128 block, the cache entry
    // count and the record's slot-4 completion dword; one line per change after the request is known.
    std::thread([] {
        StreamReap& watch = stream_reap();
        std::uint64_t last_request[8] = {};
        std::uint64_t last_stream[7] = {};
        std::uint32_t last_cache_entries = 0, last_record = 0;
        std::uintptr_t last_request_ptr = 0;
        bool have = false;
        for (unsigned tick = 0; tick < 3100; ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::uintptr_t request = watch.request.load(std::memory_order_relaxed);
            const std::uintptr_t stream = watch.stream.load(std::memory_order_relaxed);
            if (!request) continue;
            std::uint64_t now_request[8] = {};
            std::uint64_t now_stream[7] = {};
            for (unsigned i = 0; i < 8; ++i) now_request[i] = executor_watch_read_u64(request + i * 8);
            if (stream)
                for (unsigned i = 0; i < 7; ++i) now_stream[i] = executor_watch_read_u64(stream + 0xfc + i * 8);
            std::uint32_t now_cache_entries = 0, now_record = 0;
            {
                const std::uintptr_t cache = static_cast<std::uintptr_t>(
                    executor_watch_read_u64(sr_image_base + kSrRequestCacheGlobalRva));
                if (cache) now_cache_entries = executor_watch_read_u32(cache + 0x358);
            }
            bool ok_record = false;
            now_record = title_read_u32(sr_image_base, kTitleLoadingRecordRva + 4 * 0x50 + 0x18, ok_record);
            bool changed = !have || request != last_request_ptr || now_cache_entries != last_cache_entries ||
                           now_record != last_record;
            for (unsigned i = 0; i < 8; ++i)
                if (now_request[i] != last_request[i]) changed = true;
            for (unsigned i = 0; i < 7; ++i)
                if (now_stream[i] != last_stream[i]) changed = true;
            if (!changed) continue;
            if (watch.fine_lines.fetch_add(1, std::memory_order_relaxed) >= kSrFineLines) return;
            char head[128];
            std::snprintf(head, sizeof head, "[rt] streamreap t=%.3f fine request 0x%llx stream 0x%llx",
                          executor_watch_seconds(), static_cast<unsigned long long>(request),
                          static_cast<unsigned long long>(stream));
            std::string text = head;
            text += " request" + sr_qwords("", request, 8);
            if (stream) text += sr_qwords(" stream +0xfc", stream + 0xfc, 7);
            text += " cache-entries " + std::to_string(now_cache_entries) + " record-slot4-comp " +
                    std::to_string(now_record);
            text += sr_hits_summary();
            text += "\n";
            sr_print(text);
            for (unsigned i = 0; i < 8; ++i) last_request[i] = now_request[i];
            for (unsigned i = 0; i < 7; ++i) last_stream[i] = now_stream[i];
            last_request_ptr = request;
            last_cache_entries = now_cache_entries;
            last_record = now_record;
            have = true;
        }
    }).detach();
}

// ---- Stream open-transition probe (PALMETTO_STREAMOPEN) ------------------------------------------------------------
// docs/decomp_stage.md section 14 measured the session-3 stream request created, cached and attached once, stepped by the
// loading update every frame, with its async command engine entered only for 0.7 s around the attach: the stream's own
// open gate game+0x45e640 was true in that window and false for the rest of the boot, so the request stayed pending and
// every completion entry point stayed at zero. Section 14.7 names this probe as the next falsifier: a hardware write
// watch on the manager-table slot and on the entry's state block (the two fields the gate reads), execution probes on
// the gate's two lookups and on the registration/open path, and a fine sampler so a sub-second open window is measured
// instead of missed.
//
// The gate and the table, from the image's own bytes (RVAs and offsets only):
//   0x45e640(id, table)  gate: true when 0x45e690(id, table) is non-null, or when 0x45e6d0(id, table) is non-null and
//                        *(u32*)(entry+0xac) == 2; 0x45e690 reads *(u64*)(table + (id-1)*0xc0 + 0x10), 0x45e6d0 reads
//                        *(u64*)(table + (id-1)*0xc0 + 0x18);
//   0x402f120(stream)    command start: handler lookup 0x402f1c0, registration 0x402d010(queue = stream+0xe8), then the
//                        gate again; only a true gate raises the start event on stream+0x128 and returns 1;
//   0x402d010(queue)     registration/open: three registry setters, a record vector commit, two queue-object virtual
//                        calls (vtable slot +0x18), a registry update 0x1c9b8e0, and a final gate check whose result is
//                        discarded - the caller re-reads the gate itself;
//   0x5aeac40/50/60      the registry setters: store to table + (id-1)*0xc0 + 0x8 / + 0x10 / + 0x18 (the gate reads
//                        the last two); 0x5aeace0 stores +0xb8, 0x5aead00 stores the record vector at +0x20/+0x28/+0x30
//                        and its count at +0xb0, 0x5aeabf0 clears +0x8/+0x10/+0x18 and the vector (the close path), and
//                        0x1c9b8e0 is the registry update that can store the +0x10 engine pointer.
// The write watch (PALMETTO_STREAMOPEN_WATCH=1, on by default when the probe is on) uses all four data breakpoints:
//   DR0 table+(id-1)*0xc0+0x10  the registry engine pointer the gate's first lookup reads;
//   DR1 table+(id-1)*0xc0+0x18  the entry pointer the gate's second lookup reads;
//   DR2 stream+0x118             the stream state byte +0x118, the owner-busy bit +0x11a the request step tests,
//                               and the pending request slot +0x120 (one aligned 8-byte window);
//   DR3 game+0x78acb08          the session-3 poll's slot-4 tick: the same-boot title-store live control section 11.3
//                               measured (writer game+0x41e8fd).
// The points are installed on the loading task's own thread at the subscriber start and the command start (so the
// registration's stores are caught), and a sweep arms every other thread for later writers. Everything is opt-in and
// bounded: one cached getenv when unset, per-site caps, a 10 ms change log, a 2 s sampler, a 10 s counter reporter, and
// with the switch unset no thread starts, no handler is installed and the default path is unchanged. RVAs only.
constexpr std::uint32_t kSoLiveControlRva = 0x78ACB08;
constexpr unsigned kSoSiteCount = 30;
constexpr unsigned kSoTotalLines = 800;
constexpr unsigned kSoSamplerLines = 130;
constexpr unsigned kSoFineLines = 260;
constexpr unsigned kSoWatchLines = 160;
constexpr unsigned kSoMaxThreads = 4096;
constexpr unsigned kSoPendingThreads = 512;

enum SoSiteIndex : unsigned {
    kSoSubscriberStart = 0,
    kSoStreamSubmit,
    kSoPendingRegister,
    kSoCommandStart,
    kSoHandlerLookup,
    kSoRegisterOpen,
    kSoRegisterCancel,
    kSoOpenGate,
    kSoRegistryEngineLookup,
    kSoQueueVtableSite,
    kSoEngineState,
    kSoRecordKeyLookup,
    kSoRegistryUpdate,
    kSoRegistryClose,
    kSoRegistrySetJob,
    kSoRegistrySetEngine,
    kSoRegistrySetEntry,
    kSoRegistrySetTail,
    kSoRegistrySetRecords,
    kSoStreamPump,
    kSoStreamStep,
    kSoAsyncEngineStep,
    kSoCompletionDispatch,
    kSoCacheComplete,
    kSoOwnerBusyCheck,
    kSoRequestStep,
    kSoRequestConsume,
    kSoCompletionRun,
    kSoQueueProcess,
    kSoQueuePoll,
};

struct SoSite {
    std::uint32_t rva;
    const char* name;
    unsigned cap;
};

constexpr SoSite kSoSites[kSoSiteCount] = {
    {0x2355DD0, "subscriber-start", 3},
    {0x1DBC0F0, "stream-submit", 3},
    {0x402F0F0, "stream-pending-register", 3},
    {0x402F120, "stream-command-start", 4},
    {0x402F1C0, "handler-lookup", 3},
    {0x402D010, "register-open", 6},
    {0x402D320, "register-cancel", 6},
    {0x45E640, "stream-open-gate", 8},
    {0x45E690, "registry-engine-lookup", 12},
    {0x402D15D, "queue-vtable-site", 4},
    {0x402D430, "engine-state", 12},
    {0x1C9B8D0, "record-key-lookup", 6},
    {0x1C9B8E0, "registry-update", 10},
    {0x5AEABF0, "registry-close", 10},
    {0x5AEAC40, "registry-set-job", 8},
    {0x5AEAC50, "registry-set-engine", 10},
    {0x5AEAC60, "registry-set-entry", 10},
    {0x5AEACE0, "registry-set-tail", 6},
    {0x5AEAD00, "registry-set-records", 8},
    {0x45E570, "stream-pump", 3},
    {0x402E260, "stream-step", 3},
    {0x402E340, "async-engine-step", 10},
    {0x402E8A0, "completion-dispatch", 8},
    {0x4043390, "cache-complete", 6},
    {0x88B3F0, "owner-busy-check", 3},
    {0x45E430, "request-step", 4},
    {0x4043300, "request-consume", 6},
    {0x402E050, "completion-run", 6},
    {0x599B7B0, "queue-process", 6},
    {0x599B790, "queue-poll", 6},
};

struct StreamOpen {
    std::atomic<bool> ready{false};
    std::atomic<std::uintptr_t> address[kSoSiteCount]{};
    std::atomic<unsigned> original[kSoSiteCount]{};
    std::atomic<unsigned> hits[kSoSiteCount]{};
    std::atomic<unsigned> logged[kSoSiteCount]{};
    std::atomic<unsigned> lines{0};
    std::atomic<unsigned> sampler_lines{0};
    std::atomic<unsigned> fine_lines{0};
    std::atomic<std::uintptr_t> subscriber{0};
    std::atomic<std::uintptr_t> stream{0};
    std::atomic<std::uintptr_t> request{0};
    std::atomic<std::uintptr_t> table{0};
    std::atomic<std::uintptr_t> entry{0};
    std::atomic<bool> watch_enabled{false};
    std::atomic<unsigned> point_version{0};
    std::atomic<std::uintptr_t> points[4]{};
    std::atomic<std::uint64_t> point_last[3]{};
    std::atomic<unsigned> watch_hits[4]{};
    std::atomic<unsigned> watch_lines{0};
    std::atomic<unsigned> watch_armed{0};
};

StreamOpen& stream_open() {
    static StreamOpen probe;
    return probe;
}

std::uintptr_t so_image_base = 0;
std::mutex& so_print_mutex() {
    static std::mutex mutex;
    return mutex;
}

void so_print(const std::string& text) {
    std::lock_guard lock(so_print_mutex());
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

std::uintptr_t so_record_slot(std::uintptr_t table, std::uint32_t id, std::uintptr_t offset) {
    if (!table || id == 0 || id > 0x100) return 0;
    return table + offset + static_cast<std::uintptr_t>(id - 1) * 0xc0;
}

std::uintptr_t so_stream_table(std::uintptr_t stream, std::uint32_t* id_out) {
    if (!stream) {
        if (id_out) *id_out = 0;
        return 0;
    }
    const std::uint32_t id = executor_watch_read_u32(stream + 0xfc);
    if (id_out) *id_out = id;
    return static_cast<std::uintptr_t>(executor_watch_read_u64(stream + 0x100));
}

std::uintptr_t so_table_engine(std::uintptr_t table, std::uint32_t id) {
    return static_cast<std::uintptr_t>(executor_watch_read_u64(so_record_slot(table, id, 0x10)));
}

std::uintptr_t so_table_entry(std::uintptr_t table, std::uint32_t id) {
    return static_cast<std::uintptr_t>(executor_watch_read_u64(so_record_slot(table, id, 0x18)));
}

bool so_gate_open(std::uintptr_t engine, std::uintptr_t entry) {
    return engine != 0 || (entry != 0 && executor_watch_read_u32(entry + 0xac) == 2);
}

// The manager-table record for the stream's id, read the way the gate and the registry setters read it.
std::string so_gate_text(std::uintptr_t stream) {
    std::uint32_t id = 0;
    const std::uintptr_t table = so_stream_table(stream, &id);
    std::string text = " gate id " + std::to_string(id) + " table " + executor_watch_hex(table);
    if (!table || id == 0 || id > 0x100) return text;
    const std::uintptr_t record = so_record_slot(table, id, 0);
    const std::uintptr_t engine = static_cast<std::uintptr_t>(executor_watch_read_u64(record + 0x10));
    const std::uintptr_t entry = static_cast<std::uintptr_t>(executor_watch_read_u64(record + 0x18));
    text += " rec " + executor_watch_hex(record) + " +0x8 " +
            executor_watch_hex(executor_watch_read_u64(record + 0x8)) + " engine " + executor_watch_hex(engine) +
            " entry " + executor_watch_hex(entry);
    if (engine) {
        text += " engine-vtable " + sr_label(executor_watch_read_u64(engine)) + " engine+0x58 " +
                std::to_string(executor_watch_read_u32(engine + 0x58)) + " engine+0x40 " +
                executor_watch_hex(executor_watch_read_u64(engine + 0x40));
    }
    if (entry) {
        text += " entry-vtable " + sr_label(executor_watch_read_u64(entry)) + " entry+0x58 " +
                std::to_string(executor_watch_read_u32(entry + 0x58)) + " state+0xac " +
                std::to_string(executor_watch_read_u32(entry + 0xac));
    }
    text += " open " + std::string(so_gate_open(engine, entry) ? "yes" : "no");
    return text;
}

std::string so_entry_text(std::uintptr_t entry) {
    if (!entry) return " entry 0";
    std::string text = " entry " + executor_watch_hex(entry) + " vtable " + sr_label(executor_watch_read_u64(entry));
    text += sr_qwords(" +0x0", entry, 5);
    text += " state+0x58 " + std::to_string(executor_watch_read_u32(entry + 0x58)) + " +0x5c " +
            std::to_string(executor_watch_read_u32(entry + 0x5c)) + " state+0xa8 " +
            executor_watch_hex(executor_watch_read_u64(entry + 0xa8)) + " +0xb0 " +
            executor_watch_hex(executor_watch_read_u64(entry + 0xb0));
    return text;
}

std::string so_engine_text(std::uintptr_t engine) {
    if (!engine) return " engine 0";
    std::string text = " engine " + executor_watch_hex(engine) + " vtable " + sr_label(executor_watch_read_u64(engine));
    text += sr_qwords(" +0x0", engine, 5);
    text += " +0x28 " + executor_watch_hex(executor_watch_read_u64(engine + 0x28)) + " +0x40 " +
            executor_watch_hex(executor_watch_read_u64(engine + 0x40)) + " state+0x58 " +
            std::to_string(executor_watch_read_u32(engine + 0x58)) + " +0x5c " +
            std::to_string(executor_watch_read_u32(engine + 0x5c));
    return text;
}

// The stream's embedded async queue (stream+0xe8): the fields 0x402d010 reads, and the virtual-call target the
// registration invokes as (*(*(queue+0)) + 0x18).
std::string so_queue_text(std::uintptr_t queue) {
    if (!queue) return " queue 0";
    const std::uintptr_t holder = static_cast<std::uintptr_t>(executor_watch_read_u64(queue));
    const std::uintptr_t vtable =
        holder ? static_cast<std::uintptr_t>(executor_watch_read_u64(holder)) : 0;
    std::string text = " queue " + executor_watch_hex(queue) + " id " +
                       std::to_string(executor_watch_read_u32(queue + 0x14)) + " table " +
                       executor_watch_hex(executor_watch_read_u64(queue + 0x18)) + " holder " +
                       executor_watch_hex(holder) + " vtable " + sr_label(vtable);
    if (vtable) {
        text += " slot+0x18 " + sr_label(executor_watch_read_u64(vtable + 0x18)) + " slot+0x20 " +
                sr_label(executor_watch_read_u64(vtable + 0x20));
    }
    text += sr_qwords(" queue", queue, 4);
    return text;
}

std::string so_hits_summary() {
    StreamOpen& probe = stream_open();
    std::string text = " hits";
    for (unsigned i = 0; i < kSoSiteCount; ++i) {
        const unsigned hits = probe.hits[i].load(std::memory_order_relaxed);
        if (!hits) continue;
        text += std::string(" ") + kSoSites[i].name + " " + std::to_string(hits);
    }
    text += " watch";
    for (unsigned i = 0; i < 4; ++i)
        text += " " + std::to_string(probe.watch_hits[i].load(std::memory_order_relaxed));
    return text;
}

std::string so_state_text() { return sr_state_text(); }

// The table and entry in full, for the first command-start hit and the 2 s sampler.
void so_store_stream(std::uintptr_t stream) {
    StreamOpen& probe = stream_open();
    if (!stream) return;
    probe.stream.store(stream, std::memory_order_relaxed);
    std::uint32_t id = 0;
    const std::uintptr_t table = so_stream_table(stream, &id);
    if (table) {
        probe.table.store(table, std::memory_order_relaxed);
        const std::uintptr_t entry = so_table_entry(table, id);
        if (entry) probe.entry.store(entry, std::memory_order_relaxed);
    }
}

// One logged execution-probe hit. Everything printed is read from title memory at the hit.
void so_log_site(unsigned slot, CONTEXT& context) {
    StreamOpen& probe = stream_open();
    if (probe.lines.fetch_add(1, std::memory_order_relaxed) >= kSoTotalLines) return;
    const std::uint64_t rcx = context.Rcx;
    const std::uint64_t rdx = context.Rdx;
    const std::uint64_t r8 = context.R8;
    const std::uint64_t r9 = context.R9;
    const std::uintptr_t our_table = probe.table.load(std::memory_order_relaxed);
    char head[256];
    std::snprintf(head, sizeof head,
                  "[rt] streamopen t=%.3f (%.1f s) site %s hit %u tid %lu rcx 0x%llx rdx 0x%llx r8 0x%llx r9 0x%llx",
                  executor_watch_seconds(),
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - sampler_epoch()).count(),
                  kSoSites[slot].name, probe.hits[slot].load(std::memory_order_relaxed),
                  static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(rcx),
                  static_cast<unsigned long long>(rdx), static_cast<unsigned long long>(r8),
                  static_cast<unsigned long long>(r9));
    std::string text = head;
    switch (slot) {
        case kSoSubscriberStart: {  // subscriber in rcx; its stream object at +0x50
            probe.subscriber.store(static_cast<std::uintptr_t>(rcx), std::memory_order_relaxed);
            const std::uintptr_t stream = rcx ? static_cast<std::uintptr_t>(executor_watch_read_u64(rcx + 0x50)) : 0;
            if (stream) {
                probe.stream.store(stream, std::memory_order_relaxed);
                std::uint32_t id = 0;
                const std::uintptr_t table = so_stream_table(stream, &id);
                if (table) probe.table.store(table, std::memory_order_relaxed);
            }
            text += sr_subscriber_text(static_cast<std::uintptr_t>(rcx));
            text += sr_stream_text(stream) + so_gate_text(stream) + so_state_text();
            break;
        }
        case kSoStreamSubmit: {  // subscriber in rcx, command id in rdx, flag in r8b
            const std::uintptr_t stream = rcx ? static_cast<std::uintptr_t>(executor_watch_read_u64(rcx + 0x50)) : 0;
            text += " subscriber 0x" + executor_watch_hex(rcx).substr(2) + " command-id " +
                    std::to_string(rdx & 0xFFFFFFFFu) + " flag " + std::to_string(r8 & 0xFF) + " stream " +
                    executor_watch_hex(stream) + " request-before " +
                    executor_watch_hex(rcx ? executor_watch_read_u64(rcx + 0x58) : 0);
            break;
        }
        case kSoPendingRegister:  // stream in rcx, request in rdx, key in r8d
            if (rcx) so_store_stream(static_cast<std::uintptr_t>(rcx));
            if (rdx) probe.request.store(static_cast<std::uintptr_t>(rdx), std::memory_order_relaxed);
            text += sr_stream_text(static_cast<std::uintptr_t>(rcx)) +
                    sr_request_text(static_cast<std::uintptr_t>(rdx)) + so_gate_text(static_cast<std::uintptr_t>(rcx)) +
                    " key 0x" + executor_watch_hex(r8 & 0xFFFFFFFFu).substr(2);
            break;
        case kSoCommandStart:  // stream in rcx, command id in rdx
            so_store_stream(static_cast<std::uintptr_t>(rcx));
            probe.point_version.fetch_add(1, std::memory_order_relaxed);
            text += sr_stream_text(static_cast<std::uintptr_t>(rcx)) +
                    so_gate_text(static_cast<std::uintptr_t>(rcx)) +
                    " queue" + so_queue_text(static_cast<std::uintptr_t>(rcx) + 0xe8) + " command-id " +
                    std::to_string(rdx & 0xFFFFFFFFu);
            break;
        case kSoHandlerLookup: {  // handler count in ecx, command array in rdx, command id in r8d
            text += " count " + std::to_string(rcx & 0xFFFFFFFFu) + " array " + executor_watch_hex(rdx) +
                    " command-id " + std::to_string(r8 & 0xFFFFFFFFu) + " array+0xb0-0xb8";
            if (rdx) {
                text += " " + executor_watch_hex(executor_watch_read_u64(rdx + 0xb0)) + " " +
                        executor_watch_hex(executor_watch_read_u64(rdx + 0xb8));
            }
            break;
        }
        case kSoRegisterOpen: {  // queue in rcx, arg2 in rdx, arg3 in r8, handler in r9
            const std::uintptr_t queue = static_cast<std::uintptr_t>(rcx);
            text += so_queue_text(queue) + " arg2 " + executor_watch_hex(rdx) + " arg3 " + executor_watch_hex(r8) +
                    " handler " + executor_watch_hex(r9);
            const std::uint32_t id = executor_watch_read_u32(queue + 0x14);
            const std::uintptr_t table = static_cast<std::uintptr_t>(executor_watch_read_u64(queue + 0x18));
            text += " rec+0x8 " + executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x8))) +
                    " rec+0x10 " + executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x10))) +
                    " rec+0x18 " + executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x18)));
            break;
        }
        case kSoRegisterCancel:  // queue in rcx
            text += so_queue_text(static_cast<std::uintptr_t>(rcx));
            break;
        case kSoOpenGate: {  // id in ecx, table in rdx
            const std::uint32_t id = static_cast<std::uint32_t>(rcx);
            const std::uintptr_t table = static_cast<std::uintptr_t>(rdx);
            const std::uintptr_t engine = so_table_engine(table, id);
            const std::uintptr_t entry = so_table_entry(table, id);
            text += " gate id " + std::to_string(id) + " table " + executor_watch_hex(table) + " engine " +
                    executor_watch_hex(engine) + " entry " + executor_watch_hex(entry) + " state+0xac " +
                    std::to_string(entry ? executor_watch_read_u32(entry + 0xac) : 0) + " open " +
                    std::string(so_gate_open(engine, entry) ? "yes" : "no");
            if (table && table == our_table) text += " OUR-TABLE";
            if (entry && entry == probe.entry.load(std::memory_order_relaxed)) text += " OUR-ENTRY";
            break;
        }
        case kSoRegistryEngineLookup: {  // id in ecx, table in rdx
            const std::uint32_t id = static_cast<std::uint32_t>(rcx);
            const std::uintptr_t table = static_cast<std::uintptr_t>(rdx);
            const std::uintptr_t engine = so_table_engine(table, id);
            text += " lookup id " + std::to_string(id) + " table " + executor_watch_hex(table) + " engine " +
                    executor_watch_hex(engine);
            if (engine) text += so_engine_text(engine);
            if (table && table == our_table) text += " OUR-TABLE";
            break;
        }
        case kSoQueueVtableSite: {  // queue in rsi (the site is after the argument copy), just before the +0x18 call
            const std::uintptr_t queue = static_cast<std::uintptr_t>(context.Rsi);
            const std::uintptr_t holder = static_cast<std::uintptr_t>(executor_watch_read_u64(queue));
            const std::uintptr_t owner = holder >= 0x10 ? holder - 0x10 : 0;
            text += so_queue_text(queue);
            if (owner) text += sr_stream_text(owner) + so_gate_text(owner);
            text += so_state_text();
            break;
        }
        case kSoEngineState: {  // engine object in rcx, second argument in rdx, flag in r8b
            text += so_engine_text(static_cast<std::uintptr_t>(rcx)) + " arg2 " + executor_watch_hex(rdx) +
                    " flag " + std::to_string(r8 & 0xFF) + " state " +
                    std::to_string(rcx ? executor_watch_read_u32(rcx + 0x58) : 0);
            break;
        }
        case kSoRecordKeyLookup: {  // id in ecx, table in rdx (reads +0x8+(id-1)*0xc0)
            const std::uint32_t id = static_cast<std::uint32_t>(rcx);
            const std::uintptr_t table = static_cast<std::uintptr_t>(rdx);
            text += " id " + std::to_string(id) + " table " + executor_watch_hex(table) + " record+0x8 " +
                    executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x8)));
            if (table && table == our_table) text += " OUR-TABLE";
            break;
        }
        case kSoRegistryUpdate: {  // queue in rcx, parameters rdx/r8, flag r9
            const std::uintptr_t queue = static_cast<std::uintptr_t>(rcx);
            const std::uint32_t id = executor_watch_read_u32(queue + 0x14);
            const std::uintptr_t table = static_cast<std::uintptr_t>(executor_watch_read_u64(queue + 0x18));
            text += " id " + std::to_string(id) + " table " + executor_watch_hex(table) + " rec+0x8 " +
                    executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x8))) + " rec+0x10 " +
                    executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x10))) + " rec+0x18 " +
                    executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x18))) + " arg2 " +
                    executor_watch_hex(rdx) + " arg3 " + executor_watch_hex(r8) + " flag " +
                    std::to_string(r9 & 0xFF);
            if (table && table == our_table) text += " OUR-TABLE";
            break;
        }
        case kSoRegistryClose: {  // queue-like object in rcx
            const std::uintptr_t queue = static_cast<std::uintptr_t>(rcx);
            const std::uint32_t id = executor_watch_read_u32(queue + 0x14);
            const std::uintptr_t table = static_cast<std::uintptr_t>(executor_watch_read_u64(queue + 0x18));
            text += " id " + std::to_string(id) + " table " + executor_watch_hex(table) + " rec+0x8 " +
                    executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x8))) + " rec+0x10 " +
                    executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x10))) + " rec+0x18 " +
                    executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x18)));
            if (table && table == our_table) text += " OUR-TABLE";
            break;
        }
        case kSoRegistrySetJob:     // id in ecx, table in rdx, value in r8
        case kSoRegistrySetEngine:
        case kSoRegistrySetEntry:
        case kSoRegistrySetTail: {
            const std::uint32_t id = static_cast<std::uint32_t>(rcx);
            const std::uintptr_t table = static_cast<std::uintptr_t>(rdx);
            static constexpr std::uintptr_t kOffset[4] = {0x8, 0x10, 0x18, 0xb8};
            const std::uintptr_t offset = kOffset[slot - kSoRegistrySetJob];
            const std::uintptr_t address = so_record_slot(table, id, offset);
            text += " id " + std::to_string(id) + " table " + executor_watch_hex(table) + " record+0x" +
                    executor_watch_hex(offset).substr(2) + " <- " + executor_watch_hex(r8) + " (now " +
                    executor_watch_hex(executor_watch_read_u64(address)) + ")";
            if (table && table == our_table) text += " OUR-TABLE";
            break;
        }
        case kSoRegistrySetRecords: {  // id in ecx, table in rdx, vector in r8
            const std::uint32_t id = static_cast<std::uint32_t>(rcx);
            const std::uintptr_t table = static_cast<std::uintptr_t>(rdx);
            const std::uintptr_t vector = static_cast<std::uintptr_t>(r8);
            text += " id " + std::to_string(id) + " table " + executor_watch_hex(table) + " vector " +
                    executor_watch_hex(vector) + " vector-count " +
                    std::to_string(vector ? executor_watch_read_u32(vector + 0x90) : 0) + " record+0xb0 " +
                    executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0xb0)));
            if (table && table == our_table) text += " OUR-TABLE";
            break;
        }
        case kSoStreamPump:  // stream in rcx
            text += sr_stream_text(static_cast<std::uintptr_t>(rcx)) + so_gate_text(static_cast<std::uintptr_t>(rcx));
            break;
        case kSoStreamStep:  // stream in rcx
            text += sr_stream_text(static_cast<std::uintptr_t>(rcx)) + so_gate_text(static_cast<std::uintptr_t>(rcx)) +
                    sr_request_text(static_cast<std::uintptr_t>(executor_watch_read_u64(
                        static_cast<std::uintptr_t>(rcx) + 0x120)));
            break;
        case kSoAsyncEngineStep:  // queue (stream+0xe8) in rcx, out in r8, pending request in r9
            text += so_queue_text(static_cast<std::uintptr_t>(rcx)) + " out " + executor_watch_hex(r8) +
                    " request " + executor_watch_hex(r9) + " engine " +
                    executor_watch_hex(so_table_engine(static_cast<std::uintptr_t>(executor_watch_read_u64(
                                                            static_cast<std::uintptr_t>(rcx) + 0x18)),
                                                        executor_watch_read_u32(static_cast<std::uintptr_t>(rcx) + 0x14)));
            if (r9 && r9 == probe.request.load(std::memory_order_relaxed)) text += " OUR-REQUEST";
            break;
        case kSoCompletionDispatch: {  // stream in rcx, found entry in rdx, out record in r8
            const std::uintptr_t out_record = static_cast<std::uintptr_t>(r8);
            text += sr_stream_text(static_cast<std::uintptr_t>(rcx)) +
                    so_gate_text(static_cast<std::uintptr_t>(rcx)) + " found " + executor_watch_hex(rdx) +
                    " out-record " + executor_watch_hex(out_record) + " out-vtable " +
                    sr_label(out_record ? executor_watch_read_u64(out_record) : 0) + " out-state+0xac " +
                    std::to_string(out_record ? executor_watch_read_u32(out_record + 0xac) : 0) + " out+0x58 " +
                    std::to_string(out_record ? executor_watch_read_u32(out_record + 0x58) : 0);
            if (out_record) {
                text += " out+0x40 " +
                        executor_watch_hex(executor_watch_read_u64(out_record + 0x40)) +
                        sr_qwords(" out", out_record, 6);
            }
            break;
        }
        case kSoCacheComplete:  // cache entry/request in rcx
            text += sr_request_text(static_cast<std::uintptr_t>(rcx));
            if (rcx && rcx == probe.request.load(std::memory_order_relaxed)) text += " OURS";
            break;
        case kSoOwnerBusyCheck:  // stream owner in rcx; returns *(u8*)(owner+0x11a) & 1
            text += " owner " + executor_watch_hex(rcx) + " +0x11a 0x" +
                    executor_watch_hex(rcx ? executor_watch_read_u32(static_cast<std::uintptr_t>(rcx) + 0x118) : 0).substr(2) +
                    " busy " + std::to_string(rcx ? (executor_watch_read_u32(static_cast<std::uintptr_t>(rcx) + 0x118) >> 16) & 0xFF : 0);
            break;
        case kSoRequestStep: {  // request/queue entry in rcx, frame delta in xmm1
            const std::uintptr_t queue = static_cast<std::uintptr_t>(rcx);
            const std::uintptr_t owner = static_cast<std::uintptr_t>(executor_watch_read_u64(queue + 0x30));
            text += so_queue_text(queue) + " owner " + executor_watch_hex(owner) + " owner+0x11a 0x" +
                    executor_watch_hex(owner ? executor_watch_read_u32(owner + 0x118) : 0).substr(2) + " request " +
                    executor_watch_hex(executor_watch_read_u64(queue + 0x120));
            break;
        }
        case kSoRequestConsume: {  // the request consume: request in rcx, walked callbacks +0x20/+0x28
            const std::uintptr_t request = static_cast<std::uintptr_t>(rcx);
            text += sr_request_text(request);
            if (request && request == probe.request.load(std::memory_order_relaxed)) text += " OURS";
            break;
        }
        case kSoCompletionRun: {  // the state-2 completion dispatch: stream in rcx, engine in rdx, handler in r8
            text += sr_stream_text(static_cast<std::uintptr_t>(rcx)) +
                    so_gate_text(static_cast<std::uintptr_t>(rcx)) + " engine " + executor_watch_hex(rdx) + " handler " +
                    executor_watch_hex(r8);
            break;
        }
        case kSoQueueProcess:  // the queue holder's vtable +0x18: id in ecx, table in rdx (the value it returns)
        case kSoQueuePoll: {   // the queue holder's vtable +0x20
            const std::uint32_t id = static_cast<std::uint32_t>(rcx);
            const std::uintptr_t table = static_cast<std::uintptr_t>(rdx);
            text += " id " + std::to_string(id) + " table " + executor_watch_hex(table) + " record-key " +
                    executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x8))) + " record-engine " +
                    executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x10)));
            if (table && table == our_table) text += " OUR-TABLE";
            break;
        }
        default:
            break;
    }
    text += "\n  chain:" + sr_chain(context) + "\n";
    so_print(text);
}

void so_write_byte(unsigned slot, unsigned char value) {
    StreamOpen& probe = stream_open();
    const auto address = probe.address[slot].load(std::memory_order_relaxed);
    if (!address) return;
    DWORD old_protect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *reinterpret_cast<unsigned char*>(address) = value;
        VirtualProtect(reinterpret_cast<void*>(address), 1, old_protect, &old_protect);
    }
}

thread_local unsigned so_repoke_slot = 0;

// The four data watch points for the measured stream, or zero for a point that cannot be armed yet.
void so_points(std::uintptr_t out[4]) {
    StreamOpen& probe = stream_open();
    out[0] = out[1] = out[2] = out[3] = 0;
    const std::uintptr_t stream = probe.stream.load(std::memory_order_relaxed);
    if (!stream) return;
    std::uint32_t id = 0;
    const std::uintptr_t table = so_stream_table(stream, &id);
    if (!table || id == 0 || id > 0x100) return;
    probe.table.store(table, std::memory_order_relaxed);
    out[0] = so_record_slot(table, id, 0x10);
    out[1] = so_record_slot(table, id, 0x18);
    // The stream's +0x118..+0x120 block: the +0x118 state byte, the +0x11a owner-busy bit the request step tests, and
    // the +0x120 pending request slot. The entry's +0xac state block was the section-14 candidate, but the measured
    // registration never writes it, so the third breakpoint watches the busy/state block instead.
    out[2] = stream + 0x118;
    probe.entry.store(so_table_entry(table, id), std::memory_order_relaxed);
    out[3] = so_image_base + kSoLiveControlRva;
}

void so_publish_points(const std::uintptr_t points[4]) {
    StreamOpen& probe = stream_open();
    for (unsigned i = 0; i < 4; ++i) probe.points[i].store(points[i], std::memory_order_relaxed);
    for (unsigned i = 0; i < 3; ++i) probe.point_last[i].store(executor_watch_read_u64(points[i]));
}

// Install the points on the calling thread inside the int3 handler, so the registration's own stores are watched
// without waiting for the sweep.
void so_arm_this_thread(CONTEXT& context) {
    StreamOpen& probe = stream_open();
    if (!probe.watch_enabled.load(std::memory_order_relaxed)) return;
    std::uintptr_t points[4] = {};
    so_points(points);
    if (!points[0] || !points[1]) return;
    so_publish_points(points);
    probe.point_version.fetch_add(1, std::memory_order_relaxed);
    context.Dr0 = points[0];
    context.Dr1 = points[1];
    context.Dr2 = points[2];
    context.Dr3 = points[3];
    context.Dr6 = 0;
    context.Dr7 = sr_watch_dr7(points);
}

// One hardware write-watch hit: writer RIP and the bytes before it (the trap fires after the store), the value now and
// before, the table/entry/engine state at that instant, and the unwound writer chain.
void so_watch_hit(unsigned index, CONTEXT& context) {
    StreamOpen& probe = stream_open();
    const unsigned hit = probe.watch_hits[index].fetch_add(1, std::memory_order_relaxed) + 1;
    static constexpr unsigned kCap[4] = {24, 16, 16, 3};
    const bool log = hit <= kCap[index] || (hit % 4096 == 0);
    if (index == 1) {
        probe.entry.store(static_cast<std::uintptr_t>(
                              executor_watch_read_u64(probe.points[1].load(std::memory_order_relaxed))),
                          std::memory_order_relaxed);
    }
    if (!log) return;
    if (probe.watch_lines.fetch_add(1, std::memory_order_relaxed) >= kSoWatchLines) return;
    const std::uintptr_t address = probe.points[index].load(std::memory_order_relaxed);
    std::uint64_t now = 0;
    const bool read_ok = executor_watch_read_block(address, &now, 8);
    std::uint64_t previous = 0;
    if (index < 3) previous = probe.point_last[index].exchange(now, std::memory_order_relaxed);
    const std::uint64_t rip = context.Rip;
    unsigned char bytes[16] = {};
    char store_bytes[96] = " (unreadable)";
    if (rip >= 16 && executor_watch_read_block(static_cast<std::uintptr_t>(rip) - 16, bytes, 16)) {
        int at = 0;
        for (unsigned i = 0; i < 16 && at + 4 < static_cast<int>(sizeof store_bytes); ++i)
            at += std::snprintf(store_bytes + at, sizeof store_bytes - static_cast<std::size_t>(at), " %02x", bytes[i]);
    }
    static const char* const kNames[4] = {"TABLE +0x10 ENGINE", "TABLE +0x18 ENTRY", "STREAM +0x118 STATE",
                                          "LIVE-CONTROL"};
    char head[320];
    std::snprintf(head, sizeof head,
                  "[rt] streamopen watch t=%.3f (%.1f s) %s hit %u tid %lu writer %s value %s0x%llx (was 0x%llx)",
                  executor_watch_seconds(),
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - sampler_epoch()).count(),
                  kNames[index], hit, static_cast<unsigned long>(GetCurrentThreadId()), sr_label(rip).c_str(),
                  read_ok ? "" : "UNREADABLE ", static_cast<unsigned long long>(now),
                  static_cast<unsigned long long>(previous));
    std::string text = head;
    const std::uintptr_t stream = probe.stream.load(std::memory_order_relaxed);
    text += so_gate_text(stream) + sr_stream_text(stream);
    const std::uintptr_t entry = probe.entry.load(std::memory_order_relaxed);
    if (entry) text += so_entry_text(entry);
    const std::uintptr_t engine = static_cast<std::uintptr_t>(
        executor_watch_read_u64(so_record_slot(probe.table.load(std::memory_order_relaxed),
                                               executor_watch_read_u32(stream + 0xfc), 0x10)));
    if (engine) text += so_engine_text(engine);
    text += so_state_text();
    text += "\n  bytes before writer rip:" + std::string(store_bytes) +
            "\n  rax " + executor_watch_hex(context.Rax) + " rbx " + executor_watch_hex(context.Rbx) + " rcx " +
            executor_watch_hex(context.Rcx) + " rdx " + executor_watch_hex(context.Rdx) + " rsi " +
            executor_watch_hex(context.Rsi) + " rdi " + executor_watch_hex(context.Rdi) +
            "\n  chain:" + sr_chain(context) + "\n";
    so_print(text);
}

LONG CALLBACK so_exception(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    StreamOpen& probe = stream_open();
    if (!probe.ready.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        unsigned slot = kSoSiteCount;
        for (unsigned i = 0; i < kSoSiteCount; ++i)
            if (probe.address[i].load(std::memory_order_relaxed) == at) {
                slot = i;
                break;
            }
        if (slot == kSoSiteCount) return EXCEPTION_CONTINUE_SEARCH;
        const unsigned hit = probe.hits[slot].fetch_add(1, std::memory_order_relaxed) + 1;
        const bool wanted = hit <= kSoSites[slot].cap || (hit % 4096 == 0) ||
                            ((slot == kSoOpenGate || slot == kSoRegistryEngineLookup) &&
                             probe.table.load(std::memory_order_relaxed) ==
                                 static_cast<std::uintptr_t>(context.Rdx));
        if (wanted && probe.logged[slot].fetch_add(1, std::memory_order_relaxed) < 256) so_log_site(slot, context);
        if (slot == kSoSubscriberStart || slot == kSoCommandStart || slot == kSoRegisterOpen) so_arm_this_thread(context);
        so_write_byte(slot, static_cast<unsigned char>(probe.original[slot].load(std::memory_order_relaxed)));
        so_repoke_slot = slot + 1;
        context.EFlags |= 0x100;  // trap after the original instruction, then put the int3 back
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        if (so_repoke_slot) {
            const unsigned slot = so_repoke_slot - 1;
            so_repoke_slot = 0;
            so_write_byte(slot, 0xCC);
            context.EFlags &= ~static_cast<DWORD>(0x100);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        const DWORD dr6 = context.Dr6;
        if ((dr6 & 0xFu) == 0) return EXCEPTION_CONTINUE_SEARCH;
        context.Dr6 = 0;
        for (unsigned index = 0; index < 4; ++index)
            if ((dr6 & (1u << index)) != 0) so_watch_hit(index, context);
        return EXCEPTION_CONTINUE_EXECUTION;  // a data breakpoint does not consume the instruction
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// The arming sweep: waits for the command start to name the stream, configures every thread that has been alive across
// two scans, and keeps picking up threads created later.
void so_start_watch_sweep() {
    std::thread([] {
        StreamOpen& probe = stream_open();
        for (unsigned tick = 0; tick < 12000; ++tick) {
            if (probe.table.load(std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!probe.table.load(std::memory_order_relaxed)) return;
        static DWORD configured[kSoMaxThreads];
        static DWORD seen_tid[kSoMaxThreads];
        static ULONGLONG seen_at[kSoMaxThreads];
        unsigned configured_count = 0, seen_count = 0;
        unsigned last_version = 0;
        const auto find = [](const DWORD* table, unsigned count, DWORD tid) -> int {
            for (unsigned i = 0; i < count; ++i)
                if (table[i] == tid) return static_cast<int>(i);
            return -1;
        };
        std::uintptr_t points[4] = {};
        DWORD dr7 = 0;
        for (;;) {
            const unsigned version = probe.point_version.load(std::memory_order_relaxed);
            const std::uintptr_t table = probe.table.load(std::memory_order_relaxed);
            if (table && (version != last_version || configured_count == 0)) {
                so_points(points);
                if (points[0] && points[1]) {
                    so_publish_points(points);
                    dr7 = sr_watch_dr7(points);
                    configured_count = 0;
                    last_version = version;
                    char log[512];
                    std::snprintf(log, sizeof log,
                                  "[rt] streamopen watch: table 0x%llx DR0 game+0x%x DR1 game+0x%x DR2 game+0x%x "
                                  "DR3 game+0x%x (live control); dr7 0x%lx; arming every thread\n",
                                  static_cast<unsigned long long>(table),
                                  static_cast<unsigned>(points[0] - so_image_base),
                                  static_cast<unsigned>(points[1] - so_image_base),
                                  static_cast<unsigned>(points[2] ? points[2] - so_image_base : 0),
                                  static_cast<unsigned>(points[3] - so_image_base), static_cast<unsigned long>(dr7));
                    so_print(log);
                }
            }
            const ULONGLONG now = GetTickCount64();
            DWORD pending[kSoPendingThreads];
            unsigned pending_count = 0;
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{};
                entry.dwSize = sizeof entry;
                const DWORD pid = GetCurrentProcessId();
                const DWORD self = GetCurrentThreadId();
                if (Thread32First(snapshot, &entry))
                    do {
                        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
                        if (find(configured, configured_count, entry.th32ThreadID) >= 0) continue;
                        const int seat = find(seen_tid, seen_count, entry.th32ThreadID);
                        if (seat < 0) {
                            if (seen_count < kSoMaxThreads) {
                                seen_tid[seen_count] = entry.th32ThreadID;
                                seen_at[seen_count] = now;
                                ++seen_count;
                            }
                            continue;  // first sight: wait one scan before suspending it
                        }
                        if (now - seen_at[seat] < 1000) continue;
                        if (pending_count < kSoPendingThreads) pending[pending_count++] = entry.th32ThreadID;
                    } while (Thread32Next(snapshot, &entry));
                CloseHandle(snapshot);
            }
            for (unsigned i = 0; i < pending_count; ++i)
                if (dr7 && sr_watch_configure_thread(pending[i], points, dr7)) {
                    if (configured_count < kSoMaxThreads) configured[configured_count++] = pending[i];
                }
            probe.watch_armed.store(configured_count, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }).detach();
}

void start_stream_open_probe(std::uintptr_t image_base, std::size_t image_size, bool watch_enabled) {
    (void)image_size;
    StreamOpen& probe = stream_open();
    so_image_base = image_base;
    sr_image_base = image_base;  // the shared read helpers (sr_state_text and friends) are image-relative readers
    probe.watch_enabled.store(watch_enabled, std::memory_order_relaxed);
    unsigned armed = 0;
    std::string list;
    for (unsigned i = 0; i < kSoSiteCount; ++i) {
        auto* const at = reinterpret_cast<unsigned char*>(image_base + kSoSites[i].rva);
        DWORD old_protect = 0;
        if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
            list += std::string(" ") + kSoSites[i].name + "=FAILED";
            continue;
        }
        probe.original[i].store(*at, std::memory_order_relaxed);
        probe.address[i].store(reinterpret_cast<std::uintptr_t>(at), std::memory_order_relaxed);
        *at = 0xCC;
        VirtualProtect(at, 1, old_protect, &old_protect);
        ++armed;
        char item[64];
        std::snprintf(item, sizeof item, " %s=game+0x%x", kSoSites[i].name,
                      static_cast<unsigned>(kSoSites[i].rva));
        list += item;
    }
    AddVectoredExceptionHandler(1, so_exception);
    probe.ready.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "[rt] streamopen: %u/%u int3 site(s) armed:%s; watch %s\n", armed, kSoSiteCount,
                 list.c_str(), watch_enabled ? "on" : "off");
    std::fflush(stderr);
    if (watch_enabled) so_start_watch_sweep();
    // The 10 ms fine sampler: the gate's two inputs, the entry state block, the engine state, the request's first eight
    // qwords and the site counters; one line per change, so the short open window is dated.
    std::thread([] {
        StreamOpen& probe = stream_open();
        std::uint64_t last[20] = {};
        bool have = false;
        for (unsigned tick = 0; tick < 30000; ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const std::uintptr_t stream = probe.stream.load(std::memory_order_relaxed);
            if (!stream) continue;
            std::uint32_t id = 0;
            const std::uintptr_t table = so_stream_table(stream, &id);
            if (!table || id == 0 || id > 0x100) continue;
            const std::uintptr_t record = so_record_slot(table, id, 0);
            const std::uintptr_t engine = static_cast<std::uintptr_t>(executor_watch_read_u64(record + 0x10));
            const std::uintptr_t entry = static_cast<std::uintptr_t>(executor_watch_read_u64(record + 0x18));
            std::uint64_t now[20] = {};
            now[0] = engine;
            now[1] = entry;
            now[2] = executor_watch_read_u64(stream + 0x118);  // +0x118 state byte, +0x11a owner-busy bit
            now[3] = entry ? executor_watch_read_u32(entry + 0xb0) : 0;
            now[4] = engine ? executor_watch_read_u64(engine + 0x40) : 0;
            now[5] = engine ? (executor_watch_read_u32(engine + 0x58) | (std::uint64_t{executor_watch_read_u32(engine + 0x5c)} << 32)) : 0;
            now[6] = static_cast<std::uint64_t>(so_gate_open(engine, entry));
            now[7] = probe.hits[kSoAsyncEngineStep].load(std::memory_order_relaxed);
            now[8] = probe.hits[kSoOpenGate].load(std::memory_order_relaxed);
            now[9] = probe.hits[kSoStreamPump].load(std::memory_order_relaxed);
            const std::uintptr_t request = probe.request.load(std::memory_order_relaxed);
            now[10] = request ? executor_watch_read_u64(request + 0x18) : 0;
            now[11] = request ? executor_watch_read_u64(request + 0x20) : 0;
            now[12] = request ? executor_watch_read_u64(request + 0x28) : 0;
            now[13] = request ? executor_watch_read_u64(request + 0x30) : 0;
            now[14] = request ? executor_watch_read_u64(request + 0x38) : 0;
            now[15] = static_cast<std::uint64_t>(table);
            now[16] = probe.hits[kSoRequestStep].load(std::memory_order_relaxed);
            now[17] = probe.hits[kSoRequestConsume].load(std::memory_order_relaxed);
            now[18] = probe.hits[kSoCompletionRun].load(std::memory_order_relaxed);
            now[19] = executor_watch_read_u64(record + 0x8);
            bool changed = !have;
            for (unsigned i = 0; i < 20; ++i)
                if (now[i] != last[i]) changed = true;
            if (!changed) continue;
            char line[640];
            std::snprintf(line, sizeof line,
                          "[rt] streamopen t=%.3f fine id %u table 0x%llx engine 0x%llx entry 0x%llx stream118 0x%llx "
                          "entryb0 0x%llx engine40 0x%llx engine58 0x%llx open %llu engine-steps %llu gate %llu "
                          "pump %llu req18 0x%llx req20 0x%llx req28 0x%llx req30 0x%llx req38 0x%llx "
                          "steps %llu consume %llu compl-run %llu job 0x%llx",
                          executor_watch_seconds(), id, static_cast<unsigned long long>(table),
                          static_cast<unsigned long long>(engine), static_cast<unsigned long long>(entry),
                          static_cast<unsigned long long>(now[2]), static_cast<unsigned long long>(now[3]),
                          static_cast<unsigned long long>(now[4]), static_cast<unsigned long long>(now[5]),
                          static_cast<unsigned long long>(now[6]), static_cast<unsigned long long>(now[7]),
                          static_cast<unsigned long long>(now[8]), static_cast<unsigned long long>(now[9]),
                          static_cast<unsigned long long>(now[10]), static_cast<unsigned long long>(now[11]),
                          static_cast<unsigned long long>(now[12]), static_cast<unsigned long long>(now[13]),
                          static_cast<unsigned long long>(now[14]), static_cast<unsigned long long>(now[16]),
                          static_cast<unsigned long long>(now[17]), static_cast<unsigned long long>(now[18]),
                          static_cast<unsigned long long>(now[19]));
            if (probe.fine_lines.fetch_add(1, std::memory_order_relaxed) >= kSoFineLines) return;
            so_print(std::string(line) + "\n");
            for (unsigned i = 0; i < 20; ++i) last[i] = now[i];
            have = true;
        }
    }).detach();
    // The 2 s sampler: the three state words, the movie player, the loading record's slot-4 completion dword, and the
    // full table/entry/engine record plus the site counters.
    std::thread([] {
        for (unsigned tick = 1; tick <= 320; ++tick) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (tick % 2 != 0) continue;
            StreamOpen& probe = stream_open();
            const std::uintptr_t stream = probe.stream.load(std::memory_order_relaxed);
            if (!stream) continue;
            if (probe.sampler_lines.fetch_add(1, std::memory_order_relaxed) >= kSoSamplerLines) return;
            std::uint32_t id = 0;
            const std::uintptr_t table = so_stream_table(stream, &id);
            char head[96];
            std::snprintf(head, sizeof head, "[rt] streamopen at %u s:", tick);
            std::string text = head;
            text += so_state_text();
            text += so_gate_text(stream);
            text += sr_stream_text(stream);
            text += sr_request_text(probe.request.load(std::memory_order_relaxed));
            text += so_queue_text(stream + 0xe8);
            if (table) {
                text += " rec+0x8 " + executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0x8)));
                text += " rec+0xb0 " + executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0xb0)));
                text += " rec+0xb8 " + executor_watch_hex(executor_watch_read_u64(so_record_slot(table, id, 0xb8)));
                const std::uintptr_t engine = so_table_engine(table, id);
                if (engine) text += so_engine_text(engine);
                const std::uintptr_t entry = so_table_entry(table, id);
                if (entry) text += so_entry_text(entry);
            }
            text += so_hits_summary();
            text += " armed " + std::to_string(probe.watch_armed.load(std::memory_order_relaxed));
            text += "\n";
            so_print(text);
        }
    }).detach();
    // The 10 s counter reporter: a boot without a writer hit still shows what the probe saw.
    std::thread([] {
        for (unsigned sample = 1; sample <= 30; ++sample) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            std::fprintf(stderr, "[rt] streamopen counters at %u s:%s\n", sample * 10, so_hits_summary().c_str());
            std::fflush(stderr);
        }
    }).detach();
}

// ---- Bink library diagnostic (PALMETTO_BINK_PROBE) ------------------------------------------------------------
// docs/decomp_first_ui.md section 18: the movie/Bink path enters the package's decoder (Bink2Scarlett.dll), and one
// measured arm stopped inside it at decoder+0x5de9 reading 0x1660000005c, i.e. the async service thread loaded the
// qword at HBINK+0x1b8 and used it as the track-array base. This probe measures where that field comes from: the
// decoder's own open builds a 0x6e8-byte context on the stack and copies it into a block it allocates, so the field's
// value is the block allocator's answer for the track-array slot. Three points are logged:
//   image+0x6c33  the first open path, right after the context block is allocated (rax = block, rbx = context);
//   image+0x6f11  the second open path, right after its context block is allocated (rax = block) and after the slot
//                 table has written the track-array pointer into the stack template at [rbp+0x178];
//   image+0x6f88  the second open path, right after the template copy into the context (rdi = context) - the field
//                 HBINK+0x1b8 is final here;
//   image+0x5de2  the async service thread's load of HBINK+0x1b8 (rbx = the list node, the HBINK) - the measured
//                 crash site's preceding instruction.
// The library is a package module the loader maps by name; the export table gives its base, so every printed address
// is an RVA into that library. Opt-in; default path unchanged. No game data: RVAs, field values and counts only.
constexpr std::uint32_t kBinkOpenWithOptionsExportRva = 0x4710;  // measured export RVA (local decompilation)
constexpr unsigned kBinkProbeSites = 4;
constexpr std::uint32_t kBinkProbeSiteRvas[kBinkProbeSites] = {0x6C33, 0x6F11, 0x6F88, 0x5DE2};
constexpr const char* kBinkProbeSiteNames[kBinkProbeSites] = {"open1-context", "open2-block", "open2-context",
                                                              "worker-node"};
constexpr unsigned kBinkProbeLines = 24;

struct BinkProbe {
    std::atomic<bool> ready{false};
    std::atomic<std::uintptr_t> address[kBinkProbeSites] = {};
    unsigned char original[kBinkProbeSites] = {};
    std::atomic<unsigned> hits[kBinkProbeSites] = {};
    std::atomic<unsigned> logged{0};
    std::uintptr_t base = 0;
};

BinkProbe& bink_probe() {
    static BinkProbe probe;
    return probe;
}

thread_local unsigned bink_probe_repoke_slot = 0;

void bink_probe_write_byte(unsigned slot, unsigned char value) {
    BinkProbe& probe = bink_probe();
    const auto address = probe.address[slot].load(std::memory_order_relaxed);
    if (!address) return;
    DWORD old_protect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *reinterpret_cast<unsigned char*>(address) = value;
        VirtualProtect(reinterpret_cast<void*>(address), 1, old_protect, &old_protect);
    }
}

void bink_probe_log_hit(unsigned slot, CONTEXT& context) {
    BinkProbe& probe = bink_probe();
    const std::uintptr_t base = probe.base;
    const auto at = static_cast<std::uintptr_t>(context.Rip);
    char head[224];
    std::snprintf(head, sizeof head,
                  "[rt] bink probe t=%.3f site %s hit %u tid %lu at bink+0x%llx rax 0x%llx rbx 0x%llx rcx 0x%llx "
                  "rdx 0x%llx rdi 0x%llx rbp 0x%llx rsi 0x%llx",
                  executor_watch_seconds(), kBinkProbeSiteNames[slot],
                  probe.hits[slot].load(std::memory_order_relaxed), static_cast<unsigned long>(GetCurrentThreadId()),
                  static_cast<unsigned long long>(at - base), static_cast<unsigned long long>(context.Rax),
                  static_cast<unsigned long long>(context.Rbx), static_cast<unsigned long long>(context.Rcx),
                  static_cast<unsigned long long>(context.Rdx), static_cast<unsigned long long>(context.Rdi),
                  static_cast<unsigned long long>(context.Rbp), static_cast<unsigned long long>(context.Rsi));
    std::string text = head;
    const std::uintptr_t caller = executor_watch_read_u64(static_cast<std::uintptr_t>(context.Rsp));
    text += " caller " + rt::shims::module_offset(caller);
    const std::uintptr_t block = static_cast<std::uintptr_t>(context.Rax);
    const std::uintptr_t candidate = slot == 0   ? static_cast<std::uintptr_t>(context.Rbx)
                                     : slot == 1 ? block
                                     : slot == 2 ? static_cast<std::uintptr_t>(context.Rdi)
                                                 : static_cast<std::uintptr_t>(context.Rbx);
    if (slot == 1) {
        // The allocator has just filled the stack template's track-array slot, and the same instruction's rbp is that
        // template's frame: [rbp+0x178] is the pointer the copy will put at HBINK+0x1b8.
        const std::uint64_t slot_value = executor_watch_read_u64(static_cast<std::uintptr_t>(context.Rbp) + 0x178);
        text += " block " + executor_watch_hex(block) + " templ+0x1b8 " + executor_watch_hex(slot_value);
    } else if (candidate) {
        text += " candidate " + rt::shims::module_offset(candidate);
        text += " +0x0 " + executor_watch_hex(executor_watch_read_u64(candidate));
        text += " +0x4c " + std::to_string(executor_watch_read_u32(candidate + 0x4c));
        text += " +0x12c " + std::to_string(executor_watch_read_u32(candidate + 0x12c));
        const std::uint64_t array = executor_watch_read_u64(candidate + 0x1b8);
        text += " +0x1b8 " + executor_watch_hex(array);
        if (array >= 0x10000) {
            const std::uint32_t status = executor_watch_read_u32(static_cast<std::uintptr_t>(array) + 0x5c);
            text += " array+0x5c " + std::to_string(status);
        }
        if (slot == 3) text += " next " + executor_watch_hex(executor_watch_read_u64(candidate + 0xb8));
    }
    text += "\n";
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

LONG CALLBACK bink_probe_exception(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    BinkProbe& probe = bink_probe();
    if (!probe.ready.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        unsigned slot = kBinkProbeSites;
        for (unsigned i = 0; i < kBinkProbeSites; ++i)
            if (probe.address[i].load(std::memory_order_relaxed) == at) {
                slot = i;
                break;
            }
        if (slot == kBinkProbeSites) return EXCEPTION_CONTINUE_SEARCH;
        probe.hits[slot].fetch_add(1, std::memory_order_relaxed);
        if (probe.logged.fetch_add(1, std::memory_order_relaxed) < kBinkProbeLines) bink_probe_log_hit(slot, context);
        // The service thread is a loop: after a generous number of hits the site is disarmed permanently so the probe
        // cannot become the measurement. The open sites are cold and keep their int3 for a retry.
        if (slot == 3 && probe.hits[slot].load(std::memory_order_relaxed) > 64) {
            bink_probe_write_byte(slot, probe.original[slot]);
            probe.address[slot].store(0, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        bink_probe_write_byte(slot, probe.original[slot]);
        bink_probe_repoke_slot = slot + 1;
        context.EFlags |= 0x100;  // trap after the original instruction, then put the int3 back
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        if (!bink_probe_repoke_slot) return EXCEPTION_CONTINUE_SEARCH;
        const unsigned slot = bink_probe_repoke_slot - 1;
        bink_probe_repoke_slot = 0;
        bink_probe_write_byte(slot, 0xCC);
        context.EFlags &= ~static_cast<DWORD>(0x100);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Arms the probe. The library is looked up by the name the loader mapped, and its export table supplies the base; a
// module that is not mapped yet (the boot maps it at ~0.9 s) simply leaves the probe unarmed.
void start_bink_probe() {
    BinkProbe& probe = bink_probe();
    void* const module = rt::shims::find_loaded_bundled_module("bink2scarlett.dll");
    bool owns = false;
    void* const open = module ? rt::shims::get_bundled_proc_address(module, "BinkOpenWithOptions", owns) : nullptr;
    if (!open) {
        std::fprintf(stderr,
                     "[rt] bink probe: Bink2Scarlett.dll is not mapped through the bundled loader (module %p); not "
                     "armed\n",
                     module);
        std::fflush(stderr);
        return;
    }
    probe.base = reinterpret_cast<std::uintptr_t>(open) - kBinkOpenWithOptionsExportRva;
    void* const handler = AddVectoredExceptionHandler(1, bink_probe_exception);
    unsigned installed = 0;
    for (unsigned i = 0; i < kBinkProbeSites; ++i) {
        auto* const at = reinterpret_cast<unsigned char*>(probe.base + kBinkProbeSiteRvas[i]);
        DWORD old = 0;
        if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) continue;
        probe.original[i] = *at;
        *at = 0xCC;
        VirtualProtect(at, 1, old, &old);
        probe.address[i].store(reinterpret_cast<std::uintptr_t>(at), std::memory_order_relaxed);
        ++installed;
    }
    probe.ready.store(installed != 0, std::memory_order_relaxed);
    std::fprintf(stderr, "[rt] bink probe: base bink+0x0 = 0x%llx, %u of %u int3 site(s) installed (handler %p)\n",
                 static_cast<unsigned long long>(probe.base), installed, kBinkProbeSites, handler);
    std::fflush(stderr);
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
        // PALMETTO_TITLE_CLASS0A_SIZE=1: title-side mitigation for the class-0xa zero-byte constant-table upload
        // (user-approved; docs/decomp_first_ui.md section 16). The title's own row-stride switch image+0x1555C10 has no
        // case for the class its format table gives the 256x1 float4 table, so the upload asks its allocator for 0
        // bytes and then copies 0x1000 bytes over the live command object (docs/vertex_buffer_va.md sections 14-16).
        // This redirects the stride switch's direct callers to a trampoline that answers width*16 for class 0xa only
        // and jumps to the original bytes for every other class; installed after the image is mapped and before any
        // image code runs, uninstalled when the run returns. Off by default.
        palmetto::title_class0a::ScopedPatch class0a_size;
        if (palmetto::title_class0a::switch_enabled(std::getenv("PALMETTO_TITLE_CLASS0A_SIZE"))) {
            class0a_size.patch =
                palmetto::title_class0a::install_row_stride_patch(main_module.base, main_module.size);
            if (class0a_size.patch.ok)
                std::fprintf(stderr,
                             "[rt] class-0xa size mitigation: image+0x%x redirected at %u direct call site(s); class 0xa "
                             "row stride = width*0x10, every other class runs the original bytes\n",
                             palmetto::title_class0a::kRowStrideRva, class0a_size.patch.site_count);
            else
                std::fprintf(stderr,
                             "[rt] class-0xa size mitigation: NOT installed (no executable direct caller of "
                             "image+0x%x could be redirected); the default path runs\n",
                             palmetto::title_class0a::kRowStrideRva);
            std::fflush(stderr);
        }
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
        // PALMETTO_MOVIE_PROBE=<group>: bounded opt-in int3 entry probes on the movie/Bink path
        // (docs/decomp_first_ui.md sections 13-15). "player" is the four middleware play steps the component's
        // per-player update drives: per-frame decode 0x2EF0E0, probe/metadata open 0x442EA70, playback open 0x199EF20
        // and frame-buffer registration 0x442F760. "player2" is the four steps around them: the player prepare
        // 0x199EF10, the player wrapper 0x18AA500, the frame-buffer configure 0x442F0B0 and the finalise-to-playing
        // step 0x5536E00 (the state machine's 8 -> 9 transition). "trigger" is the game-side request chain: module
        // init 0x577F500, play request 0x3A16260, descriptor helper 0x18501E0 and stop 0x3A1A860. "frontend" is the
        // front-end manager the request's UI owner runs through: manager create/init 0x3DA1A80, manager teardown
        // 0x3DA2FB0, member destroy 0x3DADD00 and the last service getter of the per-frame render FUN_1403DB3F0
        // 0x3DB492 (the site whose indirect jump is the measured stop). Same one-shot machinery and logging as
        // PALMETTO_POKE_LOG; a hit means the measured boot reached a path that the static negative says it does not,
        // so the probe set is a reach signal, not a hot-path trace. Off when unset.
        constexpr std::uint32_t kMoviePlayerProbeSites[] = {0x2EF0E0, 0x442EA70, 0x199EF20, 0x442F760};
        constexpr std::uint32_t kMoviePlayerStep2ProbeSites[] = {0x199EF10, 0x18AA500, 0x442F0B0, 0x5536E00};
        constexpr std::uint32_t kMovieTriggerProbeSites[] = {0x577F500, 0x3A16260, 0x18501E0, 0x3A1A860};
        constexpr std::uint32_t kMovieFrontEndProbeSites[] = {0x3DA1A80, 0x3DA2FB0, 0x3DADD00, 0x3DB492};
        auto poke_log_sites = [&main_module](const std::uint32_t* rvas, unsigned count) {
            std::uintptr_t addresses[4] = {};
            unsigned char originals[4] = {};
            unsigned installed = 0;
            for (; installed < count && installed < 4; ++installed) {
                auto* const at = main_module.base + rvas[installed];
                DWORD old = 0;
                if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) break;
                originals[installed] = *at;
                *at = 0xCC;
                VirtualProtect(at, 1, old, &old);
                addresses[installed] = reinterpret_cast<std::uintptr_t>(at);
                std::printf("[rt] poked a register log at image+0x%llx (byte now %02x, was %02x)\n",
                            static_cast<unsigned long long>(rvas[installed]), *at, originals[installed]);
            }
            rt::startup_trace::set_poke_log(addresses, originals, installed);
            std::fflush(stdout);
            return installed;
        };
        if (const DWORD size = GetEnvironmentVariableW(L"PALMETTO_POKE_LOG", nullptr, 0); size > 1) {
            std::wstring value(size, L'\0');
            const DWORD n = GetEnvironmentVariableW(L"PALMETTO_POKE_LOG", value.data(), size);
            if (n > 0 && n < size) value.resize(n);
            std::uint32_t rvas[4] = {};
            unsigned count = 0;
            size_t start = 0;
            while (start <= value.size() && count < 4) {
                const size_t comma = value.find(L',', start);
                const std::wstring piece =
                    value.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                if (!piece.empty()) rvas[count++] = static_cast<std::uint32_t>(
                    std::wcstoull(piece.c_str(), nullptr, 16));
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
            poke_log_sites(rvas, count);
        }
        if (const DWORD size = GetEnvironmentVariableW(L"PALMETTO_MOVIE_PROBE", nullptr, 0); size > 1) {
            std::wstring value(size, L'\0');
            const DWORD n = GetEnvironmentVariableW(L"PALMETTO_MOVIE_PROBE", value.data(), size);
            if (n > 0 && n < size) value.resize(n);
            if (value == L"player") {
                std::printf("[rt] PALMETTO_MOVIE_PROBE=player: %u movie play-step probe(s)\n",
                            poke_log_sites(kMoviePlayerProbeSites, 4));
            } else if (value == L"player2") {
                std::printf("[rt] PALMETTO_MOVIE_PROBE=player2: %u movie frame-buffer/finalise probe(s)\n",
                            poke_log_sites(kMoviePlayerStep2ProbeSites, 4));
            } else if (value == L"trigger") {
                std::printf("[rt] PALMETTO_MOVIE_PROBE=trigger: %u movie request-chain probe(s)\n",
                            poke_log_sites(kMovieTriggerProbeSites, 4));
            } else if (value == L"frontend") {
                std::printf("[rt] PALMETTO_MOVIE_PROBE=frontend: %u front-end manager probe(s)\n",
                            poke_log_sites(kMovieFrontEndProbeSites, 4));
            } else {
                std::printf("[rt] PALMETTO_MOVIE_PROBE: unknown group (use player, player2, trigger or frontend); no "
                            "probe installed\n");
            }
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
        // PALMETTO_STAGE3_PUMP=<seconds,...>: wait for the state machine's current stage to read 3, then unwind every
        // other thread's call chain at those offsets after it and print the chains that reach the pump, the transition
        // executor or the per-frame heartbeat tick (diagnostic; see the stage-3 pump probe above). Off when unset.
        if (const char* spec = std::getenv("PALMETTO_STAGE3_PUMP"); spec && *spec)
            start_stage3_pump_probe(reinterpret_cast<std::uintptr_t>(main_module.base), spec);
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
        // PALMETTO_STAGE2_DISPATCH=<seconds>: date the stage-2 loader batch's dispatch - the armed dword, the scheduler
        // queue ring (head/tail/lock), the ready set and the registered jobs' state/counter fields - at 1 ms from the
        // named second, and capture every thread's stack once the wait is entered (diagnostic; see the dispatch probe
        // above). Off when unset.
        if (const char* spec = std::getenv("PALMETTO_STAGE2_DISPATCH"); spec && *spec) {
            const double at_seconds = std::strtod(spec, nullptr);
            if (at_seconds >= 0.0)
                start_stage2_dispatch_probe(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size,
                                            at_seconds);
        }
        // PALMETTO_STAGE2_READY=<seconds>: resolve the ready-set job ids through the queue's id->job table to their
        // class lane, class, dependency graph, work function and per-id request mask, dump the queue's class-lane
        // masks and the tracker's registered job table once at the named second and again 8 s later (diagnostic; see
        // the ready-set probe above). Off when unset.
        if (const char* spec = std::getenv("PALMETTO_STAGE2_READY"); spec && *spec) {
            const double at_seconds = std::strtod(spec, nullptr);
            if (at_seconds >= 0.0)
                start_stage2_ready_probe(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size,
                                         at_seconds);
        }
        // PALMETTO_LOADER_HOOK=1: inline entry detours on the loader begin/end/re-init, with per-call records
        // (diagnostic; see the loader entry detours above). Off by default.
        if (const char* hook_switch = std::getenv("PALMETTO_LOADER_HOOK"); hook_switch && *hook_switch &&
            *hook_switch != '0')
            start_loader_hooks(reinterpret_cast<std::uintptr_t>(main_module.base));
        std::printf("running %s\n", main_module.name.c_str());
        // PALMETTO_TIMELINE's clock origin: the runtime's timeline lines (vulkan_replay.h) print steady_clock
        // seconds; this line is printed at the run's start so a trace-relative second can be mapped onto them when
        // the boot stops (docs/vertex_buffer_va.md section 11). One line, only when the switch is on.
        if (const char* timeline = std::getenv("PALMETTO_TIMELINE"); timeline && *timeline && *timeline != '0') {
            const double now =
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            std::fprintf(stderr, "[rt] timeline origin t=%.3f (the run's start; trace time counts from trace open)\n", now);
            std::fflush(stderr);
        }
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
        // PALMETTO_SLOT_WATCH=<slot>: a hardware 8-byte write watch on the session-3 loading record's slot completion
        // dword (default slot 4 = game+0x78ACB20), with a live control on the record's own slot 0 tick and a method
        // control on a probe-owned slot, so the writer of the value the session-3 poll waits for is named in one boot
        // (diagnostic; see the loading-record writer watch above). Off by default.
        if (const char* watch = std::getenv("PALMETTO_SLOT_WATCH"); watch && *watch && *watch != '0')
            start_slot_watch(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size, watch);
        // PALMETTO_S3DISPATCH=1: int3 sites on the session-3 record's two completion callers (the streaming subscriber
        // completion vtable +0x60 and the task callback), on the subscriber start and the job/async submission it runs,
        // on the global event bus raise and the subscriber's registered 0xe handler, plus the completion writer as a
        // control; a 2 s sampler prints the subscriber's async handle state and the record/state words, so one boot
        // separates a finished job with no dispatched completion from a job that never finished (diagnostic; see the
        // session-3 completion dispatch probe above). Off by default.
        if (const char* dispatch = std::getenv("PALMETTO_S3DISPATCH"); dispatch && *dispatch && *dispatch != '0')
            start_s3_dispatch_probe(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size);
        // PALMETTO_STREAMREAP=1: int3 probes and samplers on the session-3 stream request's completion producer - the
        // request/stream class vtable slots, the request cache's key/lookup/completion family, the streaming manager's
        // job reap and the manager global game+0x7455F40 - so one boot names whether a producer runs for this request;
        // PALMETTO_STREAMREAP_WATCH=1 adds the hardware write watch on request +0x18/+0x20 and the stream pending slot
        // +0x120, with the loading record's slot-4 poll tick as the same-boot title-store live control. Off by default.
        if (const char* spec = std::getenv("PALMETTO_STREAMREAP"); spec && *spec && *spec != '0') {
            const char* watch = std::getenv("PALMETTO_STREAMREAP_WATCH");
            start_stream_reap_probe(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size,
                                    watch && *watch && *watch != '0');
        }
        // PALMETTO_STREAMOPEN=1: the stream open-transition probe - int3 sites on the manager-table gate
        // (game+0x45e640), its registry lookup, the command start and its registration/open path (0x402f1c0,
        // 0x402d010), the async registry setters the registration calls, the engine's state/step/completion sites and
        // the request step/consume/owner-busy checks, plus a 10 ms sampler; PALMETTO_STREAMOPEN_WATCH=1 (default when
        // the probe is on) adds the hardware write watch on the gate's two table qwords and the stream's +0x118 state
        // block, with the session-3 poll's slot-4 tick as the same-boot live control. Diagnostics only; off by default.
        if (const char* spec = std::getenv("PALMETTO_STREAMOPEN"); spec && *spec && *spec != '0') {
            const char* watch = std::getenv("PALMETTO_STREAMOPEN_WATCH");
            const bool watch_on = !watch || (*watch && *watch != '0');
            start_stream_open_probe(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size, watch_on);
        }
        // PALMETTO_LOADER_WATCH=1: hardware data watchpoints on the loader object's armed flag (+0x78) and in-flight
        // count (+0x64), so the storing title code and its stack are captured at each transition (diagnostic; see the
        // loader write watch above). Off by default.
        if (const char* watch = std::getenv("PALMETTO_LOADER_WATCH"); watch && *watch && *watch != '0')
            start_loader_watch(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size, 0x79F6D70ull);
        // PALMETTO_EXECUTOR_WATCH=1: the renderer executor's hardware breakpoint pair (executor entry and the title's
        // block free) plus the tracked object's first-qword hardware write watch (diagnostic; see the renderer
        // executor watch above). Off by default.
        if (const char* watch = std::getenv("PALMETTO_EXECUTOR_WATCH"); watch && *watch && *watch != '0')
            start_executor_watch(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size);
        // PALMETTO_OBJECT_WRITE_WATCH=1: an int3 on the renderer executor's entry plus a page write watch on the
        // object the executor virtual-calls through, so the store that replaces its vtable is named in-process
        // without a debugger (diagnostic; see the title renderer-object write watch above). Off by default.
        if (const char* watch = std::getenv("PALMETTO_OBJECT_WRITE_WATCH"); watch && *watch && *watch != '0')
            start_title_write_watch(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size);
        // PALMETTO_WRITER_WATCH=1: an int3 on the renderer executor's entry (no debug register at the site) plus a
        // one-page write watch on the object the executor virtual-calls through, armed from each pass, so the store
        // that replaces its vtable is named in-process (diagnostic; see the writer watch above).
        if (const char* watch = std::getenv("PALMETTO_WRITER_WATCH"); watch && *watch && *watch != '0')
            start_writer_watch(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size);
        // PALMETTO_ALLOC_WATCH=1: int3 sites on the renderer executor entry (the stage gate), the flush's job
        // allocation and the constant helper's block allocation, so the 0x1000-byte table block's requested size and
        // its range against the live command object are measured at the allocation (diagnostic; see the allocation
        // trace above). Off by default; PALMETTO_ALLOC_WATCH_FREE=1 adds the allocator's free entry.
        if (const char* watch = std::getenv("PALMETTO_ALLOC_WATCH"); watch && *watch && *watch != '0')
            start_alloc_watch(reinterpret_cast<std::uintptr_t>(main_module.base), main_module.size);
        // PALMETTO_BINK_PROBE=1: int3 points on the package Bink decoder's open (its block allocation and the context's
        // HBINK+0x1b8 write) and on the async service thread's HBINK+0x1b8 load, so a boot measures where the track-array
        // pointer comes from; the library base is read from its export table (diagnostic; see the probe above). Off by
        // default.
        if (const char* bink = std::getenv("PALMETTO_BINK_PROBE"); bink && *bink && *bink != '0') start_bink_probe();
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
