#include "pe_image_fixture.h"
#include "pe_mapper.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using pe_image_fixture::Builder;
using rt::pe::mapper::Allocator;
using rt::pe::mapper::MappedImage;
using rt::pe::mapper::PageProtection;
using rt::pe::mapper::Protector;
using rt::pe::mapper::map_image;

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void check_equal(std::uint64_t got, std::uint64_t want, const char* what) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL: %s (got %llu, want %llu)\n", what, static_cast<unsigned long long>(got),
                 static_cast<unsigned long long>(want));
    ++failures;
}

void check_string(const std::string& got, const char* want, const char* what) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL: %s (got '%s', want '%s')\n", what, got.c_str(), want);
    ++failures;
}

template <class Function>
void check_error(Function&& function, const char* fragment, const char* what) {
    try {
        function();
    } catch (const rt::pe::Error& error) {
        const std::string message = error.what();
        if (message.rfind("synthetic.dll: ", 0) != 0 || message.find(fragment) == std::string::npos) {
            std::fprintf(stderr, "FAIL: %s (unexpected message: %s)\n", what, error.what());
            ++failures;
        }
        return;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s (wrong exception: %s)\n", what, error.what());
        ++failures;
        return;
    }
    std::fprintf(stderr, "FAIL: %s (mapping succeeded instead of refusing)\n", what);
    ++failures;
}

struct SyntheticAllocator final : Allocator {
    std::vector<std::uint8_t> storage;
    std::size_t calls = 0;
    std::size_t caller_releases = 0;
    std::uint64_t requested_base = 0;
    std::size_t requested_size = 0;
    bool allow_rebase = false;
    bool fail = false;
    std::uint8_t initial_byte = 0xCC;

    void prepare(std::size_t size, std::uint8_t fill = 0xCC) { storage.assign(size, fill); }

    std::uint8_t* allocate(std::uint64_t preferred_base, std::size_t size, bool may_rebase) override {
        ++calls;
        requested_base = preferred_base;
        requested_size = size;
        allow_rebase = may_rebase;
        if (fail) return nullptr;
        if (storage.size() != size) storage.assign(size, initial_byte);
        return storage.data();
    }

    // map_image intentionally does not call this. The caller releases both successful mappings and allocations
    // left behind by a later mapping refusal.
    void release_by_caller() {
        std::vector<std::uint8_t>().swap(storage);
        ++caller_releases;
    }
};

struct SyntheticProtector final : Protector {
    struct Call {
        std::uint8_t* address = nullptr;
        std::size_t size = 0;
        PageProtection protection = PageProtection::no_access;
    };
    std::vector<Call> calls;
    bool fail = false;

    bool protect(std::uint8_t* address, std::size_t size, PageProtection protection) override {
        calls.push_back({address, size, protection});
        return !fail;
    }
};

std::uint64_t set_fixture_base(Builder& builder, const SyntheticAllocator& allocator) {
    const std::uint64_t actual = reinterpret_cast<std::uintptr_t>(allocator.storage.data());
    check(actual > 0x1000, "synthetic allocation address can be rebased safely");
    const std::uint64_t preferred = actual - 0x1000;
    builder.u64(Builder::kOptional + 24, preferred);
    // Keep the fixture's absolute TLS VAs valid against this preferred base.
    builder.u64(builder.file_of(0x2300), preferred + 0x2700);
    builder.u64(builder.file_of(0x2308), preferred + 0x2708);
    builder.u64(builder.file_of(0x2310), preferred + 0x2500);
    builder.u64(builder.file_of(0x2318), preferred + 0x2520);
    builder.u64(builder.file_of(0x2520), preferred + 0x1000);
    return preferred;
}

