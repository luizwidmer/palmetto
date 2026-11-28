// Loader audit contracts for static TLS, thread events, unwind registration and lifecycle.
//
// Every image here is built byte by byte in a temporary directory: no game data, no fixture DLL is mapped for these
// cases except the locally built tiny_tls.dll, which the high-module-index case needs only to make the process hold
// more than 64 static-TLS modules. Nothing is executed past the synthetic stubs below.
//
// The checks are the contracts the inherited loader (port/src/pe_loader.cpp) should satisfy, not its current
// behaviour. Findings from the audit are tagged AUDIT-F<n>; run with --expect-faults to count a reproduced fault as
// "REPRO" instead of a failure, which keeps the target green while the named patch is pending:
//
//   AUDIT-F0  initialise() re-entry: a mapped DllMain that runtime-loads a bundled module re-enters its own
//             DLL_PROCESS_ATTACH without limit (stack overflow), and the state it appends invalidates the range-for
//             over init_order_ that is being iterated.
//   AUDIT-F1  /GA index-0 reads must use a distinct TlsAlloc slot per image; if no slot below 64 is available, the
//             loader must refuse the image instead of continuing with those reads pointed at the host TLS view.
//   AUDIT-F2  kMaxModuleTlsSlots (64) is the TlsAlloc slot count, not the module TLS index space: a reserved module
//             index >= 64 is rejected even though the OS assigned and supports it (measured past 1101 here).
//   AUDIT-F3  Thread events run the mapped TLS callbacks only: a mapped DLL's DllMain never receives
//             DLL_THREAD_ATTACH/DLL_THREAD_DETACH, which Windows delivers.
//   AUDIT-F5  A TLS directory whose declared Size is smaller than IMAGE_TLS_DIRECTORY64 is read as a full directory
//             instead of being refused (the landed pe_image parser refuses it).
//   AUDIT-F6  An exception-directory entry with the indirect UnwindData form (bit 0) is accepted after masking the
//             bit; the chained RUNTIME_FUNCTION it points at is never validated.
//
// The process is expected to be a normal x64 Windows process. Build with /BASE:0x10000000000 so the synthetic images
// at 0x180000000+ are free; palmetto_tls_host.dll must sit next to this executable (the loader's reserved-index
// helper), as for loader_tls_test.
#include <windows.h>
#include <intrin.h>

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

// The process EXE owns module TLS index 0, as palmetto.exe does for the mapped game EXE. The /GA rewrite is only
// exercised while the helper's reserved index is not 0, so this EXE keeps that index (as loader_tls_test does).
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

// A check whose expected-before-patch result is "the fault is present". With --expect-faults a reproduced fault is
// reported as REPRO and does not fail the run; an unexpected pass says FIXED so the flag can be dropped once patched.
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

// ---- Synthetic image builder -------------------------------------------------------------------------------------

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

