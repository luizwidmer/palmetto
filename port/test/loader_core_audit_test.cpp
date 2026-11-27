// Core-mapping audit of the inherited PE loader (port/src/pe_loader.cpp): section data copy rule, zero raw pointer,
// relocations into the section's last page, alignment validation, shared-page protection conflicts, PAGE_* derivation
// and the base-reservation commit path.
//
// Every image here is built byte by byte below: no game, package or fixture DLL is read. The expected behaviour is
// taken from the PE/COFF specification and from the local Windows image loader, measured with the same layouts through
// LoadLibrary (the measurements are quoted at each check). Each refusal is a named error; the cases Windows accepts
// anyway (an unaligned SizeOfImage, an unaligned SizeOfHeaders) are kept accepted here, so the audit does not hide a
// compatibility regression behind stricter validation.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "free_image_base.h"
#include "pe_loader.h"

namespace {

namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;
unsigned g_files = 0;
fs::path g_temp_dir;
uint64_t g_next_base = 0x1A0000000ull;

void check(bool condition, const std::string& what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (!condition) ++g_failures;
}

// A distinct, free 64 KiB-aligned preferred base for one synthetic image. Preferred bases are probed so a test maps
// where it asks instead of being relocated for an unrelated reason.
uint64_t next_image_base() {
    for (int attempt = 0; attempt < 64; ++attempt) {
        const uint64_t candidate = g_next_base;
        g_next_base += 0x1000000ull;
        void* probe = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(candidate)), 0x10000, MEM_RESERVE,
                                   PAGE_NOACCESS);
        if (!probe) continue;
        VirtualFree(probe, 0, MEM_RELEASE);
        return candidate;
    }
    return palmetto_test::free_image_base(0x1A0000000ull);
}

std::filesystem::path write_image(const std::vector<uint8_t>& bytes) {
    const auto path = g_temp_dir / ("core_audit_" + std::to_string(++g_files) + ".bin");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create " + path.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
    return path;
}

// -------------------------------------------------------------------------------------------------------------------
// Synthetic PE32+ builder (our own bytes only)
// -------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kNtOffset = 0x80;
constexpr uint32_t kOptionalOffset = kNtOffset + 4 + 20;  // 0x98
constexpr uint32_t kSectionTableOffset = kOptionalOffset + 240;  // 0x188

struct SectionSpec {
    const char* name = "";
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t characteristics = 0;
    std::vector<uint8_t> raw;       // file-backed bytes; SizeOfRawData is this rounded up to the FileAlignment
    uint32_t raw_pointer = 0xFFFFFFFFu;  // override; 0xFFFFFFFF = let the builder place it
    uint32_t raw_size = 0;          // override; 0 = align_up(raw.size(), FileAlignment)
};

struct Patch {
    size_t offset = 0;
    unsigned width = 4;
    uint64_t value = 0;
};

struct ImageSpec {
    uint64_t image_base = 0;
    uint32_t section_alignment = 0x1000;
    uint32_t file_alignment = 0x200;
    uint32_t size_of_headers = 0;  // 0 = computed
    uint32_t size_of_image = 0;    // 0 = computed
    uint32_t entry_point = 0;
    bool dll = true;
    std::vector<SectionSpec> sections;
    std::vector<Patch> patches;
};

uint32_t align_up(uint32_t value, uint32_t alignment) {
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}

