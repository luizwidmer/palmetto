// Independent mapped-image self-check for the old rt::Loader (docs/pe_loader_rewrite.md, phase-2 falsifier).
//
// The harness maps each synthetic PE in loader_selfcheck_fixture.h through the OLD rt::Loader, then predicts every byte
// of the mapping from rt::pe::Image alone - the bounds-safe decoder that does not map or run anything:
//   * the SizeOfHeaders bytes of the file at file offset 0,
//   * each section's file-backed bytes at its virtual address, by the mapper's page-tail copy rule (the raw data of
//     every 4 KiB page the virtual extent touches, capped by SizeOfRawData; see predicted_section_copies()),
//   * zero fill everywhere else (VirtualAlloc commits zeroed pages),
//   * DIR64 and HIGHLOW base relocations applied by the load delta (base - preferred base) when the image moved,
//   * each import address slot replaced by the pointer an independent resolver predicts (the test shim table, a
//     bundled synthetic module, the host GetProcAddress, or the unimplemented-import thunk),
//   * the module TLS index the loader writes into the image's _tls_index slot,
//   * page protections for the headers and every section footprint, simulated in the loader's own VirtualProtect order.
// It also cross-checks the Module description (base, size, entry, exports, forwarders, import records, exception count,
// TLS fields, the TLS template copy in the thread's TEB slot) and loader.find_address(). A negative control injects five
// known differences (headers, section data, an IAT slot, the TLS index and a DIR64 relocation) and requires each to be
// detected with its category and severity, so a "0 faults" result cannot come from a detector that never fires.
//
// Every mismatch is reported as a loader fault with a severity and bytes as evidence:
//   critical - an execution-affecting difference: an IAT pointer, a base relocation, the TLS index/template, a valid
//              image the loader refused, a relocation-less image mapped off its base, or an overflowed HIGHLOW slot;
//   major    - mapped bytes that differ (headers, section data, zero fill), a Module description difference, or a
//              protection difference on an executable/writable range;
//   minor    - a protection difference elsewhere (read-only/zero pages);
//   info     - a documented refusal that was verified, or an environment limit.
// The exit code is the number of faults. The full report (every fault, not just the console first 200 lines) is
// written next to the synthetic fixtures; its path is printed at the end.
//
// No game data: every image is built by loader_selfcheck_fixture.h. The target needs palmetto_tls_host.dll next to the
// executable for the TLS fixtures, like loader_tls_test; the CMake dependency and this run's layout provide that.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../shims/shims.h"
#include "../src/pe_image.h"
#include "../src/pe_loader.h"
#include "loader_selfcheck_fixture.h"

namespace {

namespace fs = std::filesystem;

// ---- Reporting ------------------------------------------------------------------------------------------------------

enum class Severity { Critical, Major, Minor, Info };

const char* severity_name(Severity severity) {
    switch (severity) {
        case Severity::Critical: return "critical";
        case Severity::Major: return "major";
        case Severity::Minor: return "minor";
        case Severity::Info: return "info";
    }
    return "unknown";
}

int g_checks = 0;
int g_faults = 0;
std::map<std::string, int> g_faults_by_severity;
// Coverage counters for the summary: how much of a mapping the run actually compared.
std::uint64_t g_modules_verified = 0;
std::uint64_t g_bytes_verified = 0;
std::uint64_t g_pages_verified = 0;
std::uint64_t g_iat_verified = 0;
std::uint64_t g_relocations_verified = 0;
std::string g_report;
std::vector<std::string> g_pending;  // console lines; capped, the report file always gets everything
constexpr std::size_t kConsoleLines = 400;

void emit(const std::string& line) {
    g_report += line;
    g_report += '\n';
    g_pending.push_back(line);
}

void flush_console() {
    std::size_t shown = 0;
    for (const auto& line : g_pending) {
        if (shown++ >= kConsoleLines) {
            std::printf("... (further lines are only in the report file)\n");
            break;
        }
        std::printf("%s\n", line.c_str());
    }
    g_pending.clear();
    std::fflush(stdout);
}

void note(const std::string& line) { emit("info  " + line); }

void fault(Severity severity, const std::string& what, const std::string& evidence) {
    ++g_faults;
    ++g_faults_by_severity[severity_name(severity)];
    emit(std::string("FAULT [") + severity_name(severity) + "] " + what + " | " + evidence);
}

void expect(bool condition, Severity severity, const std::string& what, const std::string& evidence) {
    ++g_checks;
    if (!condition) fault(severity, what, evidence);
}

std::string hex(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string decimal(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%llu", static_cast<unsigned long long>(value));
    return buffer;
}

std::string signed_decimal(std::int64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(value));
    return buffer;
}

std::string bytes_hex(const std::uint8_t* bytes, std::size_t size, std::size_t limit = 24) {
    std::string text;
    char byte[4];
    for (std::size_t i = 0; i < size && i < limit; ++i) {
        std::snprintf(byte, sizeof byte, "%02x", bytes[i]);
        text += byte;
    }
    if (size > limit) text += "..";
    return text;
}

std::string prot_name(DWORD prot) {
    switch (prot & 0xFFu) {
        case PAGE_NOACCESS: return "PAGE_NOACCESS";
        case PAGE_READONLY: return "PAGE_READONLY";
        case PAGE_READWRITE: return "PAGE_READWRITE";
        case PAGE_WRITECOPY: return "PAGE_WRITECOPY";
        case PAGE_EXECUTE: return "PAGE_EXECUTE";
        case PAGE_EXECUTE_READ: return "PAGE_EXECUTE_READ";
        case PAGE_EXECUTE_READWRITE: return "PAGE_EXECUTE_READWRITE";
        case PAGE_EXECUTE_WRITECOPY: return "PAGE_EXECUTE_WRITECOPY";
        default: return "protection " + hex(prot);
    }
}

// ---- Work directory and fixture files -------------------------------------------------------------------------------

fs::path g_dir;

fs::path work_dir() {
    wchar_t* env = nullptr;
    std::size_t size = 0;
    fs::path base;
    if (_wdupenv_s(&env, &size, L"PALMETTO_TEST_DIR") == 0 && env && *env) base = env;
    std::free(env);
    if (base.empty()) base = fs::temp_directory_path();
    return base / "palmetto_loader_selfcheck" / std::to_string(GetCurrentProcessId());
}

// The directory the running executable is in: Loader::initialize_tls looks for palmetto_tls_host.dll there.
fs::path executable_dir() {
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof path / sizeof path[0]));
    if (!length || length >= sizeof path / sizeof path[0]) return {};
    return fs::path(path).parent_path();
}

std::string lower(std::string text) { return rt::lower(std::move(text)); }

fs::path write_fixture(const loader_selfcheck::Fixture& fixture) {
    const fs::path path = g_dir / fixture.filename;
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write(reinterpret_cast<const char*>(fixture.bytes.data()), static_cast<std::streamsize>(fixture.bytes.size()));
    if (!out) throw std::runtime_error("cannot finish writing " + path.string());
    return path;
}

// ---- Preferred bases -------------------------------------------------------------------------------------------------

// A free image base in the range Windows keeps free for synthetic x64 images (the same range loader_test and
// free_image_base.h use). The candidate is checked free and then re-checked to be free at the exact base, so a taken
// candidate falls through to the next one instead of turning into a spurious loader fault.
bool range_free(std::uint64_t base, std::uint64_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(base), &info, sizeof info)) return false;
    if (info.State != MEM_FREE) return false;
    const std::uint64_t end = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    return base + size <= end;
}