void test_copy_relocation_and_protection_plan() {
    SyntheticAllocator allocator;
    allocator.prepare(0x4000);
    Builder builder;
    const std::uint64_t preferred = set_fixture_base(builder, allocator);
    builder.u32(Builder::kSections + 8, 0x100);   // .text VirtualSize 0x100, SizeOfRawData 0x200, so the page tail
    builder.bytes[builder.file_of(0x1000)] = 0xA1;  // from 0x1100 to 0x1200 is file-backed, as Windows maps it.
    builder.bytes[builder.file_of(0x1000) + 1] = 0xB2;
    builder.bytes[builder.file_of(0x1000) + 0x100] = 0xC3;
    builder.u64(builder.file_of(0x1010), preferred + 0x1234);
    builder.u32(builder.file_of(0x1020), 0x2000);
    builder.set_relocation_entries(0x1000,
                                   {static_cast<std::uint16_t>((10u << 12) | 0x10),
                                    static_cast<std::uint16_t>((3u << 12) | 0x20), 0});
    const auto image = rt::pe::Image::parse(builder.bytes, "synthetic.dll");
    SyntheticProtector protector;

    const MappedImage mapped = map_image(image, allocator, protector);
    const std::uint64_t actual = reinterpret_cast<std::uintptr_t>(allocator.storage.data());
    check(mapped.base == allocator.storage.data(), "mapped base is the injected allocation");
    check_equal(mapped.size, 0x4000, "mapped image size");
    check(mapped.relocated, "mapping at a non-preferred base is marked relocated");
    check_equal(allocator.calls, 1, "allocator called once");
    check_equal(allocator.requested_base, preferred, "allocator receives preferred image base");
    check_equal(allocator.requested_size, 0x4000, "allocator receives SizeOfImage");
    check(allocator.allow_rebase, "allocator may return a fallback base when relocations exist");

    check(std::memcmp(mapped.base, builder.bytes.data(), image.size_of_headers()) == 0, "headers copied byte-for-byte");
    check(mapped.base[0x1000] == 0xA1 && mapped.base[0x1001] == 0xB2, "file-backed .text bytes copied");
    check(std::memcmp(mapped.base + 0x2090, "Alpha", 5) == 0, "file-backed .rdata bytes copied");
    std::uint64_t dir64 = 0;
    std::memcpy(&dir64, mapped.base + 0x1010, sizeof(dir64));
    check_equal(dir64, actual + 0x1234, "DIR64 adds the base delta");
    std::uint32_t highlow = 0;
    std::memcpy(&highlow, mapped.base + 0x1020, sizeof(highlow));
    check_equal(highlow, 0x3000, "HIGHLOW adds the base delta when it fits");
    check(mapped.base[0x1000] == 0xA1, "ABSOLUTE relocation is skipped");
    check(mapped.base[0x1100] == 0xC3, "page tail past VirtualSize keeps file bytes");
    check(std::all_of(mapped.base + 0x1101, mapped.base + 0x1200, [](std::uint8_t byte) { return byte == 0; }),
          "unused part of the copied page stays zero");
    check(std::all_of(mapped.base + 0x1200, mapped.base + 0x2000, [](std::uint8_t byte) { return byte == 0; }),
          "unmapped image gap is zero-filled");
    check(mapped.base[0x3FFF] == 0, "end of image is zero-filled");

    check_equal(mapped.sections.size(), 2, "mapped section count");
    check_string(mapped.sections[0].name, ".text", "first mapped section name");
    check_equal(mapped.sections[0].rva, 0x1000, "text RVA");
    check_equal(mapped.sections[0].mapped_size, 0x200, "text mapped extent");
    check_equal(mapped.sections[0].copied_size, 0x200, "text page-tail copy size");
    check(mapped.base[0x1100] == 0xC3, "page tail past VirtualSize keeps file bytes");
    check(mapped.base[0x1200] == 0, "bytes past the copied page remain zero");
    check_string(mapped.sections[1].name, ".rdata", "second mapped section name");
    check_equal(mapped.sections[1].copied_size, 0x1000, "rdata file-backed copy size");

    check_equal(mapped.protect_plan.size(), 3, "protection plan has headers and both sections");
    check_string(mapped.protect_plan[0].name, "headers", "header protection region name");
    check_equal(mapped.protect_plan[0].rva, 0, "header protection RVA");
    check_equal(mapped.protect_plan[0].size, 0x400, "header protection size");
    check(mapped.protect_plan[0].protection == PageProtection::read_only, "headers are read-only");
    check_string(mapped.protect_plan[1].name, ".text", "text protection region name");
    check_equal(mapped.protect_plan[1].rva, 0x1000, "text protection RVA");
    check_equal(mapped.protect_plan[1].size, 0x200, "text protection size");
    check(mapped.protect_plan[1].protection == PageProtection::execute_read, "read+execute section is executable/read-only");
    check(mapped.protect_plan[2].protection == PageProtection::read_only, "read-only section stays read-only");
    check_equal(protector.calls.size(), mapped.protect_plan.size(), "protector applies every plan region");
    for (std::size_t i = 0; i < mapped.protect_plan.size() && i < protector.calls.size(); ++i) {
        const auto& region = mapped.protect_plan[i];
        const auto& call = protector.calls[i];
        check(call.address == mapped.base + region.rva, "protection address matches the plan");
        check_equal(call.size, region.size, "protection size matches the plan");
        check(call.protection == region.protection, "protection flags match the plan");
    }
    allocator.release_by_caller();
}

