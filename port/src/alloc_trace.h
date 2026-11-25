#pragma once

// Bounded, opt-in attribution for the host memory a boot allocates outside this repository's own shims.
//
// docs/story_load_probe.md measures a 300 s boot whose process private bytes grow from 16.7 GB to 83.6 GB with no file
// read, DirectStorage request or platform call after ~35 s. PALMETTO_MEM_TRACE's counters account for the title's
// XMem entry points, its physical pages and the process heaps, and still leave tens of GB unexplained - so the growth
// arrives through a host entry point this repository does not shim. This header finds it at the two lowest public host
// allocators: ntdll's NtAllocateVirtualMemory/NtFreeVirtualMemory and RtlAllocateHeap/RtlFreeHeap. Every loaded
// module's import slots for those four names are rewritten to a counting thunk, so the driver's own allocations
// (nvoglv64.dll) and every host DLL's are seen as well as this repository's. Each request at or above 1 MiB captures a
// short call stack; per stack the counters keep live bytes (allocated minus freed), the cumulative bytes and the call
// count, and the mem trace thread prints the largest live stacks as module+offset chains.
//
// Off by default (PALMETTO_ALLOC_TRACE=1): no IAT entry is touched and every hook body returns immediately. Bounded:
// at most 512 distinct stacks, at most 16 printed per interval, 16 captured frames each. Read-only over the target: it
// rewrites only this process's own import slots and reads its own stack. No game data, no path: module names and
// offsets only.

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace palmetto::alloc_trace {

constexpr unsigned kFrameCount = 32;
constexpr unsigned kPrintFrames = 24;
constexpr unsigned kMaxStacks = 4096;
constexpr unsigned kPrintStacks = 10;
constexpr std::uint64_t kMinBytes = 1u << 20;  // 1 MiB: below this a stack is not worth capturing

inline std::atomic<bool> g_enabled{false};
inline std::atomic<bool> g_started{false};
inline std::mutex g_mutex;
inline std::atomic<bool> g_overflow{false};
inline std::atomic<std::uint64_t> g_calls{0}, g_bytes{0};
inline std::atomic<std::uint64_t> g_frees{0}, g_freed_bytes{0};
inline thread_local bool g_in_walk = false;  // a note taken while the walk itself allocates is not a title request

struct Entry {
    std::uint64_t hash = 0;
    std::uint64_t live = 0;
    std::uint64_t bytes = 0;
    std::uint64_t count = 0;
    std::uint64_t peak = 0;
    const char* entry_point = "";
    unsigned frame_count = 0;
    void* frames[kFrameCount] = {};
};

inline std::vector<Entry> g_entries;

struct Live {
    std::uint64_t size = 0;
    std::uint64_t hash = 0;
};

// The live blocks at or above the threshold, by base address: the free side of each pair takes its bytes back from the
// stack entry it was allocated on, which is what turns a cumulative total into an owner.
inline std::unordered_map<std::uintptr_t, Live> g_live;

inline Entry* find_entry(std::uint64_t hash) {
    for (Entry& entry : g_entries)
        if (entry.hash == hash) return &entry;
    return nullptr;
}

// A leaf lock over a bounded table. A request that arrives while another thread holds the lock is counted in the
// totals only, never waited for: these are host allocators on the title's and the driver's hot paths.
inline void note_alloc(const char* entry_point, std::uintptr_t address, std::uint64_t bytes);
inline void note_free(std::uintptr_t address) {
    if (!g_enabled.load(std::memory_order_relaxed) || g_in_walk || !address) return;
    static thread_local bool busy = false;
    if (busy) return;
    busy = true;
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        busy = false;
        return;
    }
    const auto it = g_live.find(address);
    if (it != g_live.end()) {
        const Live live = it->second;
        if (Entry* entry = find_entry(live.hash)) {
            entry->live = entry->live >= live.size ? entry->live - live.size : 0;
            g_frees.fetch_add(1, std::memory_order_relaxed);
            g_freed_bytes.fetch_add(live.size, std::memory_order_relaxed);
        }
        g_live.erase(it);
    }
    busy = false;
}

