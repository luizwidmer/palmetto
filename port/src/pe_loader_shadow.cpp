#include "pe_loader_shadow.h"

#include "pe_image.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace rt::pe_loader_shadow {
namespace {

std::string bounded_message(std::string_view message) {
    if (message.size() <= kMaxDiagnosticMessageBytes) return std::string(message);
    constexpr std::string_view suffix = "...";
    const std::size_t prefix_size = kMaxDiagnosticMessageBytes - suffix.size();
    std::string bounded(message.substr(0, prefix_size));
    bounded.append(suffix);
    return bounded;
}

std::string hex_value(std::uint64_t value) {
    char digits[16]{};
    const auto converted = std::to_chars(digits, digits + sizeof(digits), value, 16);
    return "0x" + std::string(digits, converted.ptr);
}

void append_field(std::string& fields, std::string_view field) {
    if (!fields.empty()) fields += ", ";
    fields += field;
}

std::string section_difference(const pe::Section& actual, const SectionFacts& expected, std::size_t index) {
    std::string fields;
    if (actual.name != expected.name) append_field(fields, "name");
    if (actual.virtual_address != expected.virtual_address) append_field(fields, "VirtualAddress");
    if (actual.virtual_size != expected.virtual_size) append_field(fields, "VirtualSize");
    if (actual.raw_pointer != expected.raw_pointer) append_field(fields, "PointerToRawData");
    if (actual.raw_size != expected.raw_size) append_field(fields, "SizeOfRawData");
    if (actual.characteristics != expected.characteristics) append_field(fields, "Characteristics");
    return "section[" + std::to_string(index) + "] differs: " + fields;
}

void append_diagnostic(Result& result, std::size_t& omitted, DiagnosticCode code, std::string message) {
    if (result.diagnostics.size() < kMaxDiagnostics - 1) {
        result.diagnostics.push_back({code, bounded_message(message)});
    } else if (omitted < std::numeric_limits<std::size_t>::max()) {
        ++omitted;
    }
}

void finish(Result& result, std::size_t omitted) {
    if (omitted) {
        result.diagnostics.push_back(
            {DiagnosticCode::additional_diagnostics_omitted,
             std::to_string(omitted) + " additional mismatch diagnostic(s) omitted"});
    }
    result.matches = result.parse_succeeded && result.diagnostics.empty();
}

}  // namespace

Switches switches_from_values(std::string_view shadow_parse, std::string_view shadow_strict) noexcept {
    const bool enabled = shadow_parse == "1";
    return {enabled, enabled && shadow_strict == "1"};
}

Result compare(std::span<const std::uint8_t> file, const OldLoaderFacts& expected, std::string image_name) {
    Result result;
    try {
        const pe::Image parsed = pe::Image::parse(file, std::move(image_name));
        result.parse_succeeded = true;
        std::size_t omitted = 0;

        if (parsed.image_base() != expected.image_base) {
            append_diagnostic(result, omitted, DiagnosticCode::image_base_mismatch,
                              "ImageBase parsed=" + hex_value(parsed.image_base()) +
                                  " old=" + hex_value(expected.image_base));
        }
        if (parsed.size_of_image() != expected.size_of_image) {
            append_diagnostic(result, omitted, DiagnosticCode::size_of_image_mismatch,
                              "SizeOfImage parsed=" + hex_value(parsed.size_of_image()) +
                                  " old=" + hex_value(expected.size_of_image));
        }
        if (parsed.entry_point() != expected.entry_point) {
            append_diagnostic(result, omitted, DiagnosticCode::entry_point_mismatch,
                              "AddressOfEntryPoint parsed=" + hex_value(parsed.entry_point()) +
                                  " old=" + hex_value(expected.entry_point));
        }

        const auto& parsed_sections = parsed.sections();
        if (parsed_sections.size() != expected.sections.size()) {
            append_diagnostic(result, omitted, DiagnosticCode::section_count_mismatch,
                              "section count parsed=" + std::to_string(parsed_sections.size()) +
                                  " old=" + std::to_string(expected.sections.size()));
        }
        const std::size_t common_sections = std::min(parsed_sections.size(), expected.sections.size());
        for (std::size_t i = 0; i < common_sections; ++i) {
            const pe::Section& actual = parsed_sections[i];
            const SectionFacts& old = expected.sections[i];
            if (actual.name != old.name || actual.virtual_address != old.virtual_address ||
                actual.virtual_size != old.virtual_size || actual.raw_pointer != old.raw_pointer ||
                actual.raw_size != old.raw_size || actual.characteristics != old.characteristics) {
                append_diagnostic(result, omitted, DiagnosticCode::section_mismatch,
                                  section_difference(actual, old, i));
            }
        }

        for (std::size_t i = 0; i < kDirectoryCount; ++i) {
            const pe::Directory& actual = parsed.directory(static_cast<unsigned>(i));
            const DirectoryFacts& old = expected.directories[i];
            if (actual.rva != old.rva || actual.size != old.size) {
                append_diagnostic(result, omitted, DiagnosticCode::directory_mismatch,
                                  "directory[" + std::to_string(i) + "] parsed={" + hex_value(actual.rva) + ", " +
                                      hex_value(actual.size) + "} old={" + hex_value(old.rva) + ", " +
                                      hex_value(old.size) + "}");
            }
        }
        finish(result, omitted);
    } catch (const pe::Error& error) {
        result.parse_succeeded = false;
        result.matches = false;
        result.diagnostics.push_back({DiagnosticCode::invalid_image, bounded_message(error.what())});
    }
    return result;
}

std::string_view diagnostic_name(DiagnosticCode code) noexcept {
    switch (code) {
        case DiagnosticCode::invalid_image: return "invalid_image";
        case DiagnosticCode::image_base_mismatch: return "image_base_mismatch";
        case DiagnosticCode::size_of_image_mismatch: return "size_of_image_mismatch";
        case DiagnosticCode::entry_point_mismatch: return "entry_point_mismatch";
        case DiagnosticCode::section_count_mismatch: return "section_count_mismatch";
        case DiagnosticCode::section_mismatch: return "section_mismatch";
        case DiagnosticCode::directory_mismatch: return "directory_mismatch";
        case DiagnosticCode::additional_diagnostics_omitted: return "additional_diagnostics_omitted";
    }
    return "unknown";
}

}  // namespace rt::pe_loader_shadow
