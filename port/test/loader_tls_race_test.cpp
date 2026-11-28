// Loader TLS race and lock-order contracts for the audit findings F4 and F7, on this tree still open:
//
//   AUDIT-F4  install_on_existing_threads() treats "the helper DLL already gave this thread a TLS block" as
//             "this thread has no block yet": a thread created after the TLS owner is published but before the
//             thread snapshot is taken gets a second block installed over the first, so its DLL_THREAD_DETACH
//             callbacks run twice (once from the existing-thread record, once from the slot loop).
//   AUDIT-F7  on_host_thread_event() holds g_tls_lock shared while it runs mapped TLS callbacks. A callback that
//             runtime-loads a bundled module with static TLS reaches initialize_tls(), which asks for the same lock
//             exclusively on the same thread. SRW locks are not recursive or upgradable, so the thread deadlocks.
//
// Both cases are synthetic and need no game data: every PE image is built byte by byte in a temporary directory,
// and the only module loaded from a file is the locally built palmetto_tls_host.dll the loader needs for a module
// TLS index (it must sit next to this executable, as for loader_tls_test).
//
// The F4 window is deterministic, not timing-based. This test patches its own executable import of
// CreateToolhelp32Snapshot (the call that snapshots the threads inside install_on_existing_threads) and, on that
// one call, creates a thread and waits for its DLL_THREAD_ATTACH to finish before forwarding to the real API. The
// thread is therefore certainly created after the owner was published and certainly listed by the snapshot, which is
// exactly the racing window. From the test's point of view:
//   - today the seam thread gets the helper-event block, then the existing-threads block over it, and receives two
//     DLL_THREAD_DETACH callbacks (AUDIT-F4 reproduced);
//   - with a fix that records each helper-event install so the scan skips a served thread, the same seam thread
//     receives one DLL_THREAD_DETACH (AUDIT-F4 fixed).
// The seam is a test-only IAT patch: pe_loader.cpp is not edited. A hard precondition checks that the seam really
// observed the helper-event install, so a tree that changes the publication order cannot silently pass.
//
// F7 runs in child processes: the fault arm hangs by design, so a parent watchdog kills it after a timeout. Three
// arms isolate the lock order:
//   --child-f7-attach        a mapped TLS callback runtime-loads a TLS module on DLL_PROCESS_ATTACH (no TLS lock
//                            is held there): must complete, so the callback -> runtime load path itself works;
//   --child-f7-preloaded     the same callback runs on DLL_THREAD_ATTACH but the dependency is already loaded, so
//                            no initialize_tls()/exclusive lock happens: must complete;
//   --child-f7-thread-event  the same callback runs on DLL_THREAD_ATTACH and the dependency is not loaded: today
//                            the child deadlocks inside the helper's DllMain under the shared TLS lock.
//
// Run with --expect-faults to count a reproduced finding as REPRO (exit 0) while the named fixes are pending; the
// strict form (no flag) is the gate once a fix lands: a reproduced finding is a failure there.
//
// Process shape: a normal x64 Windows process linked with /BASE:0x10000000000 so the synthetic images at
// 0x180000000+ have their preferred bases free. Ordinary test code, not game content.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "free_image_base.h"
#include "pe_loader.h"

