// Pure shadow-comparison tests over a tiny hand-built PE32+ image. No game data, OS loader, or mapped code is used.
#include "pe_loader_shadow.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using rt::pe_loader_shadow::DiagnosticCode;
using rt::pe_loader_shadow::OldLoaderFacts;
using rt::pe_loader_shadow::Result;
using rt::pe_loader_shadow::SectionFacts;

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

bool has_code(const Result& result, DiagnosticCode code) {
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.code == code) return true;
    return false;
}

void check_code(const Result& result, DiagnosticCode code, const char* what) {
    if (has_code(result, code)) return;
    std::fprintf(stderr, "FAIL: %s (diagnostic %.*s missing)\n", what,
                 static_cast<int>(rt::pe_loader_shadow::diagnostic_name(code).size()),
                 rt::pe_loader_shadow::diagnostic_name(code).data());
    ++failures;
}

struct Builder {
    static constexpr std::uint64_t kImageBase = 0x140000000ull;
    static constexpr std::uint32_t kNt = 0x80;
    static constexpr std::uint32_t kOptional = 0x98;
    static constexpr std::uint32_t kSections = 0x188;
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x800, 0);

    void u16(std::uint32_t offset, std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    }

    void u32(std::uint32_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    }

    void u64(std::uint32_t offset, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    }

    void write_section(unsigned index, const char* name, std::uint32_t virtual_address, std::uint32_t virtual_size,
                       std::uint32_t raw_pointer, std::uint32_t raw_size, std::uint32_t characteristics) {
        const std::uint32_t offset = kSections + index * 40;
        const std::size_t name_size = std::min<std::size_t>(std::strlen(name), 8);
        std::memcpy(bytes.data() + offset, name, name_size);
        u32(offset + 8, virtual_size);
        u32(offset + 12, virtual_address);
        u32(offset + 16, raw_size);
        u32(offset + 20, raw_pointer);
        u32(offset + 36, characteristics);
    }

    void directory(unsigned index, std::uint32_t rva, std::uint32_t size) {
        u32(kOptional + 112 + index * 8, rva);
        u32(kOptional + 112 + index * 8 + 4, size);
    }

    static std::uint32_t file_offset(std::uint32_t rva) {
        return rva < 0x1000 ? rva : rva - 0x2000 + 0x600;
    }

    Builder() {
        u16(0, 0x5A4D);               // MZ
        u32(0x3C, kNt);                // e_lfanew
        u32(kNt, 0x00004550);         // PE\0\0
        u16(kNt + 4, 0x8664);         // AMD64
        u16(kNt + 6, 2);              // section count
        u16(kNt + 20, 0xF0);          // SizeOfOptionalHeader
        u16(kNt + 22, 0x0022);        // executable, large address aware
        u16(kOptional, 0x020B);       // PE32+
        u32(kOptional + 16, 0x1000);  // entry point
        u64(kOptional + 24, kImageBase);
        u32(kOptional + 56, 0x4000);  // SizeOfImage
        u32(kOptional + 60, 0x400);   // SizeOfHeaders
        u32(kOptional + 108, 16);     // NumberOfRvaAndSizes
        write_section(0, ".text", 0x1000, 0x200, 0x400, 0x200, 0x60000020);
        write_section(1, ".rdata", 0x2000, 0x200, 0x600, 0x200, 0x40000040);
        directory(2, 0x2020, 0x10);   // In-image resource-directory shape; not interpreted by Image.
    }
};

OldLoaderFacts expected_facts() {
    OldLoaderFacts facts;
    facts.image_base = Builder::kImageBase;
    facts.size_of_image = 0x4000;
    facts.entry_point = 0x1000;
    facts.sections = {
        SectionFacts{".text", 0x1000, 0x200, 0x400, 0x200, 0x60000020},
        SectionFacts{".rdata", 0x2000, 0x200, 0x600, 0x200, 0x40000040},
    };
    facts.directories[2] = {0x2020, 0x10};
    return facts;
}

Result compare(const Builder& builder, const OldLoaderFacts& facts, std::string name = "synthetic.dll") {
    return rt::pe_loader_shadow::compare(
        std::span<const std::uint8_t>(builder.bytes.data(), builder.bytes.size()), facts, std::move(name));
}

void test_switch_values() {
    using rt::pe_loader_shadow::switches_from_values;
    const auto off = switches_from_values("", "");
    check(!off.enabled && !off.strict, "empty switch values leave shadow parsing disabled");
    const auto parse_only = switches_from_values("1", "");
    check(parse_only.enabled && !parse_only.strict, "parse switch enables non-strict shadow mode");
    const auto strict = switches_from_values("1", "1");
    check(strict.enabled && strict.strict, "strict requires and follows the parse switch");
    const auto strict_only = switches_from_values("", "1");
    check(!strict_only.enabled && !strict_only.strict, "strict-only is a no-op");
    const auto truthy_word = switches_from_values("true", "1");
    check(!truthy_word.enabled && !truthy_word.strict, "only the exact value 1 is accepted");
}