const uint64_t image_base(const std::vector<uint8_t>& img) {
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

// movabs rax, target ; jmp rax: a mapped stub that calls a host observer with its arguments untouched, so a
// notification can be seen after the image is unmapped. 12 bytes.
void emit_jump_stub(std::vector<uint8_t>& img, uint32_t rva, const void* target) {
    uint8_t* p = text_at(img, rva);
    const uint64_t address = reinterpret_cast<uint64_t>(target);
    p[0] = 0x48;
    p[1] = 0xB8;
    std::memcpy(p + 2, &address, sizeof address);
    p[10] = 0xFF;
    p[11] = 0xE0;
}

// The /GA index-0 TLS read the loader rewrites: mov rax, gs:[58h] ; mov rax, [rax] ; mov eax, [rax] ; ret. It reads
// ThreadLocalStoragePointer[0] and the template's first int exactly as a /GA-built EXE does. 16 bytes.
void emit_ga_tls_read(std::vector<uint8_t>& img, uint32_t rva) {
    static const uint8_t code[] = {0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,
                                   0x48, 0x8B, 0x00,
                                   0x8B, 0x00,
                                   0xC3};
    std::memcpy(text_at(img, rva), code, sizeof code);
}

void set_tls_directory(std::vector<uint8_t>& img, uint32_t dir_rva, uint32_t template_rva, uint32_t template_size,
                       uint32_t zero_fill, uint32_t index_rva, uint32_t callbacks_rva, uint32_t declared_size) {
    const uint64_t base = image_base(img);
    auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(text_at(img, dir_rva));
    tls->StartAddressOfRawData = template_size ? base + template_rva : 0;
    tls->EndAddressOfRawData = template_size ? base + template_rva + template_size : 0;
    tls->AddressOfIndex = index_rva ? base + index_rva : 0;
    tls->AddressOfCallBacks = callbacks_rva ? base + callbacks_rva : 0;
    tls->SizeOfZeroFill = zero_fill;
    tls->Characteristics = 0;
    set_directory(img, IMAGE_DIRECTORY_ENTRY_TLS, dir_rva, declared_size);
}

struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        path = fs::temp_directory_path() / ("palmetto_loader_tls_audit_" + std::string(tag) + "_" +
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

// ---- Observers for mapped notifications ---------------------------------------------------------------------------

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

BOOL NTAPI observe_dllmain(PVOID, DWORD reason, PVOID) {
    record_observation(reason, false);
    return TRUE;
}

std::vector<Observed> observed_snapshot() {
    std::lock_guard lock(g_observed_mutex);
    return g_observed;
}

void reset_observers() {
    std::lock_guard lock(g_observed_mutex);
    g_observed.clear();
    g_observed_sequence = 0;
}

// Highest sequence number of a matching observation, or 0 when none matches (0 is not a valid sequence index).
uint64_t observed_at(const std::vector<Observed>& all, DWORD reason, bool tls, DWORD tid) {
    uint64_t at = 0;
    for (const Observed& o : all)
        if (o.reason == reason && o.tls == tls && o.tid == tid && o.sequence + 1 > at) at = o.sequence + 1;
    return at;
}

int observed_count(const std::vector<Observed>& all, DWORD reason, bool tls, DWORD tid) {
    int count = 0;
    for (const Observed& o : all)
        if (o.reason == reason && o.tls == tls && o.tid == tid) ++count;
    return count;
}

// ---- Image shapes ------------------------------------------------------------------------------------------------

// A DLL with a TLS template, an AddressOfIndex and one TLS callback, and a DllMain; both jump to the host observers.
std::vector<uint8_t> make_event_dll(uint64_t base) {
    auto img = make_image(base, /*dll=*/true);
    set_entry(img, 0x1040);
    emit_jump_stub(img, 0x1000, reinterpret_cast<const void*>(&observe_tls));
    emit_jump_stub(img, 0x1040, reinterpret_cast<const void*>(&observe_dllmain));
    *reinterpret_cast<uint64_t*>(text_at(img, 0x1160)) = image_base(img) + 0x1000;  // callbacks[0]
    *reinterpret_cast<uint64_t*>(text_at(img, 0x1168)) = 0;                         // callbacks[1] = terminator
    *reinterpret_cast<uint32_t*>(text_at(img, 0x1180)) = 7;                         // the template's int
    set_tls_directory(img, 0x11B0, 0x1180, sizeof(int32_t), 0, 0x11A0, 0x1160,
                      static_cast<uint32_t>(sizeof(IMAGE_TLS_DIRECTORY64)));
    return img;
}

// A DLL whose TLS data is 12 recognisable bytes followed by an 8-byte zero fill, and a canary after AddressOfIndex.
std::vector<uint8_t> make_tls_data_dll(uint64_t base, uint32_t declared_size) {
    auto img = make_image(base, /*dll=*/true);
    static const uint8_t kTemplate[12] = {'T', 'L', 'S', '-', 'T', 'E', 'M', 'P', 'L', 'A', 'T', 'E'};
    std::memcpy(text_at(img, 0x1180), kTemplate, sizeof kTemplate);
    *reinterpret_cast<uint32_t*>(text_at(img, 0x11A0)) = 0xA5A5A5A5;
    *reinterpret_cast<uint32_t*>(text_at(img, 0x11A4)) = 0x5A5A5A5A;  // canary: the index write must not reach it
    set_tls_directory(img, 0x11B0, 0x1180, sizeof kTemplate, 8, 0x11A0, 0, declared_size);
    return img;
}

// A non-DLL image built the way a /GA EXE reads its TLS: the index-0 read in .text and a template int at 0x1180.
std::vector<uint8_t> make_ga_exe(uint64_t base, uint32_t template_value) {
    auto img = make_image(base, /*dll=*/false);
    emit_ga_tls_read(img, 0x1000);
    *reinterpret_cast<uint32_t*>(text_at(img, 0x1180)) = template_value;
    set_tls_directory(img, 0x11B0, 0x1180, sizeof(uint32_t), 0, 0x11A0, 0,
                      static_cast<uint32_t>(sizeof(IMAGE_TLS_DIRECTORY64)));
    return img;
}

// .pdata at 0x1100 (12-byte RUNTIME_FUNCTIONs), a 4-byte UNWIND_INFO at 0x1180, and an optional chain entry at 0x11A0.
std::vector<uint8_t> make_unwind_image(uint64_t base, const std::vector<RUNTIME_FUNCTION>& entries) {
    auto img = make_image(base, /*dll=*/true);
    const uint8_t unwind_info[4] = {1, 0, 0, 0};  // Version 1, no flags, no codes
    std::memcpy(text_at(img, 0x1180), unwind_info, sizeof unwind_info);
    if (entries.size() > 8) throw std::runtime_error("too many test unwind entries");
    std::memcpy(text_at(img, 0x1100), entries.data(), entries.size() * sizeof(RUNTIME_FUNCTION));
    set_directory(img, IMAGE_DIRECTORY_ENTRY_EXCEPTION, 0x1100,
                  static_cast<uint32_t>(entries.size() * sizeof(RUNTIME_FUNCTION)));
    return img;
}

// A DLL whose DllMain calls LoadLibraryA("audit_dep.dll") through its import table, so a run-time load of another
// bundled module happens from inside mapped DllMain code. The shim library loader maps the dependency through the
// audited Loader (the operand is a RIP-relative string; the IAT slot is filled by bind_imports).
std::vector<uint8_t> make_nested_importer(uint64_t base) {
    auto img = make_image(base, /*dll=*/true);
    set_entry(img, 0x1040);
    uint8_t* code = text_at(img, 0x1040);
    size_t n = 0;
    code[n++] = 0x48;
    code[n++] = 0x83;
    code[n++] = 0xEC;
    code[n++] = 0x28;  // sub rsp, 28h
    code[n++] = 0x48;
    code[n++] = 0x8D;
    code[n++] = 0x0D;  // lea rcx, [rip+disp32] -> "audit_dep.dll" at 0x11C0
    int32_t disp = static_cast<int32_t>(0x11C0 - (0x1040 + n + 4));
    std::memcpy(code + n, &disp, sizeof disp);
    n += 4;
    code[n++] = 0x48;
    code[n++] = 0x8B;
    code[n++] = 0x05;  // mov rax, [rip+disp32] -> IAT slot at 0x11E0
    disp = static_cast<int32_t>(0x11E0 - (0x1040 + n + 4));
    std::memcpy(code + n, &disp, sizeof disp);
    n += 4;
    code[n++] = 0xFF;
    code[n++] = 0xD0;  // call rax
    code[n++] = 0x48;
    code[n++] = 0x83;
    code[n++] = 0xC4;
    code[n++] = 0x28;  // add rsp, 28h
    code[n++] = 0xB8;
    code[n++] = 0x01;
    code[n++] = 0x00;
    code[n++] = 0x00;
    code[n++] = 0x00;  // mov eax, 1
    code[n++] = 0xC3;  // ret

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(text_at(img, 0x1100));
    descriptor->Name = 0x1140;
    descriptor->OriginalFirstThunk = 0x1170;
    descriptor->FirstThunk = 0x11E0;
    std::memcpy(text_at(img, 0x1140), "kernel32.dll", sizeof "kernel32.dll");
    auto* hint_name = text_at(img, 0x1160);
    hint_name[0] = 0;
    hint_name[1] = 0;
    std::memcpy(hint_name + 2, "LoadLibraryA", sizeof "LoadLibraryA");
    const uint64_t thunk = 0x1160;  // RVA of the hint/name entry, as the PE import lookup table carries it
    std::memcpy(text_at(img, 0x1170), &thunk, sizeof thunk);  // import lookup table entry
    *reinterpret_cast<uint64_t*>(text_at(img, 0x1178)) = 0;   // its terminator
    std::memcpy(text_at(img, 0x11C0), "audit_dep.dll", sizeof "audit_dep.dll");
    set_directory(img, IMAGE_DIRECTORY_ENTRY_IMPORT, 0x1100, sizeof(IMAGE_IMPORT_DESCRIPTOR));
    return img;
}

// The dependency the importer's DllMain loads: a plain DLL whose DllMain observes the attach, so the test can tell the
// nested mapping really was initialised through the Loader.
std::vector<uint8_t> make_nested_dependency(uint64_t base) {
    auto img = make_image(base, /*dll=*/true);
    set_entry(img, 0x1040);
    emit_jump_stub(img, 0x1040, reinterpret_cast<const void*>(&observe_dllmain));
    return img;
}

const IMAGE_TLS_DIRECTORY64* mapped_tls(const rt::Module& m) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir.Size || !dir.VirtualAddress) return nullptr;
    return reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(m.base + dir.VirtualAddress);
}