std::vector<uint8_t> build_image(const ImageSpec& spec) {
    if (!spec.image_base || (spec.image_base & 0xFFFFull))
        throw std::logic_error("the audit fixtures need a 64 KiB-aligned ImageBase");
    const uint32_t fa = spec.file_alignment;
    const uint32_t sa = spec.section_alignment;
    const uint32_t sections = static_cast<uint32_t>(spec.sections.size());
    const uint32_t headers_end = kSectionTableOffset + sections * 40;
    const uint32_t size_of_headers =
        spec.size_of_headers ? spec.size_of_headers : align_up(headers_end, fa < 0x200 ? 0x200 : fa);

    std::vector<uint32_t> raw_pointers(sections, 0);
    uint32_t cursor = size_of_headers;
    for (uint32_t i = 0; i < sections; ++i) {
        if (spec.sections[i].raw.empty()) continue;
        raw_pointers[i] = cursor;
        cursor += align_up(static_cast<uint32_t>(spec.sections[i].raw.size()), fa);
    }
    const uint32_t file_size = cursor;

    uint32_t image_end = size_of_headers;
    for (const auto& section : spec.sections)
        image_end = (std::max)(image_end, section.virtual_address + (std::max)(section.virtual_size,
                                                                              align_up(static_cast<uint32_t>(section.raw.size()), fa)));
    const uint32_t size_of_image = spec.size_of_image ? spec.size_of_image : align_up(image_end, sa ? sa : 0x1000);

    std::vector<uint8_t> bytes(file_size, 0);
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(bytes.data() + at, &v, sizeof(v)); };
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(bytes.data() + at, &v, sizeof(v)); };
    auto put64 = [&](size_t at, uint64_t v) { std::memcpy(bytes.data() + at, &v, sizeof(v)); };

    put16(0, IMAGE_DOS_SIGNATURE);
    put32(0x3C, kNtOffset);
    put32(kNtOffset, IMAGE_NT_SIGNATURE);
    put16(kNtOffset + 4, IMAGE_FILE_MACHINE_AMD64);
    put16(kNtOffset + 6, static_cast<uint16_t>(sections));
    put16(kNtOffset + 20, static_cast<uint16_t>(sizeof(IMAGE_OPTIONAL_HEADER64)));
    put16(kNtOffset + 22, static_cast<uint16_t>(IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE |
                                               (spec.dll ? IMAGE_FILE_DLL : 0)));
    put16(kOptionalOffset, IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    put32(kOptionalOffset + 4, 0);  // SizeOfCode (not consulted by the loader)
    put32(kOptionalOffset + 16, spec.entry_point);
    put64(kOptionalOffset + 24, spec.image_base);
    put32(kOptionalOffset + 32, sa);
    put32(kOptionalOffset + 36, fa);
    put16(kOptionalOffset + 40, 6);  // MajorOperatingSystemVersion
    put16(kOptionalOffset + 48, 6);  // MajorSubsystemVersion
    put32(kOptionalOffset + 56, size_of_image);
    put32(kOptionalOffset + 60, size_of_headers);
    put32(kOptionalOffset + 64, 0);  // CheckSum
    put16(kOptionalOffset + 68, IMAGE_SUBSYSTEM_WINDOWS_CUI);
    put16(kOptionalOffset + 70, 0);  // DllCharacteristics
    put64(kOptionalOffset + 72, 0x100000);  // SizeOfStackReserve
    put64(kOptionalOffset + 80, 0x1000);    // SizeOfStackCommit
    put64(kOptionalOffset + 88, 0x100000);  // SizeOfHeapReserve
    put64(kOptionalOffset + 96, 0x1000);    // SizeOfHeapCommit
    put32(kOptionalOffset + 104, 0);        // LoaderFlags
    put32(kOptionalOffset + 108, IMAGE_NUMBEROF_DIRECTORY_ENTRIES);

    for (uint32_t i = 0; i < sections; ++i) {
        const auto& section = spec.sections[i];
        const size_t at = kSectionTableOffset + static_cast<size_t>(i) * 40;
        const size_t name_length = (std::min)(std::strlen(section.name), static_cast<size_t>(8));
        std::memcpy(bytes.data() + at, section.name, name_length);
        const uint32_t size_of_raw_data =
            section.raw_size ? section.raw_size : align_up(static_cast<uint32_t>(section.raw.size()), fa);
        const uint32_t pointer = section.raw_pointer != 0xFFFFFFFFu ? section.raw_pointer : raw_pointers[i];
        put32(at + 8, section.virtual_size);
        put32(at + 12, section.virtual_address);
        put32(at + 16, size_of_raw_data);
        put32(at + 20, pointer);
        put32(at + 36, section.characteristics);
        if (!section.raw.empty() && pointer && pointer <= bytes.size()) {
            const size_t copy = (std::min)(section.raw.size(), bytes.size() - pointer);
            std::memcpy(bytes.data() + pointer, section.raw.data(), copy);
        }
    }

    for (const Patch& patch : spec.patches) {
        if (patch.offset + patch.width > bytes.size()) throw std::logic_error("audit fixture patch out of range");
        if (patch.width == 2) put16(patch.offset, static_cast<uint16_t>(patch.value));
        if (patch.width == 4) put32(patch.offset, static_cast<uint32_t>(patch.value));
        if (patch.width == 8) put64(patch.offset, patch.value);
    }
    return bytes;
}

