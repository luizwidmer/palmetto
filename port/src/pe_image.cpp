#include "pe_image.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace rt::pe {
namespace {

constexpr std::uint16_t kDosSignature = 0x5A4D;       // "MZ"
constexpr std::uint32_t kNtSignature = 0x00004550;    // "PE\0\0"
constexpr std::uint16_t kAmd64Machine = 0x8664;
constexpr std::uint16_t kPe32PlusMagic = 0x020B;
constexpr std::uint16_t kFileDll = 0x2000;
constexpr std::uint32_t kSectionExecutable = 0x20000000;
constexpr std::uint32_t kSectionReadable = 0x40000000;
constexpr std::uint32_t kSectionWritable = 0x80000000;
constexpr unsigned kMaxSections = 96;
constexpr unsigned kMaxExportFunctions = 1u << 20;
constexpr unsigned kMaxImportLibraries = 4096;
constexpr unsigned kMaxImportSymbols = 1u << 16;
constexpr unsigned kMaxTlsCallbacks = 1024;
constexpr std::uint32_t kExportDirectoryBytes = 40;
constexpr std::uint32_t kImportDescriptorBytes = 20;
constexpr std::uint32_t kTlsDirectoryBytes = 40;

// PE32+ optional-header field offsets, from the start of the optional header.
constexpr std::uint32_t kOptEntryPoint = 16;
constexpr std::uint32_t kOptImageBase = 24;
constexpr std::uint32_t kOptSizeOfImage = 56;
constexpr std::uint32_t kOptSizeOfHeaders = 60;
constexpr std::uint32_t kOptSubsystem = 68;
constexpr std::uint32_t kOptDllCharacteristics = 70;
constexpr std::uint32_t kOptDirectoryCount = 108;
constexpr std::uint32_t kOptDirectories = 112;
constexpr std::uint32_t kOptMinimumBytes = kOptDirectories;

constexpr std::uint16_t kRelAbsolute = 0;
constexpr std::uint16_t kRelHighLow = 3;
constexpr std::uint16_t kRelDir64 = 10;

std::uint64_t read_le(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned width) {
    if (offset > bytes.size() || width > bytes.size() - offset) return 0;
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    return value;
}

template <class T>
T load(const std::uint8_t* bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}

std::string section_name(const std::uint8_t* raw) {
    std::string name;
    for (unsigned i = 0; i < 8 && raw[i]; ++i) name.push_back(static_cast<char>(raw[i]));
    return name;
}

}  // namespace

std::uint32_t Section::mapped_raw_size() const {
    if (!raw_size) return 0;
    return virtual_size ? std::min(raw_size, virtual_size) : raw_size;
}

bool Section::executable() const { return (characteristics & kSectionExecutable) != 0; }
bool Section::writable() const { return (characteristics & kSectionWritable) != 0; }
bool Section::readable() const { return (characteristics & kSectionReadable) != 0; }

bool Section::contains(std::uint64_t rva, std::uint64_t size) const {
    const std::uint64_t start = rva;
    const std::uint64_t end = rva + size;
    const std::uint64_t section_end =
        static_cast<std::uint64_t>(virtual_address) + std::max(virtual_size, mapped_raw_size());
    return end >= start && start >= virtual_address && end <= section_end;
}

bool Image::is_dll() const { return (characteristics_ & kFileDll) != 0; }

bool Image::rva_in_image(std::uint64_t rva, std::uint64_t size) const {
    return rva + size >= rva && rva + size <= size_of_image_ && rva < size_of_image_;
}

void Image::fail(const std::string& message) const { throw Error(name_ + ": " + message); }

Image Image::parse(std::span<const std::uint8_t> file, std::string name) {
    Image image;
    image.file_ = file;
    image.name_ = std::move(name);
    image.parse_headers();
    image.parse_directories();
    return image;
}

