// Synthetic fixtures for the PE-image parsing core (docs/pe_loader_rewrite.md). No game data: every header, section and
// table below is built by this test, so the shapes are the test's own and the real images are never needed.
#include "pe_image.h"
#include "pe_image_fixture.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
int refusal_shapes = 0;
int positive_edge_shapes = 0;

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
    ++refusal_shapes;
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

using pe_image_fixture::Builder;

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
        b.u32(b.file_of(0x2000) + 24, 0);  // NumberOfNames must also be zero
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check(image.exports().empty(), "zero-function export directory is empty");
    }
    {
        auto b = Builder();
        b.u32(Builder::kOptional + 112 + 1 * 8, 0);  // import directory rva 0
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check(image.imports().empty(), "absent import directory is empty");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2000) + 20, 0);  // names cannot exist without function slots
        check_error(b.bytes, "export name table has no function table", "export names without functions");
    }
    {
        auto b = Builder();
        b.directory(1, 0x2100, 20);  // only the non-null descriptor fits; no terminator is declared
        check_error(b.bytes, "not terminated inside its declared size", "import directory terminator past its size");
    }
    {
        auto b = Builder();
        b.directory(0, 0x2000, 0xA1);  // the forwarder starts at 0x20A0, but its NUL is outside the directory
        check_error(b.bytes, "not terminated inside the export directory", "forwarder past export-directory end");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2050), 0x11FF);  // string at the last .text byte must not continue into .rdata
        b.bytes[b.file_of(0x11FF)] = 'X';
        b.bytes[b.file_of(0x2000)] = 'Y';
        check_error(b.bytes, "not terminated in its file-backed range", "export name crosses a section boundary");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2200), 0x3000);  // page-aligned target at the final image byte
        b.u16(b.file_of(0x2208), static_cast<std::uint16_t>((10u << 12) | 0xFFFu));
        check_error(b.bytes, "relocation target runs outside the image", "DIR64 target crosses image end");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2200), 0xFFFFF801u);  // old 32-bit page+offset arithmetic could wrap into the headers
        b.u16(b.file_of(0x2208), static_cast<std::uint16_t>((10u << 12) | 0xFFFu));
        check_error(b.bytes, "page RVA is not 4-KiB aligned", "relocation page/target wrap");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2000) + 16, 0xFFFFFFFFu);  // the second name has ordinal index 1
        check_error(b.bytes, "export ordinal exceeds the 32-bit range", "export ordinal overflow");
    }
    {
        auto b = Builder();
        b.u32(Builder::kSections + 16, 0);  // virtual-only .text still occupies [0x1000, 0x1200)
        b.u32(Builder::kSections + 20, 0);
        b.u32(Builder::kSections + 40 + 12, 0x1100);  // .rdata intersects the virtual-only section
        check_error(b.bytes, "overlap in the image", "virtual-only section overlap");
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2100) + 16, 0);  // lookup exists, but FirstThunk/IAT is absent
        check_error(b.bytes, "no import address table", "import descriptor without IAT");
    }
}

void test_positive_edge_shapes() {
    {
        auto b = Builder();
        b.u32(Builder::kSections + 16, 0);  // .text has a virtual span but no file-backed bytes
        b.u32(Builder::kSections + 20, 0xFFFFFFFFu);  // PointerToRawData is ignored when SizeOfRawData is zero
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check_equal(image.sections()[0].mapped_raw_size(), 0, "zero-raw section has no copied bytes");
        check(image.section_data(image.sections()[0]).empty(), "zero-raw section data is empty");
        check(image.sections()[0].contains(0x1100, 1), "zero-raw section still contains its virtual range");
        ++positive_edge_shapes;
    }
    {
        auto b = Builder();
        b.u16(Builder::kNt + 6, 0);  // header-only PE, no section headers
        b.u32(Builder::kOptional + 16, 0);  // no entry point
        b.u32(Builder::kOptional + 56, 0x400);  // image consists only of its headers
        for (unsigned i = 0; i < rt::pe::Image::kDirectoryCount; ++i) b.directory(i, 0, 0);
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check(image.sections().empty(), "header-only image has no sections");
        check(!image.directory(rt::pe::Image::kDirectoryExport).present(), "header-only image has no exports");
        check(image.exports().empty() && image.imports().empty(), "header-only image has no import/export symbols");
        check_equal(image.size_of_image(), 0x400, "header-only image size equals its headers");
        ++positive_edge_shapes;
    }
    {
        auto b = Builder();
        b.u16(b.file_of(0x2062), 0);  // both Alpha and Alias name function-table slot zero
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check_equal(image.exports().size(), 3, "two aliases plus one ordinal-only forwarder");
        check_string(image.exports()[0].name, "Alpha", "first alias name");
        check_string(image.exports()[1].name, "Alias", "second alias name");
        check_equal(image.exports()[0].rva, 0x1000, "first alias target");
        check_equal(image.exports()[1].rva, 0x1000, "second alias shares its target RVA");
        check_equal(image.exports()[0].ordinal, image.exports()[1].ordinal, "aliases share one ordinal");
        check(image.exports()[2].name.empty(), "forwarder is retained as an ordinal-only export");
        check_string(image.exports()[2].forwarder, "other.Symbol", "forwarder string inside export directory");
        ++positive_edge_shapes;
    }
    {
        auto b = Builder();
        b.u32(b.file_of(0x2040), 0);  // Alpha's name points at a zero EAT slot
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check_equal(image.exports().size(), 1, "only the non-hole named export is emitted");
        check_string(image.exports()[0].name, "Alias", "non-hole export remains available");
        check_string(image.exports()[0].forwarder, "other.Symbol", "named hole is skipped like the legacy loader");
        ++positive_edge_shapes;
    }
    {
        auto b = Builder();
        b.u64(b.file_of(0x2310), 0);  // no index slot
        b.u64(b.file_of(0x2318), 0);  // no callback table
        b.u32(b.file_of(0x2320), 0);  // no zero-fill tail
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check(image.tls().present, "template-only TLS directory is present");
        check_equal(image.tls().template_rva, 0x2700, "template-only TLS RVA");
        check_equal(image.tls().template_size, 8, "template-only TLS size");
        check_equal(image.tls().index_rva, 0, "template-only TLS has no index");
        check_equal(image.tls().callbacks_rva, 0, "template-only TLS has no callback table");
        check(image.tls().callback_rvas.empty(), "template-only TLS has zero callbacks");
        ++positive_edge_shapes;
    }
    {
        auto b = Builder();
        b.directory(rt::pe::Image::kDirectoryException, 0x2400, 0);  // zero size means absent
        auto image = rt::pe::Image::parse(b.bytes, "synthetic.dll");
        check(!image.directory(rt::pe::Image::kDirectoryException).present(), "zero-sized exception directory is absent");
        check(!image.exceptions().present && image.exceptions().size == 0, "zero-sized exceptions stay absent");
        ++positive_edge_shapes;
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
    try {
        test_positive_edge_shapes();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: positive edge-shape test threw: %s\n", e.what());
        ++failures;
    }
    if (failures) {
        std::fprintf(stderr, "pe_image_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("pe_image_test: OK (valid synthetic image, %d refusal shapes, %d positive edge shapes)\n",
                refusal_shapes, positive_edge_shapes);
    return 0;
}