// The parts of a built image the tests edit: header fields and data directories.
IMAGE_NT_HEADERS64* nt_of(std::vector<uint8_t>& bytes) {
    return reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes.data() + kNtOffset);
}
IMAGE_SECTION_HEADER* section_of(std::vector<uint8_t>& bytes, unsigned index) {
    return reinterpret_cast<IMAGE_SECTION_HEADER*>(bytes.data() + kSectionTableOffset + index * 40u);
}
void set_directory(std::vector<uint8_t>& bytes, unsigned index, uint32_t rva, uint32_t size) {
    auto& entry = nt_of(bytes)->OptionalHeader.DataDirectory[index];
    entry.VirtualAddress = rva;
    entry.Size = size;
}

// Deterministic section payload: byte i is a function of its offset, so a misplaced or truncated copy shows up.
std::vector<uint8_t> payload(size_t size) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    return bytes;
}

struct Mapped {
    rt::Loader loader;
    rt::Module* m = nullptr;
    explicit Mapped(std::vector<uint8_t> bytes, const fs::path& dir) : loader(dir) {
        m = &loader.load(write_image(bytes));
    }
};

bool expect_rejection(const char* what, const std::vector<uint8_t>& bytes, const char* needle) {
    try {
        rt::Loader loader(g_temp_dir);
        loader.load(write_image(bytes));
        check(false, std::string(what) + " (the image loaded)");
        return false;
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        const bool named = message.find(needle) != std::string::npos;
        if (named) {
            check(true, what);
        } else {
            check(false, what);
            std::printf("     unexpected refusal: %s\n", message.c_str());
        }
        return named;
    } catch (const std::exception& e) {
        check(false, std::string(what) + " (non-runtime_error: " + e.what() + ")");
        return false;
    }
}

bool expect_load(const char* what, const std::vector<uint8_t>& bytes) {
    try {
        Mapped mapped(bytes, g_temp_dir);  // map only: the Loader unmaps at the end of the scope
        check(true, what);
        return true;
    } catch (const std::exception& e) {
        check(false, std::string(what) + " (" + e.what() + ")");
        return false;
    }
}

// -------------------------------------------------------------------------------------------------------------------
// 1. Section copy rule: Windows maps the raw data of every page the virtual extent touches.
// -------------------------------------------------------------------------------------------------------------------