namespace {

namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;
int g_reproduced = 0;
bool g_expect_faults = false;

// The process EXE keeps module TLS index 0, as palmetto.exe does for the mapped game EXE: a mapped TLS module then
// gets a reserved index above it (the loader records the helper DLL's index in Module::tls_index).
__declspec(thread) int g_host_tls_sentinel = 1;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("PASS  %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

// A check whose expected-before-fix result is "the fault is present". With --expect-faults a reproduced fault is
// REPRO and does not fail the run; an unexpected pass says FIXED, so the flag can be dropped once patched.
void fault_check(bool reproduced, const char* id, const char* what) {
    ++g_checks;
    if (reproduced) {
        if (g_expect_faults) {
            ++g_reproduced;
            std::printf("REPRO %s  %s (fault present)\n", id, what);
        } else {
            ++g_failures;
            std::printf("FAIL  %s  %s\n", id, what);
        }
    } else {
        std::printf("FIXED %s  %s\n", id, what);
    }
}

// The host observer the mapped TLS callbacks jump to; defined with the observers below.
void NTAPI observe_tls(PVOID, DWORD, PVOID);

// ---- Synthetic image builder ------------------------------------------------------------------------------------
// One .text section at RVA 0x1000, 0x200 file-backed bytes, image at a preferred base. The same shape the other
// loader audit tests build, so the images here go through the loader's ordinary validation path.
constexpr uint32_t kTextRva = 0x1000;
constexpr uint32_t kTextRaw = 0x200;
constexpr uint32_t kSectionSize = 0x200;
constexpr uint32_t kImageSize = 0x2000;
constexpr uint32_t kHeadersSize = 0x200;

std::vector<uint8_t> make_image(uint64_t preferred_base, bool dll) {
    std::vector<uint8_t> img(kTextRaw + kSectionSize, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(img.data() + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->FileHeader.Characteristics = dll ? IMAGE_FILE_DLL : 0;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.ImageBase = palmetto_test::free_image_base(preferred_base);
    nt->OptionalHeader.AddressOfEntryPoint = 0;
    nt->OptionalHeader.SizeOfHeaders = kHeadersSize;
    nt->OptionalHeader.SizeOfImage = kImageSize;
    nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    std::memcpy(sec->Name, ".text", 6);
    sec->VirtualAddress = kTextRva;
    sec->Misc.VirtualSize = kSectionSize;
    sec->PointerToRawData = kTextRaw;
    sec->SizeOfRawData = kSectionSize;
    sec->Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    return img;
}

IMAGE_NT_HEADERS64* nt_of(std::vector<uint8_t>& img) {
    return reinterpret_cast<IMAGE_NT_HEADERS64*>(img.data() + 0x80);
}

uint64_t image_base(const std::vector<uint8_t>& img) {
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(img.data() + 0x80)->OptionalHeader.ImageBase;
}

uint8_t* text_at(std::vector<uint8_t>& img, uint32_t rva) {
    if (rva < kTextRva || rva >= kTextRva + kSectionSize) throw std::runtime_error("test image RVA is not in .text");
    return img.data() + kTextRaw + (rva - kTextRva);
}

void set_entry(std::vector<uint8_t>& img, uint32_t rva) { nt_of(img)->OptionalHeader.AddressOfEntryPoint = rva; }

void set_directory(std::vector<uint8_t>& img, unsigned index, uint32_t rva, uint32_t size) {
    auto& dir = nt_of(img)->OptionalHeader.DataDirectory[index];
    dir.VirtualAddress = rva;
    dir.Size = size;
}

// movabs rax, target ; jmp rax: a mapped stub that reaches a host observer with the callback arguments untouched.
void emit_jump_stub(std::vector<uint8_t>& img, uint32_t rva, const void* target) {
    uint8_t* p = text_at(img, rva);
    const uint64_t address = reinterpret_cast<uint64_t>(target);
    p[0] = 0x48;
    p[1] = 0xB8;
    std::memcpy(p + 2, &address, sizeof address);
    p[10] = 0xFF;
    p[11] = 0xE0;
}

void set_tls_directory(std::vector<uint8_t>& img, uint32_t dir_rva, uint32_t template_rva, uint32_t template_size,
                       uint32_t zero_fill, uint32_t index_rva, uint32_t callbacks_rva) {
    const uint64_t base = image_base(img);
    auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(text_at(img, dir_rva));
    tls->StartAddressOfRawData = template_size ? base + template_rva : 0;
    tls->EndAddressOfRawData = template_size ? base + template_rva + template_size : 0;
    tls->AddressOfIndex = index_rva ? base + index_rva : 0;
    tls->AddressOfCallBacks = callbacks_rva ? base + callbacks_rva : 0;
    tls->SizeOfZeroFill = zero_fill;
    tls->Characteristics = 0;
    set_directory(img, IMAGE_DIRECTORY_ENTRY_TLS, dir_rva, static_cast<uint32_t>(sizeof(IMAGE_TLS_DIRECTORY64)));
}

// A DLL with a 4-byte TLS template, an AddressOfIndex and one TLS callback that reaches observe_tls. No DllMain:
// this is the module whose thread events the F4 seam makes duplicate.
std::vector<uint8_t> make_window_dll(uint64_t base) {
    auto img = make_image(base, /*dll=*/true);
    emit_jump_stub(img, 0x1000, reinterpret_cast<const void*>(&observe_tls));
    *reinterpret_cast<uint64_t*>(text_at(img, 0x1160)) = image_base(img) + 0x1000;  // callbacks[0]
    *reinterpret_cast<uint64_t*>(text_at(img, 0x1168)) = 0;                         // terminator
    *reinterpret_cast<uint32_t*>(text_at(img, 0x1180)) = 7;                         // template int
    set_tls_directory(img, 0x11B0, 0x1180, sizeof(uint32_t), 0, 0x11A0, 0x1160);
    return img;
}

// A DLL with a 4-byte TLS template and an AddressOfIndex, no callbacks: enough for initialize_tls() to take a
// reserved slot and ask for the exclusive TLS lock when it is loaded at run time.
std::vector<uint8_t> make_tls_module_dll(uint64_t base) {
    auto img = make_image(base, /*dll=*/true);
    *reinterpret_cast<uint32_t*>(text_at(img, 0x1180)) = 11;  // template int
    set_tls_directory(img, 0x11B0, 0x1180, sizeof(uint32_t), 0, 0x11A0, 0);
    return img;
}

// A DLL whose TLS callback runtime-loads dep_name through its own import of kernel32!LoadLibraryA when the TLS event
// matches load_reason. The loader binds that import to the library_loader shim, so the call reaches the audited
// Loader's run-time module path. Code at 0x1000, TLS callback table 0x1040, template 0x1050, index 0x1054, TLS
// directory 0x1060; the import machinery mirrors the audit tests' nested importer (descriptor 0x1100, DLL name
// 0x1140, hint/name 0x1160, lookup table 0x1170, dependency name 0x11C0, IAT 0x11E0).
std::vector<uint8_t> make_load_callback_dll(uint64_t base, uint32_t load_reason, const char* dep_name) {
    auto img = make_image(base, /*dll=*/true);
    uint8_t* code = text_at(img, 0x1000);
    size_t n = 0;
    code[n++] = 0x83;  // cmp edx, load_reason
    code[n++] = 0xFA;
    code[n++] = static_cast<uint8_t>(load_reason);
    code[n++] = 0x75;  // jne skip
    const size_t jne_operand = n;
    code[n++] = 0;
    code[n++] = 0x48;  // sub rsp, 28h
    code[n++] = 0x83;
    code[n++] = 0xEC;
    code[n++] = 0x28;
    code[n++] = 0x48;  // lea rcx, [rip+disp32] -> dep_name at 0x11C0
    code[n++] = 0x8D;
    code[n++] = 0x0D;
    int32_t disp = static_cast<int32_t>(0x11C0 - (0x1000 + n + 4));
    std::memcpy(code + n, &disp, sizeof disp);
    n += 4;
    code[n++] = 0xFF;  // call qword ptr [rip+disp32] -> IAT slot at 0x11E0
    code[n++] = 0x15;
    disp = static_cast<int32_t>(0x11E0 - (0x1000 + n + 4));
    std::memcpy(code + n, &disp, sizeof disp);
    n += 4;
    code[n++] = 0x48;  // add rsp, 28h
    code[n++] = 0x83;
    code[n++] = 0xC4;
    code[n++] = 0x28;
    const size_t skip = n;
    code[n++] = 0xC3;  // ret
    code[jne_operand] = static_cast<uint8_t>(skip - (jne_operand + 1));

    *reinterpret_cast<uint64_t*>(text_at(img, 0x1040)) = image_base(img) + 0x1000;  // callbacks[0]
    *reinterpret_cast<uint64_t*>(text_at(img, 0x1048)) = 0;                         // terminator
    *reinterpret_cast<uint32_t*>(text_at(img, 0x1050)) = 7;                         // template int
    set_tls_directory(img, 0x1060, 0x1050, sizeof(uint32_t), 0, 0x1054, 0x1040);

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(text_at(img, 0x1100));
    descriptor->Name = 0x1140;
    descriptor->OriginalFirstThunk = 0x1170;
    descriptor->FirstThunk = 0x11E0;
    std::memcpy(text_at(img, 0x1140), "kernel32.dll", sizeof "kernel32.dll");
    auto* hint_name = text_at(img, 0x1160);
    hint_name[0] = 0;
    hint_name[1] = 0;
    std::memcpy(hint_name + 2, "LoadLibraryA", sizeof "LoadLibraryA");
    const uint64_t thunk = 0x1160;  // RVA of the hint/name entry, as the PE lookup table carries it
    std::memcpy(text_at(img, 0x1170), &thunk, sizeof thunk);
    *reinterpret_cast<uint64_t*>(text_at(img, 0x1178)) = 0;  // lookup table terminator
    std::memcpy(text_at(img, 0x11C0), dep_name, std::strlen(dep_name) + 1);
    set_directory(img, IMAGE_DIRECTORY_ENTRY_IMPORT, 0x1100, sizeof(IMAGE_IMPORT_DESCRIPTOR));
    return img;
}

struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        path = fs::temp_directory_path() / ("palmetto_loader_tls_race_" + std::string(tag) + "_" +
                                             std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

fs::path write_image(const fs::path& dir, const std::string& name, const std::vector<uint8_t>& bytes) {
    const fs::path p = dir / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create " + p.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + p.string());
    return p;
}

// ---- Observers for mapped TLS callbacks -------------------------------------------------------------------------

struct Observed {
    DWORD tid = 0;
    DWORD reason = 0;
    bool tls = false;
    uint64_t sequence = 0;
};

std::mutex g_observed_mutex;
std::vector<Observed> g_observed;
std::atomic<uint64_t> g_observed_sequence{0};

void record_observation(DWORD reason, bool tls) {
    Observed o;
    o.tid = GetCurrentThreadId();
    o.reason = reason;
    o.tls = tls;
    o.sequence = g_observed_sequence.fetch_add(1);
    std::lock_guard lock(g_observed_mutex);
    g_observed.push_back(o);
}

void NTAPI observe_tls(PVOID, DWORD reason, PVOID) { record_observation(reason, true); }

std::vector<Observed> observed_snapshot() {
    std::lock_guard lock(g_observed_mutex);
    return g_observed;
}

void reset_observers() {
    std::lock_guard lock(g_observed_mutex);
    g_observed.clear();
    g_observed_sequence = 0;
}

int observed_count(const std::vector<Observed>& all, DWORD reason, bool tls, DWORD tid) {
    int count = 0;
    for (const Observed& o : all)
        if (o.reason == reason && o.tls == tls && o.tid == tid) ++count;
    return count;
}

// ---- Reading the helper DLL and another thread's TEB (the F4 precondition) -------------------------------------

HMODULE tls_helper_module() { return GetModuleHandleW(L"palmetto_tls_host.dll"); }

// The module TLS index the OS assigned to the helper: its own AddressOfIndex. This is the index the loader reuses
// for the first mapped TLS module.
DWORD tls_helper_index() {
    HMODULE helper = tls_helper_module();
    if (!helper) return 0xFFFFFFFFu;
    auto* base = reinterpret_cast<uint8_t*>(helper);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir.Size || !dir.VirtualAddress) return 0xFFFFFFFFu;
    auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(base + dir.VirtualAddress);
    if (!tls->AddressOfIndex) return 0xFFFFFFFFu;
    return *reinterpret_cast<DWORD*>(tls->AddressOfIndex);
}

bool inside_tls_helper_image(const void* p) {
    HMODULE helper = tls_helper_module();
    if (!helper || !p) return false;
    auto* base = reinterpret_cast<const uint8_t*>(helper);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const uintptr_t start = reinterpret_cast<uintptr_t>(base);
    const uintptr_t end = start + nt->OptionalHeader.SizeOfImage;
    const uintptr_t value = reinterpret_cast<uintptr_t>(p);
    return value >= start && value < end;
}

// THREAD_BASIC_INFORMATION (winternl.h keeps it opaque), x64. Same shape pe_loader.cpp uses.
struct ThreadBasicInfo {
    LONG exit_status;
    void* teb;
    void* process_id;
    void* thread_id;
    ULONG_PTR affinity;
    LONG priority;
    LONG base_priority;
};
static_assert(sizeof(ThreadBasicInfo) == 48);

using QueryThreadFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);

QueryThreadFn query_thread_fn() {
    static const auto fn = reinterpret_cast<QueryThreadFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    return fn;
}

// Another running thread's TEB->ThreadLocalStoragePointer[index], or null when it is gone.
void* read_thread_tls_slot(HANDLE thread, DWORD index) {
    ThreadBasicInfo info{};
    const QueryThreadFn query = query_thread_fn();
    if (!query || query(thread, 0 /* ThreadBasicInformation */, &info, sizeof info, nullptr) < 0) return nullptr;
    if (!info.teb || index >= 0x10000) return nullptr;
    auto** slots = *reinterpret_cast<void***>(static_cast<uint8_t*>(info.teb) + 0x58);
    return slots ? slots[index] : nullptr;
}

// ---- The F4 seam: one thread created inside install_on_existing_threads' snapshot -------------------------------
//
// The seam patched into this executable's own import table. On the one armed call it creates a thread and waits for
// that thread's start routine, which runs only after the Windows loader has delivered its DLL_THREAD_ATTACH
// (the helper DLL's DllMain -> on_host_thread_event installs that thread's block). Forwarding to the real
// CreateToolhelp32Snapshot then lists the thread, so install_on_existing_threads sees a saved block the helper put
// there and installs a second one over it. This is the F4 window without any timing dependency.
using SnapshotFn = HANDLE(WINAPI*)(DWORD, DWORD);

SnapshotFn g_real_snapshot = nullptr;
std::atomic<bool> g_seam_armed{false};
std::atomic<bool> g_seam_event_block_before_snapshot{false};
HANDLE g_seam_thread = nullptr;
DWORD g_seam_tid = 0;

struct WaitCtx {
    HANDLE started = nullptr;
    HANDLE release = nullptr;
};

WaitCtx* g_seam_ctx = nullptr;

DWORD WINAPI wait_thread_main(void* parameter) {
    auto* ctx = static_cast<WaitCtx*>(parameter);
    SetEvent(ctx->started);
    WaitForSingleObject(ctx->release, 60000);
    return 0;
}

void inject_window_thread() {
    g_seam_ctx = new WaitCtx;
    g_seam_ctx->started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_seam_ctx->release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_seam_thread = CreateThread(nullptr, 0, wait_thread_main, g_seam_ctx, 0, &g_seam_tid);
    if (!g_seam_thread) return;
    WaitForSingleObject(g_seam_ctx->started, 10000);  // the start routine runs after DLL_THREAD_ATTACH completed
    const DWORD index = tls_helper_index();
    void* block = index == 0xFFFFFFFFu ? nullptr : read_thread_tls_slot(g_seam_thread, index);
    // A helper-event install replaces the helper's own TLS block (inside the helper image) with a separate
    // VirtualAlloc block; seeing a value outside the helper image is the deterministic precondition.
    g_seam_event_block_before_snapshot.store(block != nullptr && !inside_tls_helper_image(block));
}

HANDLE WINAPI snapshot_hook(DWORD flags, DWORD process_id) {
    if (g_seam_armed.exchange(false)) inject_window_thread();
    return g_real_snapshot(flags, process_id);
}

bool patch_exe_import(const char* function_name, void* replacement, void*** out_slot, void** out_original) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (uint64_t i = 0;; ++i) {
        auto* descriptor =
            reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress + i * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (!descriptor->Name) break;
        const uint64_t lookup_rva = descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        for (uint64_t j = 0;; ++j) {
            auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + lookup_rva + j * sizeof(IMAGE_THUNK_DATA64));
            if (!lookup->u1.AddressOfData) break;
            if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) continue;
            const char* name = reinterpret_cast<const char*>(
                base + (lookup->u1.AddressOfData & 0x7FFFFFFFull) + sizeof(uint16_t));
            if (std::strcmp(name, function_name) != 0) continue;
            auto** slot = reinterpret_cast<void**>(base + descriptor->FirstThunk + j * sizeof(IMAGE_THUNK_DATA64));
            DWORD protection = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
            *out_original = *slot;
            *out_slot = slot;
            *slot = replacement;
            VirtualProtect(slot, sizeof(void*), protection, &protection);
            return true;
        }
    }
    return false;
}

