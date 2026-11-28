#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <cwchar>

#include "../src/pe_image.h"
#include "../src/pe_loader.h"
#include "../src/pe_mapper.h"
#include "loader_selfcheck_fixture.h"

// Synthetic mapped-image parity: the old Loader and new mapper receive byte-identical files.
// Nothing in this test reads package or game data, and no image code is executed.
namespace {

namespace fs = std::filesystem;
namespace fixture = loader_selfcheck;
using rt::pe::Image;
using rt::pe::mapper::Allocator;
using rt::pe::mapper::MappedImage;
using rt::pe::mapper::PageProtection;
using rt::pe::mapper::Protector;

constexpr std::uint32_t kPageSize = 0x1000;
constexpr std::uint32_t kRawText = 0x400;
constexpr std::uint32_t kRawData = 0x600;
constexpr std::uint32_t kRawRdata = 0xA00;
constexpr std::uint32_t kDefaultTextFlags = fixture::kScnCode | fixture::kScnExecute | fixture::kScnRead;
constexpr std::uint32_t kDefaultDataFlags = fixture::kScnInitializedData | fixture::kScnRead | fixture::kScnWrite;
constexpr std::uint32_t kDefaultRdataFlags = fixture::kScnInitializedData | fixture::kScnRead;

int g_checks = 0;
int g_failures = 0;
unsigned g_file_sequence = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

bool contains(const std::string& text, const std::string& fragment) {
    return text.find(fragment) != std::string::npos;
}

void write_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) throw std::runtime_error("fixture write past end");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) throw std::runtime_error("fixture write past end");
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) throw std::runtime_error("fixture write past end");
    for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || sizeof(std::uint32_t) > bytes.size() - offset) throw std::runtime_error("fixture read past end");
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || sizeof(std::uint64_t) > bytes.size() - offset) throw std::runtime_error("fixture read past end");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    return value;
}

// Use the existing hand-written self-check PE builder, then deliberately make .data name raw bytes at offset zero.
fixture::Fixture make_fixture(std::uint64_t image_base, const char* name, std::uint32_t text_flags = kDefaultTextFlags,
                              std::uint32_t data_flags = kDefaultDataFlags, std::uint32_t rdata_flags = kDefaultRdataFlags,
                              bool with_relocation = false, std::uint16_t relocation_type = 10,
                              std::uint32_t relocation_target = 0x1010) {
    fixture::PeBuilder builder(image_base);
    builder.add_section(".text", 0x1000, 0x180, kRawText, 0x200, text_flags);
    builder.add_section(".data", 0x2000, 0x500, kRawData, 0x400, data_flags);
    builder.add_section(".rdata", 0x3000, 0x600, kRawRdata, 0x600, rdata_flags);
    builder.set_entry(0x1000);
    if (with_relocation) {
        builder.put64(relocation_target, image_base + 0x1234);
        builder.directory(5, 0x3400, 12);
        builder.put32(0x3400, relocation_target & 0xFFFFF000u);
        builder.put32(0x3404, 12);
        builder.put16(0x3408, static_cast<std::uint16_t>((relocation_type << 12) | (relocation_target & 0xFFFu)));
        builder.put16(0x340A, 0);
    }
    auto bytes = builder.bytes();
    // A non-zero SizeOfRawData with PointerToRawData == 0 has no file-backed section bytes.
    write_u32(bytes, fixture::kSectionTable + 40 + 20, 0);
    return {name, std::move(bytes)};
}

void set_section_rva(fixture::Fixture& image, unsigned index, std::uint32_t rva) {
    write_u32(image.bytes, fixture::kSectionTable + static_cast<std::size_t>(index) * 40 + 12, rva);
}

void set_section_raw_pointer(fixture::Fixture& image, unsigned index, std::uint32_t pointer) {
    write_u32(image.bytes, fixture::kSectionTable + static_cast<std::size_t>(index) * 40 + 20, pointer);
}

void set_section_raw_size(fixture::Fixture& image, unsigned index, std::uint32_t size) {
    write_u32(image.bytes, fixture::kSectionTable + static_cast<std::size_t>(index) * 40 + 16, size);
}