void test_protection_uses_section_rwx_bits() {
    SyntheticAllocator allocator;
    allocator.prepare(0x4000);
    Builder builder;
    (void)set_fixture_base(builder, allocator);
    builder.u32(Builder::kSections + 36, 0x20000020);           // .text: execute only
    builder.u32(Builder::kSections + 40 + 36, 0xC0000040);     // .rdata: read + write
    const auto image = rt::pe::Image::parse(builder.bytes, "synthetic.dll");
    SyntheticProtector protector;

    const auto mapped = map_image(image, allocator, protector);
    check(mapped.protect_plan[0].protection == PageProtection::read_only, "headers remain read-only");
    check(mapped.protect_plan[1].protection == PageProtection::execute, "execute-only section uses PAGE_EXECUTE");
    check(mapped.protect_plan[2].protection == PageProtection::read_write, "read+write section uses PAGE_READWRITE");
    allocator.release_by_caller();
}

void test_highlow_overflow_refusal_keeps_allocation() {
    SyntheticAllocator allocator;
    allocator.prepare(0x4000);
    Builder builder;
    const std::uint64_t preferred = set_fixture_base(builder, allocator);
    builder.u32(builder.file_of(0x1020), 0xFFFFFFF0u);
    builder.set_relocation_entries(0x1000, {static_cast<std::uint16_t>((3u << 12) | 0x20)});
    const auto image = rt::pe::Image::parse(builder.bytes, "synthetic.dll");
    SyntheticProtector protector;

    check_error([&] { (void)map_image(image, allocator, protector); }, "HIGHLOW", "HIGHLOW overflow is refused");
    check_equal(allocator.requested_base, preferred, "overflow path requests the preferred base");
    check(allocator.storage.size() == image.size_of_image(), "failed mapping leaves reservation owned by the caller");
    check(protector.calls.empty(), "protection does not run after relocation refusal");
    allocator.release_by_caller();
    check(allocator.storage.empty(), "caller releases the failed mapping");
}

void test_unsupported_relocation_refusal() {
    SyntheticAllocator allocator;
    allocator.prepare(0x4000);
    Builder builder;
    const std::uint64_t preferred = set_fixture_base(builder, allocator);
    builder.set_relocation_entries(0x1000, {static_cast<std::uint16_t>((5u << 12) | 0x30)});
    const auto image = rt::pe::Image::parse(builder.bytes, "synthetic.dll");
    SyntheticProtector protector;

    check_error([&] { (void)map_image(image, allocator, protector); }, "unsupported relocation type 5",
                "unsupported relocation type is named");
    check_equal(allocator.requested_base, preferred, "unsupported relocation path requests preferred base");
    check(protector.calls.empty(), "protection does not run after unsupported relocation");
    allocator.release_by_caller();
}

void test_relocation_into_headers_refused() {
    SyntheticAllocator allocator;
    allocator.prepare(0x4000);
    Builder builder;
    const std::uint64_t preferred = set_fixture_base(builder, allocator);
    builder.u64(0x300, preferred + 0x1000);
    builder.set_relocation_entries(0, {static_cast<std::uint16_t>((10u << 12) | 0x300)});
    const auto image = rt::pe::Image::parse(builder.bytes, "synthetic.dll");
    SyntheticProtector protector;

    check_error([&] { (void)map_image(image, allocator, protector); }, "relocation lands in the headers",
                "header relocation target is refused");
    check(protector.calls.empty(), "protection does not run after header-target refusal");
    allocator.release_by_caller();
}

void test_allocation_failure_stops_before_protection() {
    SyntheticAllocator allocator;
    allocator.fail = true;
    Builder builder;
    const auto image = rt::pe::Image::parse(builder.bytes, "synthetic.dll");
    SyntheticProtector protector;

    check_error([&] { (void)map_image(image, allocator, protector); }, "VirtualAlloc failed",
                "allocation failure is reported");
    check_equal(allocator.calls, 1, "failed allocator called once");
    check_equal(allocator.requested_base, image.image_base(), "allocation failure receives preferred base");
    check(allocator.allow_rebase, "allocation request records available relocations");
    check(allocator.storage.empty(), "failed allocation owns no storage to release");
    check(protector.calls.empty(), "protection is not attempted after allocation failure");
}

}  // namespace

int main() {
    try {
        test_copy_relocation_and_protection_plan();
        test_protection_uses_section_rwx_bits();
        test_highlow_overflow_refusal_keeps_allocation();
        test_unsupported_relocation_refusal();
        test_relocation_into_headers_refused();
        test_allocation_failure_stops_before_protection();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: uncaught exception: %s\n", error.what());
        ++failures;
    }
    if (failures) {
        std::fprintf(stderr, "pe_mapper_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("pe_mapper_test: OK (injected mapping, relocations, protection, refusals)\n");
    return 0;
}
