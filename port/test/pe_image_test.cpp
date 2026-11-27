// Synthetic fixtures for the PE-image parsing core (docs/pe_loader_rewrite.md). No game data: every header, section and
// table below is built by this test, so the shapes are the test's own and the real images are never needed.
#include "pe_image.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void check_equal(std::uint64_t got, std::uint64_t want, const char* what) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL: %s (got %llu, want %llu)\n", what, static_cast<unsigned long long>(got),
                 static_cast<unsigned long long>(want));
    ++failures;
}

void check_string(const std::string& got, const std::string& want, const char* what) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL: %s (got '%s', want '%s')\n", what, got.c_str(), want.c_str());
    ++failures;
}

// A malformed image must be refused, and the refusal must name the image and the specific shape.
void check_error(const std::vector<std::uint8_t>& bytes, const char* fragment, const char* what) {
    try {
        (void)rt::pe::Image::parse(bytes, "synthetic.dll");
    } catch (const rt::pe::Error& e) {
        if (std::strstr(e.what(), "synthetic.dll: ") != e.what() || !std::strstr(e.what(), fragment)) {
            std::fprintf(stderr, "FAIL: %s (unexpected message: %s)\n", what, e.what());
            ++failures;
        }
        return;
    }
    std::fprintf(stderr, "FAIL: %s (parsed instead of refusing)\n", what);
    ++failures;
}