void set_optional_u32(fixture::Fixture& image, std::size_t offset, std::uint32_t value) {
    write_u32(image.bytes, fixture::kOptionalOffset + offset, value);
}

void shorten_optional_header(fixture::Fixture& image) {
    constexpr std::size_t short_size = 112;
    constexpr std::size_t section_count = 3;
    const std::size_t short_table = fixture::kOptionalOffset + short_size;
    std::memmove(image.bytes.data() + short_table, image.bytes.data() + fixture::kSectionTable, section_count * 40);
    write_u16(image.bytes, fixture::kNtOffset + 4 + 16, static_cast<std::uint16_t>(short_size));
    write_u32(image.bytes, fixture::kOptionalOffset + 108, 0);  // no directories after the short optional header
}

bool range_free(std::uint64_t base, std::uint64_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(base)), &info, sizeof(info))) return false;
    if (info.State != MEM_FREE) return false;
    const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return base <= region_end && size <= region_end - base;
}

std::uint64_t inherited_reservation_base() {
    wchar_t marker[96] = {};
    const DWORD length = GetEnvironmentVariableW(L"PALMETTO_BASE_RESERVED", marker, static_cast<DWORD>(std::size(marker)));
    if (!length || length >= std::size(marker)) return 0;
    unsigned long long base = 0;
    unsigned long long size = 0;
    if (swscanf_s(marker, L"%llx:%llx", &base, &size) != 2 || !size) return 0;
    return static_cast<std::uint64_t>(base);
}

std::uint64_t free_image_base() {
    constexpr std::array<std::uint64_t, 10> candidates = {
        0x140000000ull, 0x150000000ull, 0x160000000ull, 0x170000000ull, 0x180000000ull,
        0x190000000ull, 0x1A0000000ull, 0x1B0000000ull, 0x1C0000000ull, 0x1D0000000ull};
    const std::uint64_t reserved_base = inherited_reservation_base();
    for (const std::uint64_t candidate : candidates) {
        // Do not let the old Loader treat a test blocker as its inherited base reservation.
        if (candidate == reserved_base || !range_free(candidate, fixture::kSizeOfImage)) continue;
        void* probe = VirtualAlloc(reinterpret_cast<void*>(static_cast<std::uintptr_t>(candidate)), fixture::kSizeOfImage,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!probe) continue;
        const bool exact = reinterpret_cast<std::uintptr_t>(probe) == candidate;
        VirtualFree(probe, 0, MEM_RELEASE);
        if (exact) return candidate;
    }
    void* probe = VirtualAlloc(nullptr, fixture::kSizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!probe) throw std::runtime_error("no allocatable preferred address for a synthetic PE image");
    const auto candidate = reinterpret_cast<std::uintptr_t>(probe);
    VirtualFree(probe, 0, MEM_RELEASE);
    return candidate;
}

class HeldBase {
public:
    HeldBase(std::uint64_t base, std::uint32_t size)
        : address_(VirtualAlloc(reinterpret_cast<void*>(static_cast<std::uintptr_t>(base)), size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS)) {
        if (!address_ || reinterpret_cast<std::uintptr_t>(address_) != base)
            throw std::runtime_error("cannot reserve synthetic preferred base");
    }
    ~HeldBase() { if (address_) VirtualFree(address_, 0, MEM_RELEASE); }
    HeldBase(const HeldBase&) = delete;
    HeldBase& operator=(const HeldBase&) = delete;

private:
    void* address_ = nullptr;
};

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() / "palmetto_pe_mapper_parity" / std::to_string(GetCurrentProcessId());
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
        if (ec || !fs::is_directory(path_)) throw std::runtime_error("cannot create synthetic PE fixture directory");
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

fs::path write_fixture(const fs::path& dir, const fixture::Fixture& image) {
    const fs::path path = dir / (std::to_string(++g_file_sequence) + "_" + image.filename);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create synthetic PE fixture file");
    out.write(reinterpret_cast<const char*>(image.bytes.data()), static_cast<std::streamsize>(image.bytes.size()));
    if (!out) throw std::runtime_error("cannot write synthetic PE fixture file");
    return path;
}