void Image::parse_headers() {
    if (file_.size() < 64) fail("not an MZ image (file is shorter than a DOS header)");
    if (read_le(file_, 0, 2) != kDosSignature) fail("not an MZ image");
    const std::uint32_t nt_offset = static_cast<std::uint32_t>(read_le(file_, 0x3C, 4));
    if (nt_offset > file_.size() || file_.size() - nt_offset < 4 + 20 + kOptMinimumBytes)
        fail("the NT headers are outside the file");
    if (read_le(file_, nt_offset, 4) != kNtSignature) fail("the NT signature is missing");
    const auto* file_header = file_.data() + nt_offset + 4;
    const std::uint16_t machine = load<std::uint16_t>(file_header, 0);
    const std::uint16_t section_count = load<std::uint16_t>(file_header, 2);
    const std::uint16_t optional_size = load<std::uint16_t>(file_header, 16);
    characteristics_ = load<std::uint16_t>(file_header, 18);
    if (machine != kAmd64Machine) fail("not an x86-64 PE (machine " + std::to_string(machine) + ")");
    if (optional_size < kOptMinimumBytes) fail("the optional header is too small for a PE32+ image");
    if (section_count == 0) fail("the image has no sections");
    if (section_count > kMaxSections) fail("the image has more than " + std::to_string(kMaxSections) + " sections");

    const std::size_t optional_offset = nt_offset + 4 + 20;
    if (file_.size() - optional_offset < optional_size) fail("the optional header runs past the end of the file");
    const auto* optional = file_.data() + optional_offset;
    if (load<std::uint16_t>(optional, 0) != kPe32PlusMagic) fail("not a PE32+ image");

    image_base_ = load<std::uint64_t>(optional, kOptImageBase);
    entry_point_ = load<std::uint32_t>(optional, kOptEntryPoint);
    size_of_image_ = load<std::uint32_t>(optional, kOptSizeOfImage);
    size_of_headers_ = load<std::uint32_t>(optional, kOptSizeOfHeaders);
    subsystem_ = load<std::uint16_t>(optional, kOptSubsystem);
    dll_characteristics_ = load<std::uint16_t>(optional, kOptDllCharacteristics);
    directory_count_ = std::min<std::uint32_t>(load<std::uint32_t>(optional, kOptDirectoryCount), kDirectoryCount);
    if (optional_size < kOptDirectories + directory_count_ * 8) fail("the data directories run past the optional header");

    const std::uint32_t actual_directory_bytes = std::min<std::uint32_t>(
        static_cast<std::uint32_t>((optional_size - kOptDirectories) / 8), directory_count_);
    for (std::uint32_t i = 0; i < actual_directory_bytes; ++i) {
        const auto* entry = optional + kOptDirectories + i * 8;
        directories_[i].rva = load<std::uint32_t>(entry, 0);
        directories_[i].size = load<std::uint32_t>(entry, 4);
    }
    directory_count_ = actual_directory_bytes;

    if (size_of_image_ == 0) fail("SizeOfImage is zero");
    if (image_base_ == 0) fail("ImageBase is zero");
    if (entry_point_ >= size_of_image_) fail("the entry point is outside the image");

    const std::uint64_t section_table = static_cast<std::uint64_t>(nt_offset) + 4 + 20 + optional_size;
    const std::uint64_t section_table_end = section_table + static_cast<std::uint64_t>(section_count) * 40;
    if (section_table_end > file_.size()) fail("the section table runs past the end of the file");
    if (!size_of_headers_ || size_of_headers_ < section_table_end || size_of_headers_ > size_of_image_ ||
        size_of_headers_ > file_.size())
        fail("SizeOfHeaders does not cover the headers inside the file and the image");

    parse_sections(static_cast<std::uint32_t>(section_table), section_count);
}

void Image::parse_sections(std::uint32_t section_table, unsigned count) {
    sections_.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        const auto* raw = file_.data() + section_table + static_cast<std::size_t>(i) * 40;
        Section section;
        section.name = section_name(raw);
        section.virtual_size = load<std::uint32_t>(raw, 8);
        section.virtual_address = load<std::uint32_t>(raw, 12);
        section.raw_size = load<std::uint32_t>(raw, 16);
        section.raw_pointer = load<std::uint32_t>(raw, 20);
        section.characteristics = load<std::uint32_t>(raw, 36);
        const std::string label = section.name.empty() ? "#" + std::to_string(i) : section.name;
        if (section.virtual_address < size_of_headers_)
            fail("section " + label + " overlaps the headers");
        const std::uint64_t span = std::max<std::uint64_t>(section.virtual_size, section.mapped_raw_size());
        if (!span) fail("section " + label + " has no size");
        if (!rva_in_image(section.virtual_address, span)) fail("section " + label + " runs outside the image");
        if (section.raw_size) {
            if (section.raw_pointer > file_.size() || section.raw_size > file_.size() - section.raw_pointer)
                fail("section " + label + " reads past the end of the file");
            if (section.mapped_raw_size() > section.raw_size)
                fail("section " + label + " copies more bytes than it has");
        }
        for (const auto& other : sections_) {
            const std::uint64_t a_start = section.virtual_address;
            const std::uint64_t a_end = a_start + std::max(section.virtual_size, section.mapped_raw_size());
            const std::uint64_t b_start = other.virtual_address;
            const std::uint64_t b_end = b_start + std::max(other.virtual_size, other.mapped_raw_size());
            if (a_start < b_end && b_start < a_end)
                fail("sections " + other.name + " and " + label + " overlap in the image");
        }
        sections_.push_back(std::move(section));
    }
}