void restore_exe_import(void** slot, void* original) {
    if (!slot) return;
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return;
    *slot = original;
    VirtualProtect(slot, sizeof(void*), protection, &protection);
}

// ---- AUDIT-F4 ---------------------------------------------------------------------------------------------------

void test_f4_duplicate_install_window() {
    std::printf("\n-- AUDIT-F4: a thread created inside load()'s publication->snapshot window --\n");
    TempDir temp("f4_window");
    reset_observers();
    const fs::path path = write_image(temp.path, "f4_window.dll", make_window_dll(0x180000000));

    void** slot = nullptr;
    void* original = nullptr;
    const bool patched =
        patch_exe_import("CreateToolhelp32Snapshot", reinterpret_cast<void*>(&snapshot_hook), &slot, &original);
    check(patched, "the test executable's CreateToolhelp32Snapshot import is patched for the F4 seam");
    if (!patched) return;
    g_real_snapshot = reinterpret_cast<SnapshotFn>(original);

    DWORD seam_tid = 0;
    DWORD control_tid = 0;
    {
        rt::Loader loader(temp.path);
        g_seam_armed.store(true);
        try {
            loader.load(path);  // the seam fires inside install_on_existing_threads
        } catch (...) {
            g_seam_armed.store(false);
            restore_exe_import(slot, original);
            throw;
        }
        g_seam_armed.store(false);
        restore_exe_import(slot, original);
        loader.initialise();
        check(g_seam_thread != nullptr, "the F4 seam created its thread inside the loader's thread snapshot");
        seam_tid = g_seam_tid;

        // Control: a thread started after load() gets the helper's single event install and no existing-thread record.
        auto* control_ctx = new WaitCtx;
        control_ctx->started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        control_ctx->release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE control = CreateThread(nullptr, 0, wait_thread_main, control_ctx, 0, nullptr);
        if (control) {
            WaitForSingleObject(control_ctx->started, 10000);
            control_tid = GetThreadId(control);
        }

        if (g_seam_ctx) SetEvent(g_seam_ctx->release);  // let the seam thread end: that is where the detach runs
        if (g_seam_thread) {
            WaitForSingleObject(g_seam_thread, 30000);
            CloseHandle(g_seam_thread);
            g_seam_thread = nullptr;
        }
        if (control) {
            SetEvent(control_ctx->release);
            WaitForSingleObject(control, 30000);
            CloseHandle(control);
        }
        delete control_ctx;
        delete g_seam_ctx;
        g_seam_ctx = nullptr;
    }

    const auto all = observed_snapshot();
    const int seam_detaches = observed_count(all, DLL_THREAD_DETACH, true, seam_tid);
    const int control_detaches = observed_count(all, DLL_THREAD_DETACH, true, control_tid);
    const bool helper_installed = g_seam_event_block_before_snapshot.load();
    std::printf("     seam thread %lu: %d DLL_THREAD_DETACH; control thread %lu: %d; helper block before snapshot: %s\n",
                static_cast<unsigned long>(seam_tid), seam_detaches, static_cast<unsigned long>(control_tid),
                control_detaches, helper_installed ? "yes" : "no");
    check(helper_installed,
          "the seam thread already had a helper-event TLS block before install_on_existing_threads' snapshot");
    check(control_detaches == 1, "a thread started after load() gets exactly one DLL_THREAD_DETACH (control)");
    fault_check(seam_detaches >= 2, "AUDIT-F4",
                "a thread created inside load()'s window gets exactly one DLL_THREAD_DETACH");
}