void** module_tls_slots() { return reinterpret_cast<void**>(__readgsqword(0x58)); }

// ---- AUDIT-F5: TLS directory size ------------------------------------------------------------------------------

void test_tls_directory_size() {
    std::printf("\n-- TLS directory declared size --\n");
    TempDir temp("tls_size");
    // A directory declared as 1 byte but holding a full IMAGE_TLS_DIRECTORY64 inside the image. The read past the
    // declared size is in bounds, so the old loader trusts it; the landed pe_image parser refuses it ("the TLS
    // directory is smaller than its header").
    auto img = make_tls_data_dll(0x180000000, /*declared_size=*/1);
    const fs::path path = write_image(temp.path, "tls_small_dir.dll", img);
    bool rejected = false;
    std::string message;
    try {
        rt::Loader loader(temp.path);
        loader.load(path);
    } catch (const std::exception& e) {
        rejected = true;
        message = e.what();
    }
    std::printf("     load %s%s%s\n", rejected ? "threw: " : "accepted a TLS directory whose declared size is 1",
                rejected ? message.c_str() : "", rejected ? "" : " (full 40 bytes were read anyway)");
    fault_check(!rejected, "AUDIT-F5", "a TLS directory smaller than its 40-byte header is refused");
}

// ---- TLS block / index-write contracts (expected to hold) ---------------------------------------------------------