void Image::parse_directories() {
    for (std::uint32_t i = 0; i < directory_count_; ++i) {
        const Directory& dir = directories_[i];
        if (dir.present() && !rva_in_image(dir.rva, dir.size))
            fail("data directory " + std::to_string(i) + " runs outside the image");
    }
    const Directory export_dir = directory(kDirectoryExport);
    if (export_dir.present()) parse_exports(export_dir);
    const Directory import_dir = directory(kDirectoryImport);
    if (import_dir.present()) parse_imports(import_dir);
    const Directory reloc_dir = directory(kDirectoryBaseReloc);
    if (reloc_dir.present()) parse_relocations(reloc_dir);
    const Directory tls_dir = directory(kDirectoryTls);
    if (tls_dir.present()) parse_tls(tls_dir);
    const Directory exception_dir = directory(kDirectoryException);
    if (exception_dir.present()) parse_exceptions(exception_dir);
}

const Section* Image::section_for(std::uint64_t rva) const {
    for (const auto& section : sections_)
        if (section.contains(rva, 0)) return &section;
    return nullptr;
}

const Directory& Image::directory(unsigned index) const {
    static const Directory empty;
    return index < directory_count_ ? directories_[index] : empty;
}

std::size_t Image::file_offset(std::uint32_t rva, std::uint32_t size, const char* what) const {
    if (size == 0) size = 1;
    const std::uint64_t end = static_cast<std::uint64_t>(rva) + size;
    if (end > size_of_image_) fail(std::string(what) + " runs outside the image");
    if (rva < size_of_headers_) {
        if (end > size_of_headers_ || end > file_.size()) fail(std::string(what) + " runs outside the headers");
        return rva;
    }
    for (const auto& section : sections_) {
        const std::uint64_t section_start = section.virtual_address;
        const std::uint64_t section_end = section_start + section.mapped_raw_size();
        if (rva < section_start || end > section_end) continue;
        const std::size_t offset = section.raw_pointer + (rva - section.virtual_address);
        if (offset > file_.size() || size > file_.size() - offset)
            fail(std::string(what) + " reads past the end of the file");
        return offset;
    }
    fail(std::string(what) + " is not backed by file data");
}

void Image::require_rva(std::uint32_t rva, std::uint64_t size, const char* what) const {
    if (!rva_in_image(rva, size)) fail(std::string(what) + " runs outside the image");
}

std::uint8_t Image::byte_at(std::uint32_t rva, const char* what) const {
    return file_[file_offset(rva, 1, what)];
}
std::uint16_t Image::word_at(std::uint32_t rva, const char* what) const {
    return static_cast<std::uint16_t>(read_le(file_, file_offset(rva, 2, what), 2));
}
std::uint32_t Image::dword_at(std::uint32_t rva, const char* what) const {
    return static_cast<std::uint32_t>(read_le(file_, file_offset(rva, 4, what), 4));
}
std::uint64_t Image::qword_at(std::uint32_t rva, const char* what) const {
    return read_le(file_, file_offset(rva, 8, what), 8);
}

std::string Image::read_string(std::uint32_t rva, const char* what) const {
    const std::size_t offset = file_offset(rva, 1, what);
    std::size_t end = offset;
    while (end < file_.size() && file_[end]) {
        ++end;
        if (end - offset > 4096) fail(std::string(what) + " is longer than 4096 characters");
    }
    if (end == file_.size()) fail(std::string(what) + " is not terminated inside the file");
    return std::string(reinterpret_cast<const char*>(file_.data() + offset), end - offset);
}

std::span<const std::uint8_t> Image::section_data(const Section& section) const {
    const std::uint32_t size = section.mapped_raw_size();
    if (!size) return {};
    if (section.raw_pointer > file_.size() || size > file_.size() - section.raw_pointer) return {};
    return file_.subspan(section.raw_pointer, size);
}