std::uint64_t free_base(std::uint32_t index) {
    // The usual Windows image-base range. The test executable is linked at 0x10000000000 (/BASE), like the loader
    // tests, so this whole span is free and does not depend on the OS handing out a low first-fit address.
    const std::uint64_t candidates[] = {0x140000000ull, 0x150000000ull, 0x160000000ull, 0x170000000ull,
                                       0x180000000ull, 0x190000000ull, 0x1A0000000ull, 0x1B0000000ull,
                                       0x1C0000000ull, 0x1D0000000ull};
    for (std::uint64_t candidate : candidates) {
        if (!range_free(candidate + static_cast<std::uint64_t>(index) * 0x1000000ull, loader_selfcheck::kSizeOfImage))
            continue;
        const std::uint64_t base = candidate + static_cast<std::uint64_t>(index) * 0x1000000ull;
        // Reserve and release it: this proves the range really is allocatable at exactly that address.
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(base), loader_selfcheck::kSizeOfImage,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
            VirtualFree(p, 0, MEM_RELEASE);
            return base;
        }
    }
    // Last resort for hosts that keep allocations inside the usual range (arm64 Wine): one fresh address from the same
    // first-fit search the loader uses. Distinct calls can then collide, which the range-free checks before each hold
    // catch and the run reports instead of silently mapping two fixtures at one address.
    if (void* p = VirtualAlloc(nullptr, loader_selfcheck::kSizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
        const std::uint64_t base = reinterpret_cast<std::uint64_t>(p);
        VirtualFree(p, 0, MEM_RELEASE);
        return base;
    }
    throw std::runtime_error("no free preferred image base for the synthetic fixtures");
}

// Holds a preferred base page so the loader has to relocate the image. The loader also uses the base as its preferred
// address; a reservation there makes that VirtualAlloc fail exactly as a taken address does for a real image.
struct HeldBase {
    explicit HeldBase(std::uint64_t base, std::uint32_t size)
        : address(VirtualAlloc(reinterpret_cast<void*>(base), size, MEM_RESERVE, PAGE_NOACCESS)) {
        if (!address) throw std::runtime_error("cannot reserve the preferred base " + hex(base));
    }
    ~HeldBase() {
        if (address) VirtualFree(address, 0, MEM_RELEASE);
    }
    HeldBase(const HeldBase&) = delete;
    HeldBase& operator=(const HeldBase&) = delete;
    void* address;
};

// ---- Expected bindings ----------------------------------------------------------------------------------------------

struct BindingPrediction {
    void* value = nullptr;
    std::string binding;  // Shim / Host / Module / Unimplemented, as rt::Binding spells it
    std::string note;
};

// What an import must resolve to, worked out from the fixture images and the test's shim table - not from the loader.
struct SymbolTable {
    std::map<std::string, void*> shim;  // "<lower dll>!<name or #ordinal>"
    struct Bundled {
        const rt::pe::Image* image;
        std::uint64_t base;  // the image's own ImageBase: these fixtures have no relocations, so that is where it maps
    };
    std::map<std::string, Bundled> bundled;  // "<lower dll file name>"

    BindingPrediction import(const std::string& dll, const std::string& symbol) const {
        const std::string key = lower(dll);
        if (auto it = shim.find(key + "!" + symbol); it != shim.end())
            return {it->second, "Shim", "test shim table"};
        if (auto it = bundled.find(key); it != bundled.end()) {
            BindingPrediction deeper;
            if (export_pointer(*it->second.image, it->second.base, symbol, deeper, 0)) return deeper;
        }
        // Everything else the old loader tries next: a system DLL through LoadLibraryExA + GetProcAddress.
        if (HMODULE module = LoadLibraryExA(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            const bool by_ordinal = symbol.size() > 1 && symbol[0] == '#';
            FARPROC proc = nullptr;
            if (by_ordinal) {
                std::uint32_t ordinal = 0;
                for (std::size_t i = 1; i < symbol.size(); ++i) ordinal = ordinal * 10 + (symbol[i] - '0');
                proc = GetProcAddress(module, MAKEINTRESOURCEA(static_cast<WORD>(ordinal)));
            } else {
                proc = GetProcAddress(module, symbol.c_str());
            }
            if (proc) return {reinterpret_cast<void*>(proc), "Host", "host module " + dll};
        }
        // resolve() falls back to the unimplemented thunk, whose pointer is stable per (dll, symbol).
        return {rt::shims::make_unimplemented_thunk(dll, symbol), "Unimplemented", "unimplemented thunk"};
    }

private:
    bool export_pointer(const rt::pe::Image& image, std::uint64_t base, const std::string& symbol,
                        BindingPrediction& out, int depth) const {
        for (const auto& symbol_entry : image.exports()) {
            const bool by_ordinal = symbol.size() > 1 && symbol[0] == '#';
            const std::string ordinal_key = "#" + std::to_string(symbol_entry.ordinal);
            const bool matches = by_ordinal ? (ordinal_key == symbol) : (symbol_entry.name == symbol);
            if (!matches) continue;
            if (!symbol_entry.is_forwarder()) {
                out = {reinterpret_cast<void*>(base + symbol_entry.rva), "Module", "bundled export " + symbol};
                return true;
            }
            return forwarder(symbol_entry.forwarder, out, depth + 1);
        }
        return false;
    }

    bool forwarder(const std::string& spec, BindingPrediction& out, int depth) const {
        if (depth >= 8) return false;
        const std::size_t dot = spec.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= spec.size()) return false;
        std::string module = lower(spec.substr(0, dot));
        const std::string symbol = spec.substr(dot + 1);
        std::string shim_key = module;
        if (shim_key.size() < 4 || shim_key.compare(shim_key.size() - 4, 4, ".dll") != 0) shim_key += ".dll";
        if (auto it = shim.find(shim_key + "!" + symbol); it != shim.end()) {
            out = {it->second, "Shim", "forwarded to the test shim table"};
            return true;
        }
        for (const auto& candidate : {module, module + ".dll"}) {
            auto it = bundled.find(candidate);
            if (it == bundled.end()) continue;
            return export_pointer(*it->second.image, it->second.base, symbol, out, depth + 1);
        }
        if (HMODULE host = LoadLibraryExA(module.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            if (FARPROC proc = GetProcAddress(host, symbol.c_str())) {
                out = {reinterpret_cast<void*>(proc), "Host", "forwarded to a host module"};
                return true;
            }
        }
        out = {rt::shims::make_unimplemented_thunk(module, symbol), "Unimplemented", "forwarder not resolvable"};
        return true;
    }
};

const char* binding_name(rt::Binding binding) {
    switch (binding) {
        case rt::Binding::Shim: return "Shim";
        case rt::Binding::Host: return "Host";
        case rt::Binding::Module: return "Module";
        case rt::Binding::Unimplemented: return "Unimplemented";
    }
    return "unknown";
}

// The test's own resolver: two exports of a synthetic "selfcheck.dll". The loader's shim registry returns these exact
// function addresses, so the harness knows every IAT pointer the binding must write.
int g_shim_target = 0;
void* selfcheck_shim_a() { return &g_shim_target; }
SHIM_TABLE(loader_selfcheck_shims, {"selfcheck.dll", "A", reinterpret_cast<void*>(&selfcheck_shim_a)},
           {"selfcheck.dll", "#7", reinterpret_cast<void*>(&selfcheck_shim_a)});

SymbolTable shim_table() {
    SymbolTable table;
    table.shim["selfcheck.dll!A"] = reinterpret_cast<void*>(&selfcheck_shim_a);
    table.shim["selfcheck.dll!#7"] = reinterpret_cast<void*>(&selfcheck_shim_a);
    return table;
}

// ---- Mapped-image prediction ----------------------------------------------------------------------------------------

struct Region {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::string kind;
    Severity severity = Severity::Major;
};

std::uint64_t align_up64(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// The bytes the current mapper copies for a section (pe_loader.cpp, map_image()): the raw data of every 4 KiB page the
// section's virtual extent touches, capped by SizeOfRawData and by the next section's first byte. This is the rule the
// loader documents as measured against Windows; rt::pe::Image::section_data() exposes the narrower PE/COFF rule
// min(SizeOfRawData, VirtualSize), so the page tail is reported as a decoder/mapper contract gap in a note, not as a
// loader fault. The prediction below still comes only from the decoder's header fields and the raw file bytes.
struct SectionCopy {
    const rt::pe::Section* section = nullptr;
    std::uint32_t begin = 0;
    std::uint32_t length = 0;
    std::uint32_t raw_pointer = 0;
};

std::vector<SectionCopy> predicted_section_copies(const rt::pe::Image& image) {
    std::vector<SectionCopy> copies;
    for (const auto& section : image.sections()) {
        SectionCopy copy;
        copy.section = &section;
        copy.begin = section.virtual_address;
        copy.raw_pointer = section.raw_pointer;
        if (section.raw_size && section.raw_pointer) {
            const std::uint64_t virtual_size = section.virtual_size ? section.virtual_size : section.raw_size;
            std::uint64_t length = std::min<std::uint64_t>(
                section.raw_size, align_up64(static_cast<std::uint64_t>(section.virtual_address) + virtual_size, 0x1000) -
                                      section.virtual_address);
            for (const auto& other : image.sections())
                if (other.virtual_address > section.virtual_address)
                    length = std::min<std::uint64_t>(length, other.virtual_address - section.virtual_address);
            length = std::min<std::uint64_t>(length, image.size_of_image() - section.virtual_address);
            copy.length = static_cast<std::uint32_t>(length);
        }
        copies.push_back(copy);
    }
    return copies;
}

// The ordered byte categories a mismatch is attributed to. The loader-written slots come first because they sit inside
// section data.
std::vector<Region> byte_regions(const rt::pe::Image& image, std::int64_t delta) {
    std::vector<Region> special;
    for (const auto& library : image.imports()) {
        for (const auto& symbol : library.symbols) {
            special.push_back({symbol.iat_rva, symbol.iat_rva + 8, "IAT binding slot", Severity::Critical});
        }
    }
    if (image.tls().present && image.tls().index_rva)
        special.push_back({image.tls().index_rva, image.tls().index_rva + 4, "TLS module index slot", Severity::Critical});
    if (delta != 0) {
        for (const auto& relocation : image.relocations()) {
            if (relocation.type == rt::pe::Relocation::kDir64)
                special.push_back({relocation.rva, relocation.rva + 8, "base relocation qword", Severity::Critical});
            else if (relocation.type == rt::pe::Relocation::kHighLow)
                special.push_back({relocation.rva, relocation.rva + 4, "base relocation dword", Severity::Critical});
        }
    }
    return special;
}

std::string classify(const std::vector<Region>& special, const rt::pe::Image& image,
                     const std::vector<SectionCopy>& copies, std::uint32_t rva, Severity& severity) {
    for (const auto& region : special) {
        if (rva >= region.begin && rva < region.end) {
            severity = region.severity;
            return region.kind;
        }
    }
    if (rva < image.size_of_headers()) {
        severity = Severity::Major;
        return "PE headers";
    }
    for (const auto& copy : copies) {
        const auto& section = *copy.section;
        const std::uint32_t raw_end = copy.begin + copy.length;
        const std::uint64_t span = std::max<std::uint64_t>(section.virtual_size, section.mapped_raw_size());
        const std::uint64_t end = copy.begin + span;
        if (rva >= copy.begin && rva < raw_end) {
            severity = Severity::Major;
            return "section " + section.name + " raw data";
        }
        if (rva >= raw_end && rva < end) {
            severity = Severity::Major;
            return "section " + section.name + " zero fill";
        }
    }
    severity = Severity::Major;
    return "image zero fill";
}

std::uint64_t read_u64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof value);
    return value;
}

