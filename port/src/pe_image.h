// Bounds-safe decoder for the PE32+ images the runtime maps.
//
// This is the parsing core of the PE-loader rewrite (docs/pe_loader_rewrite.md). It turns a file image into a validated,
// structured description - headers, sections, directories, exports, imports, base relocations, TLS and the exception
// table - without mapping it, running code, or touching the host. The mapper and resolver consume this description; the
// old loader remains the reference implementation until the rewrite reaches parity, and the two are compared on the
// same synthetic fixtures.
//
// Every read is checked against the file and every RVA is checked against the image, so a corrupt or hostile image is
// refused with a named error instead of being partially mapped. The decoder does not interpret relocation semantics,
// bind imports, install TLS or register unwind data: those are the mapper's and resolver's jobs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace rt::pe {

// A parse or consistency failure. The message always starts with the image name.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Section {
    std::string name;
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_pointer = 0;
    uint32_t raw_size = 0;
    uint32_t characteristics = 0;

    // The amount of the section that exists in the file and is copied by a mapping: min(raw size, virtual size), with
    // a zero virtual size meaning the raw size. This is the rule the reference loader has always used.
    uint32_t mapped_raw_size() const;
    bool executable() const;
    bool writable() const;
    bool readable() const;
    bool contains(std::uint64_t rva, std::uint64_t size) const;
};

struct Directory {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
    bool present() const { return rva != 0 && size != 0; }
};

struct ExportSymbol {
    std::string name;      // empty for an export that has only an ordinal
    std::uint32_t ordinal = 0;
    std::uint32_t rva = 0;
    std::string forwarder;  // "module.symbol" for a re-export; empty otherwise
    bool by_name = false;   // the export appears in the name pointer table
    bool is_forwarder() const { return !forwarder.empty(); }
};

struct ImportSymbol {
    std::string name;    // empty for an import by ordinal
    std::uint16_t ordinal = 0;
    std::uint32_t iat_rva = 0;  // the slot the loader writes in the image
    bool by_ordinal = false;
};

struct ImportLibrary {
    std::string name;  // the DLL name as the import directory spells it
    std::vector<ImportSymbol> symbols;
};

struct Relocation {
    static constexpr std::uint16_t kAbsolute = 0;
    static constexpr std::uint16_t kHighLow = 3;
    static constexpr std::uint16_t kDir64 = 10;

    std::uint16_t type = kAbsolute;  // IMAGE_REL_BASED_*
    std::uint32_t rva = 0;           // the page RVA plus the entry offset
};

struct TlsInfo {
    bool present = false;
    std::uint32_t template_rva = 0;
    std::uint32_t template_size = 0;
    std::uint32_t zero_fill = 0;
    std::uint32_t index_rva = 0;
    std::uint32_t callbacks_rva = 0;
    std::vector<std::uint32_t> callback_rvas;
};

struct ExceptionInfo {
    bool present = false;
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
    std::uint32_t entry_count = 0;  // RUNTIME_FUNCTION entries of 12 bytes
};

class Image {
public:
    static constexpr unsigned kDirectoryExport = 0;
    static constexpr unsigned kDirectoryImport = 1;
    static constexpr unsigned kDirectoryException = 3;
    static constexpr unsigned kDirectoryBaseReloc = 5;
    static constexpr unsigned kDirectoryTls = 9;
    static constexpr unsigned kDirectoryCount = 16;

    // Decodes and validates `file`. `name` is used in every diagnostic (normally the module file name). The span must
    // outlive the Image: section_data() points into it. Throws Error on any inconsistency.
    static Image parse(std::span<const std::uint8_t> file, std::string name);

    const std::string& name() const { return name_; }
    std::span<const std::uint8_t> file_bytes() const { return file_; }

    std::uint64_t image_base() const { return image_base_; }
    std::uint32_t size_of_image() const { return size_of_image_; }
    std::uint32_t size_of_headers() const { return size_of_headers_; }
    std::uint32_t entry_point() const { return entry_point_; }
    std::uint16_t characteristics() const { return characteristics_; }
    std::uint16_t subsystem() const { return subsystem_; }
    std::uint16_t dll_characteristics() const { return dll_characteristics_; }
    bool is_dll() const;

    const std::vector<Section>& sections() const { return sections_; }
    const Section* section_for(std::uint64_t rva) const;  // the containing section, or null for the headers

    const Directory& directory(unsigned index) const;
    const std::vector<ExportSymbol>& exports() const { return exports_; }
    const std::vector<ImportLibrary>& imports() const { return imports_; }
    const std::vector<Relocation>& relocations() const { return relocations_; }
    const TlsInfo& tls() const { return tls_; }
    const ExceptionInfo& exceptions() const { return exceptions_; }

    // True when [rva, rva + size) lies inside the image (headers included). A zero size checks only the start.
    bool rva_in_image(std::uint64_t rva, std::uint64_t size) const;
    // The file bytes a mapper copies for a section: mapped_raw_size() bytes at raw_pointer, or empty when the section
    // has no file data. The section must come from this Image.
    std::span<const std::uint8_t> section_data(const Section& section) const;

    // A NUL-terminated string at `rva` in the image, read from its file bytes. Throws Error when the string does not
    // lie in file-backed data or is not terminated inside its section.
    std::string read_string(std::uint32_t rva, const char* what) const;

private:
    Image() = default;
    void parse_headers();
    void parse_sections(std::uint32_t section_table, unsigned count);
    void parse_directories();
    void parse_exports(const Directory& directory);
    void parse_imports(const Directory& directory);
    void parse_relocations(const Directory& directory);
    void parse_tls(const Directory& directory);
    void parse_exceptions(const Directory& directory);

    // [rva, rva + size) must lie inside the file-backed part of one section. Returns that file offset.
    std::size_t file_offset(std::uint32_t rva, std::uint32_t size, const char* what) const;
    std::uint8_t byte_at(std::uint32_t rva, const char* what) const;
    std::uint16_t word_at(std::uint32_t rva, const char* what) const;
    std::uint32_t dword_at(std::uint32_t rva, const char* what) const;
    std::uint64_t qword_at(std::uint32_t rva, const char* what) const;
    void require_rva(std::uint32_t rva, std::uint64_t size, const char* what) const;
    [[noreturn]] void fail(const std::string& message) const;

    std::span<const std::uint8_t> file_;
    std::string name_;
    std::uint64_t image_base_ = 0;
    std::uint32_t size_of_image_ = 0;
    std::uint32_t size_of_headers_ = 0;
    std::uint32_t entry_point_ = 0;
    std::uint16_t characteristics_ = 0;
    std::uint16_t subsystem_ = 0;
    std::uint16_t dll_characteristics_ = 0;
    std::uint32_t directory_count_ = 0;
    Directory directories_[kDirectoryCount]{};
    std::vector<Section> sections_;
    std::vector<ExportSymbol> exports_;
    std::vector<ImportLibrary> imports_;
    std::vector<Relocation> relocations_;
    TlsInfo tls_;
    ExceptionInfo exceptions_;
};

}  // namespace rt::pe