void test_copy_rule() {
    std::printf("\n-- section copy: page-rounded virtual extent (Windows-measured) --\n");
    const auto raw = payload(0x400);
    {
        // VirtualSize 0x123 is one page; SizeOfRawData 0x400. Measured through LoadLibrary on this host: the whole
        // first page is file-backed up to SizeOfRawData (bytes 0x123..0x400 are the file's, not zero) and zero past it.
        ImageSpec spec;
        spec.image_base = next_image_base();
        spec.size_of_image = 0x2000;
        spec.sections.push_back({".text", 0x1000, 0x123, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
                                 raw});
        Mapped mapped(build_image(spec), g_temp_dir);
        const auto& m = *mapped.m;
        check(m.base == reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(spec.image_base)),
              "copy rule: the image maps at its preferred base");
        bool matches = std::memcmp(m.base + 0x1000, raw.data(), raw.size()) == 0;
        if (!matches) {
            for (size_t i = 0; i < raw.size(); ++i)
                if (m.base[0x1000 + i] != raw[i]) {
                    std::printf("     first mismatch at RVA %#zx: mapped %#02x, file %#02x\n", 0x1000 + i,
                                m.base[0x1000 + i], raw[i]);
                    break;
                }
        }
        check(matches, "copy rule: raw data past VirtualSize in the section's page is copied (Windows maps it)");
        bool zero_past_raw = true;
        for (uint32_t rva = 0x1000 + static_cast<uint32_t>(raw.size()); rva < 0x2000; ++rva)
            if (m.base[rva] != 0) zero_past_raw = false;
        check(zero_past_raw, "copy rule: bytes past SizeOfRawData in the page stay zero");
    }
    {
        // Page-aligned VirtualSize (0x1000) with SizeOfRawData 0x1400: only the virtual extent's page is mapped; the
        // extra raw page must not leak into the image (and must not overwrite the next section).
        const auto data = payload(0x200);
        ImageSpec spec;
        spec.image_base = next_image_base();
        spec.size_of_image = 0x3000;
        spec.sections.push_back({".text", 0x1000, 0x1000, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
                                 payload(0x1400)});
        spec.sections.push_back({".data", 0x2000, 0x200, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
                                                      IMAGE_SCN_MEM_WRITE,
                                 data});
        std::vector<uint8_t> bytes = build_image(spec);
        // The second raw block starts at SizeOfHeaders + 0x1400; the builder wrote it there, check the mapping.
        Mapped mapped(bytes, g_temp_dir);
        const auto& m = *mapped.m;
        check(m.base[0x2000] == data[0] && std::memcmp(m.base + 0x2000, data.data(), data.size()) == 0,
              "copy rule: a raw page past VirtualSize does not overwrite the next section");
        check(m.base[0x1ff0] != 0, "copy rule: the page inside VirtualSize is still copied");
    }
    {
        // Windows leaves a section with PointerToRawData 0 zero-filled even when SizeOfRawData is non-zero
        // (measured: LoadLibrary maps zeros there, not the file's MZ header). The old loader copied the header.
        ImageSpec spec;
        spec.image_base = next_image_base();
        spec.size_of_image = 0x2000;
        spec.sections.push_back({".text", 0x1000, 0x200, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
                                 payload(0x200)});
        spec.sections[0].raw_pointer = 0;
        Mapped mapped(build_image(spec), g_temp_dir);
        const auto& m = *mapped.m;
        bool zero = true;
        for (uint32_t rva = 0x1000; rva < 0x1200; ++rva)
            if (m.base[rva] != 0) zero = false;
        check(zero, "copy rule: PointerToRawData 0 means no file data (Windows zero-fills), not a header copy");
    }
}

// -------------------------------------------------------------------------------------------------------------------
// 2. A relocation target in the last page past VirtualSize: Windows has file bytes there, so the fixup is a real VA.
// -------------------------------------------------------------------------------------------------------------------

void test_page_tail_relocation() {
    std::printf("\n-- relocation into the section's page tail --\n");
    const uint64_t preferred = palmetto_test::free_image_base(0x140000000ull);
    ImageSpec spec;
    spec.image_base = preferred;
    spec.size_of_image = 0x3000;
    std::vector<uint8_t> text = payload(0x200);
    // Slot at RVA 0x1100 (past VirtualSize 0x80, inside the mapped page) holds an absolute pointer to RVA 0x1000.
    const uint64_t original = preferred + 0x1000;
    std::memcpy(text.data() + 0x100, &original, sizeof(original));
    spec.sections.push_back({".text", 0x1000, 0x80, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
                             std::move(text)});
    std::vector<uint8_t> reloc_block(sizeof(IMAGE_BASE_RELOCATION) + sizeof(uint16_t), 0);
    auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(reloc_block.data());
    block->VirtualAddress = 0x1000;
    block->SizeOfBlock = static_cast<uint32_t>(reloc_block.size());
    const uint16_t entry = static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) | 0x100);
    std::memcpy(reloc_block.data() + sizeof(IMAGE_BASE_RELOCATION), &entry, sizeof(entry));
    spec.sections.push_back({".reloc", 0x2000, 0x200, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ,
                             std::move(reloc_block)});
    std::vector<uint8_t> bytes = build_image(spec);
    set_directory(bytes, IMAGE_DIRECTORY_ENTRY_BASERELOC, 0x2000, 10);

    // Hold the preferred base so the loader must relocate (and the fixture has a relocation directory to allow it).
    void* hold = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(preferred)), 0x3000,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!hold) {
        check(false, "page-tail relocation: could not hold the preferred base");
        return;
    }
    try {
        Mapped mapped(bytes, g_temp_dir);
        const auto& m = *mapped.m;
        check(m.relocated, "page-tail relocation: the image was relocated away from the held base");
        uint64_t got = 0;
        std::memcpy(&got, m.base + 0x1100, sizeof(got));
        const uint64_t expected = reinterpret_cast<uint64_t>(m.base) + 0x1000;
        if (got != expected)
            std::printf("     slot at RVA 0x1100 = %#llx, expected the new base + 0x1000 = %#llx\n",
                        static_cast<unsigned long long>(got), static_cast<unsigned long long>(expected));
        check(got == expected,
              "page-tail relocation: the pointer in the page tail is fixed up from file bytes, not from zero");
    } catch (const std::exception& e) {
        check(false, std::string("page-tail relocation: load failed: ") + e.what());
    }
    VirtualFree(hold, 0, MEM_RELEASE);
}

