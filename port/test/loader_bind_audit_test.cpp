// Import/export binding contract audit for the inherited PE loader (port/src/pe_loader.cpp).
//
// The images are built byte by byte here (no game data, no files from the repo), each case maps a synthetic module
// through the public rt::Loader API, and the assertions state the contract the loader is supposed to keep. The list
// below is the audit's fault table; the "known fault" cases are the contracts the current loader does NOT keep, so
// each one prints "FAULT" until the loader is patched and "PATCHED" afterwards. They do not fail the binary unless
// PALMETTO_LOADER_AUDIT_STRICT=1 is set, which is the switch for the patch round: with it, a still-present fault is
// a test failure, so the suite can be added to CTest now and used as the regression gate once the fixes land.
//
// Ranked faults (file:line in pe_loader.cpp at the time of the audit):
//   F1 HIGH   bind_imports: an import descriptor with FirstThunk == 0 is accepted; the IAT walk starts at RVA 0 and
//             overwrites the mapped DOS/NT headers with the resolved pointers. The delay-import path already refuses
//             a missing address table (pe_loader.cpp:2107), the normal path does not (:2060-:2081, :2091-:2092).
//   F2 HIGH   resolve(): when a *bundled* module exists but does not export the symbol, binding falls through to a
//             host LoadLibraryEx of the same DLL name (:1776-:1801), silently substituting a System32 implementation
//             for the package's console module. resolve_forwarder() deliberately does not do this (:1724).
//   F3 MED    bind_thunks: ordinal thunks are masked with IMAGE_ORDINAL64 (low 16 bits) and the bits in between are
//             dropped (:2066-:2067), so 0x8000000000010005 binds ordinal 5. resolve_forwarder refuses a long ordinal
//             rather than truncating it (:1701-:1709), so the two ordinal paths disagree.
//   F4 MED    resolve_forwarder: a forwarder target name that exists in the package as a non-PE file (or a
//             directory) is passed to load(), whose exception aborts the whole importer load (:1713-:1717). A
//             forwarder that cannot be resolved is supposed to be a non-fatal "not found" (:1685).
//   F5 MED/LOW resolve_forwarder tries the configured candidate spellings in order but returns after the first
//             existing one (:1713-:1725), so an extensionless file in the package shadows the target's ".dll" file.
//   F6 LOW    parse_exports: an export whose RVA lands in the image headers is accepted and handed to callers as
//             base+rva (:1665-:1673); relocations that land in the headers are refused (:1497-:1500), so a caller
//             can jump into the DOS header.
//   F7 LOW    bind_imports: a bound image (BOUND_IMPORT directory, no name table) has no way to be rebound from
//             names; the failure is a generic bounds message from reading the pre-bound IAT as a name table
//             (:2091, :2069-:2072) instead of a diagnostic that names the bound-import condition.
//
// What the suite pins down as already correct: the 8-byte IAT slot write and the untouched terminator, a by-ordinal
// import, the FirstThunk fallback for OriginalFirstThunk == 0, eager delay-import binding and its descriptor
// refusals, export aliases (several names to one address) and forwarded aliases, forwarder chains and forwarder
// loops, the IAT being writable while bound and read-only afterwards, the complete IAT being visible before DllMain
// runs, missing-symbol failure thunks, run-time LoadLibrary/GetProcAddress/mapping of bundled modules, an import
// cycle between two bundled modules, and a pre-bound IAT being rebuilt from the name table.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/pe_loader.h"

namespace {

int g_checks = 0;
int g_held = 0;
int g_failed = 0;
int g_faults = 0;
int g_patched = 0;
std::filesystem::path g_dir;

// File layout shared by every fixture: one .text section at RVA 0x1000 with 0x400 bytes backed by the file at
// offset 0x200; the image is 0x2000. All tables/strings live between 0x1000 and 0x1400, so they are in a
// read+execute (never writable) section, which is what the IAT-protection contract needs to observe.
constexpr uint32_t kTextRva = 0x1000;
constexpr uint32_t kTextBytes = 0x400;
constexpr uint32_t kFileText = 0x200;
constexpr uint32_t kSizeOfHeaders = 0x200;
constexpr uint32_t kSizeOfImage = 0x2000;

// Import layout. The export layout of make_exports_image() shares 0x1000..0x1300 when a module has both.
constexpr uint32_t kImportDescRva = 0x1000;
constexpr uint32_t kOftRva = 0x1080;
constexpr uint32_t kIatRva = 0x1100;
constexpr uint32_t kHintNameRva = 0x1180;
constexpr uint32_t kDllNameRva = 0x1300;
constexpr uint32_t kEntryStubRva = 0x13C0;

// Export layout: the export directory at 0x1000, its tables from 0x1040, name strings from 0x1100, forwarder
// strings from 0x1200. The directory's Size is 0x300, so an address-table entry inside 0x1000..0x1300 is a
// forwarder string by the PE rule; code exports live at kCodeRva outside that range.
constexpr uint32_t kExportDirRva = 0x1000;
constexpr uint32_t kExportFuncsRva = 0x1040;
constexpr uint32_t kExportNamesRva = 0x1080;
constexpr uint32_t kExportOrdsRva = 0x10C0;
constexpr uint32_t kExportStringsRva = 0x1100;
constexpr uint32_t kForwardStringsRva = 0x1200;
constexpr uint32_t kExportDirSize = 0x300;
constexpr uint32_t kCodeRva = 0x13C0;

bool strict_audit() {
    char text[8] = {};
    const DWORD n = GetEnvironmentVariableA("PALMETTO_LOADER_AUDIT_STRICT", text, sizeof text);
    return n == 1 && text[0] == '1';
}

void pass(const char* what) {
    ++g_checks;
    ++g_held;
    std::printf("ok      %s\n", what);
}

void failed(const char* what, const std::string& why) {
    ++g_checks;
    ++g_failed;
    std::printf("FAIL    %s: %s\n", what, why.c_str());
}

// A contract the current loader does not keep. The test states the expected behaviour; this reports what the loader
// did instead. Strict mode makes a still-present fault fail the run, which is the switch for the patch round.
void fault(const char* what, const std::string& observed) {
    ++g_checks;
    ++g_faults;
    std::printf("FAULT   %s: %s\n", what, observed.c_str());
}

void patched(const char* what) {
    ++g_checks;
    ++g_patched;
    std::printf("PATCHED %s\n", what);
}

std::filesystem::path write_image_as(const std::vector<uint8_t>& bytes, const std::string& name) {
    const auto p = g_dir / name;
    std::ofstream out(p, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + p.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<long long>(bytes.size()));
    return p;
}

void write_data_file(const std::string& name, const char* text) {
    const auto p = g_dir / name;
    std::ofstream out(p, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + p.string());
    out.write(text, static_cast<long long>(std::strlen(text)));
}

template <class T>
T* rva_at(std::vector<uint8_t>& img, uint32_t rva) {
    if (rva < kTextRva || rva >= kTextRva + kTextBytes) throw std::runtime_error("fixture RVA outside .text");
    return reinterpret_cast<T*>(img.data() + kFileText + (rva - kTextRva));
}

IMAGE_NT_HEADERS64* nt_of(std::vector<uint8_t>& bytes) {
    return reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes.data() + 0x80);
}