// ---- AUDIT-F7: child processes with a watchdog ------------------------------------------------------------------

struct ChildResult {
    bool started = false;
    bool timed_out = false;
    DWORD exit_code = 0;
};

ChildResult run_child(const std::wstring& arguments, DWORD timeout_ms) {
    ChildResult result;
    wchar_t self[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return result;
    std::wstring command = L"\"" + std::wstring(self) + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    std::fflush(stdout);
    std::fflush(stderr);
    if (!CreateProcessW(self, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        std::printf("     could not start the child process (error %lu)\n", GetLastError());
        return result;
    }
    result.started = true;
    const DWORD waited = WaitForSingleObject(process.hProcess, timeout_ms);
    if (waited == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(process.hProcess, 0x70000001u);
        WaitForSingleObject(process.hProcess, 5000);
    }
    if (!GetExitCodeProcess(process.hProcess, &result.exit_code)) result.exit_code = 0xFFFFFFFFu;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

rt::Module* find_module(rt::Loader& loader, const char* name) {
    for (rt::Module* module : loader.modules())
        if (module->name == name) return module;
    return nullptr;
}

// The parked worker whose DLL_THREAD_ATTACH runs the mapped TLS callback.
DWORD WINAPI parked_thread_main(void*) {
    return 0;
}

// Control arm 1: the same mapped TLS callback runtime-loads a TLS module on DLL_PROCESS_ATTACH, where the loader
// holds no TLS lock. Completing proves the callback -> shim LoadLibrary -> run-time load path works on this tree.
int run_f7_attach_child(const fs::path& dir) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        rt::Loader loader(dir);
        loader.load(dir / "f7_attach.dll");
        loader.initialise();  // the callback runs here, on the loading thread, with no TLS lock held
        rt::Module* dependency = find_module(loader, "f7_dep_attach.dll");
        if (!dependency || !dependency->initialised) {
            std::printf("child: the process-attach callback did not runtime-load/initialise f7_dep_attach.dll\n");
            return 40;
        }
        std::printf("child: process-attach TLS callback runtime-loaded and initialised f7_dep_attach.dll\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("child: process-attach arm threw: %s\n", e.what());
        return 41;
    }
}

// Control arm 2: the thread-event callback runs but the dependency is already loaded, so its LoadLibrary returns
// the existing module and initialize_tls() (and the exclusive TLS lock) is never reached. Must complete.
int run_f7_preloaded_child(const fs::path& dir) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        rt::Loader loader(dir);
        loader.load(dir / "f7_thread.dll");
        loader.load(dir / "f7_dep_thread.dll");  // preloaded: the callback's LoadLibrary will find it
        loader.initialise();
        std::printf("child: dependency preloaded; creating the thread whose attach callback calls LoadLibrary\n");
        HANDLE worker = CreateThread(nullptr, 0, parked_thread_main, nullptr, 0, nullptr);
        if (!worker) return 40;
        const DWORD waited = WaitForSingleObject(worker, 30000);
        if (waited != WAIT_OBJECT_0) {
            std::printf("child: the preloaded-dependency worker did not finish (wait result %lu)\n", waited);
            return 40;
        }
        CloseHandle(worker);
        rt::Module* dependency = find_module(loader, "f7_dep_thread.dll");
        if (!dependency || !dependency->initialised) return 40;
        std::printf("child: thread-attach callback loaded the already-mapped dependency and the worker exited\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("child: preloaded arm threw: %s\n", e.what());
        return 41;
    }
}

// Fault arm: the thread-attach TLS callback runtime-loads a TLS module that is not loaded yet. On the current tree
// the callback runs under g_tls_lock shared and initialize_tls() asks for it exclusively on the same thread, so the
// worker never returns. Self-report after 4 s and terminate (TerminateProcess, so no exit-time loader lock wait).
int run_f7_thread_event_child(const fs::path& dir) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        rt::Loader loader(dir);
        loader.load(dir / "f7_thread.dll");
        loader.initialise();
        std::printf("child: mapped and initialised; the next thread's attach callback runtime-loads f7_dep_thread.dll\n");
        HANDLE worker = CreateThread(nullptr, 0, parked_thread_main, nullptr, 0, nullptr);
        if (!worker) {
            std::printf("child: could not create the worker\n");
            return 40;
        }
        const DWORD waited = WaitForSingleObject(worker, 4000);
        if (waited == WAIT_TIMEOUT) {
            std::printf("child: DEADLOCK: the thread-attach TLS callback did not finish its runtime load in 4 s\n");
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 21);
            return 21;  // not reached
        }
        CloseHandle(worker);
        rt::Module* dependency = find_module(loader, "f7_dep_thread.dll");
        if (!dependency || !dependency->initialised) {
            std::printf("child: the thread-attach callback did not runtime-load/initialise f7_dep_thread.dll\n");
            return 40;
        }
        std::printf("child: thread-attach callback runtime-loaded and initialised f7_dep_thread.dll\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("child: thread-event arm threw: %s\n", e.what());
        return 41;
    }
}