// -------------------------------------------------------------------------------------------------------------------
// 3. PAGE_* derivation follows the section's access bits, as the Windows loader does.
// -------------------------------------------------------------------------------------------------------------------

DWORD protection_at(const rt::Module& m, uint32_t rva) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(m.base + rva, &info, sizeof(info))) return 0;
    return info.Protect;
}

void test_protection_derivation() {
    std::printf("\n-- section protection derivation --\n");
    struct Case {
        uint32_t characteristics;
        DWORD expected;
        const char* what;
    };
    const Case cases[] = {
        {IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, PAGE_EXECUTE_READ, "RX -> PAGE_EXECUTE_READ"},
        {IMAGE_SCN_MEM_EXECUTE, PAGE_EXECUTE, "X only -> PAGE_EXECUTE (Windows keeps READ separate)"},
        {IMAGE_SCN_MEM_READ, PAGE_READONLY, "R only -> PAGE_READONLY"},
        {IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, PAGE_READWRITE, "RW -> PAGE_READWRITE"},
        {0, PAGE_NOACCESS, "no access bits -> PAGE_NOACCESS"},
    };
    for (const Case& c : cases) {
        ImageSpec spec;
        spec.image_base = next_image_base();
        spec.size_of_image = 0x3000;
        // A real image needs an executable section for the Windows loader to map it at its ImageBase, which is how the
        // expected PAGE_* values below were measured; the tested characteristics belong to the section at 0x2000.
        spec.sections.push_back({".text", 0x1000, 0x200,
                                 IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, payload(0x200)});
        spec.sections.push_back({".sec", 0x2000, 0x200, c.characteristics, payload(0x200)});
        try {
            Mapped mapped(build_image(spec), g_temp_dir);
            const DWORD got = protection_at(*mapped.m, 0x2000);
            if (got != c.expected) std::printf("     protection %#lx, expected %#lx\n", got, c.expected);
            check(got == c.expected, c.what);
            check(protection_at(*mapped.m, 0x1000) == PAGE_EXECUTE_READ, "a normal RX section stays PAGE_EXECUTE_READ");
            check(protection_at(*mapped.m, 0) == PAGE_READONLY, "headers -> PAGE_READONLY");
        } catch (const std::exception& e) {
            check(false, std::string(c.what) + " (load failed: " + e.what() + ")");
        }
    }
}

// -------------------------------------------------------------------------------------------------------------------
// 4. Alignment validation: the layouts the Windows loader refuses are refused here with named errors.
// -------------------------------------------------------------------------------------------------------------------

std::vector<uint8_t> aligned_image(uint64_t base) {
    ImageSpec spec;
    spec.image_base = base;
    spec.size_of_image = 0x2000;
    spec.sections.push_back({".text", 0x1000, 0x200, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
                             payload(0x200)});
    return build_image(spec);
}

void test_alignment_refusals() {
    std::printf("\n-- alignment validation (Windows refuses these layouts) --\n");
    {
        auto bytes = aligned_image(next_image_base());
        nt_of(bytes)->OptionalHeader.ImageBase += 0x1000;
        expect_rejection("ImageBase not a multiple of 64 KiB is refused", bytes, "ImageBase is not a multiple of 64 KiB");
    }
    {
        auto bytes = aligned_image(next_image_base());
        nt_of(bytes)->OptionalHeader.FileAlignment = 0x100;
        expect_rejection("FileAlignment below 0x200 is refused", bytes, "FileAlignment is not a power of two");
    }
    {
        auto bytes = aligned_image(next_image_base());
        nt_of(bytes)->OptionalHeader.FileAlignment = 0x300;
        expect_rejection("FileAlignment not a power of two is refused", bytes, "FileAlignment is not a power of two");
    }
    {
        auto bytes = aligned_image(next_image_base());
        nt_of(bytes)->OptionalHeader.SectionAlignment = 0x1800;
        expect_rejection("SectionAlignment not a power of two is refused", bytes,
                         "SectionAlignment is not a power of two");
    }
    {
        auto bytes = aligned_image(next_image_base());
        nt_of(bytes)->OptionalHeader.SectionAlignment = 0x100;
        expect_rejection("SectionAlignment smaller than FileAlignment is refused", bytes,
                         "SectionAlignment is not a power of two");
    }
    {
        auto bytes = aligned_image(next_image_base());
        section_of(bytes, 0)->VirtualAddress = 0x1800;
        expect_rejection("a section not aligned to SectionAlignment is refused", bytes,
                         "is not aligned to SectionAlignment");
    }
}