void emit_jump_stub(std::vector<uint8_t>& img, size_t off, uint64_t target) {
    img[off++] = 0x48;
    img[off++] = 0xB8;  // movabs rax, target; jmp rax (preserves the DllMain arguments)
    std::memcpy(img.data() + off, &target, sizeof target);
    off += sizeof target;
    img[off++] = 0xFF;
    img[off] = 0xE0;
}

// Copies a NUL-terminated string used by a fixture, without the deprecation warnings of strcpy.
void copy_text(std::vector<uint8_t>& img, uint32_t rva, const char* text) {
    std::memcpy(rva_at<char>(img, rva), text, std::strlen(text) + 1);
}

std::vector<uint8_t> make_base(uint64_t preferred) {
    std::vector<uint8_t> img(0x600, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = nt_of(img);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.ImageBase = preferred;
    nt->OptionalHeader.AddressOfEntryPoint = kTextRva;
    nt->OptionalHeader.SizeOfHeaders = kSizeOfHeaders;
    nt->OptionalHeader.SizeOfImage = kSizeOfImage;
    nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    std::memcpy(sec->Name, ".text", 6);
    sec->VirtualAddress = kTextRva;
    sec->Misc.VirtualSize = kTextBytes;
    sec->PointerToRawData = kFileText;
    sec->SizeOfRawData = kTextBytes;
    sec->Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    return img;
}

// ---- import fixtures ------------------------------------------------------------------------------------------

struct ImportThunk {
    const char* name = nullptr;  // import by name
    uint16_t ordinal = 0;        // import by ordinal
    uint64_t raw = 0;            // non-zero: the thunk value verbatim (an ordinal thunk with stray bits)
};

struct ImportImageOptions {
    bool original_first_thunk = true;  // false: OriginalFirstThunk is zero and the walk falls back to FirstThunk
    bool iat_holds_names = false;      // with original_first_thunk == false: the initial IAT really holds name RVAs
    bool prebound = false;             // descriptor TimeDateStamp set, BOUND_IMPORT directory present, IAT full of VAs
    uint32_t first_thunk = kIatRva;    // 0: an import descriptor with no address table at all
    bool dll = false;
    void* dllmain = nullptr;  // when set, the entry point jumps to it (so the test can read the IAT from DllMain)
};

std::vector<uint8_t> make_import_image(uint64_t preferred, const char* dll, const std::vector<ImportThunk>& thunks,
                                       const ImportImageOptions& opts = {}) {
    auto img = make_base(preferred);
    auto* nt = nt_of(img);
    if (opts.dll) nt->FileHeader.Characteristics |= IMAGE_FILE_DLL;
    if (opts.dllmain) {
        nt->OptionalHeader.AddressOfEntryPoint = kEntryStubRva;
        emit_jump_stub(img, kFileText + (kEntryStubRva - kTextRva),
                       reinterpret_cast<uint64_t>(opts.dllmain));
    }
    auto* d = rva_at<IMAGE_IMPORT_DESCRIPTOR>(img, kImportDescRva);
    d->Name = kDllNameRva;
    d->OriginalFirstThunk = opts.original_first_thunk ? kOftRva : 0;
    d->FirstThunk = opts.first_thunk;
    d->TimeDateStamp = opts.prebound ? 1 : 0;

    auto* oft = opts.original_first_thunk ? rva_at<uint64_t>(img, kOftRva) : nullptr;
    auto* iat = opts.first_thunk ? rva_at<uint64_t>(img, opts.first_thunk) : nullptr;
    for (size_t i = 0; i < thunks.size(); ++i) {
        uint64_t value = 0;
        if (thunks[i].name) {
            const uint32_t name_rva = kHintNameRva + static_cast<uint32_t>(i) * 0x20;
            auto* entry = rva_at<uint8_t>(img, name_rva);
            *reinterpret_cast<uint16_t*>(entry) = 0;  // hint
            std::memcpy(entry + 2, thunks[i].name, std::strlen(thunks[i].name) + 1);
            value = name_rva;
        } else if (thunks[i].raw) {
            value = thunks[i].raw;
        } else {
            value = 0x8000000000000000ull | thunks[i].ordinal;
        }
        if (oft) oft[i] = value;
        if (iat) {
            if (opts.prebound)
                iat[i] = 0x00007FF812345678ull + static_cast<uint64_t>(i) * 8;  // a bound address, not a name RVA
            else if (opts.iat_holds_names)
                iat[i] = value;
        }
    }
    if (opts.prebound) {
        auto& bound = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT];
        bound.VirtualAddress = 0x1340;
        bound.Size = 8;  // zero bytes there; the loader must not trust them
    }
    std::memcpy(rva_at<char>(img, kDllNameRva), dll, std::strlen(dll) + 1);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    dir.VirtualAddress = kImportDescRva;
    dir.Size = 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR);
    return img;
}