void write_u64(std::uint8_t* bytes, std::uint64_t value) { std::memcpy(bytes, &value, sizeof value); }
void write_u32(std::uint8_t* bytes, std::uint32_t value) { std::memcpy(bytes, &value, sizeof value); }

struct ByteRun {
    std::uint32_t rva = 0;
    std::uint32_t length = 0;
    std::string kind;
    Severity severity = Severity::Major;
};

// The independent prediction: one buffer the size of the mapped image, built only from the decoded Image and the fixture
// knowledge the loader also has (the independent resolver for IAT pointers, the load delta, the module TLS index).
std::vector<std::uint8_t> predict_mapped_bytes(const rt::pe::Image& image, const rt::Module& module,
                                              const SymbolTable& symbols, const std::vector<SectionCopy>& copies) {
    const std::uint32_t size = image.size_of_image();
    const std::int64_t delta =
        reinterpret_cast<std::int64_t>(module.base) - static_cast<std::int64_t>(image.image_base());
    std::vector<std::uint8_t> expected(size, 0);

    // 1. The headers the mapper copies from the start of the file.
    std::memcpy(expected.data(), image.file_bytes().data(), image.size_of_headers());

    // 2. Each section's file-backed bytes at its virtual address, by the mapper's page-tail copy rule above.
    for (const auto& copy : copies) {
        if (!copy.length) continue;
        std::memcpy(expected.data() + copy.begin, image.file_bytes().data() + copy.raw_pointer, copy.length);
    }

    // 3. Base relocations, exactly the loader's rules: add the load delta to DIR64 qwords and HIGHLOW dwords; a
    //    HIGHLOW value that no longer fits 32 bits is refused by the loader, so reaching here with one overflowing
    //    would mean the loader accepted a mapping it declares unsupported.
    if (delta != 0) {
        for (const auto& relocation : image.relocations()) {
            if (relocation.type == rt::pe::Relocation::kDir64) {
                if (static_cast<std::uint64_t>(relocation.rva) + 8 > size) continue;
                write_u64(expected.data() + relocation.rva,
                          read_u64(expected.data() + relocation.rva) + static_cast<std::uint64_t>(delta));
            } else if (relocation.type == rt::pe::Relocation::kHighLow) {
                if (static_cast<std::uint64_t>(relocation.rva) + 4 > size) continue;
                std::uint32_t stored = 0;
                std::memcpy(&stored, expected.data() + relocation.rva, sizeof stored);
                const std::int64_t moved = static_cast<std::int64_t>(stored) + delta;
                if (moved >= 0 && moved <= 0xFFFFFFFFll)
                    write_u32(expected.data() + relocation.rva, static_cast<std::uint32_t>(moved));
            }
        }
    }

    // 4. Import address slots, from the independent resolver above.
    for (const auto& library : image.imports()) {
        for (const auto& symbol : library.symbols) {
            const std::string symbol_key =
                symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name;
            const BindingPrediction prediction = symbols.import(library.name, symbol_key);
            write_u64(expected.data() + symbol.iat_rva, reinterpret_cast<std::uint64_t>(prediction.value));
        }
    }

    // 5. The module TLS index the loader writes into the image's _tls_index slot.
    if (image.tls().present && image.tls().index_rva)
        write_u32(expected.data() + image.tls().index_rva, module.tls_index);
    return expected;
}