// -------------------------------------------------------------------------------------------------------------------
// 5. Sections sharing a page with different protections: page protection cannot represent both.
// -------------------------------------------------------------------------------------------------------------------

void test_shared_page_conflict() {
    std::printf("\n-- sub-page sections sharing a page with different protections --\n");
    ImageSpec spec;
    spec.image_base = next_image_base();
    spec.section_alignment = 0x200;
    spec.file_alignment = 0x200;
    spec.size_of_headers = 0x200;
    spec.size_of_image = 0x800;
    spec.sections.push_back({".text", 0x200, 0x200, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
                             payload(0x200)});
    spec.sections.push_back({".data", 0x400, 0x200, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
                                                  IMAGE_SCN_MEM_WRITE,
                             payload(0x200)});
    expect_rejection("sections sharing a page with different protections are refused", build_image(spec),
                     "share a page with different protections");

    // The same layout with one section still loads: a single section in the header page is representable.
    ImageSpec single;
    single.image_base = next_image_base();
    single.section_alignment = 0x200;
    single.file_alignment = 0x200;
    single.size_of_headers = 0x200;
    single.size_of_image = 0x400;
    single.sections.push_back({".text", 0x200, 0x200,
                               IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, payload(0x200)});
    expect_load("a single sub-page section still loads (existing loader tests use this layout)", build_image(single));
}

// -------------------------------------------------------------------------------------------------------------------
// 6. The Windows-accepted layouts must stay accepted: the audit must not over-validate.
// -------------------------------------------------------------------------------------------------------------------

void test_windows_acceptances_kept() {
    std::printf("\n-- layouts Windows accepts are still accepted --\n");
    {
        // Measured: LoadLibrary accepts SizeOfImage not a multiple of SectionAlignment.
        ImageSpec spec;
        spec.image_base = next_image_base();
        spec.size_of_image = 0x1800;
        spec.sections.push_back({".text", 0x1000, 0x200,
                                 IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, payload(0x200)});
        expect_load("SizeOfImage not a multiple of SectionAlignment still loads", build_image(spec));
    }
    {
        // Measured: LoadLibrary accepts SizeOfHeaders not a multiple of FileAlignment.
        ImageSpec spec;
        spec.image_base = next_image_base();
        spec.size_of_image = 0x2000;
        spec.size_of_headers = 0x300;
        spec.sections.push_back({".text", 0x1000, 0x200,
                                 IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, payload(0x200)});
        expect_load("SizeOfHeaders not a multiple of FileAlignment still loads", build_image(spec));
    }
    {
        // The old loader's synthetic fixtures leave both alignment fields zero; that compatibility path stays.
        ImageSpec spec;
        spec.image_base = next_image_base();
        spec.section_alignment = 0;
        spec.file_alignment = 0;
        spec.size_of_headers = 0x400;
        spec.size_of_image = 0x2000;
        spec.sections.push_back({".text", 0x1000, 0x200,
                                 IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, payload(0x200)});
        expect_load("a zero SectionAlignment/FileAlignment fixture still loads", build_image(spec));
    }
}

// -------------------------------------------------------------------------------------------------------------------
// 7. Base-reservation commit path (base_reservation.h): commit inside the inherited reservation, fall back otherwise.
// -------------------------------------------------------------------------------------------------------------------

