// Synthetic PE32+ fixtures for loader_selfcheck_test - the independent mapped-image self-check of the old rt::Loader.
//
// Nothing in this header uses the loader's own parsing helpers: every header, section, table and string is written by
// the test, byte by byte, from a small builder. The test maps these images through rt::Loader and predicts the mapped
// bytes with rt::pe::Image, so the two implementations are compared rather than one feeding the other. No game data is
// involved and the shapes are the test's own.
//
// The geometry is fixed and deliberately awkward for a mapper:
//   headers   0x0000-0x0400
//   .text      RVA 0x1000, VirtualSize 0x180, raw 0x200 at file 0x400: SizeOfRawData > VirtualSize, so only the
//              mapper's page-tail rule (measured Windows behaviour) copies the 0x80 raw bytes past VirtualSize
//   .data      RVA 0x2000, VirtualSize 0x500, raw 0x400 at file 0x600: VirtualSize > SizeOfRawData and not a page
//              multiple, so 0x100 bytes of the section are zero fill
//   .rdata     RVA 0x3000, VirtualSize 0x600, raw 0x600 at file 0xA00
//   SizeOfImage 0x4000, file size 0x1000
// Section virtual addresses are page aligned (Windows refuses anything else); the sizes are not page or FileAlignment
// multiples, and the file offsets of the sections do not follow their virtual addresses.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace loader_selfcheck {

inline constexpr std::uint32_t kSectionAlignment = 0x1000;
inline constexpr std::uint32_t kFileAlignment = 0x200;
inline constexpr std::uint32_t kSizeOfHeaders = 0x400;
inline constexpr std::uint32_t kSizeOfImage = 0x4000;
inline constexpr std::uint32_t kFileSize = 0x1000;
inline constexpr std::uint32_t kNtOffset = 0x80;
inline constexpr std::uint32_t kOptionalOffset = kNtOffset + 4 + 20;  // PE32+ optional header
inline constexpr std::uint32_t kSectionTable = kOptionalOffset + 0xF0;

// Section characteristics, written by value like every other header field.
inline constexpr std::uint32_t kScnCode = 0x00000020;
inline constexpr std::uint32_t kScnInitializedData = 0x00000040;
inline constexpr std::uint32_t kScnExecute = 0x20000000;
inline constexpr std::uint32_t kScnRead = 0x40000000;
inline constexpr std::uint32_t kScnWrite = 0x80000000;

inline constexpr std::uint32_t kTextRva = 0x1000;
inline constexpr std::uint32_t kDataRva = 0x2000;
inline constexpr std::uint32_t kRdataRva = 0x3000;

// The 16-byte TLS template the TLS fixtures copy; the test also looks for it in the module TLS slot of the thread.
inline constexpr std::uint8_t kTlsTemplate[16] = {'T', 'L', 'S', '-', 'S', 'E', 'L', 'F',
                                                  'C', 'H', 'E', 'C', 'K', '-', '0', '1'};

// A synthetic PE32+ image under construction. Every field the loader reads is set explicitly in the constructor or by
// the fixture functions below; nothing is inherited from a real image.
class PeBuilder {
public:
    explicit PeBuilder(std::uint64_t image_base, bool dll = true, std::uint32_t entry_rva = kTextRva)
        : bytes_(kFileSize, 0) {
        put16(0x00, 0x5A4D);              // MZ
        put32(0x3C, kNtOffset);           // e_lfanew
        put32(kNtOffset, 0x00004550);     // PE\0\0
        put16(kNtOffset + 4, 0x8664);     // AMD64
        put16(kNtOffset + 6, 0);          // NumberOfSections, kept current by add_section()
        put16(kNtOffset + 20, 0xF0);      // SizeOfOptionalHeader
        put16(kNtOffset + 22, static_cast<std::uint16_t>(0x0022 | (dll ? 0x2000 : 0)));
        put16(kOptionalOffset, 0x020B);   // PE32+
        put32(kOptionalOffset + 16, entry_rva);
        put64(kOptionalOffset + 24, image_base);
        put32(kOptionalOffset + 32, kSectionAlignment);
        put32(kOptionalOffset + 36, kFileAlignment);
        put16(kOptionalOffset + 40, 6);   // MajorOperatingSystemVersion
        put16(kOptionalOffset + 48, 6);   // MajorSubsystemVersion
        put32(kOptionalOffset + 56, kSizeOfImage);
        put32(kOptionalOffset + 60, kSizeOfHeaders);
        put16(kOptionalOffset + 68, 3);   // Subsystem: console
        put16(kOptionalOffset + 70, 0x0100);  // NX_COMPAT
        put64(kOptionalOffset + 72, 0x100000);  // SizeOfStackReserve
        put64(kOptionalOffset + 80, 0x1000);    // SizeOfStackCommit
        put64(kOptionalOffset + 88, 0x100000);  // SizeOfHeapReserve
        put64(kOptionalOffset + 96, 0x1000);    // SizeOfHeapCommit
        put32(kOptionalOffset + 108, 16);  // NumberOfRvaAndSizes
    }