struct Builder {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x1600, 0);
    static constexpr std::uint64_t kImageBase = 0x140000000ull;
    static constexpr std::uint32_t kNt = 0x80;
    static constexpr std::uint32_t kOptional = 0x98;
    static constexpr std::uint32_t kSections = 0x188;

    void u16(std::uint32_t off, std::uint16_t v) { std::memcpy(bytes.data() + off, &v, sizeof(v)); }
    void u32(std::uint32_t off, std::uint32_t v) { std::memcpy(bytes.data() + off, &v, sizeof(v)); }
    void u64(std::uint32_t off, std::uint64_t v) { std::memcpy(bytes.data() + off, &v, sizeof(v)); }
    void text(std::uint32_t off, const char* s) { std::memcpy(bytes.data() + off, s, std::strlen(s) + 1); }
    std::uint32_t file_of(std::uint32_t rva) const { return rva < 0x1000 ? rva : rva - 0x2000 + 0x600; }

    Builder() {
        // DOS header.
        u16(0, 0x5A4D);
        u32(0x3C, kNt);
        // NT headers and file header.
        u32(kNt, 0x00004550);
        u16(kNt + 4, 0x8664);   // AMD64
        u16(kNt + 6, 2);        // sections
        u16(kNt + 20, 0xF0);    // SizeOfOptionalHeader
        u16(kNt + 22, 0x0022);  // executable, large address aware
        // PE32+ optional header.
        u16(kOptional, 0x020B);
        u32(kOptional + 16, 0x1000);  // entry point
        u64(kOptional + 24, kImageBase);
        u32(kOptional + 32, 0x1000);  // SectionAlignment
        u32(kOptional + 36, 0x200);   // FileAlignment
        u32(kOptional + 56, 0x4000);  // SizeOfImage
        u32(kOptional + 60, 0x400);   // SizeOfHeaders
        u16(kOptional + 68, 3);       // console
        u32(kOptional + 108, 16);     // NumberOfRvaAndSizes
        // Section table.
        write_section(0, ".text", 0x1000, 0x200, 0x400, 0x200, 0x60000020);
        write_section(1, ".rdata", 0x2000, 0x1000, 0x600, 0x1000, 0x40000040);
        // Export directory at 0x2000: three functions, two names (an alias), one forwarder.
        directory(0, 0x2000, 0x100);
        u32(file_of(0x2000) + 12, 0x2070);  // Name
        u32(file_of(0x2000) + 16, 5);       // Base ordinal
        u32(file_of(0x2000) + 20, 3);       // NumberOfFunctions
        u32(file_of(0x2000) + 24, 2);       // NumberOfNames
        u32(file_of(0x2000) + 28, 0x2040);  // AddressOfFunctions
        u32(file_of(0x2000) + 32, 0x2050);  // AddressOfNames
        u32(file_of(0x2000) + 36, 0x2060);  // AddressOfNameOrdinals
        u32(file_of(0x2040), 0x1000);
        u32(file_of(0x2044), 0x20A0);  // forwarder target
        u32(file_of(0x2048), 0);
        u32(file_of(0x2050), 0x2090);  // "Alpha"
        u32(file_of(0x2054), 0x2096);  // "Alias"
        u16(file_of(0x2060), 0);
        u16(file_of(0x2062), 1);
        text(file_of(0x2080), "synthetic.dll");
        text(file_of(0x2090), "Alpha");
        text(file_of(0x2096), "Alias");
        text(file_of(0x20A0), "other.Symbol");
        // Import directory at 0x2100: one descriptor, one by-name import, one by-ordinal import.
        directory(1, 0x2100, 0x100);
        u32(file_of(0x2100), 0x2140);   // OriginalFirstThunk
        u32(file_of(0x2104), 0);        // TimeDateStamp
        u32(file_of(0x2108), 0);        // ForwarderChain
        u32(file_of(0x210C), 0x2180);   // Name
        u32(file_of(0x2110), 0x21A0);   // FirstThunk
        u64(file_of(0x2140), 0x2160);
        u64(file_of(0x2148), 0x8000000000000002ull);
        u64(file_of(0x2150), 0);
        u16(file_of(0x2160), 0);
        text(file_of(0x2162), "FuncA");
        text(file_of(0x2180), "Synthetic.dll");
        u64(file_of(0x21A0), 0x2160);
        u64(file_of(0x21A8), 0x8000000000000002ull);
        u64(file_of(0x21B0), 0);
        // Base relocations at 0x2200: one DIR64 entry and one absolute filler.
        directory(5, 0x2200, 12);
        u32(file_of(0x2200), 0x1000);
        u32(file_of(0x2204), 12);
        u16(file_of(0x2208), static_cast<std::uint16_t>((10u << 12) | 0x10));
        u16(file_of(0x220A), 0);
        // TLS directory at 0x2300: template at 0x2700, index at 0x2500, callbacks at 0x2520.
        directory(9, 0x2300, 40);
        u64(file_of(0x2300), kImageBase + 0x2700);
        u64(file_of(0x2308), kImageBase + 0x2708);
        u64(file_of(0x2310), kImageBase + 0x2500);
        u64(file_of(0x2318), kImageBase + 0x2520);
        u32(file_of(0x2320), 4);
        u64(file_of(0x2520), kImageBase + 0x1000);
        u64(file_of(0x2528), 0);
        // Exception directory at 0x2400: two RUNTIME_FUNCTION entries.
        directory(3, 0x2400, 24);
    }

    void write_section(unsigned index, const char* name, std::uint32_t va, std::uint32_t vsize, std::uint32_t raw_ptr,
                       std::uint32_t raw_size, std::uint32_t characteristics) {
        const std::uint32_t off = kSections + index * 40;
        std::memcpy(bytes.data() + off, name, std::strlen(name));
        u32(off + 8, vsize);
        u32(off + 12, va);
        u32(off + 16, raw_size);
        u32(off + 20, raw_ptr);
        u32(off + 36, characteristics);
    }

    void directory(unsigned index, std::uint32_t rva, std::uint32_t size) {
        u32(kOptional + 112 + index * 8, rva);
        u32(kOptional + 112 + index * 8 + 4, size);
    }
};