inline void note_alloc(const char* entry_point, std::uintptr_t address, std::uint64_t bytes) {
    if (!g_enabled.load(std::memory_order_relaxed) || g_in_walk || bytes < kMinBytes || !address) return;
    static thread_local bool busy = false;
    if (busy) return;  // the record's own vector growth may allocate through the same entry points
    busy = true;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(bytes, std::memory_order_relaxed);
    void* frames[kFrameCount] = {};
    const unsigned count = RtlCaptureStackBackTrace(2, kFrameCount, frames, nullptr);
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned i = 0; i < count; ++i) {
        hash ^= reinterpret_cast<std::uintptr_t>(frames[i]);
        hash *= 1099511628211ull;
    }
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        busy = false;
        return;
    }
    Entry* entry = find_entry(hash);
    if (!entry) {
        if (g_entries.size() >= kMaxStacks) {
            g_overflow.store(true, std::memory_order_relaxed);
            busy = false;
            return;
        }
        Entry made;
        made.hash = hash;
        made.entry_point = entry_point;
        made.frame_count = count;
        for (unsigned i = 0; i < count; ++i) made.frames[i] = frames[i];
        g_entries.push_back(made);
        entry = &g_entries.back();
    }
    entry->live += bytes;
    entry->bytes += bytes;
    ++entry->count;
    if (entry->live > entry->peak) entry->peak = entry->live;
    g_live[address] = Live{bytes, hash};
    busy = false;
}

// ---- the thunks ------------------------------------------------------------------------------------------------
// Calling conventions are the host's: every argument register and stack slot is forwarded unchanged; only the size and
// the returned base are read. Each original comes from the host module by name.

using NtAllocateVirtualMemoryFn = LONG(NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
using NtFreeVirtualMemoryFn = LONG(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG);
using RtlAllocateHeapFn = PVOID(NTAPI*)(PVOID, ULONG, SIZE_T);
using RtlFreeHeapFn = BOOLEAN(NTAPI*)(PVOID, ULONG, PVOID);

inline std::atomic<void*> g_orig_NtAllocateVirtualMemory{nullptr};
inline std::atomic<void*> g_orig_NtFreeVirtualMemory{nullptr};
inline std::atomic<void*> g_orig_RtlAllocateHeap{nullptr};
inline std::atomic<void*> g_orig_RtlFreeHeap{nullptr};

inline LONG NTAPI trace_NtAllocateVirtualMemory(HANDLE process, PVOID* address, ULONG_PTR zero_bits, PSIZE_T size,
                                                ULONG type, ULONG protect) {
    const auto original =
        reinterpret_cast<NtAllocateVirtualMemoryFn>(g_orig_NtAllocateVirtualMemory.load(std::memory_order_relaxed));
    if (!original) return -1;
    const LONG status = original(process, address, zero_bits, size, type, protect);
    if (status >= 0 && address && *address && size)
        note_alloc("NtAllocateVirtualMemory", reinterpret_cast<std::uintptr_t>(*address),
                   static_cast<std::uint64_t>(*size));
    return status;
}

inline LONG NTAPI trace_NtFreeVirtualMemory(HANDLE process, PVOID* address, PSIZE_T size, ULONG type) {
    const auto original = reinterpret_cast<NtFreeVirtualMemoryFn>(g_orig_NtFreeVirtualMemory.load(std::memory_order_relaxed));
    if (!original) return -1;
    const auto base = address ? reinterpret_cast<std::uintptr_t>(*address) : 0;
    const LONG status = original(process, address, size, type);
    if (status >= 0) note_free(base);
    return status;
}

inline PVOID NTAPI trace_RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T bytes) {
    const auto original = reinterpret_cast<RtlAllocateHeapFn>(g_orig_RtlAllocateHeap.load(std::memory_order_relaxed));
    if (!original) return nullptr;
    PVOID block = original(heap, flags, bytes);
    if (block) note_alloc("RtlAllocateHeap", reinterpret_cast<std::uintptr_t>(block), bytes);
    return block;
}