    // Adds a section at the next free section-table slot. The raw bytes are a deterministic pattern so a copy at the
    // wrong offset, a copy of the wrong length or a missed copy is visible byte by byte. Tables are written afterwards
    // with the put* helpers, which also zero every terminator the loader and the decoder require.
    void add_section(const char* name, std::uint32_t va, std::uint32_t vsize, std::uint32_t raw_ptr,
                     std::uint32_t raw_size, std::uint32_t characteristics) {
        const std::size_t index = sections_.size();
        if (index >= 16) throw std::runtime_error("fixture: too many sections");
        if (raw_ptr > kFileSize || raw_size > kFileSize - raw_ptr)
            throw std::runtime_error("fixture: section raw data past the file");
        if (va < kSizeOfHeaders || static_cast<std::uint64_t>(va) + vsize > kSizeOfImage ||
            static_cast<std::uint64_t>(va) + raw_size > kSizeOfImage || vsize == 0)
            throw std::runtime_error("fixture: section outside the image");
        const std::uint32_t off = kSectionTable + static_cast<std::uint32_t>(index) * 40;
        std::memcpy(bytes_.data() + off, name, std::strlen(name));
        put32(off + 8, vsize);
        put32(off + 12, va);
        put32(off + 16, raw_size);
        put32(off + 20, raw_ptr);
        put32(off + 36, characteristics);
        put16(kNtOffset + 6, static_cast<std::uint16_t>(index + 1));
        for (std::uint32_t i = 0; i < raw_size; ++i)
            bytes_[raw_ptr + i] = static_cast<std::uint8_t>((raw_ptr * 37u + i * 11u + 0x5C) & 0xFFu);
        sections_.push_back({va, vsize, raw_ptr, raw_size});
    }

    void set_entry(std::uint32_t rva) { put32(kOptionalOffset + 16, rva); }

    void directory(unsigned index, std::uint32_t rva, std::uint32_t size) {
        if (index >= 16) throw std::runtime_error("fixture: bad directory index");
        put32(kOptionalOffset + 112 + index * 8, rva);
        put32(kOptionalOffset + 112 + index * 8 + 4, size);
    }

    void put8(std::uint32_t rva, std::uint8_t value) { write_bytes(rva, &value, 1); }
    void put16(std::uint32_t rva, std::uint16_t value) { write_bytes(rva, &value, 2); }
    void put32(std::uint32_t rva, std::uint32_t value) { write_bytes(rva, &value, 4); }
    void put64(std::uint32_t rva, std::uint64_t value) { write_bytes(rva, &value, 8); }

    void put_bytes(std::uint32_t rva, const void* data, std::size_t size) { write_bytes(rva, data, size); }

    // A NUL-terminated string, terminator included.
    void put_text(std::uint32_t rva, const char* text) {
        write_bytes(rva, text, std::strlen(text) + 1);
    }

    // A zero-filled range, for the terminators every table needs when the section's default fill is not zero.
    void zero(std::uint32_t rva, std::size_t size) {
        const std::uint32_t off = file_offset(rva, size);
        std::memset(bytes_.data() + off, 0, size);
    }

    std::uint32_t file_offset(std::uint32_t rva) const { return file_offset(rva, 1); }