void test_valid_image() {
    const Builder b;
    auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
    check_string(image.name(), "synthetic.dll", "image name");
    check_equal(image.image_base(), Builder::kImageBase, "image base");
    check_equal(image.size_of_image(), 0x4000, "SizeOfImage");
    check_equal(image.size_of_headers(), 0x400, "SizeOfHeaders");
    check_equal(image.entry_point(), 0x1000, "entry point");
    check_equal(image.subsystem(), 3, "subsystem");
    check(!image.is_dll(), "characteristics do not mark a DLL");
    check_equal(image.sections().size(), 2, "section count");
    check_string(image.sections()[0].name, ".text", "first section name");
    check_string(image.sections()[1].name, ".rdata", "second section name");
    check(image.sections()[0].executable(), "text is executable");
    check(image.sections()[1].readable() && !image.sections()[1].writable(), "rdata is read-only");

    check(image.directory(rt::pe::Image::kDirectoryExport).present(), "export directory is present");
    check(image.directory(rt::pe::Image::kDirectoryImport).present(), "import directory is present");
    check(image.directory(rt::pe::Image::kDirectoryBaseReloc).present(), "relocation directory is present");
    check(image.directory(rt::pe::Image::kDirectoryTls).present(), "TLS directory is present");
    check(image.directory(rt::pe::Image::kDirectoryException).present(), "exception directory is present");

    const auto& exports = image.exports();
    check_equal(exports.size(), 2, "export symbol count");
    check_string(exports[0].name, "Alpha", "first export name");
    check_equal(exports[0].ordinal, 5, "first export ordinal");
    check_equal(exports[0].rva, 0x1000, "first export rva");
    check(!exports[0].is_forwarder(), "first export is not a forwarder");
    check_string(exports[1].name, "Alias", "alias export name");
    check_equal(exports[1].ordinal, 6, "alias export ordinal");
    check_string(exports[1].forwarder, "other.Symbol", "alias export forwarder");

    const auto& imports = image.imports();
    check_equal(imports.size(), 1, "import library count");
    check_string(imports[0].name, "Synthetic.dll", "import library name");
    check_equal(imports[0].symbols.size(), 2, "import symbol count");
    check_string(imports[0].symbols[0].name, "FuncA", "first import name");
    check(!imports[0].symbols[0].by_ordinal, "first import is by name");
    check_equal(imports[0].symbols[0].iat_rva, 0x21A0, "first import IAT rva");
    check(imports[0].symbols[1].by_ordinal, "second import is by ordinal");
    check_equal(imports[0].symbols[1].ordinal, 2, "second import ordinal");
    check_equal(imports[0].symbols[1].iat_rva, 0x21A8, "second import IAT rva");

    check_equal(image.relocations().size(), 2, "relocation count");
    check_equal(image.relocations()[0].type, rt::pe::Relocation::kDir64, "DIR64 relocation type");
    check_equal(image.relocations()[0].rva, 0x1010, "DIR64 relocation rva");
    check_equal(image.relocations()[1].type, rt::pe::Relocation::kAbsolute, "ABSOLUTE relocation type");

    check(image.tls().present, "TLS is present");
    check_equal(image.tls().template_rva, 0x2700, "TLS template rva");
    check_equal(image.tls().template_size, 8, "TLS template size");
    check_equal(image.tls().zero_fill, 4, "TLS zero fill");
    check_equal(image.tls().index_rva, 0x2500, "TLS index rva");
    check_equal(image.tls().callback_rvas.size(), 1, "TLS callback count");
    check_equal(image.tls().callback_rvas[0], 0x1000, "TLS callback rva");

    check(image.exceptions().present, "exceptions are present");
    check_equal(image.exceptions().entry_count, 2, "exception entry count");
    check_string(image.read_string(0x2080, "a string"), "synthetic.dll", "read_string from .rdata");

    const auto text = image.section_data(image.sections()[0]);
    check_equal(text.size(), 0x200, "text section data size");
    const auto rdata = image.section_data(image.sections()[1]);
    check_equal(rdata.size(), 0x1000, "rdata section data size");
    check(image.section_for(0x1000) == &image.sections()[0], "section_for text");
    check(image.section_for(0x2500) == &image.sections()[1], "section_for rdata");
    check(image.section_for(0x400) == nullptr, "headers have no section");
}