void test_match_and_field_mismatches() {
    const Builder builder;
    const OldLoaderFacts facts = expected_facts();
    const Result matched = compare(builder, facts);
    check(matched.parse_succeeded, "valid synthetic image parses");
    check(matched.matches, "matching image and old-loader facts match");
    check(matched.diagnostics.empty(), "match has no diagnostics");

    auto changed = facts;
    changed.image_base += 0x1000;
    const Result base = compare(builder, changed);
    check(!base.matches, "different image base mismatches");
    check_code(base, DiagnosticCode::image_base_mismatch, "image-base mismatch is named");

    changed = facts;
    ++changed.size_of_image;
    const Result size = compare(builder, changed);
    check_code(size, DiagnosticCode::size_of_image_mismatch, "image-size mismatch is named");

    changed = facts;
    ++changed.entry_point;
    const Result entry = compare(builder, changed);
    check_code(entry, DiagnosticCode::entry_point_mismatch, "entry-point mismatch is named");

    changed = facts;
    changed.sections[0].raw_pointer += 1;
    const Result section = compare(builder, changed);
    check_code(section, DiagnosticCode::section_mismatch, "section-field mismatch is named");

    changed = facts;
    changed.sections[0].name = ".code";
    const Result section_name = compare(builder, changed);
    check_code(section_name, DiagnosticCode::section_mismatch, "section-name mismatch is named");

    changed = facts;
    changed.sections.pop_back();
    const Result section_count = compare(builder, changed);
    check_code(section_count, DiagnosticCode::section_count_mismatch, "section-count mismatch is named");

    changed = facts;
    ++changed.directories[2].size;
    const Result directory = compare(builder, changed);
    check_code(directory, DiagnosticCode::directory_mismatch, "directory mismatch is named");
}

void test_parser_refusals() {
    auto bad_directory = Builder();
    bad_directory.directory(2, 0x3FF0, 0x20);  // Runs past SizeOfImage.
    const Result directory = compare(bad_directory, expected_facts());
    check(!directory.parse_succeeded && !directory.matches, "out-of-image directory is refused as a mismatch");
    check_code(directory, DiagnosticCode::invalid_image, "invalid directory is named as an image parse failure");
    check(directory.diagnostics.size() == 1, "parse failure returns one bounded diagnostic");
    check(directory.diagnostics[0].message.find("data directory 2") != std::string::npos,
          "parse-failure detail retains the directory shape");

    auto bad_tls = Builder();
    const std::uint32_t tls_file = Builder::file_offset(0x2100);
    bad_tls.directory(9, 0x2100, 40);
    bad_tls.u64(tls_file, Builder::kImageBase + 0x3FFF);
    bad_tls.u64(tls_file + 8, Builder::kImageBase + 0x4001);
    const Result tls = compare(bad_tls, expected_facts());
    check(!tls.parse_succeeded, "TLS template outside the image is refused");
    check_code(tls, DiagnosticCode::invalid_image, "TLS refusal is a named parse failure");
    check(tls.diagnostics[0].message.find("TLS template") != std::string::npos,
          "TLS parse detail identifies the invalid shape");
}

void test_diagnostic_bound() {
    const Builder builder;
    auto changed = expected_facts();
    ++changed.image_base;
    ++changed.size_of_image;
    ++changed.entry_point;
    changed.sections[0].name = "different";
    changed.sections[1].characteristics ^= 1;
    for (std::size_t i = 0; i < changed.directories.size(); ++i) {
        ++changed.directories[i].rva;
        ++changed.directories[i].size;
    }
    const Result result = compare(builder, changed);
    check(!result.matches, "multiple differences remain a mismatch");
    check(result.diagnostics.size() == rt::pe_loader_shadow::kMaxDiagnostics,
          "diagnostics are capped at the documented limit");
    check(result.diagnostics.back().code == DiagnosticCode::additional_diagnostics_omitted,
          "bounded output ends with a named omission diagnostic");
    for (const auto& diagnostic : result.diagnostics)
        check(diagnostic.message.size() <= rt::pe_loader_shadow::kMaxDiagnosticMessageBytes,
              "every diagnostic message is bounded");

    auto invalid = Builder();
    invalid.u16(0, 0);
    const Result long_name = compare(invalid, expected_facts(), std::string(1000, 'x'));
    check(!long_name.parse_succeeded, "invalid bytes remain a parse mismatch");
    check(long_name.diagnostics[0].message.size() <= rt::pe_loader_shadow::kMaxDiagnosticMessageBytes,
          "image name cannot make parse diagnostics unbounded");
}

}  // namespace

int main() {
    test_switch_values();
    test_match_and_field_mismatches();
    test_parser_refusals();
    test_diagnostic_bound();
    if (failures) {
        std::fprintf(stderr, "pe_loader_shadow_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("pe_loader_shadow_test: OK (switches, field parity, bounds, bounded diagnostics)\n");
    return 0;
}