// Compares predicted with mapped bytes and returns each contiguous run of differences of one category. The caller turns
// each run into a fault with the expected/actual bytes as evidence.
std::vector<ByteRun> compare_mapped_bytes(const rt::pe::Image& image, const std::vector<std::uint8_t>& expected,
                                          const std::uint8_t* mapped, const std::vector<Region>& special,
                                          const std::vector<SectionCopy>& copies) {
    std::vector<ByteRun> runs;
    const std::uint32_t size = image.size_of_image();
    std::uint32_t offset = 0;
    while (offset < size) {
        if (expected[offset] == mapped[offset]) {
            ++offset;
            continue;
        }
        ByteRun run;
        run.rva = offset;
        run.kind = classify(special, image, copies, offset, run.severity);
        while (offset < size && expected[offset] != mapped[offset]) {
            Severity next_severity = Severity::Major;
            if (classify(special, image, copies, offset, next_severity) != run.kind) break;
            ++offset;
        }
        run.length = offset - run.rva;
        runs.push_back(run);
    }
    return runs;
}

// Predicts every mapped byte and reports each difference run as a fault with the bytes as evidence.
void check_mapped_bytes(const std::string& fixture, const rt::pe::Image& image, const rt::Module& module,
                        const SymbolTable& symbols, const std::vector<Region>& special,
                        const std::vector<SectionCopy>& copies) {
    const std::vector<std::uint8_t> expected = predict_mapped_bytes(image, module, symbols, copies);
    const std::vector<ByteRun> runs = compare_mapped_bytes(image, expected, module.base, special, copies);
    std::size_t printed = 0;
    for (const auto& run : runs) {
        const std::string evidence =
            "rva " + hex(run.rva) + " length " + decimal(run.length) + " expected " +
            bytes_hex(expected.data() + run.rva, run.length) + " actual " +
            bytes_hex(module.base + run.rva, run.length);
        ++g_checks;
        fault(run.severity, fixture + " module " + module.name + ": " + run.kind + " differs", evidence);
        if (++printed > 200) {
            note(fixture + " module " + module.name + ": more than 200 differing runs; see the report file for the rest");
            return;
        }
    }
}
// Simulates protect_sections() page by page (VirtualProtect rounds to pages, so the last protection a page sees wins)
// and compares it with VirtualQuery. The image is committed PAGE_READWRITE by VirtualAlloc first.
void check_protections(const std::string& fixture, const rt::pe::Image& image, const rt::Module& module) {
    constexpr std::uint64_t kPage = 0x1000;
    std::map<std::uint64_t, DWORD> expected;
    const std::uint32_t size = image.size_of_image();
    for (std::uint64_t page = 0; page < size; page += kPage) expected[page] = PAGE_READWRITE;
    auto apply = [&](std::uint64_t begin, std::uint64_t length, DWORD protection) {
        if (!length) return;
        for (std::uint64_t page = begin & ~(kPage - 1); page < begin + length; page += kPage)
            expected[page] = protection;
    };
    apply(0, image.size_of_headers(), PAGE_READONLY);
    for (const auto& section : image.sections()) {
        std::uint64_t footprint = section.virtual_size ? section.virtual_size : section.raw_size;
        if (section.virtual_address >= size) continue;
        footprint = std::min<std::uint64_t>(footprint, size - section.virtual_address);
        // The mapper's mapping of the section characteristics (pe_loader.cpp, section_protection()): READ is kept
        // separate from EXECUTE, and a section with no access bits is PAGE_NOACCESS. Private VirtualAlloc memory, so a
        // writable section is PAGE_READWRITE rather than Windows' PAGE_WRITECOPY for a file-backed image section.
        const bool execute = section.executable();
        const bool writable = section.writable();
        const bool readable = section.readable();
        DWORD protection = PAGE_NOACCESS;
        if (execute) protection = writable ? PAGE_EXECUTE_READWRITE : (readable ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
        else if (writable) protection = PAGE_READWRITE;
        else protection = readable ? PAGE_READONLY : PAGE_NOACCESS;
        apply(section.virtual_address, footprint, protection);
    }

    for (const auto& [rva, want] : expected) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(module.base + rva, &info, sizeof info)) {
            ++g_checks;
            fault(Severity::Critical, fixture + " module " + module.name + ": VirtualQuery failed for a mapped page",
                  "rva " + hex(rva) + " error " + decimal(GetLastError()));
            continue;
        }
        if (info.State != MEM_COMMIT) {
            ++g_checks;
            fault(Severity::Critical, fixture + " module " + module.name + ": mapped page is not committed",
                  "rva " + hex(rva) + " state " + decimal(info.State));
            continue;
        }
        ++g_checks;
        if (info.Protect == want) continue;
        const bool execution_affecting = (want & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                                  PAGE_EXECUTE_WRITECOPY)) != 0 ||
                                         (want & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
        fault(execution_affecting ? Severity::Major : Severity::Minor,
              fixture + " module " + module.name + ": page protection differs",
              "rva " + hex(rva) + " expected " + prot_name(want) + " actual " + prot_name(info.Protect) +
                  " allocation base " + hex(reinterpret_cast<std::uint64_t>(info.AllocationBase)));
    }
}

// The file bytes backing an RVA (a header or a section's file-backed data), or an empty span when the RVA is zero fill.
std::span<const std::uint8_t> file_backed(const rt::pe::Image& image, std::uint32_t rva) {
    if (rva < image.size_of_headers())
        return image.file_bytes().subspan(rva, image.size_of_headers() - rva);
    if (const rt::pe::Section* section = image.section_for(rva)) {
        const auto data = image.section_data(*section);
        const std::uint32_t offset = rva - section->virtual_address;
        if (offset < data.size()) return data.subspan(offset);
    }
    return {};
}