    std::vector<std::uint8_t>& bytes() { return bytes_; }
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    struct Section {
        std::uint32_t va;
        std::uint32_t vsize;
        std::uint32_t raw_ptr;
        std::uint32_t raw_size;
    };

    std::uint32_t file_offset(std::uint32_t rva, std::size_t size) const {
        if (size == 0) size = 1;
        if (rva < kSizeOfHeaders) {
            if (static_cast<std::uint64_t>(rva) + size > kSizeOfHeaders)
                throw std::runtime_error("fixture: write crosses the end of the headers");
            return rva;
        }
        for (const auto& section : sections_) {
            if (rva >= section.va && static_cast<std::uint64_t>(rva) + size <=
                                          static_cast<std::uint64_t>(section.va) + section.raw_size)
                return section.raw_ptr + (rva - section.va);
        }
        throw std::runtime_error("fixture: RVA " + std::to_string(rva) + " is not file-backed");
    }

    void write_bytes(std::uint32_t rva, const void* data, std::size_t size) {
        const std::uint32_t off = file_offset(rva, size);
        std::memcpy(bytes_.data() + off, data, size);
    }

    std::vector<std::uint8_t> bytes_;
    std::vector<Section> sections_;
};

// The image as the loader will see it on disk.
struct Fixture {
    std::string filename;
    std::vector<std::uint8_t> bytes;
};

namespace layout {

// Header and section table layout shared by every fixture (see the header comment): three sections, all data inside
// the file at the offsets above.
inline void add_common_sections(PeBuilder& builder, std::uint32_t data_rva = kDataRva) {
    builder.add_section(".text", kTextRva, 0x180, 0x400, 0x200, kScnCode | kScnExecute | kScnRead);
    builder.add_section(".data", data_rva, 0x500, 0x600, 0x400, kScnInitializedData | kScnRead | kScnWrite);
    builder.add_section(".rdata", kRdataRva, 0x600, 0xA00, 0x600, kScnInitializedData | kScnRead);
}

inline void export_forwarder(PeBuilder& builder, std::uint32_t directory_rva, std::uint32_t size, std::uint32_t base,
                             std::uint32_t function_count, std::uint32_t name_count, std::uint32_t functions_rva,
                             std::uint32_t names_rva, std::uint32_t ordinals_rva, std::uint32_t module_name_rva) {
    builder.directory(0, directory_rva, size);
    builder.put32(directory_rva + 12, module_name_rva);
    builder.put32(directory_rva + 16, base);
    builder.put32(directory_rva + 20, function_count);
    builder.put32(directory_rva + 24, name_count);
    builder.put32(directory_rva + 28, functions_rva);
    builder.put32(directory_rva + 32, names_rva);
    builder.put32(directory_rva + 36, ordinals_rva);
}

// The TLS directory shared by the TLS fixtures: a 16-byte template, a zero-fill tail, an index slot and a callback
// table whose entries are absolute virtual addresses (so a relocated mapping has to fix them up).
inline void write_tls(PeBuilder& builder, std::uint64_t image_base, std::uint32_t directory_rva,
                      std::uint32_t template_rva, std::uint32_t index_rva, std::uint32_t callbacks_rva,
                      std::uint32_t zero_fill) {
    builder.directory(9, directory_rva, 40);
    builder.put64(directory_rva, image_base + template_rva);
    builder.put64(directory_rva + 8, image_base + template_rva + sizeof(kTlsTemplate));
    builder.put64(directory_rva + 16, image_base + index_rva);
    builder.put64(directory_rva + 24, image_base + callbacks_rva);
    builder.put32(directory_rva + 32, zero_fill);
    // The loader overwrites the index slot; the file value must not look like a valid index of the module.
    builder.put32(index_rva, 0xFFFFFFFFu);
    // Two callback entries: one image address and the NUL that terminates the table.
    builder.put64(callbacks_rva, image_base + kTextRva);
    builder.put64(callbacks_rva + 8, 0);
    builder.put_bytes(template_rva, kTlsTemplate, sizeof(kTlsTemplate));
}

// Two RUNTIME_FUNCTION entries of 12 bytes each, both naming a minimal UNWIND_INFO inside .rdata.
inline void write_exceptions(PeBuilder& builder, std::uint32_t directory_rva, std::uint32_t first_unwind_rva,
                             std::uint32_t second_unwind_rva) {
    builder.directory(3, directory_rva, 24);
    builder.put32(directory_rva + 0, kTextRva);
    builder.put32(directory_rva + 4, kTextRva + 0x10);
    builder.put32(directory_rva + 8, first_unwind_rva);
    builder.put32(directory_rva + 12, kTextRva + 0x10);
    builder.put32(directory_rva + 16, kTextRva + 0x20);
    builder.put32(directory_rva + 20, second_unwind_rva);
    // Version 1, no flags, no prologue, no unwind codes: the loader only checks that the 4 header bytes exist.
    const std::uint8_t unwind[4] = {1, 0, 0, 0};
    builder.put_bytes(first_unwind_rva, unwind, sizeof(unwind));
    builder.put_bytes(second_unwind_rva, unwind, sizeof(unwind));
}

}  // namespace layout