std::vector<DWORD> query_page_protections(const std::uint8_t* base, std::uint32_t size, const std::string& label) {
    std::vector<DWORD> protections;
    for (std::uint32_t rva = 0; rva < size; rva += kPageSize) {
        MEMORY_BASIC_INFORMATION info{};
        const void* address = base + rva;
        const SIZE_T queried = VirtualQuery(address, &info, sizeof(info));
        const bool valid = queried == sizeof(info) && info.State == MEM_COMMIT && info.AllocationBase == base;
        check(valid, label + ": VirtualQuery returns the committed image page at RVA " + std::to_string(rva));
        protections.push_back(valid ? info.Protect : 0);
    }
    return protections;
}

void check_plan_matches_query(const MappedImage& mapped, const std::vector<DWORD>& pages, const std::string& label) {
    for (const auto& region : mapped.protect_plan) {
        if (!region.size) {
            check(false, label + ": protection plan has an empty region " + region.name);
            continue;
        }
        const std::uint64_t end = static_cast<std::uint64_t>(region.rva) + region.size;
        check(end <= mapped.size, label + ": protection plan region stays inside the mapped image: " + region.name);
        if (end > mapped.size) continue;
        const std::uint32_t first_page = region.rva & ~(kPageSize - 1);
        const std::uint64_t page_end = (end + kPageSize - 1) & ~static_cast<std::uint64_t>(kPageSize - 1);
        const DWORD expected = static_cast<DWORD>(region.protection);
        for (std::uint64_t page = first_page; page < page_end; page += kPageSize) {
            const std::size_t index = static_cast<std::size_t>(page / kPageSize);
            const bool found = index < pages.size() && pages[index] == expected;
            check(found, label + ": VirtualQuery PAGE_* matches plan " + region.name + " at page RVA " +
                        std::to_string(page));
        }
    }
}

struct Snapshot {
    std::uint64_t base = 0;
    std::uint32_t size = 0;
    std::uint32_t entry_rva = 0;
    bool relocated = false;
    std::vector<std::uint8_t> bytes;
    std::vector<DWORD> page_protections;
};