// Cross-checks everything the Module description says about the mapped image against the decoded image.
void check_module_description(const std::string& fixture, const rt::pe::Image& image, const rt::Module& module,
                              const SymbolTable& symbols, rt::Loader& loader) {
    const std::string where = fixture + " module " + module.name + ": ";
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(module.base);
    const std::int64_t delta = static_cast<std::int64_t>(base) - static_cast<std::int64_t>(image.image_base());

    expect(module.name == image.name(), Severity::Major, where + "module name differs",
           "module name " + module.name + " image name " + image.name());
    expect(module.size == image.size_of_image(), Severity::Critical,
           where + "mapped-size differs from SizeOfImage",
           "module size " + hex(module.size) + " SizeOfImage " + hex(image.size_of_image()));
    expect(module.preferred_base == image.image_base(), Severity::Major, where + "preferred base differs",
           "module preferred " + hex(module.preferred_base) + " ImageBase " + hex(image.image_base()));
    expect(module.entry_rva == image.entry_point(), Severity::Major, where + "entry RVA differs",
           "module entry " + hex(module.entry_rva) + " AddressOfEntryPoint " + hex(image.entry_point()));
    expect(module.relocated == (delta != 0), Severity::Critical, where + "relocated flag differs",
           "module " + decimal(module.relocated) + " load delta " + signed_decimal(delta));
    expect(base % 0x10000u == 0, Severity::Critical, where + "mapped base is not 64 KiB aligned", "base " + hex(base));
    expect(module.initialised == false, Severity::Major, where + "load() marked the module initialised without DllMain",
           "initialised " + decimal(module.initialised));

    // Exports and forwarders. Every non-zero address-table entry is exported as "#<ordinal>"; a named export also gets
    // its name. An entry whose RVA lies in the export directory is a forwarder string, not code.
    std::map<std::string, std::string> expected_exports;
    std::map<std::string, std::string> expected_forwarders;
    for (const auto& symbol : image.exports()) {
        const std::string ordinal_key = "#" + std::to_string(symbol.ordinal);
        if (symbol.is_forwarder()) {
            expected_forwarders[ordinal_key] = symbol.forwarder;
            if (symbol.by_name) expected_forwarders[symbol.name] = symbol.forwarder;
        } else {
            expected_exports[ordinal_key] = hex(base + symbol.rva);
            if (symbol.by_name) expected_exports[symbol.name] = hex(base + symbol.rva);
        }
    }
    for (const auto& [key, pointer] : expected_exports) {
        const auto it = module.exports.find(key);
        expect(it != module.exports.end() && hex(reinterpret_cast<std::uint64_t>(it->second)) == pointer,
               Severity::Critical, where + "export pointer differs", "export " + key + " expected " + pointer +
                   " actual " + (it == module.exports.end() ? std::string("absent") : hex(
                       reinterpret_cast<std::uint64_t>(it->second))));
    }
    for (const auto& [key, spec] : expected_forwarders) {
        const auto it = module.forwarders.find(key);
        expect(it != module.forwarders.end() && it->second == spec, Severity::Critical,
               where + "forwarder differs", "forwarder " + key + " expected '" + spec + "' actual '" +
                   (it == module.forwarders.end() ? std::string("absent") : it->second) + "'");
    }
    for (const auto& [key, pointer] : module.exports) {
        (void)pointer;
        expect(expected_exports.count(key) != 0, Severity::Major, where + "unexpected export in the module table",
               "export " + key);
    }
    for (const auto& [key, spec] : module.forwarders) {
        (void)spec;
        expect(expected_forwarders.count(key) != 0, Severity::Major,
               where + "unexpected forwarder in the module table", "forwarder " + key);
    }

    // Import records, in the order the import directory spells them.
    std::size_t record = 0;
    for (const auto& library : image.imports()) {
        for (const auto& symbol : library.symbols) {
            const std::string symbol_key = symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name;
            const BindingPrediction prediction = symbols.import(library.name, symbol_key);
            if (record < module.imports.size()) {
                const rt::ImportRecord& entry = module.imports[record];
                expect(entry.dll == library.name, Severity::Major, where + "import DLL name differs",
                       "expected " + library.name + " actual " + entry.dll);
                expect(entry.name == symbol_key, Severity::Major, where + "import symbol differs",
                       "expected " + symbol_key + " actual " + entry.name);
                expect(binding_name(entry.binding) == prediction.binding, Severity::Critical,
                       where + "import binding differs",
                       "import " + library.name + "!" + symbol_key + " expected " + prediction.binding + " actual " +
                           binding_name(entry.binding) + " (" + prediction.note + ")");
            } else {
                expect(false, Severity::Critical, where + "import record missing",
                       "import " + library.name + "!" + symbol_key + " is not recorded in the Module");
            }
            ++record;
        }
    }
    expect(module.imports.size() == record, Severity::Major, where + "import record count differs",
           "module has " + decimal(module.imports.size()) + " records, the image has " + decimal(record));

    // Exception directory registration.
    const std::uint32_t expected_functions = image.exceptions().present ? image.exceptions().entry_count : 0;
    expect(module.function_count == expected_functions, Severity::Major, where + "registered unwind count differs",
           "module function_count " + decimal(module.function_count) + " exception entries " +
               decimal(expected_functions));
    if (expected_functions && !module.function_table) {
        ++g_checks;
        fault(Severity::Major, where + "exception directory not registered", "function_count is " +
                  decimal(module.function_count) + " but function_table is null");
    }

    // Static TLS: the loader-installed fields and the template copy in the calling thread's TEB slot.
    const auto& tls = image.tls();
    const bool tls_present = tls.present && (tls.template_size || tls.zero_fill || tls.index_rva || tls.callbacks_rva);
    if (tls_present) {
        expect(module.tls_index != rt::kNoTlsIndex, Severity::Critical,
               where + "TLS directory present but no module TLS index installed", "tls_index is TLS_OUT_OF_INDEXES");
        expect(module.tls_raw_size == tls.template_size, Severity::Major, where + "TLS raw size differs",
               "module " + decimal(module.tls_raw_size) + " image " + decimal(tls.template_size));
        expect(module.tls_zero_fill == tls.zero_fill, Severity::Major, where + "TLS zero fill differs",
               "module " + decimal(module.tls_zero_fill) + " image " + decimal(tls.zero_fill));
        expect(module.tls_block_size == tls.template_size + tls.zero_fill, Severity::Major,
               where + "TLS block size differs",
               "module " + decimal(module.tls_block_size) + " image " +
                   decimal(static_cast<std::uint64_t>(tls.template_size) + tls.zero_fill));
        const void* expected_template = tls.template_size ? module.base + tls.template_rva : nullptr;
        expect(module.tls_template == expected_template, Severity::Critical, where + "TLS template pointer differs",
               "module template " + hex(reinterpret_cast<std::uint64_t>(module.tls_template)) + " expected " +
                   hex(reinterpret_cast<std::uint64_t>(expected_template)));
        if (module.tls_index != rt::kNoTlsIndex && module.tls_index < 64) {
            auto** slots = reinterpret_cast<void**>(__readgsqword(0x58));
            const bool slot_ok = slots && module.tls_index < 64 && slots[module.tls_index] == module.tls_block;
            expect(slot_ok, Severity::Critical, where + "TEB TLS slot does not hold the module's TLS block",
                   "tls_index " + decimal(module.tls_index) + " teb " +
                       hex(slots ? reinterpret_cast<std::uint64_t>(slots[module.tls_index]) : 0) + " module block " +
                       hex(reinterpret_cast<std::uint64_t>(module.tls_block)));
            if (slot_ok && module.tls_block) {
                const auto* block = static_cast<const std::uint8_t*>(module.tls_block);
                const auto template_file = file_backed(image, tls.template_rva);
                const bool template_ok = template_file.size() >= tls.template_size;
                const bool copy_ok = template_ok &&
                                     std::memcmp(block, template_file.data(), tls.template_size) == 0;
                expect(template_ok, Severity::Critical, where + "TLS template is not file-backed in the fixture",
                       "template rva " + hex(tls.template_rva) + " size " + decimal(tls.template_size));
                expect(copy_ok, Severity::Critical, where + "TLS block does not hold the template bytes",
                       "expected " + bytes_hex(template_file.data(), template_file.size()) + " actual " +
                           bytes_hex(block, tls.template_size));
                bool tail_zero = true;
                for (std::uint32_t i = tls.template_size; i < module.tls_block_size; ++i)
                    if (block[i] != 0) tail_zero = false;
                expect(tail_zero, Severity::Major, where + "TLS zero-fill tail is not zero",
                       "tls_zero_fill " + decimal(tls.zero_fill));
            }
        }
    } else {
        expect(module.tls_index == rt::kNoTlsIndex, Severity::Major,
               where + "module TLS index set for an image without TLS", "tls_index " + decimal(module.tls_index));
        expect(module.tls_block == nullptr, Severity::Major,
               where + "TLS block allocated for an image without TLS", "tls_block is not null");
    }

    // find_address(): the mapped range and the module name.
    std::string found_module;
    std::uint32_t rva = 0;
    const bool found = loader.find_address(base + image.entry_point(), found_module, rva);
    expect(found && lower(found_module) == lower(fixture) && rva == image.entry_point(), Severity::Major,
           where + "find_address does not locate the entry point",
           "found " + decimal(found) + " module '" + found_module + "' rva " + hex(rva));
}