void test_refusals() {
    {
        auto b = Builder();
        b.u16(0, 0);
        check_error(b.bytes, "not an MZ image", "bad DOS signature");
    }
    {
        auto b = Builder();
        b.u32(0x3C, 0xFFFFFFF0u);
        check_error(b.bytes, "NT headers are outside the file", "NT header outside the file");
    }
    {
        auto b = Builder();
        b.u16(Builder::kNt + 4, 0x14C);
        check_error(b.bytes, "not an x86-64 PE", "wrong machine");
    }
    {
        auto b = Builder();
        b.u16(Builder::kOptional, 0x010B);
        check_error(b.bytes, "not a PE32+ image", "PE32 magic");
    }
    {
        auto b = Builder();
        b.u32(Builder::kOptional + 56, 0);
        check_error(b.bytes, "SizeOfImage is zero", "zero SizeOfImage");
    }
    {
        auto b = Builder();
        b.u32(Builder::kOptional + 16, 0x4000);
        check_error(b.bytes, "entry point is outside the image", "entry point outside");
    }
    {
        auto b = Builder();
        b.u32(Builder::kOptional + 60, 0x100);
        check_error(b.bytes, "SizeOfHeaders does not cover", "SizeOfHeaders too small");
    }
    {
        auto b = Builder();
        b.u32(Builder::kSections + 16, 0x100000);  // .text raw size
        check_error(b.bytes, ".text reads past the end of the file", "section raw past EOF");
    }
    {
        auto b = Builder();
        b.u32(Builder::kSections + 12, 0x100);  // .text VA inside headers
        check_error(b.bytes, "text overlaps the headers", "section overlaps headers");
    }
    {
        auto b = Builder();
        b.u32(Builder::kSections + 40 + 12, 0x1100);  // .rdata VA inside .text
        check_error(b.bytes, "overlap in the image", "sections overlap");
    }
    {
        auto b = Builder();
        b.u32(Builder::kSections + 40 + 12, 0x3F00);  // .rdata VA near the end
        check_error(b.bytes, ".rdata runs outside the image", "section outside image");
    }
    {
        auto b = Builder();
        b.u16(b.file_of(0x2060), 9);
        check_error(b.bytes, "export name points past the function table", "export ordinal out of range");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2040), 0x4100);  // export function outside the image
        check_error(b.bytes, "export function runs outside the image", "export function outside image");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2140), 0x4100);  // import by-name rva outside the image
        check_error(b.bytes, "import-by-name entry runs outside the image", "import name outside image");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2100), 0x4100);  // thunk table outside the image
        check_error(b.bytes, "import thunk table runs outside the image", "thunk table outside image");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2204), 7);  // odd/short relocation block
        check_error(b.bytes, "relocation block has an invalid size", "bad relocation block");
    }
    {
        auto b = Builder();
        b.u64(b.file_of(0x2308), Builder::kImageBase + 0x2600);  // end before start
        check_error(b.bytes, "TLS template ends before it starts", "TLS end before start");
    }
    {
        auto b = Builder();
        b.u64(b.file_of(0x2318), Builder::kImageBase + 0x4000);  // callback table outside image
        check_error(b.bytes, "TLS callback table is outside the image", "TLS callback table outside");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2300) + 0, 0);  // clear TLS directory (requires rewriting the VA fields)
        b.u64(b.file_of(0x2300), 0);
        b.u64(b.file_of(0x2308), 0);
        b.u64(b.file_of(0x2310), 0);
        b.u64(b.file_of(0x2318), 0);
        b.u32(b.file_of(0x2320), 0);
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check(!image.tls().present || image.tls().template_size == 0, "zeroed TLS directory parses as empty");
    }
    {
        auto b = Builder();
        b.u32(Builder::kOptional + 112 + 3 * 8, 0x3FF0);  // exception directory outside image
        check_error(b.bytes, "data directory 3 runs outside the image", "directory outside image");
    }
    {
        auto b = Builder();
        b.u32(Builder::kOptional + 112 + 3 * 8 + 4, 13);  // exception size not a multiple of 12
        check_error(b.bytes, "exception directory is not a whole number", "odd exception size");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2000) + 20, 0);  // NumberOfFunctions 0 leaves no symbols
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check(image.exports().empty(), "zero-function export directory is empty");
    }
    {
        auto b = Builder();
        b.u32(Builder::kOptional + 112 + 1 * 8, 0);  // import directory rva 0
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check(image.imports().empty(), "absent import directory is empty");
    }
}

}  // namespace

int main() {
    try {
        test_valid_image();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: valid image test threw: %s\n", e.what());
        ++failures;
    }
    try {
        test_refusals();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: refusal test threw: %s\n", e.what());
        ++failures;
    }
    if (failures) {
        std::fprintf(stderr, "pe_image_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("pe_image_test: OK (valid synthetic image and %d refusal shapes)\n", 18);
    return 0;
}