inline BOOLEAN NTAPI trace_RtlFreeHeap(PVOID heap, ULONG flags, PVOID block) {
    const auto original = reinterpret_cast<RtlFreeHeapFn>(g_orig_RtlFreeHeap.load(std::memory_order_relaxed));
    if (!original) return FALSE;
    const BOOLEAN freed = original(heap, flags, block);
    if (freed) note_free(reinterpret_cast<std::uintptr_t>(block));
    return freed;
}

// ---- import-table rewriting ------------------------------------------------------------------------------------

inline void* resolve_host(const char* module, const char* name) {
    const HMODULE handle = GetModuleHandleA(module);
    return handle ? reinterpret_cast<void*>(GetProcAddress(handle, name)) : nullptr;
}

inline void install_targets() {
    if (!g_orig_NtAllocateVirtualMemory.load())
        g_orig_NtAllocateVirtualMemory.store(resolve_host("ntdll.dll", "NtAllocateVirtualMemory"));
    if (!g_orig_NtFreeVirtualMemory.load())
        g_orig_NtFreeVirtualMemory.store(resolve_host("ntdll.dll", "NtFreeVirtualMemory"));
    if (!g_orig_RtlAllocateHeap.load())
        g_orig_RtlAllocateHeap.store(resolve_host("ntdll.dll", "RtlAllocateHeap"));
    if (!g_orig_RtlFreeHeap.load()) g_orig_RtlFreeHeap.store(resolve_host("ntdll.dll", "RtlFreeHeap"));
}

struct Target {
    const char* name;
    void* thunk;
    std::atomic<void*>* original;
};

inline Target g_targets[] = {
    {"NtAllocateVirtualMemory", reinterpret_cast<void*>(&trace_NtAllocateVirtualMemory),
     &g_orig_NtAllocateVirtualMemory},
    {"NtFreeVirtualMemory", reinterpret_cast<void*>(&trace_NtFreeVirtualMemory), &g_orig_NtFreeVirtualMemory},
    {"RtlAllocateHeap", reinterpret_cast<void*>(&trace_RtlAllocateHeap), &g_orig_RtlAllocateHeap},
    {"RtlFreeHeap", reinterpret_cast<void*>(&trace_RtlFreeHeap), &g_orig_RtlFreeHeap},
};

inline bool readable_range(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof info)) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto end = start + info.RegionSize;
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return at >= start && at <= end && bytes <= end - at;
}

// A single import slot: it is patched only when it currently holds exactly the host export the target names, so a
// module that already routes the call elsewhere (our own shims, a delay-load stub) is left alone.
inline void patch_iat_entry(std::uintptr_t slot_address, void* current, void* thunk_value, void* original) {
    if (!current || !original || current != original || current == thunk_value) return;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot_address), sizeof(void*), PAGE_READWRITE, &old)) return;
    *reinterpret_cast<void**>(slot_address) = thunk_value;
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(slot_address), sizeof(void*), old, &ignored);
}

inline void patch_module(std::uintptr_t base, std::unordered_set<std::uintptr_t>& patched) {
    if (!base || patched.count(base)) return;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!readable_range(dos, sizeof *dos) || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!readable_range(nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.Size || !directory.VirtualAddress) {
        patched.insert(base);
        return;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; readable_range(descriptor, sizeof *descriptor) && descriptor->Name; ++descriptor) {
        const auto lookup = base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
                                                                   : descriptor->FirstThunk);
        const auto iat = base + descriptor->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(lookup);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(iat);
        for (; readable_range(names, sizeof *names) && readable_range(slots, sizeof *slots) &&
               names->u1.AddressOfData;
             ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (!readable_range(import, sizeof import->Name + 1)) continue;
            for (Target& target : g_targets) {
                if (std::strcmp(reinterpret_cast<const char*>(import->Name), target.name) != 0) continue;
                patch_iat_entry(reinterpret_cast<std::uintptr_t>(&slots->u1.Function),
                                reinterpret_cast<void*>(slots->u1.Function), target.thunk,
                                target.original->load(std::memory_order_relaxed));
                break;
            }
        }
    }
    patched.insert(base);
}