void test_tls_block_contract() {
    std::printf("\n-- TLS block copy, zero fill, index write and restore --\n");
    TempDir temp("tls_block");
    auto img = make_tls_data_dll(0x180000000, static_cast<uint32_t>(sizeof(IMAGE_TLS_DIRECTORY64)));
    // AddressOfIndex must be watched through the file copy before the loader maps it: the mapped image is compared later.
    const uint64_t base = image_base(img);
    const fs::path path = write_image(temp.path, "tls_data.dll", img);
    uint32_t index = rt::kNoTlsIndex;
    void* block = nullptr;
    void* saved = nullptr;
    {
        rt::Loader loader(temp.path);
        rt::Module& m = loader.load(path);
        const auto* tls = mapped_tls(m);
        check(tls != nullptr, "the TLS data image has a TLS directory");
        if (!tls) return;
        check(m.tls_raw_size == 12 && m.tls_zero_fill == 8 && m.tls_block_size == 20,
              "the Module records the 12-byte template plus the 8-byte zero fill");
        check(m.tls_template == m.base + 0x1180, "the template points inside the mapped image");
        block = m.tls_block;
        check(block != nullptr, "load() installs a TLS block in the calling thread");
        if (!block) return;
        const uint8_t expected[12] = {'T', 'L', 'S', '-', 'T', 'E', 'M', 'P', 'L', 'A', 'T', 'E'};
        check(std::memcmp(block, expected, sizeof expected) == 0, "the 12 template bytes are copied");
        const auto* tail = static_cast<const uint8_t*>(block) + 12;
        bool zero = true;
        for (int i = 0; i < 8; ++i) zero = zero && tail[i] == 0;
        check(zero, "the 8 zero-fill bytes are zero");
        MEMORY_BASIC_INFORMATION info{};
        check(VirtualQuery(block, &info, sizeof info) != 0 && info.RegionSize >= 20,
              "the block is a committed allocation of at least raw+zero-fill");
        const auto stored = *reinterpret_cast<const uint32_t*>(m.base + 0x11A0);
        check(stored == m.tls_index, "the OS-assigned module TLS index is written to AddressOfIndex");
        check(*reinterpret_cast<const uint32_t*>(m.base + 0x11A4) == 0x5A5A5A5A,
              "the index write does not touch the four bytes after AddressOfIndex");
        check(module_tls_slots()[m.tls_index] == block, "gs:[58h][tls_index] holds the installed block");
        index = m.tls_index;
        saved = m.tls_saved_block;
        check(saved != nullptr && saved != block, "the helper DLL's own block is saved before the swap");
    }
    check(module_tls_slots()[index] == saved, "~Loader restores the helper block in the loading thread");
    MEMORY_BASIC_INFORMATION info{};
    check(VirtualQuery(block, &info, sizeof info) == 0 || info.State == MEM_FREE, "~Loader frees the TLS block");
    (void)base;
}

void test_tls_rejections() {
    std::printf("\n-- TLS directory rejections --\n");
    TempDir temp("tls_reject");
    auto expect_rejected = [&](const char* what, std::vector<uint8_t> img, const char* name) {
        try {
            rt::Loader loader(temp.path);
            loader.load(write_image(temp.path, name, img));
            check(false, what);
        } catch (const std::exception&) {
            check(true, what);
        }
    };
    {
        auto img = make_tls_data_dll(0x180000000, static_cast<uint32_t>(sizeof(IMAGE_TLS_DIRECTORY64)));
        auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(text_at(img, 0x11B0));
        const uint64_t base = image_base(img);
        tls->StartAddressOfRawData = base + 0x1180;
        tls->EndAddressOfRawData = base + 0x1170;  // end before start
        expect_rejected("a TLS template that ends before it starts is refused", img, "tls_backwards.dll");
    }
    {
        auto img = make_tls_data_dll(0x180000000, static_cast<uint32_t>(sizeof(IMAGE_TLS_DIRECTORY64)));
        auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(text_at(img, 0x11B0));
        tls->StartAddressOfRawData = image_base(img) + 0x9000;  // far past SizeOfImage
        tls->EndAddressOfRawData = tls->StartAddressOfRawData + 12;
        expect_rejected("a TLS template outside the image is refused", img, "tls_outside.dll");
    }
    {
        auto img = make_tls_data_dll(0x180000000, static_cast<uint32_t>(sizeof(IMAGE_TLS_DIRECTORY64)));
        auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(text_at(img, 0x11B0));
        tls->AddressOfIndex = image_base(img) + 0x1FFF;  // last byte: no room for the index dword
        expect_rejected("an AddressOfIndex without four bytes inside the image is refused", img, "tls_index_edge.dll");
    }
}

// ---- AUDIT-F3: DllMain thread events ------------------------------------------------------------------------------