// A DLL without a relocation directory at its (already free) preferred base. It has an export by name, an alias export
// whose only entry is a forwarder, two imports from the test's shim (one by name, one by ordinal), the static TLS
// directory above, and an exception directory. Mapped at its preferred base the loader has nothing to relocate.
// `data_rva` lets the unaligned-section case move .data off the SectionAlignment grid (the table RVAs inside it then
// land at different file offsets, which the builder follows).
inline Fixture no_reloc_dll(std::uint64_t image_base, std::uint32_t data_rva = kDataRva) {
    PeBuilder builder(image_base);
    layout::add_common_sections(builder, data_rva);
    builder.set_entry(kTextRva);

    // Export directory at 0x3000 (size 0x100, so the forwarder string at 0x30A0 is inside it).
    layout::export_forwarder(builder, 0x3000, 0x100, 1, 2, 2, 0x3040, 0x3050, 0x3060, 0x3070);
    builder.put32(0x3040, kTextRva);   // function 0: Alpha
    builder.put32(0x3044, 0x30A0);     // function 1: forwarder string inside the export directory
    builder.put32(0x3050, 0x3088);     // name 0: "Alpha"
    builder.put32(0x3054, 0x3090);     // name 1: "Fwd"
    builder.put16(0x3060, 0);
    builder.put16(0x3062, 1);
    builder.put_text(0x3070, "selfcheck_no_reloc.dll");
    builder.put_text(0x3088, "Alpha");
    builder.put_text(0x3090, "Fwd");
    builder.put_text(0x30A0, "selfcheck.A");

    // Import directory at 0x3100: one descriptor (two shim symbols), one NUL descriptor, lookup table at 0x3180.
    builder.directory(1, 0x3100, 0x28);
    builder.put32(0x3100, 0x3180);  // OriginalFirstThunk
    builder.put32(0x310C, 0x31A0);  // Name
    builder.put32(0x3110, 0x2200);  // FirstThunk (IAT in .data)
    builder.zero(0x3114, 20);
    builder.put64(0x3180, 0x31C0);  // hint/name entry for "A"
    builder.put64(0x3188, 0x8000000000000007ull);  // shim ordinal 7
    builder.put64(0x3190, 0);
    builder.put16(0x31C0, 0);
    builder.put_text(0x31C2, "A");
    builder.put_text(0x31A0, "selfcheck.dll");
    builder.zero(0x2200, 16);  // IAT slots in .data, both overwritten by the loader

    layout::write_tls(builder, image_base, 0x3200, 0x3540, 0x3560, 0x3240, 8);
    layout::write_exceptions(builder, 0x3300, 0x3320, 0x3330);

    return {"selfcheck_no_reloc.dll", std::move(builder.bytes())};
}