inline std::unordered_set<std::uintptr_t> g_patched_modules;

// Every loaded module, once per call: a module loaded after the previous call is patched on the next. Called from the
// mem trace thread, so the walk costs nothing on any title path.
inline void tick() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    g_in_walk = true;
    install_targets();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W module{};
        module.dwSize = sizeof module;
        if (Module32FirstW(snapshot, &module)) {
            do {
                patch_module(reinterpret_cast<std::uintptr_t>(module.modBaseAddr), g_patched_modules);
            } while (Module32NextW(snapshot, &module));
        }
        CloseHandle(snapshot);
    }
    g_in_walk = false;
}

// ---- the report ------------------------------------------------------------------------------------------------

inline std::string stack_text(const Entry& entry) {
    std::string text;
    const unsigned shown = entry.frame_count < kPrintFrames ? entry.frame_count : kPrintFrames;
    for (unsigned i = 0; i < shown; ++i) {
        if (i) text += " <- ";
        char buffer[96] = {};
        // Module name and offset only: a module path or a package address never reaches the log.
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(entry.frames[i]), &module)) {
            char name[MAX_PATH] = {};
            GetModuleFileNameA(module, name, MAX_PATH);
            const char* leaf = std::strrchr(name, '\\');
            leaf = leaf ? leaf + 1 : name;
            const auto offset = reinterpret_cast<std::uintptr_t>(entry.frames[i]) -
                                reinterpret_cast<std::uintptr_t>(module);
            std::snprintf(buffer, sizeof buffer, "%s+0x%llx", leaf, static_cast<unsigned long long>(offset));
        } else {
            std::snprintf(buffer, sizeof buffer, "0x%llx",
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(entry.frames[i])));
        }
        text += buffer;
    }
    return text;
}

// One block of lines per mem trace interval: the totals, then the stacks with the most live bytes. Called with the mem
// trace thread.
inline void print() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::vector<Entry> top;
    std::size_t live_blocks = 0;
    std::uint64_t live_bytes = 0;
    {
        std::lock_guard lock(g_mutex);
        top = g_entries;  // bounded at kMaxStacks; the report's own allocations are below the 1 MiB threshold
        live_blocks = g_live.size();
        for (const Entry& entry : g_entries) live_bytes += entry.live;
    }
    for (std::size_t i = 0; i < top.size(); ++i)
        for (std::size_t j = i + 1; j < top.size(); ++j)
            if (top[j].live > top[i].live) std::swap(top[i], top[j]);
    if (top.size() > kPrintStacks) top.resize(kPrintStacks);
    std::fprintf(stderr,
                 "[rt] alloc trace: %llu allocation(s) >= 1 MiB totalling %.1f MB, %llu free(s) %.1f MB, %.1f MB "
                 "live in %zu block(s), %zu stack(s)%s\n",
                 static_cast<unsigned long long>(g_calls.load()), double(g_bytes.load()) / (1024.0 * 1024.0),
                 static_cast<unsigned long long>(g_frees.load()), double(g_freed_bytes.load()) / (1024.0 * 1024.0),
                 double(live_bytes) / (1024.0 * 1024.0), live_blocks, g_entries.size(),
                 g_overflow.load() ? " (stack table full; later stacks are totals only)" : "");
    for (const Entry& entry : top) {
        if (!entry.live) continue;
        std::fprintf(stderr, "[rt] alloc trace:   %-22s %8.1f MB live, peak %.1f, %llu call(s): %s\n",
                     entry.entry_point, double(entry.live) / (1024.0 * 1024.0),
                     double(entry.peak) / (1024.0 * 1024.0), static_cast<unsigned long long>(entry.count),
                     stack_text(entry).c_str());
    }
}

// PALMETTO_ALLOC_TRACE's state: read once from the environment by the caller.
inline void start(const char* value) {
    if (g_started.exchange(true)) return;
    g_enabled.store(value && *value && *value != '0', std::memory_order_relaxed);
}

}  // namespace palmetto::alloc_trace