void test_dllmain_thread_events() {
    std::printf("\n-- thread events: TLS callbacks and DllMain --\n");
    TempDir temp("thread_events");
    reset_observers();
    rt::Loader loader(temp.path);
    rt::Module& m = loader.load(write_image(temp.path, "event_dll.dll", make_event_dll(0x180000000)));
    loader.initialise();
    const DWORD main_thread = GetCurrentThreadId();
    const auto after_attach = observed_snapshot();
    check(observed_count(after_attach, DLL_PROCESS_ATTACH, true, main_thread) == 1,
          "the mapped TLS callback receives DLL_PROCESS_ATTACH once");
    check(observed_count(after_attach, DLL_PROCESS_ATTACH, false, main_thread) == 1,
          "the mapped DllMain receives DLL_PROCESS_ATTACH once");
    const uint64_t attach_tls = observed_at(after_attach, DLL_PROCESS_ATTACH, true, main_thread);
    const uint64_t attach_main = observed_at(after_attach, DLL_PROCESS_ATTACH, false, main_thread);
    check(attach_tls != 0 && attach_main != 0 && attach_tls < attach_main,
          "on process attach the TLS callback runs before DllMain");

    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct Ctx { HANDLE go, started; } ctx{go, started};
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            auto* c = static_cast<Ctx*>(p);
            SetEvent(c->started);
            WaitForSingleObject(c->go, INFINITE);
            return 0;
        },
        &ctx, 0, nullptr);
    check(thread != nullptr, "a worker thread is created after initialise()");
    if (!thread) return;
    WaitForSingleObject(started, 10000);
    const DWORD tid = GetThreadId(thread);
    Sleep(100);  // let the helper's DLL_THREAD_ATTACH event run on the worker
    const auto during = observed_snapshot();
    check(observed_count(during, DLL_THREAD_ATTACH, true, tid) == 1,
          "a thread started after load() gets the mapped TLS callback DLL_THREAD_ATTACH");
    fault_check(observed_count(during, DLL_THREAD_ATTACH, false, tid) != 1, "AUDIT-F3",
                "a thread started after load() gets DllMain DLL_THREAD_ATTACH (Windows delivers it too)");
    SetEvent(go);
    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    CloseHandle(go);
    CloseHandle(started);
    Sleep(100);  // the thread's DLL_THREAD_DETACH event runs as the thread leaves
    const auto after = observed_snapshot();
    check(observed_count(after, DLL_THREAD_DETACH, true, tid) == 1,
          "the ending thread gets the mapped TLS callback DLL_THREAD_DETACH once");
    fault_check(observed_count(after, DLL_THREAD_DETACH, false, tid) != 1, "AUDIT-F3",
                "the ending thread gets DllMain DLL_THREAD_DETACH once (Windows delivers it too)");
    const uint64_t detach_tls = observed_at(after, DLL_THREAD_DETACH, true, tid);
    const uint64_t detach_main = observed_at(after, DLL_THREAD_DETACH, false, tid);
    fault_check(detach_main == 0 || detach_tls > detach_main, "AUDIT-F3",
                "on thread detach the TLS callback runs before DllMain (Windows order)");
    // Keep the module alive to the end of the scope so the thread's detach event sees it.
    check(m.initialised, "the module stays initialised while its thread events run");
}

// ---- AUDIT-F1: /GA index-0 view and low-slot refusal -------------------------------------------------------------

void test_ga_index0_slot_isolation() {
    std::printf("\n-- /GA index-0 TLS: two mapped images --\n");
    TempDir temp("ga_slots");
    const fs::path path_a = write_image(temp.path, "ga_a.exe", make_ga_exe(0x180000000, 7));
    const fs::path path_b = write_image(temp.path, "ga_b.exe", make_ga_exe(0x190000000, 9));
    using GaRead = int (*)();
    rt::Loader loader(temp.path);
    rt::Module& a = loader.load(path_a);
    check(a.tls_ga_slot != rt::kNoTlsIndex && a.tls_ga_sites == 1,
          "the first /GA image's index-0 read is rewritten");
    if (a.tls_ga_slot == rt::kNoTlsIndex || a.tls_ga_sites != 1) return;
    const auto read_a = reinterpret_cast<GaRead>(a.base + 0x1000);
    check(read_a() == 7, "the first /GA image reads its own template with one image mapped");

    rt::Module& b = loader.load(path_b);
    check(b.tls_ga_slot != rt::kNoTlsIndex && b.tls_ga_sites == 1,
          "the second /GA image's index-0 read is rewritten");
    if (b.tls_ga_slot == rt::kNoTlsIndex || b.tls_ga_sites != 1) return;
    const auto read_b = reinterpret_cast<GaRead>(b.base + 0x1000);
    std::printf("     ga slots: first=%lu second=%lu; reads: first=%d second=%d (expect 7 and 9)\n",
                static_cast<unsigned long>(a.tls_ga_slot), static_cast<unsigned long>(b.tls_ga_slot), read_a(),
                read_b());
    fault_check(a.tls_ga_slot == b.tls_ga_slot, "AUDIT-F1",
                "each mapped /GA image gets its own TlsAlloc cell for index-0 reads");
    fault_check(read_a() != 7, "AUDIT-F1",
                "the first /GA image still reads its own TLS block after a second /GA image is mapped");
    check(read_b() == 9, "the second /GA image reads its own TLS block");
}

// AUDIT-F1 fallback: consume every TlsAlloc slot below the fixed TEB TlsSlots limit, then require a named refusal.
struct HeldTlsAllocSlots {
    std::vector<DWORD> values;
    ~HeldTlsAllocSlots() {
        for (DWORD value : values) TlsFree(value);
    }
};