// A provider DLL with four exports: a code export named "Alpha", a forwarder to the test shim, a forwarder to the
// dependency DLL below, and an ordinal-only code export (#4). No relocations: the loader must map it at its base.
inline Fixture provider_dll(std::uint64_t image_base) {
    PeBuilder builder(image_base);
    layout::add_common_sections(builder);
    builder.set_entry(kTextRva);

    layout::export_forwarder(builder, 0x3000, 0x100, 1, 4, 3, 0x3040, 0x3050, 0x3060, 0x3070);
    builder.put32(0x3040, kTextRva);  // function 0: Alpha
    builder.put32(0x3044, 0x30A8);    // function 1: forwarder "selfcheck.A"
    builder.put32(0x3048, 0x30B8);    // function 2: forwarder "selfcheck_dep.Alpha2"
    builder.put32(0x304C, kTextRva);  // function 3: ordinal-only code export #4
    builder.put32(0x3050, 0x3088);    // "Alpha"
    builder.put32(0x3054, 0x3090);    // "FwdShim"
    builder.put32(0x3058, 0x3098);    // "FwdBundled"
    builder.put16(0x3060, 0);
    builder.put16(0x3062, 1);
    builder.put16(0x3064, 2);
    builder.put_text(0x3070, "selfcheck_provider.dll");
    builder.put_text(0x3088, "Alpha");
    builder.put_text(0x3090, "FwdShim");
    builder.put_text(0x3098, "FwdBundled");
    builder.put_text(0x30A8, "selfcheck.A");
    builder.put_text(0x30B8, "selfcheck_dep.Alpha2");

    return {"selfcheck_provider.dll", std::move(builder.bytes())};
}

// The dependency the provider's second forwarder names, also without relocations: one code export "Alpha2".
inline Fixture dependency_dll(std::uint64_t image_base) {
    PeBuilder builder(image_base);
    layout::add_common_sections(builder);
    builder.set_entry(kTextRva);

    layout::export_forwarder(builder, 0x3000, 0x80, 1, 1, 1, 0x3040, 0x3050, 0x3060, 0x3070);
    builder.put32(0x3040, kTextRva);
    builder.put32(0x3050, 0x3080);
    builder.put16(0x3060, 0);
    builder.put_text(0x3070, "selfcheck_dep.dll");
    builder.put_text(0x3080, "Alpha2");

    return {"selfcheck_dep.dll", std::move(builder.bytes())};
}