// Runs the byte, protection and Module checks for every module the loader maps, using the fixture image of the same
// name. `images` keys are lower-case file names.
void verify_loaded_modules(const std::string& label, rt::Loader& loader,
                           const std::map<std::string, rt::pe::Image>& images, const SymbolTable& symbols) {
    std::set<std::string> seen;
    for (const rt::Module* module : loader.modules()) {
        const std::string key = lower(module->name);
        const auto it = images.find(key);
        if (it == images.end()) {
            ++g_checks;
            fault(Severity::Major, label + ": the loader mapped a module the harness did not build",
                  "module " + module->name);
            continue;
        }
        seen.insert(key);
        const auto& image = it->second;
        const std::int64_t delta =
            reinterpret_cast<std::int64_t>(module->base) - static_cast<std::int64_t>(image.image_base());
        const std::vector<Region> special = byte_regions(image, delta);
        const std::vector<SectionCopy> copies = predicted_section_copies(image);
        check_mapped_bytes(module->name, image, *module, symbols, special, copies);
        check_protections(module->name, image, *module);
        check_module_description(module->name, image, *module, symbols, loader);

        // The mapper copies the page tail past VirtualSize (measured Windows behaviour); the decoder's section_data()
        // exposes only min(SizeOfRawData, VirtualSize). That difference is a decoder/mapper contract gap for the
        // rewrite, not a fault in this mapping, so it is reported as an info note with the sizes and RVAs.
        for (const auto& copy : copies) {
            const std::uint32_t decoder_bytes = copy.section->mapped_raw_size();
            if (copy.length > decoder_bytes)
                note(label + ": module " + module->name + " section " + copy.section->name + " maps " +
                     decimal(copy.length) + " file bytes from rva " + hex(copy.begin) + " (page tail past VirtualSize); "
                     "rt::pe::Image::section_data() exposes " + decimal(decoder_bytes) +
                     " - a decoder/mapper contract gap for the rewrite's mapper");
        }

        ++g_modules_verified;
        g_bytes_verified += image.size_of_image();
        g_pages_verified += (image.size_of_image() + 0xFFFu) / 0x1000u;
        for (const auto& library : image.imports()) g_iat_verified += library.symbols.size();
        if (delta != 0) g_relocations_verified += image.relocations().size();
        note(label + ": module " + module->name + " mapped at " + hex(reinterpret_cast<std::uint64_t>(module->base)) +
             " (preferred " + hex(image.image_base()) + ", delta " + signed_decimal(delta) + "), " +
             hex(image.size_of_image()) + " bytes compared, " + decimal(image.imports().size()) +
             " import libraries, " + decimal(image.relocations().size()) + " relocation entries");
    }
    for (const auto& [key, image] : images) {
        (void)image;
        expect(seen.count(key) != 0, Severity::Critical, label + ": a fixture was not mapped",
               "module " + key);
    }
}

// ---- Fixture cases ---------------------------------------------------------------------------------------------------

// Parses one fixture into an Image that outlives the mapping checks. The bytes are owned by the caller.
rt::pe::Image parse_fixture(const loader_selfcheck::Fixture& fixture) {
    return rt::pe::Image::parse(fixture.bytes, fixture.filename);
}

bool environment_limit(const std::string& message) {
    // The TLS helper is a build/deployment artefact of the test target (palmetto_tls_host.dll next to the executable),
    // exactly as it is for loader_tls_test. Its absence is an environment limit, not a loader fault.
    return message.find("palmetto_tls_host.dll") != std::string::npos ||
           message.find("this thread has no TLS array") != std::string::npos ||
           message.find("cannot reserve the preferred base") != std::string::npos;
}

// A relocation-less DLL is mapped at its preferred base, and every mapped byte/protection/Module field must match.
void test_no_reloc_at_preferred() {
    const std::uint64_t base = free_base(0);
    const auto fixture = loader_selfcheck::no_reloc_dll(base);
    const fs::path path = write_fixture(fixture);
    SymbolTable symbols = shim_table();
    try {
        rt::Loader loader(g_dir);
        rt::Module& module = loader.load(path);
        expect(reinterpret_cast<std::uint64_t>(module.base) == base, Severity::Critical,
               "no-reloc image was not mapped at its preferred base",
               "mapped " + hex(reinterpret_cast<std::uint64_t>(module.base)) + " preferred " + hex(base));
        std::map<std::string, rt::pe::Image> images;
        images.emplace(lower(fixture.filename), parse_fixture(fixture));
        verify_loaded_modules("no-reloc image at its preferred base", loader, images, symbols);
    } catch (const std::exception& e) {
        if (environment_limit(e.what())) {
            note(std::string("no-reloc image: skipped the TLS part, the environment cannot install static TLS (") + e.what() + ")");
            return;
        }
        ++g_checks;
        fault(Severity::Critical, "no-reloc image at its preferred base: the loader refused a valid relocation-less image",
              e.what());
    }
}

// A section whose VirtualAddress is not on the SectionAlignment grid (.data at 0x2100 with a 0x1000 alignment). PE/COFF
// requires the alignment and Windows refuses such an image; the bounds-safe decoder accepts it. The case records what
// the loader does: a named refusal is a documented hardening (info), and a lenient loader's mapping is still predicted
// byte for byte (the prediction and the protection simulation handle a non-page-aligned section start).
void test_unaligned_section() {
    const std::string label = "section VirtualAddress off the SectionAlignment grid";
    const std::uint64_t base = free_base(7);
    const auto fixture = loader_selfcheck::no_reloc_dll(base, 0x2100);
    const fs::path path = write_fixture(fixture);
    std::map<std::string, rt::pe::Image> images;
    images.emplace(lower(fixture.filename), parse_fixture(fixture));
    SymbolTable symbols = shim_table();
    try {
        rt::Loader loader(g_dir);
        rt::Module& module = loader.load(path);
        expect(reinterpret_cast<std::uint64_t>(module.base) == base, Severity::Critical,
               label + ": the loader moved a relocation-less image",
               "mapped " + hex(reinterpret_cast<std::uint64_t>(module.base)) + " preferred " + hex(base));
        verify_loaded_modules(label, loader, images, symbols);
        note(label + ": the loader accepted the off-grid section address and the full byte/protection prediction matched");
    } catch (const std::exception& e) {
        const std::string message = e.what();
        if (environment_limit(message)) {
            note(label + ": skipped, the environment cannot run this fixture (" + message + ")");
            return;
        }
        ++g_checks;
        if (message.find("is not aligned to SectionAlignment") != std::string::npos)
            note(label + ": the loader refused it as documented (" + message + ")");
        else
            fault(Severity::Critical, label + ": the loader refused a decoder-valid image with an unexpected error",
                  message);
    }
}
// The provider and dependency are pre-mapped at their preferred bases, then the importer - whose preferred base is
// held - is mapped elsewhere. Every mapped byte must match, including the relocated DIR64 slots, the relocated TLS
// directory and every IAT pointer (bundled code, bundled ordinal, shim forwarder, bundled forwarder, host, missing).
void test_relocated_imports() {
    const std::uint64_t importer_base = free_base(0);
    const std::uint64_t provider_base = free_base(1);
    const std::uint64_t dependency_base = free_base(2);
    const auto importer = loader_selfcheck::relocated_importer_dll(importer_base);
    const auto provider = loader_selfcheck::provider_dll(provider_base);
    const auto dependency = loader_selfcheck::dependency_dll(dependency_base);
    const fs::path importer_path = write_fixture(importer);
    const fs::path provider_path = write_fixture(provider);
    const fs::path dependency_path = write_fixture(dependency);

    std::map<std::string, rt::pe::Image> images;
    images.emplace(lower(importer.filename), parse_fixture(importer));
    images.emplace(lower(provider.filename), parse_fixture(provider));
    images.emplace(lower(dependency.filename), parse_fixture(dependency));

    SymbolTable symbols = shim_table();
    symbols.bundled[lower(provider.filename)] = {&images.at(lower(provider.filename)), provider_base};
    symbols.bundled[lower(dependency.filename)] = {&images.at(lower(dependency.filename)), dependency_base};

    try {
        HeldBase hold(importer_base, loader_selfcheck::kSizeOfImage);
        rt::Loader loader(g_dir);
        rt::Module& mapped_provider = loader.load(provider_path);
        rt::Module& mapped_dependency = loader.load(dependency_path);
        expect(reinterpret_cast<std::uint64_t>(mapped_provider.base) == provider_base, Severity::Critical,
               "provider image was not mapped at its preferred base",
               "mapped " + hex(reinterpret_cast<std::uint64_t>(mapped_provider.base)) + " preferred " +
                   hex(provider_base));
        expect(reinterpret_cast<std::uint64_t>(mapped_dependency.base) == dependency_base, Severity::Critical,
               "dependency image was not mapped at its preferred base",
               "mapped " + hex(reinterpret_cast<std::uint64_t>(mapped_dependency.base)) + " preferred " +
                   hex(dependency_base));
        rt::Module& mapped_importer = loader.load(importer_path);
        expect(reinterpret_cast<std::uint64_t>(mapped_importer.base) != importer_base, Severity::Critical,
               "relocated image was mapped over its held preferred base",
               "mapped " + hex(reinterpret_cast<std::uint64_t>(mapped_importer.base)));
        expect(mapped_importer.relocated, Severity::Critical,
               "relocated image does not report the move away from its preferred base",
               "mapped " + hex(reinterpret_cast<std::uint64_t>(mapped_importer.base)) + " preferred " +
                   hex(importer_base));
        verify_loaded_modules("relocated image with imports, TLS and DIR64 relocations", loader, images, symbols);
    } catch (const std::exception& e) {
        if (environment_limit(e.what())) {
            note(std::string("relocated image: skipped the TLS part, the environment cannot install static TLS (") + e.what() + ")");
            return;
        }
        ++g_checks;
        fault(Severity::Critical, "relocated image with imports, TLS and DIR64 relocations: the loader refused a valid image",
              e.what());
    }
}

