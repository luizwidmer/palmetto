// Reusable, self-contained PE32+ file-image fixtures for loader/parser parity tests.
// Every byte is synthetic; this header deliberately contains no package or game data.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace palmetto_pe_loader_fixture {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint64_t kDefaultImageBase = 0x140000000ull;
inline constexpr std::uint32_t kImageSize = 0x4000;
inline constexpr std::uint32_t kNtOffset = 0x80;
inline constexpr std::uint32_t kOptionalOffset = 0x98;
inline constexpr std::uint32_t kSectionTableOffset = 0x188;
inline constexpr std::uint32_t kHeaderSize = 0x400;
inline constexpr std::uint32_t kTextRva = 0x1000;
inline constexpr std::uint32_t kExportDirectoryRva = 0x2000;
inline constexpr std::uint32_t kImportDirectoryRva = 0x2100;
inline constexpr std::uint32_t kRelocationDirectoryRva = 0x2300;
inline constexpr std::uint32_t kTlsDirectoryRva = 0x2340;
inline constexpr std::uint32_t kTlsCallbacksRva = 0x2390;
inline constexpr std::uint32_t kTlsTemplateRva = 0x23A0;
inline constexpr std::uint32_t kTlsIndexRva = 0x3000;
inline constexpr std::uint32_t kImageFileSize = 0x1800;

inline constexpr std::uint32_t kExportDirectorySize = 0x100;
inline constexpr std::uint32_t kImportDirectorySize = 3 * 20;
inline constexpr std::uint32_t kRelocationDirectorySize = 12 + 18;  // both relocation blocks, including TLS entries
inline constexpr std::uint32_t kTlsDirectorySize = 40;

inline std::size_t file_offset(std::uint32_t rva) {
    if (rva < 0x1000) return rva;  // the header RVAs used by these fixtures
    if (rva < 0x2000) return 0x400 + (rva - 0x1000);
    if (rva < 0x3000) return 0x600 + (rva - 0x2000);
    return 0x1600 + (rva - 0x3000);
}

inline void put_u16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

inline void put_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

inline void put_u64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

inline void put_rva_u16(Bytes& bytes, std::uint32_t rva, std::uint16_t value) {
    put_u16(bytes, file_offset(rva), value);
}

inline void put_rva_u32(Bytes& bytes, std::uint32_t rva, std::uint32_t value) {
    put_u32(bytes, file_offset(rva), value);
}

inline void put_rva_u64(Bytes& bytes, std::uint32_t rva, std::uint64_t value) {
    put_u64(bytes, file_offset(rva), value);
}

inline void put_text(Bytes& bytes, std::size_t offset, const char* text) {
    std::memcpy(bytes.data() + offset, text, std::strlen(text) + 1);
}

inline void put_rva_text(Bytes& bytes, std::uint32_t rva, const char* text) {
    put_text(bytes, file_offset(rva), text);
}

inline void put_directory(Bytes& bytes, unsigned index, std::uint32_t rva, std::uint32_t size) {
    const std::size_t offset = kOptionalOffset + 112 + index * 8;
    put_u32(bytes, offset, rva);
    put_u32(bytes, offset + 4, size);
}

inline void put_section(Bytes& bytes, unsigned index, const char* name, std::uint32_t virtual_address,
                        std::uint32_t virtual_size, std::uint32_t raw_pointer, std::uint32_t raw_size,
                        std::uint32_t characteristics) {
    const std::size_t offset = kSectionTableOffset + index * 40;
    std::memcpy(bytes.data() + offset, name, std::min<std::size_t>(std::strlen(name), 8));
    put_u32(bytes, offset + 8, virtual_size);
    put_u32(bytes, offset + 12, virtual_address);
    put_u32(bytes, offset + 16, raw_size);
    put_u32(bytes, offset + 20, raw_pointer);
    put_u32(bytes, offset + 36, characteristics);
}