void Image::parse_exports(const Directory& directory) {
    if (directory.size < kExportDirectoryBytes) fail("the export directory is smaller than its header");
    const std::uint32_t rva = directory.rva;
    const std::uint32_t name_rva = dword_at(rva + 12, "the export directory");
    const std::uint32_t base_ordinal = dword_at(rva + 16, "the export directory");
    const std::uint32_t function_count = dword_at(rva + 20, "the export directory");
    const std::uint32_t name_count = dword_at(rva + 24, "the export directory");
    const std::uint32_t functions_rva = dword_at(rva + 28, "the export directory");
    const std::uint32_t names_rva = dword_at(rva + 32, "the export directory");
    const std::uint32_t ordinals_rva = dword_at(rva + 36, "the export directory");
    (void)name_rva;
    if (function_count > kMaxExportFunctions || name_count > kMaxExportFunctions)
        fail("the export directory has too many entries");
    if (function_count == 0) return;
    require_rva(functions_rva, static_cast<std::uint64_t>(function_count) * 4, "the export function table");
    if (name_count) {
        require_rva(names_rva, static_cast<std::uint64_t>(name_count) * 4, "the export name table");
        require_rva(ordinals_rva, static_cast<std::uint64_t>(name_count) * 2, "the export ordinal table");
    }
    // A function can have several names (aliases), so read the name table first and emit one symbol per name; functions
    // without a name are then emitted once by ordinal.
    std::vector<bool> named(function_count, false);
    for (std::uint32_t i = 0; i < name_count; ++i) {
        const std::uint32_t symbol_name_rva = dword_at(names_rva + i * 4, "an export name");
        const std::uint16_t ordinal_index = word_at(ordinals_rva + i * 2, "an export ordinal");
        if (ordinal_index >= function_count) fail("an export name points past the function table");
        ExportSymbol symbol;
        symbol.name = read_string(symbol_name_rva, "an export name");
        symbol.ordinal = base_ordinal + ordinal_index;
        symbol.by_name = true;
        symbol.rva = dword_at(functions_rva + ordinal_index * 4, "an export function");
        if (!symbol.rva || !rva_in_image(symbol.rva, 1)) fail("an export function runs outside the image");
        if (symbol.rva >= directory.rva && symbol.rva < directory.rva + directory.size)
            symbol.forwarder = read_string(symbol.rva, "an export forwarder");
        named[ordinal_index] = true;
        exports_.push_back(std::move(symbol));
    }
    for (std::uint32_t i = 0; i < function_count; ++i) {
        const std::uint32_t function_rva = dword_at(functions_rva + i * 4, "an export function");
        if (function_rva == 0) continue;  // a hole in the export table
        if (!rva_in_image(function_rva, 1)) fail("an export function runs outside the image");
        if (named[i]) continue;
        ExportSymbol symbol;
        symbol.ordinal = base_ordinal + i;
        symbol.rva = function_rva;
        if (function_rva >= directory.rva && function_rva < directory.rva + directory.size)
            symbol.forwarder = read_string(function_rva, "an export forwarder");
        exports_.push_back(std::move(symbol));
    }
}

void Image::parse_imports(const Directory& directory) {
    require_rva(directory.rva, kImportDescriptorBytes, "the import directory");
    std::uint32_t descriptor_rva = directory.rva;
    for (unsigned library = 0;; ++library) {
        if (library >= kMaxImportLibraries) fail("the import directory has too many descriptors");
        if (!rva_in_image(descriptor_rva, kImportDescriptorBytes))
            fail("the import directory is not terminated inside the image");
        const std::uint32_t original_thunk = dword_at(descriptor_rva, "an import descriptor");
        const std::uint32_t name_rva = dword_at(descriptor_rva + 12, "an import descriptor");
        const std::uint32_t first_thunk = dword_at(descriptor_rva + 16, "an import descriptor");
        if (!original_thunk && !name_rva && !first_thunk) break;  // null terminator
        if (!name_rva) fail("an import descriptor has no DLL name");
        ImportLibrary import;
        import.name = read_string(name_rva, "an imported DLL name");
        const std::uint32_t thunk_rva = original_thunk ? original_thunk : first_thunk;
        if (!thunk_rva) fail("an import descriptor has no thunk table");
        require_rva(thunk_rva, 8, "an import thunk table");
        for (unsigned index = 0;; ++index) {
            if (index >= kMaxImportSymbols) fail("an import library has too many symbols");
            const std::uint64_t entry = qword_at(thunk_rva + index * 8, "an import thunk");
            if (!entry) break;
            ImportSymbol symbol;
            symbol.iat_rva = first_thunk + index * 8;
            require_rva(symbol.iat_rva, 8, "an import address slot");
            if (entry & (std::uint64_t{1} << 63)) {
                symbol.by_ordinal = true;
                symbol.ordinal = static_cast<std::uint16_t>(entry & 0xFFFF);
            } else {
                if (entry > 0xFFFFFFFFull) fail("an import-by-name thunk does not fit an RVA");
                const std::uint32_t by_name_rva = static_cast<std::uint32_t>(entry);
                require_rva(by_name_rva, 3, "an import-by-name entry");
                symbol.name = read_string(by_name_rva + 2, "an imported symbol name");
            }
            import.symbols.push_back(std::move(symbol));
        }
        imports_.push_back(std::move(import));
        descriptor_rva += kImportDescriptorBytes;
    }
}