void test_base_reservation_path() {
    std::printf("\n-- base reservation/commit path --\n");
    const uint64_t base = next_image_base();
    constexpr uint64_t kReservedSize = 0x10000;
    ImageSpec spec;
    spec.image_base = base;
    spec.size_of_image = 0x2000;
    spec.sections.push_back({".text", 0x1000, 0x200,
                             IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, payload(0x200)});
    const auto bytes = build_image(spec);

    void* reservation = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(base)), kReservedSize, MEM_RESERVE,
                                     PAGE_NOACCESS);
    if (!reservation) {
        check(false, "base reservation: could not reserve the fixture range");
        return;
    }
    {
        wchar_t marker[64] = {};
        std::swprintf(marker, 64, L"%llx:%llx", static_cast<unsigned long long>(base),
                      static_cast<unsigned long long>(kReservedSize));
        SetEnvironmentVariableW(L"PALMETTO_BASE_RESERVED", marker);
        try {
            Mapped mapped(bytes, g_temp_dir);
            const auto& m = *mapped.m;
            check(m.base == reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(base)),
                  "base reservation: the image is committed inside the inherited reservation");
            check(!m.relocated, "base reservation: no relocation is applied");
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(m.base, &info, sizeof(info));
            check(info.State == MEM_COMMIT, "base reservation: the mapped range is committed");
        } catch (const std::exception& e) {
            check(false, std::string("base reservation: load failed: ") + e.what());
        }
        SetEnvironmentVariableW(L"PALMETTO_BASE_RESERVED", nullptr);
    }
    {
        // Without the inherited marker a relocation-less image whose preferred range is already reserved must be
        // refused, not double-mapped: the old loader reports the preferred base as taken.
        void* hold = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(base)), kReservedSize,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (hold) {
            expect_rejection("base reservation: a taken preferred base without relocations is refused", bytes,
                             "preferred base is taken and the image has no relocations");
            VirtualFree(hold, 0, MEM_RELEASE);
        }
    }
    VirtualFree(reservation, 0, MEM_RELEASE);
}

// -------------------------------------------------------------------------------------------------------------------
// 8. A normal two-section image: headers, sections and protections stay as the loader's own tests expect.
// -------------------------------------------------------------------------------------------------------------------

void test_normal_image() {
    std::printf("\n-- normal two-section image --\n");
    const auto text = payload(0x200);
    const auto data = payload(0x400);
    ImageSpec spec;
    spec.image_base = next_image_base();
    spec.size_of_image = 0x3000;
    spec.entry_point = 0x1000;
    spec.sections.push_back({".text", 0x1000, 0x200, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
                             text});
    spec.sections.push_back({".data", 0x2000, 0x400, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
                                                      IMAGE_SCN_MEM_WRITE,
                             data});
    try {
        Mapped mapped(build_image(spec), g_temp_dir);
        const auto& m = *mapped.m;
        check(m.size == 0x3000, "normal image: SizeOfImage is kept");
        check(m.entry_rva == 0x1000, "normal image: the entry RVA is kept");
        check(std::memcmp(m.base + 0x1000, text.data(), text.size()) == 0, "normal image: .text matches the file");
        check(std::memcmp(m.base + 0x2000, data.data(), data.size()) == 0, "normal image: .data matches the file");
        check(protection_at(m, 0x1000) == PAGE_EXECUTE_READ, "normal image: .text is PAGE_EXECUTE_READ");
        check(protection_at(m, 0x2000) == PAGE_READWRITE, "normal image: .data is PAGE_READWRITE");
        check(protection_at(m, 0) == PAGE_READONLY, "normal image: headers are PAGE_READONLY");
        check(m.base[0] == 'M' && m.base[1] == 'Z', "normal image: the mapped headers hold the file's DOS signature");
    } catch (const std::exception& e) {
        check(false, std::string("normal image: load failed: ") + e.what());
    }
}

}  // namespace

int main() {
    std::printf("loader_core_audit_test: inherited PE loader core-map audit (synthetic images only)\n");
    g_temp_dir = std::filesystem::temp_directory_path() /
                 ("palmetto_loader_core_audit_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(g_temp_dir, ec);

    try {
        test_copy_rule();
        test_page_tail_relocation();
        test_protection_derivation();
        test_alignment_refusals();
        test_shared_page_conflict();
        test_windows_acceptances_kept();
        test_base_reservation_path();
        test_normal_image();
    } catch (const std::exception& e) {
        std::printf("FATAL: unhandled exception: %s\n", e.what());
        ++g_failures;
    }

    std::filesystem::remove_all(g_temp_dir, ec);
    std::printf("\nloader_core_audit_test: %d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