void test_ga_index0_slot_exhaustion() {
    std::printf("\n-- /GA index-0 TLS: no TlsAlloc slot below 64 --\n");
    HeldTlsAllocSlots held;
    held.values.reserve(64);
    bool no_low_slot = false;
    for (unsigned attempt = 0; attempt <= 64; ++attempt) {
        const DWORD slot = TlsAlloc();
        if (slot == TLS_OUT_OF_INDEXES) {
            no_low_slot = true;
            break;
        }
        if (slot >= 64) {
            TlsFree(slot);
            no_low_slot = true;
            break;
        }
        held.values.push_back(slot);
    }
    check(no_low_slot, "TlsAlloc has no free slot below 64 after exhaustion");
    if (!no_low_slot) return;

    TempDir temp("ga_no_slot");
    const fs::path path = write_image(temp.path, "ga_no_low_slot.exe", make_ga_exe(0x1A0000000, 7));
    bool rejected = false;
    std::string message;
    {
        rt::Loader loader(temp.path);
        try {
            loader.load(path);
        } catch (const std::exception& e) {
            rejected = true;
            message = e.what();
        }
    }  // Release the mapped module before restoring the TlsAlloc pool.
    std::printf("     load %s%s\n", rejected ? "threw: " : "accepted", rejected ? message.c_str() : "");
    const bool fault_reproduced = !rejected || message.find("GA_TLS_SLOT_UNAVAILABLE") == std::string::npos;
    fault_check(fault_reproduced, "AUDIT-F1",
                "a /GA EXE is refused by GA_TLS_SLOT_UNAVAILABLE when no TlsAlloc slot below 64 remains");
}

// ---- AUDIT-F2: module TLS index >= 64 (child process) ----------------------------------------------------------------

int run_child(const std::wstring& arguments, DWORD* exit_code) {
    wchar_t self[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return 1;
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
        return 1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    if (!GetExitCodeProcess(process.hProcess, exit_code)) *exit_code = 0xFFFFFFFFu;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}

// Child: load more than 64 static-TLS modules into the Windows process first (each gets its own OS module TLS index),
// prove Windows supports the high index, then map a TLS image with the audited loader. Exit codes: 0 accepted,
// 10 rejected with the index-cap error, 11/12/13 premise or TLS-read failures.
int run_high_index_child(const fs::path& tiny_tls) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TempDir temp(("high_index_child_" + std::to_string(GetCurrentProcessId())).c_str());
    std::vector<HMODULE> copies;
    DWORD highest = 0;
    int high_count = 0;
    for (int i = 0; i < 70; ++i) {
        const fs::path copy = temp.path / ("tiny_tls_copy_" + std::to_string(i) + ".dll");
        if (!CopyFileW(tiny_tls.c_str(), copy.c_str(), FALSE)) {
            std::printf("child: copy %d failed (%lu)\n", i, GetLastError());
            return 11;
        }
        HMODULE module = LoadLibraryExW(copy.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            std::printf("child: LoadLibrary copy %d failed (%lu) after %zu modules\n", i, GetLastError(), copies.size());
            return 11;
        }
        copies.push_back(module);
        auto* base = reinterpret_cast<uint8_t*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        DWORD index = 0xFFFFFFFF;
        if (dir.Size && dir.VirtualAddress) {
            auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(base + dir.VirtualAddress);
            if (tls->AddressOfIndex) index = *reinterpret_cast<DWORD*>(tls->AddressOfIndex);
        }
        if (index != 0xFFFFFFFF && index >= 64) ++high_count;
        if (index != 0xFFFFFFFF && index >= highest) highest = index;
    }
    std::printf("child: %zu static-TLS modules loaded through Windows; highest module TLS index %lu (%d above 63)\n",
                copies.size(), static_cast<unsigned long>(highest), high_count);
    if (high_count == 0) {
        std::printf("child: premise failed, no index above 63 was assigned\n");
        return 11;
    }
    // Windows supports the high index: read the template value of the highest-index module from a fresh thread.
    HMODULE highest_module = nullptr;
    for (size_t i = 0; i < copies.size(); ++i) {
        auto* base = reinterpret_cast<uint8_t*>(copies[i]);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(base + dir.VirtualAddress);
        if (*reinterpret_cast<DWORD*>(tls->AddressOfIndex) == highest) highest_module = copies[i];
    }
    if (!highest_module) return 12;
    auto get = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(highest_module, "tiny_tls_get")));
    if (!get) {
        std::printf("child: the fixture does not export tiny_tls_get\n");
        return 12;
    }
    std::atomic<int> read_value{-1};
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct ReadCtx { int (*get)(); std::atomic<int>* value; HANDLE done; } read_ctx{get, &read_value, done};
    HANDLE reader = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            auto* c = static_cast<ReadCtx*>(p);
            c->value->store(c->get());
            SetEvent(c->done);
            return 0;
        },
        &read_ctx, 0, nullptr);
    WaitForSingleObject(done, 10000);
    CloseHandle(reader);
    CloseHandle(done);
    if (read_value.load() != 7) {
        std::printf("child: Windows read the index-%lu module's TLS as %d, not 7\n",
                    static_cast<unsigned long>(highest), read_value.load());
        return 12;
    }
    std::printf("child: a fresh thread read the index-%lu module's TLS value 7, so the OS supports that index\n",
                static_cast<unsigned long>(highest));
    try {
        rt::Loader loader(tiny_tls.parent_path());
        rt::Module& m = loader.load(tiny_tls);
        auto get_mapped = reinterpret_cast<int (*)()>(
            m.exports.find("tiny_tls_get") == m.exports.end() ? nullptr : m.exports.at("tiny_tls_get"));
        if (!get_mapped) return 13;
        const int value = get_mapped();
        if (value != 7) {
            std::printf("child: the mapped fixture's TLS read %d, not 7\n", value);
            return 13;
        }
        std::printf("child: accepted; mapped module TLS index %u\n", m.tls_index);
        return 0;
    } catch (const std::exception& e) {
        std::printf("child: the loader rejected the TLS image: %s\n", e.what());
        return 10;
    }
}