void test_f7_thread_event_load_deadlock() {
    std::printf("\n-- AUDIT-F7: thread-event TLS callback runtime-loads a TLS module --\n");
    TempDir temp("f7");
    write_image(temp.path, "f7_thread.dll",
                make_load_callback_dll(0x180000000, DLL_THREAD_ATTACH, "f7_dep_thread.dll"));
    write_image(temp.path, "f7_dep_thread.dll", make_tls_module_dll(0x190000000));
    write_image(temp.path, "f7_attach.dll",
                make_load_callback_dll(0x1A0000000, DLL_PROCESS_ATTACH, "f7_dep_attach.dll"));
    write_image(temp.path, "f7_dep_attach.dll", make_tls_module_dll(0x1B0000000));
    const std::wstring dir_arg = L"\"" + temp.path.wstring() + L"\"";

    const ChildResult attach_arm = run_child(L"--child-f7-attach " + dir_arg, 30000);
    std::printf("     attach arm: exit 0x%08lx%s\n", static_cast<unsigned long>(attach_arm.exit_code),
                attach_arm.timed_out ? " (killed after timeout)" : "");
    check(attach_arm.started && !attach_arm.timed_out && attach_arm.exit_code == 0,
          "a mapped TLS callback can runtime-load a TLS module on process attach (F7 control 1)");

    const ChildResult preloaded_arm = run_child(L"--child-f7-preloaded " + dir_arg, 30000);
    std::printf("     preloaded arm: exit 0x%08lx%s\n", static_cast<unsigned long>(preloaded_arm.exit_code),
                preloaded_arm.timed_out ? " (killed after timeout)" : "");
    check(preloaded_arm.started && !preloaded_arm.timed_out && preloaded_arm.exit_code == 0,
          "a thread-attach TLS callback can runtime-load an already-mapped module (F7 control 2)");

    const ChildResult event_arm = run_child(L"--child-f7-thread-event " + dir_arg, 20000);
    std::printf("     thread-event arm: exit 0x%08lx%s\n", static_cast<unsigned long>(event_arm.exit_code),
                event_arm.timed_out ? " (killed after parent timeout)" : "");
    if (!event_arm.started) {
        check(false, "the thread-event child process could be measured");
    } else if (event_arm.timed_out || event_arm.exit_code == 21) {
        fault_check(true, "AUDIT-F7",
                    "a thread-attach TLS callback runtime-loading a TLS module completes instead of self-deadlocking");
    } else if (event_arm.exit_code == 0) {
        fault_check(false, "AUDIT-F7",
                    "a thread-attach TLS callback runtime-loading a TLS module completes instead of self-deadlocking");
    } else {
        std::printf("     thread-event arm failed before the lock order: exit code 0x%08lx\n",
                    static_cast<unsigned long>(event_arm.exit_code));
        check(false, "the thread-event arm reached the TLS lock order (unexpected child failure)");
    }
}