DWORD expected_section_protection(const rt::pe::Section& section) {
    const bool execute = (section.characteristics & fixture::kScnExecute) != 0;
    const bool read = (section.characteristics & fixture::kScnRead) != 0;
    const bool write = (section.characteristics & fixture::kScnWrite) != 0;
    if (execute) return write ? PAGE_EXECUTE_READWRITE : (read ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    if (write) return PAGE_READWRITE;
    return read ? PAGE_READONLY : PAGE_NOACCESS;
}

void check_expected_protections(const Image& image, const Snapshot& old_image, const Snapshot& new_image,
                               const std::string& label) {
    check(!old_image.page_protections.empty() && old_image.page_protections[0] == PAGE_READONLY &&
              !new_image.page_protections.empty() && new_image.page_protections[0] == PAGE_READONLY,
          label + ": VirtualQuery confirms PAGE_READONLY headers");
    for (const auto& section : image.sections()) {
        const std::uint64_t extent = section.virtual_size ? section.virtual_size : section.raw_size;
        if (!extent) continue;
        const std::uint64_t first_page = section.virtual_address & ~(kPageSize - 1);
        const std::uint64_t end = (static_cast<std::uint64_t>(section.virtual_address) + extent + kPageSize - 1) &
                                  ~static_cast<std::uint64_t>(kPageSize - 1);
        const DWORD expected = expected_section_protection(section);
        for (std::uint64_t page = first_page; page < end; page += kPageSize) {
            const std::size_t index = static_cast<std::size_t>(page / kPageSize);
            const bool old_matches = index < old_image.page_protections.size() &&
                                     old_image.page_protections[index] == expected;
            const bool new_matches = index < new_image.page_protections.size() &&
                                     new_image.page_protections[index] == expected;
            check(old_matches && new_matches, label + ": legacy and mapper VirtualQuery match section PAGE_* at RVA " +
                                                  std::to_string(page));
        }
    }
}

struct VirtualAllocator final : Allocator {
    explicit VirtualAllocator(std::uint64_t hint) : fallback_hint(hint) {}
    ~VirtualAllocator() override { if (allocation) VirtualFree(allocation, 0, MEM_RELEASE); }
    bool enforces_windows_image_base_alignment() const noexcept override { return true; }
    VirtualAllocator(const VirtualAllocator&) = delete;
    VirtualAllocator& operator=(const VirtualAllocator&) = delete;

    std::uint8_t* allocate(std::uint64_t preferred_base, std::size_t size, bool allow_rebase) override {
        ++calls;
        if (preferred_base <= std::numeric_limits<std::uintptr_t>::max()) {
            allocation = static_cast<std::uint8_t*>(VirtualAlloc(
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(preferred_base)), size,
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
        if (!allocation && allow_rebase && fallback_hint) {
            allocation = static_cast<std::uint8_t*>(VirtualAlloc(
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(fallback_hint)), size,
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            used_hint = allocation && reinterpret_cast<std::uintptr_t>(allocation) == fallback_hint;
        }
        if (!allocation && allow_rebase)
            allocation = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        return allocation;
    }

    std::uint64_t fallback_hint = 0;
    std::uint8_t* allocation = nullptr;
    unsigned calls = 0;
    bool used_hint = false;
};

struct VirtualProtector final : Protector {
    struct Call { std::uint8_t* address; std::size_t size; PageProtection protection; };
    bool protect(std::uint8_t* address, std::size_t size, PageProtection protection) override {
        calls.push_back({address, size, protection});
        DWORD previous = 0;
        return VirtualProtect(address, size, static_cast<DWORD>(protection), &previous) != 0;
    }
    std::vector<Call> calls;
};

Snapshot capture_snapshot(std::uint8_t* base, std::uint32_t size, std::uint32_t entry_rva, bool relocated,
                         const std::string& label) {
    Snapshot snapshot;
    snapshot.base = reinterpret_cast<std::uintptr_t>(base);
    snapshot.size = size;
    snapshot.entry_rva = entry_rva;
    snapshot.relocated = relocated;
    snapshot.page_protections = query_page_protections(base, size, label);
    DWORD previous = 0;
    if (!VirtualProtect(base, size, PAGE_READWRITE, &previous)) {
        check(false, label + ": temporarily open synthetic mapping for a byte snapshot");
        return snapshot;
    }
    snapshot.bytes.assign(base, base + size);
    return snapshot;
}

Snapshot map_with_legacy_loader(const fs::path& dir, const fs::path& path, const Image& image, const std::string& label) {
    rt::Loader loader(dir);
    rt::Module& module = loader.load(path);
    check(module.preferred_base == image.image_base(), label + ": legacy Module preferred base matches ImageBase");
    check(module.size == image.size_of_image(), label + ": legacy Module size matches SizeOfImage");
    check(module.entry_rva == image.entry_point(), label + ": legacy Module entry RVA matches AddressOfEntryPoint");
    return capture_snapshot(module.base, module.size, module.entry_rva, module.relocated, label + " old Loader");
}

Snapshot map_with_new_mapper(const Image& image, std::uint64_t fallback_hint, const std::string& label) {
    VirtualAllocator allocator(fallback_hint);
    VirtualProtector protector;
    const MappedImage mapped = rt::pe::mapper::map_image(image, allocator, protector);
    check(mapped.base != nullptr, label + ": mapper returned a mapped base");
    check(mapped.size == image.size_of_image(), label + ": MappedImage size matches SizeOfImage");
    check(mapped.relocated == (reinterpret_cast<std::uintptr_t>(mapped.base) != image.image_base()),
          label + ": MappedImage relocated flag matches its actual base");
    check(mapped.sections.size() == image.sections().size(), label + ": mapped section count matches parsed section count");
    const std::size_t count = (std::min)(mapped.sections.size(), image.sections().size());
    for (std::size_t i = 0; i < count; ++i) {
        check(mapped.sections[i].name == image.sections()[i].name &&
                  mapped.sections[i].rva == image.sections()[i].virtual_address &&
                  mapped.sections[i].characteristics == image.sections()[i].characteristics,
              label + ": mapped section identity matches parsed section " + std::to_string(i));
    }
    check(protector.calls.size() == mapped.protect_plan.size(), label + ": protector applies every planned region");
    const std::size_t call_count = (std::min)(protector.calls.size(), mapped.protect_plan.size());
    for (std::size_t i = 0; i < call_count; ++i) {
        const auto& region = mapped.protect_plan[i];
        const auto& call = protector.calls[i];
        check(call.address == mapped.base + region.rva && call.size == region.size &&
                  call.protection == region.protection,
              label + ": Protector call matches plan region " + region.name);
    }
    const auto pages = query_page_protections(mapped.base, mapped.size, label + " new mapper");
    check_plan_matches_query(mapped, pages, label);
    Snapshot snapshot = capture_snapshot(mapped.base, mapped.size, image.entry_point(), mapped.relocated,
                                         label + " new mapper");
    check(allocator.calls == 1, label + ": mapper uses one allocation attempt");
    if (fallback_hint && mapped.relocated)
        check(reinterpret_cast<std::uintptr_t>(mapped.base) == fallback_hint && allocator.used_hint,
              label + ": mapper reacquires the released legacy fallback base");
    return snapshot;
}

void normalize_relocations(std::vector<std::uint8_t>& bytes, const Image& image, std::uint64_t actual_base) {
    const std::uint64_t delta = actual_base - image.image_base();
    for (const auto& relocation : image.relocations()) {
        if (relocation.type == rt::pe::Relocation::kDir64 && relocation.rva <= bytes.size() &&
            sizeof(std::uint64_t) <= bytes.size() - relocation.rva) {
            std::uint64_t value = 0;
            std::memcpy(&value, bytes.data() + relocation.rva, sizeof(value));
            value -= delta;
            std::memcpy(bytes.data() + relocation.rva, &value, sizeof(value));
        } else if (relocation.type == rt::pe::Relocation::kHighLow && relocation.rva <= bytes.size() &&
                   sizeof(std::uint32_t) <= bytes.size() - relocation.rva) {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes.data() + relocation.rva, sizeof(value));
            value -= static_cast<std::uint32_t>(delta);
            std::memcpy(bytes.data() + relocation.rva, &value, sizeof(value));
        }
    }
}

void compare_snapshots(const Snapshot& old_image, const Snapshot& new_image, const Image& image, bool expect_relocated,
                       const std::string& label) {
    check(old_image.base != 0 && new_image.base != 0, label + ": both mappers report non-null image bases");
    check(old_image.size == new_image.size && old_image.size == image.size_of_image(),
          label + ": mapped base extents and SizeOfImage agree");
    check(old_image.entry_rva == new_image.entry_rva && old_image.entry_rva == image.entry_point(),
          label + ": mapped entry RVA agrees with AddressOfEntryPoint");
    check(old_image.base + old_image.entry_rva == new_image.base + new_image.entry_rva &&
              new_image.entry_rva == image.entry_point(),
          label + ": legacy and new mapped entry addresses agree");
    check(old_image.relocated == new_image.relocated && old_image.relocated == expect_relocated,
          label + ": both mappings agree whether relocation occurred");
    if (expect_relocated) {
        check(old_image.base != image.image_base() && new_image.base != image.image_base(),
              label + ": both mappings use a fallback base when preferred base is held");
        check(old_image.base == new_image.base, label + ": mapped bases agree after reusing the released fallback address");
    } else {
        check(old_image.base == image.image_base() && new_image.base == image.image_base(),
              label + ": both mappings use the free preferred base");
        check(old_image.base == new_image.base, label + ": mapped base agrees at the preferred address");
    }
    check(old_image.page_protections == new_image.page_protections,
          label + ": old Loader and new mapper VirtualQuery protections agree page by page");
    auto old_bytes = old_image.bytes;
    auto new_bytes = new_image.bytes;
    normalize_relocations(old_bytes, image, old_image.base);
    normalize_relocations(new_bytes, image, new_image.base);
    if (old_bytes.size() != new_bytes.size()) {
        check(false, label + ": mapped byte snapshots have equal length");
    } else {
        std::size_t first_difference = old_bytes.size();
        for (std::size_t i = 0; i < old_bytes.size(); ++i) {
            if (old_bytes[i] != new_bytes[i]) { first_difference = i; break; }
        }
        check(first_difference == old_bytes.size(),
              label + (first_difference == old_bytes.size()
                          ? ": all mapped bytes agree after normalizing the load-base relocation delta"
                          : ": mapped byte mismatch at RVA " + std::to_string(first_difference)));
    }
}

void check_relocation_results(const Snapshot& old_image, const Snapshot& new_image, const Image& image,
                              const std::string& label) {
    if (image.relocations().empty()) return;
    const auto non_absolute = std::count_if(image.relocations().begin(), image.relocations().end(),
                                            [](const auto& relocation) {
                                                return relocation.type != rt::pe::Relocation::kAbsolute;
                                            });
    const auto dir64 = std::find_if(image.relocations().begin(), image.relocations().end(), [](const auto& relocation) {
        return relocation.type == rt::pe::Relocation::kDir64;
    });
    check(non_absolute == 1 && dir64 != image.relocations().end(),
          label + ": successful relocation fixture has one DIR64 record plus alignment padding");
    if (dir64 == image.relocations().end()) return;
    const std::uint32_t rva = dir64->rva;
    const bool in_range = rva <= old_image.bytes.size() && sizeof(std::uint64_t) <= old_image.bytes.size() - rva &&
                          rva <= new_image.bytes.size() && sizeof(std::uint64_t) <= new_image.bytes.size() - rva;
    check(in_range, label + ": DIR64 target is available in both snapshots");
    if (!in_range) return;
    std::uint64_t old_value = 0;
    std::uint64_t new_value = 0;
    std::memcpy(&old_value, old_image.bytes.data() + rva, sizeof(old_value));
    std::memcpy(&new_value, new_image.bytes.data() + rva, sizeof(new_value));
    check(old_value == old_image.base + 0x1234 && new_value == new_image.base + 0x1234,
          label + ": both mappers apply the actual load delta to the DIR64 target");
}

void check_copy_rules(const fixture::Fixture& raw, const Snapshot& old_image, const Snapshot& new_image,
                      const Image& image, const std::string& label) {
    if (old_image.bytes.size() < fixture::kSizeOfImage || new_image.bytes.size() < fixture::kSizeOfImage) {
        check(false, label + ": complete mapped snapshots exist for section copy-rule checks");
        return;
    }
    bool old_tail = true;
    bool new_tail = true;
    for (std::uint32_t i = 0x180; i < 0x200; ++i) {
        const std::uint8_t expected = raw.bytes[kRawText + i];
        old_tail = old_tail && old_image.bytes[0x1000 + i] == expected;
        new_tail = new_tail && new_image.bytes[0x1000 + i] == expected;
    }
    check(old_tail && new_tail, label + ": both mappings preserve file bytes in the 0x80-byte page tail past VirtualSize");
    bool old_zero_raw = true;
    bool new_zero_raw = true;
    for (std::uint32_t i = 0; i < 0x500; ++i) {
        old_zero_raw = old_zero_raw && old_image.bytes[0x2000 + i] == 0;
        new_zero_raw = new_zero_raw && new_image.bytes[0x2000 + i] == 0;
    }
    check(image.sections().size() > 1 && image.sections()[1].raw_pointer == 0 && old_zero_raw && new_zero_raw,
          label + ": PointerToRawData=0 contributes zero-filled section bytes, not file headers/data");
    check(image.sections().size() == 3 && image.sections()[0].raw_size == 0x200 &&
              image.sections()[0].virtual_size == 0x180,
          label + ": fixture exercises SizeOfRawData greater than VirtualSize");
    if (image.sections().size() == 3 && image.sections()[2].raw_size < image.sections()[2].virtual_size) {
        bool old_zero_fill = true;
        bool new_zero_fill = true;
        for (std::uint32_t i = image.sections()[2].raw_size; i < image.sections()[2].virtual_size; ++i) {
            old_zero_fill = old_zero_fill && old_image.bytes[0x3000 + i] == 0;
            new_zero_fill = new_zero_fill && new_image.bytes[0x3000 + i] == 0;
        }
        check(old_zero_fill && new_zero_fill, label + ": both mappers zero-fill virtual bytes after raw data");
    }
}

void compare_valid_fixture(const fs::path& dir, const fixture::Fixture& raw, bool expect_relocated) {
    const std::string label = raw.filename;
    const Image image = Image::parse(raw.bytes, raw.filename);
    const fs::path path = write_fixture(dir, raw);
    std::unique_ptr<HeldBase> held;
    if (expect_relocated) held = std::make_unique<HeldBase>(image.image_base(), image.size_of_image());
    const Snapshot old_image = map_with_legacy_loader(dir, path, image, label);
    const Snapshot new_image = map_with_new_mapper(image, old_image.base, label);
    compare_snapshots(old_image, new_image, image, expect_relocated, label);
    check_expected_protections(image, old_image, new_image, label);
    check_relocation_results(old_image, new_image, image, label);
    check_copy_rules(raw, old_image, new_image, image, label);
}

void expect_refusal(const fs::path& dir, const fixture::Fixture& raw, const std::string& label,
                    const std::string& fragment, bool parser_should_accept, unsigned mapper_allocation_calls,
                    bool occupy_preferred = false) {
    const fs::path path = write_fixture(dir, raw);
    const std::uint64_t preferred = read_u64(raw.bytes, fixture::kOptionalOffset + 24);
    const std::uint32_t image_size = read_u32(raw.bytes, fixture::kOptionalOffset + 56);
    std::unique_ptr<HeldBase> held;
    if (occupy_preferred) held = std::make_unique<HeldBase>(preferred, image_size);
    std::string old_error;
    try {
        rt::Loader loader(dir);
        (void)loader.load(path);
    } catch (const std::exception& error) {
        old_error = error.what();
    }
    check(!old_error.empty() && contains(old_error, fragment),
          label + ": old Loader refuses with the named error fragment (observed: " +
              (old_error.empty() ? std::string("accepted") : old_error) + ")");

    std::string new_error;
    bool parser_accepted = false;
    VirtualAllocator allocator(0);
    VirtualProtector protector;
    try {
        const Image image = Image::parse(raw.bytes, raw.filename);
        parser_accepted = true;
        (void)rt::pe::mapper::map_image(image, allocator, protector);
    } catch (const std::exception& error) {
        new_error = error.what();
    }
    check(parser_accepted == parser_should_accept,
          label + ": parser acceptance matches the parity test expectation");
    check(!new_error.empty() && contains(new_error, fragment),
          label + ": new parse/map path refuses with the named error fragment (observed: " +
              (new_error.empty() ? std::string("accepted") : new_error) + ")");
    check(allocator.calls == mapper_allocation_calls,
          label + ": mapper allocation count is " + std::to_string(allocator.calls) + " (expected " +
              std::to_string(mapper_allocation_calls) + ")");
    std::printf("  refusal %-34s old=%s | new=%s\n", label.c_str(),
                old_error.empty() ? "accepted" : old_error.c_str(), new_error.empty() ? "accepted" : new_error.c_str());
}

void test_success_parity(const fs::path& dir) {
    std::printf("\n-- mapped bytes, bases, relocations and VirtualQuery protections --\n");
    const std::uint64_t base = free_image_base();
    auto page_tail = make_fixture(base, "mapper_page_tail_raw_zero.dll");
    set_section_raw_size(page_tail, 2, 0x200);
    compare_valid_fixture(dir, page_tail, false);
    auto zero_alignments = make_fixture(base, "mapper_zero_alignment_fields.dll");
    set_optional_u32(zero_alignments, 32, 0);
    set_optional_u32(zero_alignments, 36, 0);
    compare_valid_fixture(dir, zero_alignments, false);
    compare_valid_fixture(dir, make_fixture(base, "mapper_relocated_dir64.dll", kDefaultTextFlags, kDefaultDataFlags,
                                             kDefaultRdataFlags, true), true);
    compare_valid_fixture(dir, make_fixture(base, "mapper_execute_only_noaccess_xrw.dll", fixture::kScnExecute, 0,
                                             fixture::kScnExecute | fixture::kScnWrite), false);
    compare_valid_fixture(dir, make_fixture(base, "mapper_execute_readwrite_readonly.dll",
                                             fixture::kScnExecute | fixture::kScnWrite | fixture::kScnRead,
                                             fixture::kScnRead, fixture::kScnRead), false);
}

void test_named_refusals(const fs::path& dir) {
    std::printf("\n-- malformed shapes and named refusals --\n");
    const std::uint64_t base = free_image_base();
    {
        auto bad = make_fixture(base, "bad_imagebase_alignment.dll");
        write_u64(bad.bytes, fixture::kOptionalOffset + 24, base + 0x1000);
        expect_refusal(dir, bad, "ImageBase alignment", "ImageBase is not a multiple of 64 KiB", true, 0);
    }
    {
        auto bad = make_fixture(base, "bad_file_alignment.dll");
        set_optional_u32(bad, 36, 0x100);
        expect_refusal(dir, bad, "FileAlignment range", "FileAlignment is not a power of two", true, 0);
    }
    {
        auto bad = make_fixture(base, "bad_optional_header_size.dll");
        shorten_optional_header(bad);
        expect_refusal(dir, bad, "short PE32+ optional header", "not a PE32+ image", true, 0);
    }
    {
        auto bad = make_fixture(base, "bad_section_alignment_power.dll");
        set_optional_u32(bad, 32, 0x1800);
        expect_refusal(dir, bad, "SectionAlignment power of two",
                       "SectionAlignment is not a power of two at least FileAlignment", true, 0);
    }
    {
        auto bad = make_fixture(base, "bad_section_alignment_min.dll");
        set_optional_u32(bad, 32, 0x100);
        expect_refusal(dir, bad, "SectionAlignment below FileAlignment",
                       "SectionAlignment is not a power of two at least FileAlignment", true, 0);
    }
    {
        auto bad = make_fixture(base, "bad_section_rva_alignment.dll");
        set_section_rva(bad, 1, 0x2100);
        expect_refusal(dir, bad, "section RVA alignment", "is not aligned to SectionAlignment", true, 0);
    }
    {
        auto bad = make_fixture(base, "bad_shared_page_protection.dll");
        set_optional_u32(bad, 32, 0x200);
        set_section_rva(bad, 1, 0x1800);
        expect_refusal(dir, bad, "shared page with different PAGE_*",
                       "share a page with different protections", true, 0);
    }
    {
        auto bad = make_fixture(base, "bad_raw_data_extent.dll");
        set_section_raw_pointer(bad, 2, 0xF00);
        expect_refusal(dir, bad, "raw section runs past file", "reads past the end of the file", false, 0);
    }
    {
        auto bad = make_fixture(base, "bad_unsupported_relocation.dll", kDefaultTextFlags, kDefaultDataFlags,
                                kDefaultRdataFlags, true, 5);
        expect_refusal(dir, bad, "unsupported relocation type", "unsupported relocation type 5", true, 1, true);
    }
    {
        auto bad = make_fixture(base, "bad_header_relocation.dll", kDefaultTextFlags, kDefaultDataFlags,
                                kDefaultRdataFlags, true, 10, 0x300);
        expect_refusal(dir, bad, "relocation target in headers", "relocation lands in the headers", true, 1, true);
    }
    {
        auto fixed = make_fixture(base, "fixed_preferred_base_taken.dll");
        expect_refusal(dir, fixed, "fixed image preferred base taken",
                       "preferred base is taken and the image has no relocations", true, 1, true);
    }
}

}  // namespace

int main() {
    std::printf("pe_mapper_parity_test: synthetic old rt::Loader vs new pe_mapper\n");
    std::printf("Fixtures reuse loader_selfcheck_fixture.h and contain no game/package data.\n");
    try {
        TempDir dir;
        test_success_parity(dir.path());
        test_named_refusals(dir.path());
    } catch (const std::exception& error) {
        check(false, std::string("unexpected test exception: ") + error.what());
    }
    std::printf("\npe_mapper_parity_test: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}