std::vector<uint8_t> make_delay_import_image(uint64_t preferred, const char* dll,
                                             const std::vector<ImportThunk>& thunks, bool rva_based = true,
                                             bool with_name_table = true) {
    auto img = make_base(preferred);
    auto* nt = nt_of(img);
    auto* d = rva_at<IMAGE_DELAYLOAD_DESCRIPTOR>(img, kImportDescRva);
    d->Attributes.AllAttributes = rva_based ? 1 : 0;
    d->DllNameRVA = kDllNameRva;
    d->ImportNameTableRVA = with_name_table ? kOftRva : 0;
    d->ImportAddressTableRVA = kIatRva;
    auto* ints = rva_at<uint64_t>(img, kOftRva);
    for (size_t i = 0; i < thunks.size(); ++i) {
        const uint32_t name_rva = kHintNameRva + static_cast<uint32_t>(i) * 0x20;
        auto* entry = rva_at<uint8_t>(img, name_rva);
        const char* symbol = thunks[i].name ? thunks[i].name : "?";
        *reinterpret_cast<uint16_t*>(entry) = 0;
        std::memcpy(entry + 2, symbol, std::strlen(symbol) + 1);
        ints[i] = name_rva;
    }
    copy_text(img, kDllNameRva, dll);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    dir.VirtualAddress = kImportDescRva;
    dir.Size = 2 * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
    return img;
}

// ---- export fixtures ------------------------------------------------------------------------------------------

struct ExportItem {
    const char* name = nullptr;        // null: the entry contributes only its ordinal name ("#N")
    uint16_t index = 0;                // index into the address table (ordinal = Base + index)
    uint32_t rva = 0;                  // code export address, when forward_to is null
    const char* forward_to = nullptr;  // when set: the address-table entry points at this string inside the directory
};

std::vector<uint8_t> make_exports_image(uint64_t preferred, uint32_t ordinal_base,
                                        const std::vector<ExportItem>& items) {
    auto img = make_base(preferred);
    auto* nt = nt_of(img);
    uint32_t funcs_count = 0, names_count = 0;
    for (const auto& it : items) {
        funcs_count = std::max<uint32_t>(funcs_count, static_cast<uint32_t>(it.index) + 1);
        if (it.name) ++names_count;
    }
    auto* funcs = rva_at<uint32_t>(img, kExportFuncsRva);
    auto* names = rva_at<uint32_t>(img, kExportNamesRva);
    auto* ords = rva_at<uint16_t>(img, kExportOrdsRva);
    uint32_t named = 0, forwarded = 0;
    for (const auto& it : items) {
        uint32_t rva = it.rva;
        if (it.forward_to) {
            rva = kForwardStringsRva + forwarded * 0x20;
            ++forwarded;
            copy_text(img, rva, it.forward_to);
        }
        funcs[it.index] = rva;
        if (it.name) {
            const uint32_t name_rva = kExportStringsRva + named * 0x20;
            copy_text(img, name_rva, it.name);
            names[named] = name_rva;
            ords[named] = it.index;
            ++named;
        }
    }
    auto* ed = rva_at<IMAGE_EXPORT_DIRECTORY>(img, kExportDirRva);
    ed->Base = ordinal_base;
    ed->NumberOfFunctions = funcs_count;
    ed->NumberOfNames = names_count;
    ed->AddressOfFunctions = kExportFuncsRva;
    ed->AddressOfNames = kExportNamesRva;
    ed->AddressOfNameOrdinals = kExportOrdsRva;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    dir.VirtualAddress = kExportDirRva;
    dir.Size = kExportDirSize;
    return img;
}

// A module that both imports and exports, for the two-module import-cycle case. The export directory is small
// (just the fixed structure), so the exported "code" at kCodeRva is not mistaken for a forwarder.
std::vector<uint8_t> make_cycle_image(uint64_t preferred, const char* exported_name, const char* imported_dll,
                                      const char* imported_symbol) {
    auto img = make_import_image(preferred, imported_dll, {{imported_symbol}});
    auto* nt = nt_of(img);
    constexpr uint32_t dir_rva = 0x1340, funcs_rva = 0x1380, names_rva = 0x13A0, ords_rva = 0x13B0,
                       name_rva = 0x1390;
    auto* ed = rva_at<IMAGE_EXPORT_DIRECTORY>(img, dir_rva);
    ed->Base = 1;
    ed->NumberOfFunctions = 1;
    ed->NumberOfNames = 1;
    ed->AddressOfFunctions = funcs_rva;
    ed->AddressOfNames = names_rva;
    ed->AddressOfNameOrdinals = ords_rva;
    *rva_at<uint32_t>(img, funcs_rva) = kCodeRva;
    *rva_at<uint32_t>(img, names_rva) = name_rva;
    *rva_at<uint16_t>(img, ords_rva) = 0;
    copy_text(img, name_rva, exported_name);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    dir.VirtualAddress = dir_rva;
    dir.Size = sizeof(IMAGE_EXPORT_DIRECTORY);
    return img;
}

// ---- helpers --------------------------------------------------------------------------------------------------

void* host_symbol(const char* dll, const char* name) {
    const HMODULE h = GetModuleHandleA(dll);
    return h ? reinterpret_cast<void*>(GetProcAddress(h, name)) : nullptr;
}

// Reads the IAT slot of a mapped image (the fixture puts its address table at kIatRva).
uint64_t iat_slot(const rt::Module& m, uint32_t index = 0) {
    return *reinterpret_cast<const uint64_t*>(m.base + kIatRva + index * sizeof(uint64_t));
}

const char* binding_name(rt::Binding b) {
    switch (b) {
        case rt::Binding::Shim: return "Shim";
        case rt::Binding::Host: return "Host";
        case rt::Binding::Module: return "Module";
        case rt::Binding::Unimplemented: return "Unimplemented";
    }
    return "?";
}