void test_module_tls_index_beyond_64(const fs::path& tiny_tls) {
    std::printf("\n-- module TLS index beyond the TlsAlloc slot count --\n");
    DWORD code = 0;
    if (run_child(L"--child-high-index \"" + tiny_tls.wstring() + L"\"", &code) != 0) {
        check(false, "the high-module-index child process could be measured");
        return;
    }
    std::printf("     child exit code 0x%08lx\n", static_cast<unsigned long>(code));
    if (code == 0) {
        check(true, "the loader accepts an OS-assigned module TLS index above 63");
    } else if (code == 10) {
        fault_check(true, "AUDIT-F2",
                    "the loader accepts a TLS image whose reserved module index is above 63 (TlsAlloc's cap)");
    } else if (code == 11) {
        check(false, "the high-module-index premise holds (the OS assigned module TLS indices above 63)");
    } else {
        check(false, "the high-module-index child measured the mapped image's TLS (unexpected child failure)");
    }
}

// ---- AUDIT-F6: indirect unwind-data entry -------------------------------------------------------------------------

void test_unwind_registration() {
    std::printf("\n-- exception directory registration --\n");
    TempDir temp("unwind");
    {
        // The sound shape: two adjacent functions, one shared UNWIND_INFO, looked up through the registry.
        std::vector<RUNTIME_FUNCTION> entries = {{0x1000, 0x1010, 0x1180}, {0x1010, 0x1020, 0x1180}};
        const fs::path path = write_image(temp.path, "unwind_ok.dll", make_unwind_image(0x180000000, entries));
        uintptr_t pc = 0;
        {
            rt::Loader loader(temp.path);
            rt::Module& m = loader.load(path);
            pc = reinterpret_cast<uintptr_t>(m.base) + 0x1014;
            DWORD64 found_base = 0;
            const auto* found = RtlLookupFunctionEntry(static_cast<DWORD64>(pc), &found_base, nullptr);
            check(m.function_count == 2, "the exception directory's entry count is registered");
            check(found != nullptr && found_base == reinterpret_cast<DWORD64>(m.base),
                  "RtlLookupFunctionEntry finds the table in the mapped image");
            if (found)
                check(reinterpret_cast<const uint8_t*>(found) == m.base + 0x1100 + sizeof(RUNTIME_FUNCTION),
                      "the lookup returns the image's second RUNTIME_FUNCTION");
        }
        DWORD64 found_base = 0;
        check(pc != 0 && RtlLookupFunctionEntry(static_cast<DWORD64>(pc), &found_base, nullptr) == nullptr,
              "~Loader removes the function table before unmapping");
    }
    {
        // A trailing partial entry is tolerated, as the system loader does.
        std::vector<RUNTIME_FUNCTION> entries = {{0x1000, 0x1010, 0x1180}, {0x1010, 0x1020, 0x1180}};
        auto img = make_unwind_image(0x180000000, entries);
        auto& dir = nt_of(img)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        dir.Size += 5;
        try {
            rt::Loader loader(temp.path);
            rt::Module& m = loader.load(write_image(temp.path, "unwind_partial.dll", img));
            check(m.function_count == 2, "a trailing partial exception entry is ignored");
        } catch (const std::exception&) {
            check(false, "a trailing partial exception entry is ignored");
        }
    }
    {
        // AUDIT-F6: UnwindData bit 0 marks the indirect form, whose masked pointer is a chained RUNTIME_FUNCTION. The
        // loader only checks that the masked pointer has four bytes in the image and registers the table; the chained
        // entry itself (here: UnwindData far outside the image) is handed to the unwinder unchecked.
        std::vector<RUNTIME_FUNCTION> entries = {{0x1000, 0x1010, 0x11A1}};  // 0x11A0 | indirect
        auto img = make_unwind_image(0x180000000, entries);
        auto* chain = reinterpret_cast<RUNTIME_FUNCTION*>(text_at(img, 0x11A0));
        chain->BeginAddress = 0x1000;
        chain->EndAddress = 0x1010;
        chain->UnwindData = 0xFFFF0000;  // outside SizeOfImage: the unwinder would follow it
        bool rejected = false;
        std::string message;
        try {
            rt::Loader loader(temp.path);
            loader.load(write_image(temp.path, "unwind_indirect.dll", img));
        } catch (const std::exception& e) {
            rejected = true;
            message = e.what();
        }
        std::printf("     indirect entry load %s%s\n", rejected ? "threw: " : "accepted", rejected ? message.c_str() : "");
        fault_check(!rejected, "AUDIT-F6",
                    "an indirect unwind entry whose chained RUNTIME_FUNCTION has unwind data outside the image is refused");
    }
}

