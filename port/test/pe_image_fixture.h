// Shared synthetic PE32+ file image for the parser and mapper tests. No game data.
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace pe_image_fixture {

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

    // Convert an RVA in the fixture's headers, .text, or .rdata to its file offset.
    std::uint32_t file_of(std::uint32_t rva) const {
        if (rva < 0x1000) return rva;
        if (rva < 0x2000) return 0x400 + (rva - 0x1000);
        return 0x600 + (rva - 0x2000);
    }

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
        set_relocation_entries(0x1000, {static_cast<std::uint16_t>((10u << 12) | 0x10), 0});
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

    void set_relocation_entries(std::uint32_t page_rva, std::initializer_list<std::uint16_t> entries) {
        const std::uint32_t block_file_offset = file_of(0x2200);
        const std::uint32_t block_size = 8 + static_cast<std::uint32_t>(entries.size()) * 2;
        u32(block_file_offset, page_rva);
        u32(block_file_offset + 4, block_size);
        std::uint32_t index = 0;
        for (const auto entry : entries) u16(block_file_offset + 8 + index++ * 2, entry);
        directory(5, 0x2200, block_size);
    }
};

}  // namespace pe_image_fixture