// HIGHLOW at the preferred base: the loader applies no relocation at all, and the dword keeps its file value.
void test_highlow_at_preferred() {
    const std::uint64_t base = free_base(0);
    const auto fixture = loader_selfcheck::highlow_dll(base, 0x10001234u);
    const fs::path path = write_fixture(fixture);
    std::map<std::string, rt::pe::Image> images;
    images.emplace(lower(fixture.filename), parse_fixture(fixture));
    SymbolTable symbols = shim_table();
    try {
        rt::Loader loader(g_dir);
        rt::Module& module = loader.load(path);
        expect(reinterpret_cast<std::uint64_t>(module.base) == base, Severity::Critical,
               "HIGHLOW image at its preferred base was moved anyway",
               "mapped " + hex(reinterpret_cast<std::uint64_t>(module.base)) + " preferred " + hex(base));
        expect(!module.relocated, Severity::Critical, "HIGHLOW image at its preferred base reports relocated",
               "relocated " + decimal(module.relocated));
        verify_loaded_modules("HIGHLOW image at its preferred base", loader, images, symbols);
    } catch (const std::exception& e) {
        ++g_checks;
        fault(Severity::Critical, "HIGHLOW image at its preferred base: the loader refused a valid image", e.what());
    }
}

// HIGHLOW away from the preferred base, at an explicit preferred base (the case holds it so the loader must relocate).
// If the mapped address keeps the moved value inside 32 bits the full byte prediction applies and must match; if it does
// not, the loader must refuse with its documented message instead of truncating the slot. Returns true when the image
// was accepted (and therefore byte-verified).
bool highlow_relocated_case(const char* label, std::uint64_t preferred, std::uint32_t stored) {
    const auto fixture = loader_selfcheck::highlow_dll(preferred, stored);
    const fs::path path = write_fixture(fixture);
    std::map<std::string, rt::pe::Image> images;
    images.emplace(lower(fixture.filename), parse_fixture(fixture));
    SymbolTable symbols = shim_table();
    try {
        HeldBase hold(preferred, loader_selfcheck::kSizeOfImage);
        rt::Loader loader(g_dir);
        rt::Module& module = loader.load(path);
        // It moved and the loader accepted it: the moved dword must be exactly the file value plus the load delta.
        ++g_checks;
        std::uint32_t mapped_dword = 0;
        std::memcpy(&mapped_dword, module.base + 0x2240, sizeof mapped_dword);
        const std::int64_t delta =
            reinterpret_cast<std::int64_t>(module.base) - static_cast<std::int64_t>(preferred);
        const std::int64_t expected = static_cast<std::int64_t>(stored) + delta;
        if (expected < 0 || expected > 0xFFFFFFFFll) {
            fault(Severity::Critical, std::string(label) + ": the loader accepted a HIGHLOW relocation that cannot fit 32 bits",
                  "stored " + hex(stored) + " delta " + signed_decimal(delta) + " result " +
                      hex(static_cast<std::uint64_t>(expected)));
            return false;
        }
        expect(mapped_dword == static_cast<std::uint32_t>(expected), Severity::Critical,
               std::string(label) + ": HIGHLOW dword was not moved by exactly the load delta",
               "mapped " + hex(mapped_dword) + " expected " + hex(static_cast<std::uint64_t>(expected)));
        verify_loaded_modules(label, loader, images, symbols);
        note(std::string(label) + ": the mapper relocated the image to " +
             hex(reinterpret_cast<std::uint64_t>(module.base)) + " and the HIGHLOW value fit 32 bits (delta " + signed_decimal(delta) + ")");
        return true;
    } catch (const std::exception& e) {
        const std::string message = e.what();
        if (message.find("a 32-bit (HIGHLOW) relocation does not fit at this base") != std::string::npos) {
            ++g_checks;
            note(std::string(label) + ": the loader refused the relocation as documented (" + message + ")");
            return false;
        }
        ++g_checks;
        fault(Severity::Critical, std::string(label) + ": the loader refused a valid HIGHLOW image with an unexpected error",
              message);
    }
    return false;
}

void test_highlow_relocated() {
    // A normal 32-bit absolute value with the preferred base held: Windows' first-fit fallback is normally far enough
    // away that the 32-bit value cannot be moved, so the loader refuses. Both outcomes are checked against the decoder.
    (void)highlow_relocated_case("HIGHLOW image away from its preferred base", free_base(5), 0x10001234u);
    // A value near the top of the 32-bit range cannot survive a positive load delta and must be refused rather than
    // truncated (the byte prediction would catch a truncated slot as a critical mismatch).
    (void)highlow_relocated_case("HIGHLOW image whose 32-bit value cannot survive the load delta", free_base(6),
                                 0xFFFFFFF0u);
}