// A DLL with a relocation directory: it imports from the provider (by name, by ordinal, directly and through both
// forwarders), from the test shim by ordinal, from a host system DLL by name and from a missing DLL (unimplemented).
// Its .reloc fixes a pointer in .text, one in .data, the four absolute addresses of the TLS directory and the first
// TLS callback. The TLS template and index exercise initialize_tls on a relocated mapping.
inline Fixture relocated_importer_dll(std::uint64_t image_base) {
    PeBuilder builder(image_base);
    layout::add_common_sections(builder);
    builder.set_entry(kTextRva);

    // Absolute pointers in .text and .data; both are relocation targets.
    builder.put64(0x1010, image_base + kTextRva);
    builder.put64(0x2240, image_base + kTextRva);

    // Import directory at 0x3000: four descriptors plus the NUL descriptor.
    builder.directory(1, 0x3000, 0x64);
    builder.put32(0x3000, 0x3080);  // provider.dll: OriginalFirstThunk
    builder.put32(0x300C, 0x3480);  // Name
    builder.put32(0x3010, 0x2200);  // FirstThunk
    builder.put32(0x3014, 0x30A8);  // selfcheck.dll: OriginalFirstThunk
    builder.put32(0x3020, 0x3497);  // Name
    builder.put32(0x3024, 0x2220);  // FirstThunk
    builder.put32(0x3028, 0x30B8);  // kernel32.dll: OriginalFirstThunk
    builder.put32(0x3034, 0x34A5);  // Name
    builder.put32(0x3038, 0x2228);  // FirstThunk
    builder.put32(0x303C, 0x30C8);  // nosuch.dll: OriginalFirstThunk
    builder.put32(0x3048, 0x34B2);  // Name
    builder.put32(0x304C, 0x2230);  // FirstThunk
    builder.zero(0x3050, 20);       // NUL descriptor terminating the table

    // Lookup tables (the IAT slots in .data are filled by the loader).
    builder.put64(0x3080, 0x3440);                     // provider!Alpha (by name)
    builder.put64(0x3088, 0x3448);                     // provider!FwdShim (by name)
    builder.put64(0x3090, 0x3452);                     // provider!FwdBundled (by name)
    builder.put64(0x3098, 0x8000000000000004ull);      // provider!#4 (by ordinal)
    builder.put64(0x30A0, 0);
    builder.put64(0x30A8, 0x8000000000000007ull);      // selfcheck.dll!#7 (by ordinal)
    builder.put64(0x30B0, 0);
    builder.put64(0x30B8, 0x345F);                     // kernel32.dll!GetCurrentProcessId (by name)
    builder.put64(0x30C0, 0);
    builder.put64(0x30C8, 0x3475);                     // nosuch.dll!Missing (by name)
    builder.put64(0x30D0, 0);
    builder.zero(0x2200, 56);  // the seven IAT slots in .data

    // Hint/name entries and DLL names, both in file-backed .rdata.
    builder.put16(0x3440, 0);
    builder.put_text(0x3442, "Alpha");
    builder.put16(0x3448, 0);
    builder.put_text(0x344A, "FwdShim");
    builder.put16(0x3452, 0);
    builder.put_text(0x3454, "FwdBundled");
    builder.put16(0x345F, 0);
    builder.put_text(0x3461, "GetCurrentProcessId");
    builder.put16(0x3475, 0);
    builder.put_text(0x3477, "Missing");
    builder.put_text(0x3480, "selfcheck_provider.dll");
    builder.put_text(0x3497, "selfcheck.dll");
    builder.put_text(0x34A5, "kernel32.dll");
    builder.put_text(0x34B2, "nosuch.dll");

    layout::write_tls(builder, image_base, 0x3200, 0x3540, 0x3560, 0x3240, 8);
    layout::write_exceptions(builder, 0x3300, 0x3320, 0x3330);

    // Relocations at 0x3400: DIR64 entries on the two pointers and the five TLS absolute addresses.
    builder.directory(5, 0x3400, 44);
    builder.put32(0x3400, 0x1000);  // block: page 0x1000
    builder.put32(0x3404, 12);
    builder.put16(0x3408, static_cast<std::uint16_t>((10u << 12) | 0x010));  // DIR64, RVA 0x1010
    builder.put16(0x340A, 0);                                                // ABSOLUTE filler
    builder.put32(0x340C, 0x2000);  // block: page 0x2000
    builder.put32(0x3410, 12);
    builder.put16(0x3414, static_cast<std::uint16_t>((10u << 12) | 0x240));  // DIR64, RVA 0x2240
    builder.put16(0x3416, 0);
    builder.put32(0x3418, 0x3000);  // block: page 0x3000
    builder.put32(0x341C, 20);
    builder.put16(0x3420, static_cast<std::uint16_t>((10u << 12) | 0x200));  // TLS StartAddressOfRawData
    builder.put16(0x3422, static_cast<std::uint16_t>((10u << 12) | 0x208));  // TLS EndAddressOfRawData
    builder.put16(0x3424, static_cast<std::uint16_t>((10u << 12) | 0x210));  // TLS AddressOfIndex
    builder.put16(0x3426, static_cast<std::uint16_t>((10u << 12) | 0x218));  // TLS AddressOfCallBacks
    builder.put16(0x3428, static_cast<std::uint16_t>((10u << 12) | 0x240));  // first TLS callback entry
    builder.put16(0x342A, 0);                                                // ABSOLUTE filler

    return {"selfcheck_importer.dll", std::move(builder.bytes())};
}

// A DLL whose only relocation is a 32-bit HIGHLOW entry on the dword at 0x2240. `stored` is the absolute value in the
// file: at the preferred base the loader applies nothing, and away from it the loader adds the load delta and refuses
// the image when the moved address no longer fits in 32 bits.
inline Fixture highlow_dll(std::uint64_t image_base, std::uint32_t stored) {
    PeBuilder builder(image_base);
    layout::add_common_sections(builder);
    builder.set_entry(kTextRva);
    builder.put32(0x2240, stored);
    builder.directory(5, 0x3400, 12);
    builder.put32(0x3400, 0x2000);
    builder.put32(0x3404, 12);
    builder.put16(0x3408, static_cast<std::uint16_t>((3u << 12) | 0x240));  // HIGHLOW at RVA 0x2240
    builder.put16(0x340A, 0);
    return {"selfcheck_highlow.dll", std::move(builder.bytes())};
}

}  // namespace loader_selfcheck