// A three-section x64 DLL/EXE with aliases, name-only and ordinal-only exports, a forwarder,
// imports by name and ordinal, DIR64/ABSOLUTE fixups, and (for DLL fixtures) static TLS.
// The expected PE layout is intentionally simple: .text at RVA 0x1000, .rdata at 0x2000,
// .data at 0x3000; all tables and addresses are file-backed.
inline Bytes make_image(std::uint64_t image_base = kDefaultImageBase, bool is_dll = true, bool with_tls = true) {
    Bytes bytes(kImageFileSize, 0);

    // DOS + NT headers (PE32+); the layout is written field-by-field so this builder is reusable
    // without importing Windows headers into parser-only tests.
    put_u16(bytes, 0, 0x5A4D);  // MZ
    put_u32(bytes, 0x3C, kNtOffset);
    put_u32(bytes, kNtOffset, 0x00004550);  // PE\0\0
    put_u16(bytes, kNtOffset + 4, 0x8664);   // AMD64
    put_u16(bytes, kNtOffset + 6, 3);        // sections
    put_u16(bytes, kNtOffset + 20, 0xF0);    // PE32+ optional header size
    put_u16(bytes, kNtOffset + 22, static_cast<std::uint16_t>(0x0022 | (is_dll ? 0x2000 : 0)));
    put_u16(bytes, kOptionalOffset, 0x020B);  // PE32+
    put_u32(bytes, kOptionalOffset + 16, kTextRva);  // AddressOfEntryPoint
    put_u64(bytes, kOptionalOffset + 24, image_base);
    put_u32(bytes, kOptionalOffset + 32, 0x1000);  // SectionAlignment
    put_u32(bytes, kOptionalOffset + 36, 0x200);   // FileAlignment
    put_u32(bytes, kOptionalOffset + 56, kImageSize);
    put_u32(bytes, kOptionalOffset + 60, kHeaderSize);
    put_u16(bytes, kOptionalOffset + 68, 3);       // console subsystem
    put_u32(bytes, kOptionalOffset + 108, 16);     // NumberOfRvaAndSizes

    put_section(bytes, 0, ".text", 0x1000, 0x200, 0x400, 0x200, 0x60000020);
    put_section(bytes, 1, ".rdata", 0x2000, 0x1000, 0x600, 0x1000, 0x40000040);
    put_section(bytes, 2, ".data", 0x3000, 0x200, 0x1600, 0x200, 0xC0000040);
    std::memset(bytes.data() + 0x400, 0x90, 0x200);
    bytes[0x400] = 0xC3;
    put_rva_u64(bytes, 0x1010, image_base + 0x1020);  // relocation target in .text

    // Exports: Alpha and Alias are aliases at ordinal 5; Fwd and ForwardAlias are
    // forwarder aliases at ordinal 6; ordinal 7 is unnamed.
    constexpr std::uint32_t kExportFunctionsRva = 0x2040;
    constexpr std::uint32_t kExportNamesRva = 0x2050;
    constexpr std::uint32_t kExportOrdinalsRva = 0x2060;
    put_rva_u32(bytes, 0x2000 + 12, 0x2070);  // export DLL name
    put_rva_u32(bytes, 0x2000 + 16, 5);       // Base ordinal
    put_rva_u32(bytes, 0x2000 + 20, 3);       // NumberOfFunctions
    put_rva_u32(bytes, 0x2000 + 24, 4);       // NumberOfNames
    put_rva_u32(bytes, 0x2000 + 28, kExportFunctionsRva);
    put_rva_u32(bytes, 0x2000 + 32, kExportNamesRva);
    put_rva_u32(bytes, 0x2000 + 36, kExportOrdinalsRva);
    put_rva_u32(bytes, kExportFunctionsRva + 0, 0x1000);
    put_rva_u32(bytes, kExportFunctionsRva + 4, 0x20C0);  // forwarder string in export directory
    put_rva_u32(bytes, kExportFunctionsRva + 8, 0x1010);
    put_rva_u32(bytes, kExportNamesRva + 0, 0x2080);
    put_rva_u32(bytes, kExportNamesRva + 4, 0x2088);
    put_rva_u32(bytes, kExportNamesRva + 8, 0x2090);
    put_rva_u32(bytes, kExportNamesRva + 12, 0x2098);
    put_rva_u16(bytes, kExportOrdinalsRva + 0, 0);
    put_rva_u16(bytes, kExportOrdinalsRva + 2, 0);  // true alias of Alpha
    put_rva_u16(bytes, kExportOrdinalsRva + 4, 1);
    put_rva_u16(bytes, kExportOrdinalsRva + 6, 1);  // true alias of Fwd
    put_rva_text(bytes, 0x2070, "parity.dll");
    put_rva_text(bytes, 0x2080, "Alpha");
    put_rva_text(bytes, 0x2088, "Alias");
    put_rva_text(bytes, 0x2090, "Fwd");
    put_rva_text(bytes, 0x2098, "ForwardAlias");
    put_rva_text(bytes, 0x20C0, "palmetto_parity_missing.NoSuchSymbol");
    put_directory(bytes, 0, kExportDirectoryRva, kExportDirectorySize);

    // Imports: a known host API by name, and an ordinal from a deliberately missing synthetic DLL.
    // No game or package DLL is involved; the latter resolves to the loader's ordinary failure thunk.
    constexpr std::uint32_t kImportNameThunk = 0x21C0;
    constexpr std::uint32_t kImportNameIat = 0x21F0;
    constexpr std::uint32_t kImportOrdinalThunk = 0x2220;
    constexpr std::uint32_t kImportOrdinalIat = 0x2240;
    constexpr std::uint32_t kImportByNameRva = 0x2260;
    put_rva_u32(bytes, 0x2100 + 0, kImportNameThunk);
    put_rva_u32(bytes, 0x2100 + 12, 0x2170);
    put_rva_u32(bytes, 0x2100 + 16, kImportNameIat);
    put_rva_u32(bytes, 0x2114 + 0, kImportOrdinalThunk);
    put_rva_u32(bytes, 0x2114 + 12, 0x2190);
    put_rva_u32(bytes, 0x2114 + 16, kImportOrdinalIat);
    // The third descriptor at RVA 0x2128 is already zero.
    put_rva_text(bytes, 0x2170, "kernel32.dll");
    put_rva_text(bytes, 0x2190, "palmetto_parity_missing.dll");
    put_rva_u64(bytes, kImportNameThunk, kImportByNameRva);
    put_rva_u64(bytes, kImportNameIat, kImportByNameRva);
    put_rva_u16(bytes, kImportByNameRva, 0);  // hint
    put_rva_text(bytes, kImportByNameRva + 2, "GetCurrentProcessId");
    put_rva_u64(bytes, kImportOrdinalThunk, 0x800000000000004Dull);  // ordinal 77
    put_rva_u64(bytes, kImportOrdinalIat, 0x800000000000004Dull);
    put_directory(bytes, 1, kImportDirectoryRva, kImportDirectorySize);

    // Relocations: a DIR64 target in .text plus an ABSOLUTE filler. The DLL also
    // relocates the TLS directory's four VA fields and its first callback pointer.
    put_rva_u32(bytes, 0x2300 + 0, 0x1000);
    put_rva_u32(bytes, 0x2300 + 4, 12);
    put_rva_u16(bytes, 0x2300 + 8, static_cast<std::uint16_t>((10u << 12) | 0x010));
    put_rva_u16(bytes, 0x2300 + 10, 0);  // IMAGE_REL_BASED_ABSOLUTE
    if (with_tls) {
        put_rva_u32(bytes, 0x230C + 0, 0x2000);
        put_rva_u32(bytes, 0x230C + 4, 18);
        put_rva_u16(bytes, 0x230C + 8, static_cast<std::uint16_t>((10u << 12) | 0x340));
        put_rva_u16(bytes, 0x230C + 10, static_cast<std::uint16_t>((10u << 12) | 0x348));
        put_rva_u16(bytes, 0x230C + 12, static_cast<std::uint16_t>((10u << 12) | 0x350));
        put_rva_u16(bytes, 0x230C + 14, static_cast<std::uint16_t>((10u << 12) | 0x358));
        put_rva_u16(bytes, 0x230C + 16, static_cast<std::uint16_t>((10u << 12) | 0x390));
    }
    put_directory(bytes, 5, kRelocationDirectoryRva, with_tls ? kRelocationDirectorySize : 12);

    // File-backed TLS template, writable _tls_index in .data, and one callback. For the EXE
    // variant the same bytes remain ordinary data but the TLS directory is absent.
    if (with_tls) {
        put_rva_u64(bytes, kTlsDirectoryRva + 0, image_base + kTlsTemplateRva);
        put_rva_u64(bytes, kTlsDirectoryRva + 8, image_base + kTlsTemplateRva + 8);
        put_rva_u64(bytes, kTlsDirectoryRva + 16, image_base + kTlsIndexRva);
        put_rva_u64(bytes, kTlsDirectoryRva + 24, image_base + kTlsCallbacksRva);
        put_rva_u32(bytes, kTlsDirectoryRva + 32, 4);  // zero fill
        put_rva_u32(bytes, kTlsDirectoryRva + 36, 0);  // characteristics
        put_rva_u64(bytes, kTlsCallbacksRva, image_base + kTextRva);
        put_rva_u64(bytes, kTlsCallbacksRva + 8, 0);
        put_rva_text(bytes, kTlsTemplateRva, "TLS-data");  // exactly eight template bytes
        put_directory(bytes, 9, kTlsDirectoryRva, kTlsDirectorySize);
    }

    return bytes;
}

// Existing Loader tests intentionally keep a named export with an empty address-table slot absent.
// This variant is used to compare that legacy behavior with Image::parse's current treatment.
inline Bytes make_empty_named_export_image(std::uint64_t image_base = kDefaultImageBase) {
    Bytes bytes = make_image(image_base, /*is_dll=*/true, /*with_tls=*/false);
    put_rva_u32(bytes, 0x2040 + 2 * 4, 0);  // ordinal 7 is an empty function slot
    put_rva_u32(bytes, 0x2050 + 2 * 4, 0x20B0);
    put_rva_u16(bytes, 0x2060 + 2 * 2, 2);  // the name points at the now-empty third function slot
    put_rva_text(bytes, 0x20B0, "Empty");
    return bytes;
}

}  // namespace palmetto_pe_loader_fixture
