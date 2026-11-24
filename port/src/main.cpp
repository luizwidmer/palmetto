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

struct WaitThreadSample {
    DWORD tid = 0;
    std::uint64_t rip = 0, rsp = 0, rcx = 0, rdx = 0, r8 = 0, r9 = 0, start = 0;
    unsigned stack_count = 0;
    std::uint64_t stack[8] = {};
};

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
        if (++waiting > 24) continue;
        char type[64] = {};
        if (api && wait_takes_handle(api) && !std::strstr(api, "WaitForMultipleObjects"))
            wait_object_type((HANDLE)(std::uintptr_t)sample.rcx, type, sizeof(type));
        detail += "\n  tid " + std::to_string(sample.tid) + " rip " + site;
        if (api) detail += std::string(" wait ") + api;
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
        const auto started = std::chrono::steady_clock::now();
        for (const double at : times) {
            std::this_thread::sleep_until(started + std::chrono::milliseconds(static_cast<long long>(at * 1000.0)));
            char label[32];
            std::snprintf(label, sizeof label, "%.0f", at);
            wait_state_sample_once(image_base, image_size, apis, label);
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