void print_result() {
    if (g_failures == 0) {
        std::printf("\nloader_tls_race_test: OK (%d checks, %d reproduced faults)\n", g_checks, g_reproduced);
    } else {
        std::printf("\nloader_tls_race_test: FAILED (%d checks, %d failure%s, %d reproduced faults)\n", g_checks,
                    g_failures, g_failures == 1 ? "" : "s", g_reproduced);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fs::path child_dir;
    enum class ChildMode { None, F7Attach, F7Preloaded, F7ThreadEvent } child_mode = ChildMode::None;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--expect-faults") {
            g_expect_faults = true;
        } else if (argument == "--child-f7-attach" && i + 1 < argc) {
            child_mode = ChildMode::F7Attach;
            child_dir = argv[++i];
        } else if (argument == "--child-f7-preloaded" && i + 1 < argc) {
            child_mode = ChildMode::F7Preloaded;
            child_dir = argv[++i];
        } else if (argument == "--child-f7-thread-event" && i + 1 < argc) {
            child_mode = ChildMode::F7ThreadEvent;
            child_dir = argv[++i];
        } else if (argument.rfind("--", 0) == 0) {
            std::printf("unknown option %s\n", argument.c_str());
            return 2;
        }
    }
    if (child_mode == ChildMode::F7Attach) return run_f7_attach_child(child_dir);
    if (child_mode == ChildMode::F7Preloaded) return run_f7_preloaded_child(child_dir);
    if (child_mode == ChildMode::F7ThreadEvent) return run_f7_thread_event_child(child_dir);

    std::printf("loader_tls_race_test: loader TLS race (F4) and lock-order (F7) contracts\n");
    try {
        g_host_tls_sentinel = 2;
        test_f4_duplicate_install_window();
        test_f7_thread_event_load_deadlock();
        check(g_host_tls_sentinel == 2, "this EXE's own TLS at module index 0 is untouched");
    } catch (const std::exception& e) {
        ++g_checks;
        ++g_failures;
        std::printf("FAIL  unexpected exception: %s\n", e.what());
    }
    print_result();
    return g_failures ? 1 : 0;
}
