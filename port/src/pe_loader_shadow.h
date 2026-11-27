// Pure shadow comparison between the reference loader's PE-header facts and rt::pe::Image.
// The caller supplies the same file bytes already read by Loader::map_image and facts copied from the old loader's
// validated headers. This layer does not inspect the environment, log, map memory, or change loader policy.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rt::pe_loader_shadow {

constexpr std::size_t kDirectoryCount = 16;
// Reserve one slot for a truncation marker so even caller-supplied inconsistent facts cannot create an unbounded log.
constexpr std::size_t kMaxDiagnostics = 16;
constexpr std::size_t kMaxDiagnosticMessageBytes = 240;

struct SectionFacts {
    // The PE section name, truncated at its first NUL (or eight bytes), as IMAGE_SECTION_HEADER names are interpreted.
    std::string name;
    std::uint32_t virtual_address = 0;
    std::uint32_t virtual_size = 0;
    std::uint32_t raw_pointer = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t characteristics = 0;
};

struct DirectoryFacts {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
};

struct OldLoaderFacts {
    // Preferred ImageBase from the file header, not the actual address chosen by VirtualAlloc.
    std::uint64_t image_base = 0;
    std::uint32_t size_of_image = 0;
    std::uint32_t entry_point = 0;
    std::vector<SectionFacts> sections;
    // Fill absent entries with zero, following the old loader's directory(nt, index) helper semantics.
    std::array<DirectoryFacts, kDirectoryCount> directories{};
};

enum class DiagnosticCode {
    invalid_image,
    image_base_mismatch,
    size_of_image_mismatch,
    entry_point_mismatch,
    section_count_mismatch,
    section_mismatch,
    directory_mismatch,
    additional_diagnostics_omitted,
};

struct Diagnostic {
    DiagnosticCode code = DiagnosticCode::invalid_image;
    std::string message;
};

struct Result {
    bool parse_succeeded = false;
    bool matches = false;
    // Each message is bounded; at most kMaxDiagnostics entries are returned.
    std::vector<Diagnostic> diagnostics;
};

struct Switches {
    bool enabled = false;
    bool strict = false;
};

// Interpret environment values supplied by the caller. Only the exact string "1" enables a switch. Strict mode is
// meaningful only with shadow parsing enabled; a strict-only environment setting therefore remains a no-op.
Switches switches_from_values(std::string_view shadow_parse, std::string_view shadow_strict) noexcept;

// Parse the supplied file with rt::pe::Image, compare all fields it models against the supplied old-loader facts, and
// return named diagnostics rather than logging or throwing parse/mismatch failures. A parser refusal is a mismatch.
Result compare(std::span<const std::uint8_t> file, const OldLoaderFacts& expected, std::string image_name);

std::string_view diagnostic_name(DiagnosticCode code) noexcept;

}  // namespace rt::pe_loader_shadow