// Forcing the successful HIGHLOW path: discover the address the loader's fallback VirtualAlloc would use (one image-sized
// allocate/free with the same flags), then hold the preferred base a little above it. The fallback then lands on the
// freed block and the load delta is small and negative, so the 32-bit slot still fits and the HIGHLOW application rule
// can be compared byte by byte. If the allocator hands the fallback a different address the case degrades to the same
// accepted-or-documented-refusal check as above; the byte prediction validates whichever delta the loader used.
void test_highlow_relocated_that_fits() {
    void* probe = VirtualAlloc(nullptr, loader_selfcheck::kSizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!probe) {
        note("HIGHLOW application case skipped: no probe allocation for the fallback address");
        return;
    }
    const std::uint64_t fallback = reinterpret_cast<std::uint64_t>(probe);
    VirtualFree(probe, 0, MEM_RELEASE);
    for (const std::uint64_t offset : {0x10000ull, 0x20000ull, 0x40000ull, 0x100000ull, 0x200000ull}) {
        const std::uint64_t preferred = fallback + offset;
        if (!range_free(preferred, loader_selfcheck::kSizeOfImage)) continue;
        const bool accepted = highlow_relocated_case("HIGHLOW image relocated onto the probed fallback block", preferred,
                                                    0x80000000u);
        if (!accepted)
            note("HIGHLOW application case: the fallback address was not reproducible in this run (documented refusal)");
        return;
    }
    note("HIGHLOW application case skipped: no free preferred base just above the probed fallback address");
}
// A relocation-less image whose preferred base is taken: the loader has no way to move it and must say so instead of
// mapping it somewhere else (which would leave every absolute address wrong). This is the shape the user hypothesis
// ("no image appears") would come from, so its refusal is verified, not assumed.
void test_no_reloc_preferred_taken() {
    const std::uint64_t base = free_base(4);
    const auto fixture = loader_selfcheck::no_reloc_dll(base);
    const fs::path path = write_fixture(fixture);
    try {
        HeldBase hold(base, loader_selfcheck::kSizeOfImage);
        rt::Loader loader(g_dir);
        rt::Module& module = loader.load(path);
        ++g_checks;
        fault(Severity::Critical,
              "a relocation-less image was mapped away from its preferred base instead of being refused",
              "preferred " + hex(base) + " mapped " + hex(reinterpret_cast<std::uint64_t>(module.base)));
    } catch (const std::exception& e) {
        const std::string message = e.what();
        ++g_checks;
        if (message.find("preferred base is taken and the image has no relocations") == std::string::npos)
            fault(Severity::Critical, "a relocation-less image with a taken preferred base was refused with an unexpected error",
                  message);
        else
            note("relocation-less image with a taken preferred base: refused as documented (" + message + ")");
    }
}

// Negative control for the detector itself: a "0 faults" result must not be vacuous. The prediction and comparison are
// exercised on a decoded fixture with a fake Module (no mapping is needed to compare two buffers), five single bytes are
// flipped inside different categories, and every flip must come back as exactly one run of that category's kind and
// severity.
void test_detector_self_check() {
    const std::uint64_t preferred = free_base(7);
    const auto fixture = loader_selfcheck::relocated_importer_dll(preferred);
    const rt::pe::Image image = parse_fixture(fixture);
    const SymbolTable symbols = shim_table();

    std::vector<std::uint8_t> mapped(image.size_of_image(), 0);
    rt::Module module;
    module.name = fixture.filename;
    module.base = mapped.data();
    module.tls_index = 9;  // what the prediction writes into _tls_index
    const std::int64_t delta =
        reinterpret_cast<std::int64_t>(module.base) - static_cast<std::int64_t>(image.image_base());
    const std::vector<Region> special = byte_regions(image, delta);
    const std::vector<SectionCopy> copies = predicted_section_copies(image);
    std::vector<std::uint8_t> expected = predict_mapped_bytes(image, module, symbols, copies);
    if (image.imports().empty() || image.imports()[0].symbols.empty() || image.relocations().empty() ||
        !image.tls().index_rva) {
        ++g_checks;
        fault(Severity::Critical, "detector self-check: the fixture lacks an IAT slot, relocation or TLS index",
              fixture.filename);
        return;
    }
    struct Injection {
        std::uint32_t rva;
        const char* kind;
        Severity severity;
    };
    const std::vector<Injection> injections = {
        {0x20, "PE headers", Severity::Major},
        {0x1020, "section .text raw data", Severity::Major},
        {image.imports()[0].symbols[0].iat_rva, "IAT binding slot", Severity::Critical},
        {image.tls().index_rva, "TLS module index slot", Severity::Critical},
        {image.relocations()[0].rva, "base relocation qword", Severity::Critical},
    };
    mapped = expected;
    for (const auto& injection : injections) mapped[injection.rva] = static_cast<std::uint8_t>(expected[injection.rva] ^ 0xFFu);
    const int faults_before = g_faults;
    const std::vector<ByteRun> runs = compare_mapped_bytes(image, expected, mapped.data(), special, copies);
    expect(runs.size() == injections.size(), Severity::Critical, "detector self-check: wrong number of differing runs",
           "detected " + decimal(runs.size()) + " expected " + decimal(injections.size()));
    for (const auto& injection : injections) {
        const ByteRun* found = nullptr;
        for (const auto& run : runs)
            if (run.rva == injection.rva) found = &run;
        expect(found != nullptr, Severity::Critical, std::string("detector self-check: ") + injection.kind +
                   " difference was not detected",
               "rva " + hex(injection.rva));
        if (!found) continue;
        expect(found->length == 1, Severity::Critical, std::string("detector self-check: ") + injection.kind +
                   " run length is wrong",
               "rva " + hex(injection.rva) + " length " + decimal(found->length));
        expect(found->kind == injection.kind, Severity::Critical,
               std::string("detector self-check: ") + injection.kind + " was categorised as " + found->kind,
               "rva " + hex(injection.rva));
        expect(found->severity == injection.severity, Severity::Critical,
               std::string("detector self-check: ") + injection.kind + " has the wrong severity",
               "rva " + hex(injection.rva) + " severity " + severity_name(found->severity));
    }
    if (g_faults == faults_before)
        note("detector self-check: injected differences in headers, section data, an IAT slot, the TLS index and a DIR64 "
             "relocation were each reported with the expected category and severity");
}
}  // namespace

int main() {
    g_dir = work_dir();
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    if (ec) {
        std::printf("loader_selfcheck: cannot create %s: %s\n", g_dir.string().c_str(), ec.message().c_str());
        return 1;
    }
    std::printf("loader_selfcheck: synthetic mapped-image self-check of the old rt::Loader\n");
    std::printf("  work dir: %s\n", g_dir.string().c_str());
    const fs::path tls_helper = executable_dir() / "palmetto_tls_host.dll";
    std::printf("  TLS helper: %s%s\n", tls_helper.string().c_str(),
                fs::exists(tls_helper) ? "" : " (missing: the TLS fixtures will be reported as an environment limit)");
    std::printf("  fixture:  headers 0x400, SizeOfImage 0x4000, .text/.data/.rdata with VirtualSize != SizeOfRawData in\n"
                "            both directions (raw data past VirtualSize is mapped), a VirtualSize that is not a page\n"
                "            multiple, DIR64/HIGHLOW relocations, TLS, exports, imports and exceptions\n");

    try {
        test_detector_self_check();
        test_no_reloc_at_preferred();
        test_unaligned_section();
        test_relocated_imports();
        test_highlow_at_preferred();
        test_highlow_relocated();
        test_highlow_relocated_that_fits();
        test_no_reloc_preferred_taken();
    } catch (const std::exception& e) {
        fault(Severity::Critical, "the harness itself failed", e.what());
    }

    // The report keeps everything (the console shows the first 400 lines), next to the synthetic fixtures.
    const fs::path report_path = g_dir / "loader_selfcheck_report.txt";
    {
        std::ofstream report(report_path, std::ios::binary);
        if (report) report << g_report;
    }
    flush_console();
    std::printf("\nloader_selfcheck: %d checks, %d faults", g_checks, g_faults);
    for (const auto& [severity, count] : g_faults_by_severity) std::printf(" (%s %d)", severity.c_str(), count);
    std::printf("\n  coverage: %llu modules, %llu mapped bytes, %llu pages, %llu IAT slots, %llu relocation entries\n",
                static_cast<unsigned long long>(g_modules_verified),
                static_cast<unsigned long long>(g_bytes_verified),
                static_cast<unsigned long long>(g_pages_verified),
                static_cast<unsigned long long>(g_iat_verified),
                static_cast<unsigned long long>(g_relocations_verified));
    std::printf("report: %s\n", report_path.string().c_str());
    if (g_faults == 0) std::printf("RESULT: PASS - every mapped byte, protection and Module field matched rt::pe::Image\n");
    return g_faults == 0 ? 0 : 1;
}
