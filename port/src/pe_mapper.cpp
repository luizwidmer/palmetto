#include "pe_mapper.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rt::pe::mapper {

namespace {

constexpr std::uint64_t kImagePageSize = 0x1000;
std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// The bytes a section copies from the file, measured against the Windows loader: every 4 KiB page the declared virtual
// extent touches contributes file bytes up to SizeOfRawData (the page tail past VirtualSize is file-backed, not zero),
// clamped to SizeOfRawData and to the next section's first byte. A zero PointerToRawData means no file data.
std::uint64_t section_copy_size(const Image& image, const Section& section) {
    if (!section.raw_size || !section.raw_pointer) return 0;
    const std::uint64_t virtual_size = section.virtual_size ? section.virtual_size : section.raw_size;
    std::uint64_t size = (std::min)(static_cast<std::uint64_t>(section.raw_size),
                                    align_up(static_cast<std::uint64_t>(section.virtual_address) + virtual_size,
                                             kImagePageSize) -
                                        section.virtual_address);
    for (const Section& other : image.sections()) {
        if (other.virtual_address > section.virtual_address)
            size = (std::min)(size, static_cast<std::uint64_t>(other.virtual_address) - section.virtual_address);
    }
    return size;
}

}  // namespace

namespace {

constexpr std::uint32_t kSectionExecute = 0x20000000;
constexpr std::uint32_t kSectionRead = 0x40000000;
constexpr std::uint32_t kSectionWrite = 0x80000000;

[[noreturn]] void fail(const Image& image, const std::string& message) {
    throw Error(image.name() + ": " + message);
}

std::int64_t relocation_delta(const Image& image, std::uint64_t actual_base) {
    const std::uint64_t preferred_base = image.image_base();
    if (actual_base >= preferred_base) {
        const std::uint64_t difference = actual_base - preferred_base;
        if (difference > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            fail(image, "the relocation delta is outside the signed 64-bit range");
        return static_cast<std::int64_t>(difference);
    }

    const std::uint64_t difference = preferred_base - actual_base;
    const std::uint64_t min_magnitude = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    if (difference > min_magnitude) fail(image, "the relocation delta is outside the signed 64-bit range");
    if (difference == min_magnitude) return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(difference);
}

void require_relocation_target(const Image& image, std::uint32_t rva, std::uint32_t width) {
    if (rva < image.size_of_headers()) fail(image, "a relocation lands in the headers");
    if (!image.rva_in_image(rva, width)) fail(image, "a relocation runs outside the image");
}

void apply_relocations(const Image& image, std::uint8_t* base, std::int64_t delta) {
    for (const Relocation& relocation : image.relocations()) {
        if (relocation.type == Relocation::kAbsolute) continue;

        // Match the reference loader: every non-ABSOLUTE target is kept out of the headers, and only
        // the two supported widths are patched. The parser has already checked each RVA is in the image.
        if (relocation.rva < image.size_of_headers()) fail(image, "a relocation lands in the headers");
        if (relocation.type == Relocation::kDir64) {
            require_relocation_target(image, relocation.rva, sizeof(std::uint64_t));
            std::uint64_t value = 0;
            std::memcpy(&value, base + relocation.rva, sizeof(value));
            value += static_cast<std::uint64_t>(delta);  // PE DIR64 arithmetic wraps as an unsigned VA.
            std::memcpy(base + relocation.rva, &value, sizeof(value));
        } else if (relocation.type == Relocation::kHighLow) {
            require_relocation_target(image, relocation.rva, sizeof(std::uint32_t));
            std::uint32_t value = 0;
            std::memcpy(&value, base + relocation.rva, sizeof(value));
            const std::int64_t original = static_cast<std::int64_t>(value);
            if ((delta > 0 && original > std::numeric_limits<std::int64_t>::max() - delta) ||
                (delta < 0 && original < std::numeric_limits<std::int64_t>::min() - delta))
                fail(image, "a 32-bit (HIGHLOW) relocation does not fit at this base");
            const std::int64_t moved = original + delta;
            if (moved < 0 || moved > std::numeric_limits<std::uint32_t>::max())
                fail(image, "a 32-bit (HIGHLOW) relocation does not fit at this base");
            value = static_cast<std::uint32_t>(moved);
            std::memcpy(base + relocation.rva, &value, sizeof(value));
        } else {
            // A one-byte range check also protects the unsupported-type diagnostic if an Image implementation
            // later admits relocation RVAs that were not checked by its parser.
            if (!image.rva_in_image(relocation.rva, 1)) fail(image, "a relocation runs outside the image");
            fail(image, "unsupported relocation type " + std::to_string(relocation.type));
        }
    }
}

PageProtection protection_for(const Section& section) {
    const bool execute = (section.characteristics & kSectionExecute) != 0;
    const bool read = (section.characteristics & kSectionRead) != 0;
    const bool write = (section.characteristics & kSectionWrite) != 0;
    if (execute) {
        if (write) return PageProtection::execute_read_write;
        return read ? PageProtection::execute_read : PageProtection::execute;
    }
    if (write) return PageProtection::read_write;
    return read ? PageProtection::read_only : PageProtection::no_access;
}

std::string section_label(const Section& section, std::size_t index) {
    return section.name.empty() ? "section #" + std::to_string(index) : section.name;
}

std::uint16_t file_u16(const Image& image, std::span<const std::uint8_t> file, std::size_t offset, const char* what) {
    if (offset > file.size() || sizeof(std::uint16_t) > file.size() - offset)
        fail(image, std::string(what) + " runs past the end of the file");
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(file[offset]) |
                                     (static_cast<std::uint16_t>(file[offset + 1]) << 8));
}

std::uint32_t file_u32(const Image& image, std::span<const std::uint8_t> file, std::size_t offset, const char* what) {
    if (offset > file.size() || sizeof(std::uint32_t) > file.size() - offset)
        fail(image, std::string(what) + " runs past the end of the file");
    return static_cast<std::uint32_t>(file[offset]) | (static_cast<std::uint32_t>(file[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(file[offset + 2]) << 16) | (static_cast<std::uint32_t>(file[offset + 3]) << 24);
}

// The rewrite mapper receives Image, not the legacy Loader's optional-header view. Preserve the measured placement
// refusals that depend on alignment metadata, and refuse conflicting sub-page protections before allocating memory.
void validate_placement(const Image& image, bool enforce_image_base_alignment) {
    const auto file = image.file_bytes();
    const std::uint32_t nt_offset = file_u32(image, file, 0x3C, "the DOS header");
    const std::uint64_t optional_offset64 = static_cast<std::uint64_t>(nt_offset) + 4 + 20;
    if (optional_offset64 > file.size() || 2 > file.size() - static_cast<std::size_t>(optional_offset64))
        fail(image, "the optional header runs past the end of the file");
    const std::size_t file_header = static_cast<std::size_t>(nt_offset) + 4;
    const std::uint16_t section_count = file_u16(image, file, file_header + 2, "the file header");
    const std::uint16_t optional_size = file_u16(image, file, file_header + 16, "the file header");
    const std::uint64_t optional_end = optional_offset64 + optional_size;
    if (optional_end > file.size()) fail(image, "the optional header runs past the end of the file");
    if (optional_size < 0xF0) fail(image, "not a PE32+ image");
    if (enforce_image_base_alignment && image.image_base() && (image.image_base() & 0xFFFFull))
        fail(image, "ImageBase is not a multiple of 64 KiB");
    const auto optional_offset = static_cast<std::size_t>(optional_offset64);
    const std::uint32_t section_alignment = file_u32(image, file, optional_offset + 32, "SectionAlignment");
    const std::uint32_t file_alignment = file_u32(image, file, optional_offset + 36, "FileAlignment");
    const auto is_power_of_two = [](std::uint32_t value) { return value && !(value & (value - 1)); };
    if (file_alignment &&
        (!is_power_of_two(file_alignment) || file_alignment < 0x200 || file_alignment > 0x10000))
        fail(image, "FileAlignment is not a power of two between 0x200 and 0x10000");
    if (section_alignment &&
        (!is_power_of_two(section_alignment) || (file_alignment && section_alignment < file_alignment)))
        fail(image, "SectionAlignment is not a power of two at least FileAlignment");

    const std::uint64_t section_table = optional_end;
    if (section_table + static_cast<std::uint64_t>(section_count) * 40 > file.size())
        fail(image, "the section table runs past the end of the file");
    const auto& sections = image.sections();
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const Section& section = sections[i];
        if (section_alignment && section.virtual_address % section_alignment)
            fail(image, "section " + std::to_string(i) + " is not aligned to SectionAlignment");
        const std::uint64_t section_size = section.virtual_size ? section.virtual_size : section.raw_size;
        if (!section_size) continue;
        const std::uint64_t first_page = section.virtual_address & ~(kImagePageSize - 1);
        const std::uint64_t last_page = align_up(static_cast<std::uint64_t>(section.virtual_address) + section_size,
                                                 kImagePageSize);
        for (std::size_t j = 0; j < i; ++j) {
            const Section& other = sections[j];
            const std::uint64_t other_size = other.virtual_size ? other.virtual_size : other.raw_size;
            if (!other_size) continue;
            const std::uint64_t other_first_page = other.virtual_address & ~(kImagePageSize - 1);
            const std::uint64_t other_last_page =
                align_up(static_cast<std::uint64_t>(other.virtual_address) + other_size, kImagePageSize);
            if (first_page < other_last_page && other_first_page < last_page &&
                protection_for(section) != protection_for(other))
                fail(image, "sections " + section_label(other, j) + " and " + section_label(section, i) +
                                 " share a page with different protections");
        }
    }
}

class WindowsAllocator final : public Allocator {
public:
    bool enforces_windows_image_base_alignment() const noexcept override { return true; }

    std::uint8_t* allocate(std::uint64_t preferred_base, std::size_t size, bool allow_rebase) override {
#if defined(_WIN32)
        void* preferred = nullptr;
        if (preferred_base <= std::numeric_limits<std::uintptr_t>::max()) {
            const auto address = static_cast<std::uintptr_t>(preferred_base);
            if (static_cast<std::uint64_t>(address) == preferred_base) preferred = reinterpret_cast<void*>(address);
        }
        void* allocation = preferred ? VirtualAlloc(preferred, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : nullptr;
        if (!allocation && allow_rebase)
            allocation = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        return static_cast<std::uint8_t*>(allocation);
#else
        (void)preferred_base;
        (void)size;
        (void)allow_rebase;
        return nullptr;
#endif
    }
};

class WindowsProtector final : public Protector {
public:
    bool protect(std::uint8_t* address, std::size_t size, PageProtection protection) override {
#if defined(_WIN32)
        DWORD previous = 0;
        return VirtualProtect(address, size, static_cast<DWORD>(protection), &previous) != 0;
#else
        (void)address;
        (void)size;
        (void)protection;
        return false;
#endif
    }
};

}  // namespace

MappedImage map_image(const Image& image, Allocator& allocator, Protector& protector) {
    validate_placement(image, allocator.enforces_windows_image_base_alignment());
    const Directory& relocation_directory = image.directory(Image::kDirectoryBaseReloc);
    const bool can_rebase = relocation_directory.present();
    std::uint8_t* base = allocator.allocate(image.image_base(), image.size_of_image(), can_rebase);
    if (!base) {
        if (!can_rebase) fail(image, "preferred base is taken and the image has no relocations");
        fail(image, "VirtualAlloc failed");
    }

    const std::uint64_t actual_base = reinterpret_cast<std::uintptr_t>(base);
    const bool relocated = actual_base != image.image_base();
    if (relocated && !can_rebase) fail(image, "preferred base is taken and the image has no relocations");
    const std::int64_t delta = relocated ? relocation_delta(image, actual_base) : 0;

    // Do not rely on every injected allocator's backing store being zero-initialized. This makes all gaps,
    // virtual tails, and sections with no raw bytes deterministic even in the unit-test allocator.
    std::memset(base, 0, image.size_of_image());
    const auto file = image.file_bytes();
    std::memcpy(base, file.data(), image.size_of_headers());

    MappedImage mapped;
    mapped.base = base;
    mapped.size = image.size_of_image();
    mapped.relocated = relocated;
    mapped.sections.reserve(image.sections().size());
    for (const Section& section : image.sections()) {
        const std::uint64_t copy_size = section_copy_size(image, section);
        if (copy_size) {
            if (section.raw_pointer > file.size() || copy_size > file.size() - section.raw_pointer)
                fail(image, "section " + section.name + " reads past the end of the file");
            std::memcpy(base + section.virtual_address, file.data() + section.raw_pointer,
                        static_cast<std::size_t>(copy_size));
        }
        const auto copied_size = static_cast<std::uint32_t>(copy_size);
        const std::uint32_t mapped_size = (std::max)(section.virtual_size, copied_size);
        mapped.sections.push_back({section.name, section.virtual_address, mapped_size, copied_size,
                                   section.characteristics});
    }

    if (relocated) apply_relocations(image, base, delta);

    mapped.protect_plan.reserve(image.sections().size() + 1);
    mapped.protect_plan.push_back({"headers", 0, image.size_of_headers(), PageProtection::read_only});
    for (std::size_t i = 0; i < image.sections().size(); ++i) {
        const Section& section = image.sections()[i];
        mapped.protect_plan.push_back({section_label(section, i), section.virtual_address,
                                       mapped.sections[i].mapped_size, protection_for(section)});
    }

    for (const ProtectionRegion& region : mapped.protect_plan) {
        if (!protector.protect(base + region.rva, region.size, region.protection))
            fail(image, "VirtualProtect failed for " + region.name);
    }
    return mapped;
}

MappedImage map_image(const Image& image) {
    WindowsAllocator allocator;
    WindowsProtector protector;
    return map_image(image, allocator, protector);
}

}  // namespace rt::pe::mapper