void Image::parse_relocations(const Directory& directory) {
    require_rva(directory.rva, directory.size, "the relocation directory");
    std::uint32_t offset = 0;
    while (offset < directory.size) {
        if (directory.size - offset < 8) fail("a relocation block header runs past the directory");
        const std::uint32_t block_rva = directory.rva + offset;
        const std::uint32_t page = dword_at(block_rva, "a relocation block");
        const std::uint32_t block_size = dword_at(block_rva + 4, "a relocation block");
        if (block_size < 8 || (block_size & 1u)) fail("a relocation block has an invalid size");
        if (block_size > directory.size - offset) fail("a relocation block runs past the directory");
        const std::uint32_t count = (block_size - 8) / 2;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint16_t entry = word_at(block_rva + 8 + i * 2, "a relocation entry");
            Relocation relocation;
            relocation.type = static_cast<std::uint16_t>(entry >> 12);
            relocation.rva = page + (entry & 0xFFF);
            if (!rva_in_image(relocation.rva, 1)) fail("a relocation entry runs outside the image");
            relocations_.push_back(relocation);
        }
        offset += block_size;
    }
}

void Image::parse_tls(const Directory& directory) {
    if (directory.size < kTlsDirectoryBytes) fail("the TLS directory is smaller than its header");
    auto to_rva = [this](std::uint64_t va, const char* what) -> std::uint32_t {
        if (va < image_base_ || va - image_base_ >= size_of_image_)
            fail(std::string(what) + " is outside the image");
        return static_cast<std::uint32_t>(va - image_base_);
    };
    tls_.present = true;
    const std::uint32_t rva = directory.rva;
    const std::uint64_t start_va = qword_at(rva, "the TLS directory");
    const std::uint64_t end_va = qword_at(rva + 8, "the TLS directory");
    const std::uint64_t index_va = qword_at(rva + 16, "the TLS directory");
    const std::uint64_t callbacks_va = qword_at(rva + 24, "the TLS directory");
    tls_.zero_fill = dword_at(rva + 32, "the TLS directory");
    if (end_va < start_va) fail("the TLS template ends before it starts");
    const std::uint64_t template_size = end_va - start_va;
    if (template_size > 0xFFFFFFFFull) fail("the TLS template is too large");
    if (template_size) {
        tls_.template_rva = to_rva(start_va, "the TLS template");
        tls_.template_size = static_cast<std::uint32_t>(template_size);
        require_rva(tls_.template_rva, tls_.template_size, "the TLS template");
        if (static_cast<std::uint64_t>(tls_.template_size) + tls_.zero_fill > 0xFFFFFFFFull)
            fail("the TLS block is too large");
    }
    if (index_va) {
        tls_.index_rva = to_rva(index_va, "the TLS index");
        require_rva(tls_.index_rva, 4, "the TLS index");
    }
    if (callbacks_va) {
        tls_.callbacks_rva = to_rva(callbacks_va, "the TLS callback table");
        require_rva(tls_.callbacks_rva, 8, "the TLS callback table");
        for (unsigned i = 0;; ++i) {
            if (i >= kMaxTlsCallbacks) fail("the TLS callback table has too many entries");
            const std::uint64_t callback_va = qword_at(tls_.callbacks_rva + i * 8, "a TLS callback");
            if (!callback_va) break;
            tls_.callback_rvas.push_back(to_rva(callback_va, "a TLS callback"));
        }
    }
}

void Image::parse_exceptions(const Directory& directory) {
    if (directory.size % 12) fail("the exception directory is not a whole number of RUNTIME_FUNCTION entries");
    require_rva(directory.rva, directory.size, "the exception directory");
    exceptions_.present = true;
    exceptions_.rva = directory.rva;
    exceptions_.size = directory.size;
    exceptions_.entry_count = directory.size / 12;
}

}  // namespace rt::pe