// ---- AUDIT-F0: DllMain -> runtime load of a bundled module ---------------------------------------------------------

// Child: map a DLL whose DllMain calls LoadLibraryA("audit_dep.dll") through the shim loader. Exit codes: 0 the load
// completed and the dependency is initialised, 3 the load completed with the wrong module set, anything else is the
// crash the re-entrant initialise() produces (0xC00000FD = STATUS_STACK_OVERFLOW).
int run_nested_load_child(const fs::path& dir) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        rt::Loader loader(dir);
        rt::Module& importer = loader.load(dir / "nested_importer.dll");
        loader.initialise();
        if (!importer.initialised) return 3;
        if (!loader.find_loaded_module("audit_dep.dll")) return 3;
        rt::Module* dependency = nullptr;
        for (rt::Module* module : loader.modules())
            if (module->name == "audit_dep.dll") dependency = module;
        if (!dependency || !dependency->initialised) return 3;
        std::printf("child: the importer and its runtime-loaded dependency are initialised\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("child: nested load threw: %s\n", e.what());
        return 4;
    }
}

void test_nested_load_from_dllmain() {
    std::printf("\n-- DllMain runtime-loads a bundled module --\n");
    TempDir temp("nested");
    write_image(temp.path, "nested_importer.dll", make_nested_importer(0x180000000));
    write_image(temp.path, "audit_dep.dll", make_nested_dependency(0x190000000));
    DWORD code = 0;
    if (run_child(L"--child-nested-load \"" + temp.path.wstring() + L"\"", &code) != 0) {
        check(false, "the nested-load child process could be measured");
        return;
    }
    std::printf("     child exit code 0x%08lx\n", static_cast<unsigned long>(code));
    if (code == 0) {
        check(true, "a mapped DllMain can runtime-load a bundled module without re-entering its own attach");
    } else if (code == 0xC00000FDu) {
        fault_check(true, "AUDIT-F0",
                    "a mapped DllMain runtime-loading a bundled module does not stack-overflow in initialise() re-entry");
    } else if (code == 3 || code == 4) {
        fault_check(true, "AUDIT-F0",
                    "a mapped DllMain runtime-loads a bundled module and the load completes (initialise() re-entry)");
    } else {
        check(false, "the nested-load child process completes (unexpected crash code)");
    }
}

void print_result() {
    if (g_failures == 0) {
        std::printf("\nloader_tls_audit_test: OK (%d checks, %d reproduced faults)\n", g_checks, g_reproduced);
    } else {
        std::printf("\nloader_tls_audit_test: FAILED (%d checks, %d failure%s, %d reproduced faults)\n", g_checks,
                    g_failures, g_failures == 1 ? "" : "s", g_reproduced);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fs::path tiny_tls;
    bool child_high_index = false;
    bool child_nested = false;
    fs::path child_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--expect-faults") {
            g_expect_faults = true;
        } else if (arg == "--child-high-index" && i + 1 < argc) {
            child_high_index = true;
            tiny_tls = argv[++i];
        } else if (arg == "--child-nested-load" && i + 1 < argc) {
            child_nested = true;
            child_dir = argv[++i];
        } else if (arg.rfind("--", 0) == 0) {
            std::printf("unknown option %s\n", arg.c_str());
            return 2;
        } else if (tiny_tls.empty()) {
            tiny_tls = arg;
        }
    }
    if (child_high_index) return run_high_index_child(tiny_tls);
    if (child_nested) return run_nested_load_child(child_dir);
    std::printf("loader_tls_audit_test: static TLS, thread events, unwind and lifecycle contracts\n");
    if (tiny_tls.empty()) {
        std::fprintf(stderr, "usage: loader_tls_audit_test <tiny_tls.dll> [--expect-faults]\n");
        return 2;
    }
    try {
        check(fs::exists(tiny_tls), "the tiny_tls fixture exists");
        if (!fs::exists(tiny_tls)) {
            print_result();
            return 1;
        }
        g_host_tls_sentinel = 2;
        test_tls_directory_size();
        test_tls_block_contract();
        test_tls_rejections();
        test_dllmain_thread_events();
        test_ga_index0_slot_isolation();
        test_ga_index0_slot_exhaustion();
        test_unwind_registration();
        test_nested_load_from_dllmain();
        test_module_tls_index_beyond_64(tiny_tls);
        check(g_host_tls_sentinel == 2, "this EXE's own TLS at module index 0 is untouched");
    } catch (const std::exception& e) {
        ++g_checks;
        ++g_failures;
        std::printf("FAIL  unexpected exception: %s\n", e.what());
    }
    print_result();
    return g_failures ? 1 : 0;
}