// DllMain observer: records the IAT slot value the mapped image sees as the first image code that runs.
void* g_observer_base = nullptr;
uint64_t g_observed_iat = 0;

BOOL WINAPI observe_dllmain(void* base, DWORD reason, void* reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH && base == g_observer_base)
        g_observed_iat = *reinterpret_cast<const uint64_t*>(static_cast<uint8_t*>(base) + kIatRva);
    return TRUE;
}

}  // namespace

// =================================================================================================================
// Contracts the loader keeps today.
// =================================================================================================================

namespace {

void test_iat_slot_write() {
    const char* what = "a by-name import writes its resolved pointer into exactly its 8-byte IAT slot";
    try {
        const auto importer =
            write_image_as(make_import_image(0x140000000, "kernel32.dll",
                                             {{"GetCurrentProcessId"}, {"GetCurrentThreadId"}}),
                           "iat_slots.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        void* pid = host_symbol("kernel32.dll", "GetCurrentProcessId");
        void* tid = host_symbol("kernel32.dll", "GetCurrentThreadId");
        if (!pid || !tid) return failed(what, "the host symbols are not loaded in this process");
        if (m.imports.size() != 2) return failed(what, "expected two import records");
        if (m.imports[0].binding != rt::Binding::Host || m.imports[1].binding != rt::Binding::Host)
            return failed(what, "an import did not bind to the host");
        if (iat_slot(m, 0) != reinterpret_cast<uint64_t>(pid)) return failed(what, "the first IAT slot is wrong");
        if (iat_slot(m, 1) != reinterpret_cast<uint64_t>(tid)) return failed(what, "the second IAT slot is wrong");
        if (iat_slot(m, 2) != 0) return failed(what, "the loader wrote past the last import, into the terminator");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

void test_ordinal_import() {
    const char* what = "a by-ordinal import binds the ordinal export and records \"#N\"";
    try {
        write_image_as(make_exports_image(0x180000000, 1, {{nullptr, 6, kCodeRva}}), "ordprov.dll");
        const auto importer =
            write_image_as(make_import_image(0x140000000, "ordprov.dll", {{nullptr, 7, 0}}), "ordinal_import.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        rt::Module* dep = nullptr;
        for (auto* mod : loader.modules())
            if (mod->name == "ordprov.dll") dep = mod;
        if (!dep) return failed(what, "the ordinal provider was not loaded");
        if (m.imports.size() != 1 || m.imports[0].name != "#7")
            return failed(what, "the import record does not name ordinal 7");
        if (m.imports[0].binding != rt::Binding::Module) return failed(what, "the import did not bind to the module");
        if (iat_slot(m) != reinterpret_cast<uint64_t>(dep->base + kCodeRva))
            return failed(what, "the IAT slot does not hold the ordinal's export");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

void test_original_first_thunk_fallback() {
    const char* what = "OriginalFirstThunk == 0 falls back to FirstThunk as the name table";
    try {
        ImportImageOptions opts;
        opts.original_first_thunk = false;
        opts.iat_holds_names = true;  // an unbound image with no OFT holds name RVAs in the IAT
        const auto importer = write_image_as(
            make_import_image(0x140000000, "kernel32.dll", {{"GetCurrentProcessId"}}, opts), "oft_zero.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        void* pid = host_symbol("kernel32.dll", "GetCurrentProcessId");
        if (!pid) return failed(what, "GetCurrentProcessId is not loaded in this process");
        if (m.imports.size() != 1 || m.imports[0].binding != rt::Binding::Host)
            return failed(what, "the fallback did not bind the import to the host");
        if (iat_slot(m) != reinterpret_cast<uint64_t>(pid))
            return failed(what, "the IAT slot was not filled from the fallback name table");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

void test_delay_imports() {
    {
        const char* what = "a delay import is bound eagerly into its delay address table";
        try {
            const auto importer = write_image_as(
                make_delay_import_image(0x140000000, "kernel32.dll", {{"GetCurrentProcessId"}}), "delay_ok.exe");
            rt::Loader loader(g_dir);
            rt::Module& m = loader.load(importer);
            void* pid = host_symbol("kernel32.dll", "GetCurrentProcessId");
            if (!pid) return failed(what, "GetCurrentProcessId is not loaded in this process");
            if (m.imports.size() != 1 || m.imports[0].binding != rt::Binding::Host)
                return failed(what, "the delay import did not bind to the host");
            if (iat_slot(m) != reinterpret_cast<uint64_t>(pid))
                return failed(what, "the delay address table was not written");
            pass(what);
        } catch (const std::exception& e) {
            failed(what, e.what());
        }
    }
    {
        const char* what = "a delay descriptor without the RVA attribute is refused by name";
        try {
            write_image_as(make_delay_import_image(0x140000000, "kernel32.dll", {{"GetCurrentProcessId"}}, false),
                           "delay_attr.exe");
            rt::Loader loader(g_dir);
            loader.load(g_dir / "delay_attr.exe");
            failed(what, "the image loaded");
        } catch (const std::exception& e) {
            if (std::string(e.what()).find("RVA attribute") == std::string::npos) failed(what, e.what());
            else pass(what);
        }
    }
    {
        const char* what = "a delay descriptor without a name table is refused by name";
        try {
            write_image_as(make_delay_import_image(0x140000000, "kernel32.dll", {{"GetCurrentProcessId"}}, true, false),
                           "delay_int.exe");
            rt::Loader loader(g_dir);
            loader.load(g_dir / "delay_int.exe");
            failed(what, "the image loaded");
        } catch (const std::exception& e) {
            if (std::string(e.what()).find("name table") == std::string::npos) failed(what, e.what());
            else pass(what);
        }
    }
}

void test_export_aliases() {
    const char* what = "export aliases (two names and an ordinal to one address) all resolve to that address";
    try {
        auto img = make_exports_image(0x180000000, 1,
                                      {
                                          {"alpha", 0, kCodeRva},
                                          {"beta", 0, kCodeRva},
                                          {nullptr, 0, kCodeRva},
                                          {"fwd", 1, 0, "KERNEL32.GetCurrentProcessId"},
                                          {nullptr, 1, 0, "KERNEL32.GetCurrentProcessId"},
                                      });
        write_image_as(img, "aliasprov.dll");
        rt::Loader loader(g_dir);
        rt::Module& dep = loader.load(g_dir / "aliasprov.dll");
        const uint64_t expected = reinterpret_cast<uint64_t>(dep.base + kCodeRva);
        for (const char* key : {"alpha", "beta", "#1"}) {
            if (!dep.exports.count(key)) return failed(what, std::string(key) + " is not in the export table");
            if (reinterpret_cast<uint64_t>(dep.exports[key]) != expected)
                return failed(what, std::string(key) + " does not point at the shared address");
        }
        void* forwarded = host_symbol("kernel32.dll", "GetCurrentProcessId");
        if (!forwarded) return failed(what, "GetCurrentProcessId is not loaded in this process");
        for (const char* key : {"fwd", "#2"}) {
            bool owns = false;
            if (loader.resolve_module_export(dep.base, key, owns) != forwarded)
                return failed(what, std::string(key) + " did not resolve through the forwarder");
        }
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

void test_forwarder_chain_and_loop() {
    {
        const char* what = "a two-step forwarder chain resolves to its final target";
        try {
            write_image_as(make_exports_image(0x190000000, 1, {{"fwd", 0, 0, "KERNEL32.GetCurrentProcessId"}}),
                           "chain_b.dll");
            write_image_as(make_exports_image(0x180000000, 1, {{"fwd", 0, 0, "chain_b.fwd"}}), "chain_a.dll");
            const auto importer =
                write_image_as(make_import_image(0x140000000, "chain_a.dll", {{"fwd"}}), "chain_import.exe");
            rt::Loader loader(g_dir);
            rt::Module& m = loader.load(importer);
            void* expected = host_symbol("kernel32.dll", "GetCurrentProcessId");
            if (!expected) return failed(what, "GetCurrentProcessId is not loaded in this process");
            if (m.imports.size() != 1 || m.imports[0].binding != rt::Binding::Host)
                return failed(what, "the chained forwarder did not bind to the host");
            if (iat_slot(m) != reinterpret_cast<uint64_t>(expected))
                return failed(what, "the IAT slot does not hold the final target");
            pass(what);
        } catch (const std::exception& e) {
            failed(what, e.what());
        }
    }
    {
        const char* what = "forwarders that point at each other stop as unimplemented instead of recursing";
        try {
            write_image_as(make_exports_image(0x180000000, 1, {{"fwd", 0, 0, "loop_b.fwd"}}), "loop_a.dll");
            write_image_as(make_exports_image(0x190000000, 1, {{"fwd", 0, 0, "loop_a.fwd"}}), "loop_b.dll");
            const auto importer =
                write_image_as(make_import_image(0x140000000, "loop_a.dll", {{"fwd"}}), "loop_import.exe");
            rt::Loader loader(g_dir);
            rt::Module& m = loader.load(importer);
            if (m.imports.size() != 1 || m.imports[0].binding != rt::Binding::Unimplemented)
                return failed(what, "the loop did not end as unimplemented");
            if (!iat_slot(m)) return failed(what, "the IAT slot was left null instead of a failure thunk");
            pass(what);
        } catch (const std::exception& e) {
            failed(what, e.what());
        }
    }
}

void test_iat_protection_window() {
    const char* what = "the IAT is writable while bound, then protected read-only, and the window is closed";
    try {
        const auto importer =
            write_image_as(make_import_image(0x140000000, "kernel32.dll", {{"GetCurrentProcessId"}}), "iat_prot.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        void* pid = host_symbol("kernel32.dll", "GetCurrentProcessId");
        if (!pid) return failed(what, "GetCurrentProcessId is not loaded in this process");
        if (iat_slot(m) != reinterpret_cast<uint64_t>(pid))
            return failed(what, "the address table write did not happen (the protection window never opened)");
        MEMORY_BASIC_INFORMATION headers{};
        MEMORY_BASIC_INFORMATION text{};
        if (!VirtualQuery(m.base, &headers, sizeof headers) || !VirtualQuery(m.base + kIatRva, &text, sizeof text))
            return failed(what, "VirtualQuery failed");
        if ((headers.Protect & 0xFF) != PAGE_READONLY)
            return failed(what, "the headers are not PAGE_READONLY after load");
        if ((text.Protect & 0xFF) != PAGE_EXECUTE_READ)
            return failed(what, "the section holding the IAT is not PAGE_EXECUTE_READ after load");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

void test_iat_complete_before_dllmain() {
    const char* what = "the whole IAT is bound before any image code runs (DllMain sees the resolved slot)";
    try {
        ImportImageOptions opts;
        opts.dll = true;
        opts.dllmain = reinterpret_cast<void*>(&observe_dllmain);
        const auto module = write_image_as(
            make_import_image(0x140000000, "kernel32.dll", {{"GetCurrentProcessId"}}, opts), "iat_dllmain.dll");
        void* pid = host_symbol("kernel32.dll", "GetCurrentProcessId");
        if (!pid) return failed(what, "GetCurrentProcessId is not loaded in this process");
        g_observer_base = nullptr;
        g_observed_iat = 0;
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(module);
        g_observer_base = m.base;
        loader.initialise();
        if (g_observed_iat != reinterpret_cast<uint64_t>(pid))
            return failed(what, "DllMain read an unbound IAT slot");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

void test_missing_symbol() {
    const char* what = "a missing symbol binds a failure thunk and is recorded as unimplemented";
    try {
        const auto importer = write_image_as(
            make_import_image(0x140000000, "completely_absent_module_x.dll", {{"NoSuchExport"}}), "missing_sym.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        if (m.imports.size() != 1 || m.imports[0].binding != rt::Binding::Unimplemented)
            return failed(what, "the missing import did not bind as unimplemented");
        if (!iat_slot(m)) return failed(what, "the IAT slot was left null instead of a failure thunk");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

void test_runtime_load_paths() {
    {
        const char* what = "a run-time load maps a bundled module and its exports resolve by name and ordinal";
        try {
            write_image_as(make_exports_image(0x180000000, 1,
                                              {{nullptr, 0, kCodeRva}, {"ping", 0, kCodeRva}, {nullptr, 6, kCodeRva}}),
                           "rtdep.dll");
            rt::Loader loader(g_dir);
            bool handled = false;
            void* h = loader.load_runtime_module("rtdep.dll", 0, handled);
            if (!handled || !h) return failed(what, "a bundled module was not handled");
            rt::Module* dep = nullptr;
            for (auto* mod : loader.modules())
                if (mod->base == h) dep = mod;
            if (!dep) return failed(what, "the loaded module is not in the module list");
            if (loader.find_loaded_module("rtdep.dll") != h) return failed(what, "the module is not findable by name");
            if (!loader.owns_module_handle(h)) return failed(what, "the loader does not own its own handle");
            const uint64_t expected = reinterpret_cast<uint64_t>(dep->base + kCodeRva);
            for (const char* key : {"ping", "#1", "#7"}) {
                bool owns = false;
                if (reinterpret_cast<uint64_t>(loader.resolve_module_export(h, key, owns)) != expected || !owns)
                    return failed(what, std::string("GetProcAddress(") + key + ") did not resolve the export");
            }
            bool owns = false;
            if (loader.resolve_module_export(h, "absent", owns) != nullptr || !owns)
                return failed(what, "an absent export did not answer NULL with a loader-owned handle");
            pass(what);
        } catch (const std::exception& e) {
            failed(what, e.what());
        }
    }
    {
        const char* what = "a run-time load with DONT_RESOLVE_DLL_REFERENCES is refused by name";
        try {
            write_image_as(make_exports_image(0x180000000, 1, {{"ping", 0, kCodeRva}}), "rtdep2.dll");
            rt::Loader loader(g_dir);
            bool handled = false;
            SetLastError(0);
            void* h = loader.load_runtime_module("rtdep2.dll", 0x1 /* DONT_RESOLVE_DLL_REFERENCES */, handled);
            if (!handled || h != nullptr) return failed(what, "the flags were not refused");
            if (GetLastError() != ERROR_NOT_SUPPORTED) return failed(what, "LastError was not ERROR_NOT_SUPPORTED");
            pass(what);
        } catch (const std::exception& e) {
            failed(what, e.what());
        }
    }
    {
        const char* what = "a run-time load of a host-CRT name is left to the host (not handled)";
        try {
            rt::Loader loader(g_dir);
            bool handled = false;
            if (loader.load_runtime_module("vcruntime140.dll", 0, handled) != nullptr || handled)
                failed(what, "the host-CRT name was handled by the loader instead of the host");
            else
                pass(what);
        } catch (const std::exception& e) {
            failed(what, e.what());
        }
    }
}

void test_import_cycle() {
    const char* what = "two bundled modules that import each other's export both load and bind";
    try {
        write_image_as(make_cycle_image(0x180000000, "a", "cyc_b.dll", "b"), "cyc_a.dll");
        write_image_as(make_cycle_image(0x190000000, "b", "cyc_a.dll", "a"), "cyc_b.dll");
        const auto importer =
            write_image_as(make_import_image(0x140000000, "cyc_a.dll", {{"a"}}), "cyc_import.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        if (m.imports.size() != 1 || m.imports[0].binding != rt::Binding::Module)
            return failed(what, "the importer's import did not bind to the module");
        rt::Module* a = nullptr;
        rt::Module* b = nullptr;
        for (auto* mod : loader.modules()) {
            if (mod->name == "cyc_a.dll") a = mod;
            if (mod->name == "cyc_b.dll") b = mod;
        }
        if (!a || !b) return failed(what, "one of the two cycle modules was not loaded");
        if (iat_slot(m) != reinterpret_cast<uint64_t>(a->base + kCodeRva))
            return failed(what, "the importer's IAT slot does not hold a.dll's export");
        if (b->imports.size() != 1 || b->imports[0].binding != rt::Binding::Module)
            return failed(what, "the second module's cycle import did not bind");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

void test_prebound_rebind() {
    const char* what = "a pre-bound IAT is rebuilt from the name table (the bound timestamp is not trusted)";
    try {
        ImportImageOptions opts;
        opts.prebound = true;
        const auto importer = write_image_as(
            make_import_image(0x140000000, "kernel32.dll", {{"GetCurrentProcessId"}}, opts), "prebound.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        void* pid = host_symbol("kernel32.dll", "GetCurrentProcessId");
        if (!pid) return failed(what, "GetCurrentProcessId is not loaded in this process");
        if (iat_slot(m) != reinterpret_cast<uint64_t>(pid))
            return failed(what, "the pre-bound IAT value was not replaced by the re-resolved export");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

// A forwarder may name an API set instead of a real DLL file; the host loader resolves the set.
void test_api_set_forwarder() {
    const char* what = "a forwarder to a host API set resolves through the host loader";
    try {
        write_image_as(make_exports_image(0x180000000, 1,
                                          {{"fwd", 0, 0, "api-ms-win-core-processthreads-l1-1-0.GetCurrentProcessId"}}),
                       "apisetprov.dll");
        const auto importer =
            write_image_as(make_import_image(0x140000000, "apisetprov.dll", {{"fwd"}}), "apiset_import.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        void* pid = host_symbol("kernel32.dll", "GetCurrentProcessId");
        if (!pid) return failed(what, "GetCurrentProcessId is not loaded in this process");
        if (m.imports.size() != 1 || m.imports[0].binding != rt::Binding::Host)
            return failed(what, "the API-set forwarder did not bind to the host");
        if (iat_slot(m) != reinterpret_cast<uint64_t>(pid))
            return failed(what, "the IAT slot does not hold GetCurrentProcessId");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

// The export ordinal is Base + the name-ordinal index, not the index itself.
void test_ordinal_base_offset() {
    const char* what = "an export's ordinal name is its Base plus the address-table index";
    try {
        write_image_as(make_exports_image(0x180000000, 100, {{nullptr, 0, kCodeRva}}), "ordbase.dll");
        const auto importer =
            write_image_as(make_import_image(0x140000000, "ordbase.dll", {{nullptr, 100, 0}}), "ordbase_import.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        rt::Module* dep = nullptr;
        for (auto* mod : loader.modules())
            if (mod->name == "ordbase.dll") dep = mod;
        if (!dep) return failed(what, "the provider was not loaded");
        if (!dep->exports.count("#100")) return failed(what, "the export is not named \"#100\"");
        if (m.imports.size() != 1 || m.imports[0].binding != rt::Binding::Module)
            return failed(what, "the ordinal-100 import did not bind to the module");
        if (iat_slot(m) != reinterpret_cast<uint64_t>(dep->base + kCodeRva))
            return failed(what, "the IAT slot does not hold the export");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

// GetProcAddress matches export names exactly; the loader's export map must not fold case.
void test_export_name_case() {
    const char* what = "export names match case-sensitively, as GetProcAddress does";
    try {
        write_image_as(make_exports_image(0x180000000, 1, {{"Ping", 0, kCodeRva}}), "caseprov.dll");
        const auto importer =
            write_image_as(make_import_image(0x140000000, "caseprov.dll", {{"ping"}}), "case_import.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        if (m.imports.size() != 1 || m.imports[0].binding != rt::Binding::Unimplemented)
            return failed(what, "a differently-cased import was bound");
        pass(what);
    } catch (const std::exception& e) {
        failed(what, e.what());
    }
}

// =================================================================================================================
// The audit's fault cases: the expected contract, then what the current loader does instead.
// =================================================================================================================

void test_first_thunk_missing() {
    const char* what = "an import descriptor with no address table (FirstThunk == 0) is refused by name";
    try {
        ImportImageOptions opts;
        opts.first_thunk = 0;  // OriginalFirstThunk is present and valid; only the IAT pointer is missing
        const auto importer = write_image_as(
            make_import_image(0x140000000, "kernel32.dll", {{"GetCurrentProcessId"}}, opts), "no_iat.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        // The loader accepted it and started the IAT walk at RVA 0.
        const uint64_t rva0 = *reinterpret_cast<const uint64_t*>(m.base);
        void* pid = host_symbol("kernel32.dll", "GetCurrentProcessId");
        std::string observed = "loaded anyway";
        if (rva0 == reinterpret_cast<uint64_t>(pid))
            observed = "loaded anyway and wrote the resolved pointer over image RVA 0 (the DOS header)";
        else if (std::memcmp(m.base, "MZ", 2) != 0)
            observed = "loaded anyway and the mapped DOS header no longer starts with MZ";
        fault(what, observed);
    } catch (const std::exception& e) {
        if (std::string(e.what()).find("address table") != std::string::npos) patched(what);
        else fault(what, std::string("refused, but the message does not name the missing address table: ") + e.what());
    }
}

void test_bundled_symbol_falls_back_to_host_namesake() {
    const char* what = "a bundled module that exists but does not export the symbol is not replaced by a host namesake";
    try {
        write_image_as(make_base(0x180000000), "version.dll");  // a real, mapped package module with no exports
        const auto importer =
            write_image_as(make_import_image(0x140000000, "version.dll", {{"GetFileVersionInfoSizeW"}}),
                           "namesake_import.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        if (m.imports.size() != 1) return failed(what, "expected one import record");
        if (m.imports[0].binding != rt::Binding::Host) return patched(what);  // the intended behaviour
        void* host = host_symbol("version.dll", "GetFileVersionInfoSizeW");
        std::string observed = std::string("the import bound as ") + binding_name(m.imports[0].binding);
        if (host && iat_slot(m) == reinterpret_cast<uint64_t>(host))
            observed += " and the IAT slot holds the host System32 version.dll export, not a failure thunk";
        fault(what, observed);
    } catch (const std::exception& e) {
        fault(what, std::string("the load failed instead of binding the import: ") + e.what());
    }
}

void test_ordinal_thunk_bits() {
    const char* what = "an ordinal thunk with bits above the 16-bit ordinal field is refused, not truncated";
    try {
        write_image_as(make_exports_image(0x180000000, 1, {{nullptr, 4, kCodeRva}}), "ordbit.dll");
        // bit 63 marks an ordinal; the low 16 bits are 5; bit 16 is a stray bit the PE ordinal field must not carry.
        ImportThunk thunk;
        thunk.raw = 0x8000000000010005ull;
        const auto importer =
            write_image_as(make_import_image(0x140000000, "ordbit.dll", {thunk}), "ordbit_import.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        rt::Module* dep = nullptr;
        for (auto* mod : loader.modules())
            if (mod->name == "ordbit.dll") dep = mod;
        std::string observed = "loaded anyway";
        if (dep && m.imports.size() == 1) {
            observed = "loaded anyway and bound " + m.imports[0].name + " (";
            observed += binding_name(m.imports[0].binding);
            observed += ")";
            if (iat_slot(m) == reinterpret_cast<uint64_t>(dep->base + kCodeRva))
                observed += ", truncating the stray bits to the provider's ordinal 5 export";
        }
        fault(what, observed);
    } catch (const std::exception& e) {
        const std::string message = e.what();
        if (message.find("ordinal") != std::string::npos) patched(what);
        else fault(what, "refused, but the message does not name the ordinal: " + message);
    }
}

void test_forwarder_target_not_a_module() {
    const char* what = "a forwarder target that is a file but not a module leaves the import unimplemented";
    try {
        write_data_file("not_a_module", "this file is not a PE image\n");
        write_image_as(make_exports_image(0x180000000, 1, {{"fwd", 0, 0, "not_a_module.f"}}), "provnope.dll");
        const auto importer =
            write_image_as(make_import_image(0x140000000, "provnope.dll", {{"fwd"}}), "provnope_import.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        if (m.imports.size() == 1 && m.imports[0].binding == rt::Binding::Unimplemented) patched(what);
        else fault(what, "the import bound but was expected unimplemented");
    } catch (const std::exception& e) {
        fault(what, std::string("the importer's load was aborted instead of leaving the forwarder unresolved: ") +
                        e.what());
    }
}

void test_forwarder_second_candidate() {
    const char* what = "forwarder resolution tries the target's \".dll\" spelling when an extensionless file exists";
    try {
        write_image_as(make_base(0x190000000), "spelltgt");  // a valid PE in the package without that export
        write_image_as(make_exports_image(0x1A0000000, 1, {{"fwd", 0, 0, "KERNEL32.GetCurrentProcessId"}}),
                       "spelltgt.dll");
        write_image_as(make_exports_image(0x180000000, 1, {{"fwd", 0, 0, "spelltgt.fwd"}}), "provspell.dll");
        const auto importer =
            write_image_as(make_import_image(0x140000000, "provspell.dll", {{"fwd"}}), "provspell_import.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        void* expected = host_symbol("kernel32.dll", "GetCurrentProcessId");
        if (!expected) return failed(what, "GetCurrentProcessId is not loaded in this process");
        if (m.imports.size() == 1 && m.imports[0].binding == rt::Binding::Host &&
            iat_slot(m) == reinterpret_cast<uint64_t>(expected))
            patched(what);
        else if (m.imports.size() == 1)
            fault(what, std::string("the forwarder bound as ") + binding_name(m.imports[0].binding) +
                            " because the extensionless package file was taken as the only candidate");
        else
            fault(what, "unexpected import record count");
    } catch (const std::exception& e) {
        fault(what, std::string("the load failed: ") + e.what());
    }
}

void test_export_into_headers() {
    const char* what = "an export whose RVA points into the image headers is refused by name";
    try {
        write_image_as(make_exports_image(0x180000000, 1, {{"hdr", 0, 0x10}}), "hdrprov.dll");
        const auto importer = write_image_as(make_import_image(0x140000000, "hdrprov.dll", {{"hdr"}}), "hdr_import.exe");
        rt::Loader loader(g_dir);
        rt::Module& m = loader.load(importer);
        rt::Module* dep = nullptr;
        for (auto* mod : loader.modules())
            if (mod->name == "hdrprov.dll") dep = mod;
        std::string observed = "loaded anyway";
        if (dep && m.imports.size() == 1 && iat_slot(m) == reinterpret_cast<uint64_t>(dep->base + 0x10))
            observed = "loaded anyway and the IAT slot points at image+0x10, inside the provider's DOS header";
        fault(what, observed);
    } catch (const std::exception& e) {
        if (std::string(e.what()).find("headers") != std::string::npos) patched(what);
        else fault(what, std::string("refused, but the message does not name the headers: ") + e.what());
    }
}

void test_bound_image_without_name_table() {
    const char* what = "a bound image with no name table is refused with a message that names the bound state";
    try {
        ImportImageOptions opts;
        opts.original_first_thunk = false;  // no name table to rebind from
        opts.prebound = true;               // the IAT holds pre-bound virtual addresses
        const auto importer = write_image_as(
            make_import_image(0x140000000, "kernel32.dll", {{"GetCurrentProcessId"}}, opts), "no_name_tbl_img.exe");
        rt::Loader loader(g_dir);
        loader.load(importer);
        fault(what, "the image loaded, reading pre-bound addresses as an import name table");
    } catch (const std::exception& e) {
        const std::string message = e.what();
        // The expected diagnostic names the missing name table, so it says why the pre-bound IAT cannot be read.
        if (message.find("OriginalFirstThunk") != std::string::npos) patched(what);
        else fault(what, "refused, but the generic message does not name the bound image or its missing name table: " +
                             message);
    }
}

}  // namespace

int main() {
    char temp[MAX_PATH] = {};
    DWORD n = GetTempPathA(MAX_PATH, temp);
    if (n == 0) {
        std::printf("FAIL    cannot read the temporary directory\n");
        return 1;
    }
    g_dir = std::filesystem::path(temp) / "palmetto_loader_bind_audit";
    std::error_code ec;
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);
    if (ec) {
        std::printf("FAIL    cannot create the fixture directory\n");
        return 1;
    }

    test_iat_slot_write();
    test_ordinal_import();
    test_original_first_thunk_fallback();
    test_delay_imports();
    test_export_aliases();
    test_forwarder_chain_and_loop();
    test_iat_protection_window();
    test_iat_complete_before_dllmain();
    test_missing_symbol();
    test_runtime_load_paths();
    test_import_cycle();
    test_prebound_rebind();
    test_api_set_forwarder();
    test_ordinal_base_offset();
    test_export_name_case();

    test_first_thunk_missing();
    test_bundled_symbol_falls_back_to_host_namesake();
    test_ordinal_thunk_bits();
    test_forwarder_target_not_a_module();
    test_forwarder_second_candidate();
    test_export_into_headers();
    test_bound_image_without_name_table();

    std::filesystem::remove_all(g_dir, ec);

    std::printf("\n%d checks: %d contract(s) held, %d fault(s) still present, %d fault(s) already fixed",
                g_checks, g_held, g_faults, g_patched);
    if (g_failed) std::printf(", %d unexpected failure(s)", g_failed);
    std::printf("%s\n", strict_audit() ? " (strict: faults count as failures)" : "");
    return (g_failed + (strict_audit() ? g_faults : 0)) ? 1 : 0;
}
