// Tests for the advanced DirectStorage mechanisms in port/shims/dstorage_advanced.h: the bounded staging area, the
// destination pinner (write-watch handoff and residency), the 64 KiB page-list plan and copy math, the macro
// unshuffle, and the local BCPack predictor loader. Everything is synthetic and host-only: byte patterns generated in
// the test, predictor JSON written to the temp directory, and the page list resolved through a test callback. No game
// data, no package path, no GPU and no boot (docs/directstorage_advanced.md).
//
//   staging       reuse across borrows, the cap and its counted fallback, the peak
//   pinning       PAGE_GUARD released for the write and re-armed after it, reserved memory refused, residency accounted
//   page lists    plan shapes (offset, boundary, errors) and the copy math both ways through a resolver
//   unshuffle     modes 1-4 are permutations with the table's stripe positions, and the refusals
//   predictor     missing/unreadable/malformed/no-section/out-of-range/bad-length, a valid directory and a combined file
#include "../shims/dstorage_advanced.h"

#include "test_temp_dir.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace palmetto::dstorage;

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    std::printf("%s %s\n", ok ? "ok   " : "FAIL ", what.c_str());
    if (!ok) ++g_failed;
}

void check_predictor(PredictorError got, PredictorError want, const std::string& what) {
    check(got == want, what + " (got " + predictor_error_name(got) + ")");
}

// ---- staging -------------------------------------------------------------------------------------------------------

void test_staging() {
    BoundedScratch scratch(1024);
    check(scratch.enabled() && scratch.capacity() == 1024, "staging: a 1 KiB cap is enabled");
    auto first = scratch.borrow(300);
    check(first.size() == 300, "staging: a borrow inside the cap returns the whole request");
    std::memset(first.data(), 0x5a, first.size());
    auto second = scratch.borrow(300);
    check(second.size() == 300 && second.data() == first.data(), "staging: the next borrow reuses the same buffer");
    check(second[0] == 0x5a && second[299] == 0x5a, "staging: the reused buffer keeps its bytes");
    check(scratch.borrow(1025).empty(), "staging: a request over the cap is not served");
    check(scratch.borrow(0).empty(), "staging: a zero-size borrow is empty");
    const StagingStats& stats = scratch.stats();
    check(stats.borrows == 4 && stats.hits == 2 && stats.fallbacks == 1, "staging: borrows/hits/fallbacks counted");
    check(stats.bytes == 600 && stats.fallback_bytes == 1025, "staging: staged and fallback bytes counted");
    check(stats.peak == 300, "staging: peak is the largest staged request");

    BoundedScratch disabled;
    check(!disabled.enabled(), "staging: a zero cap is off and does not allocate");
    check(disabled.borrow(64).empty(), "staging: the off form serves nothing");

    // PALMETTO_DS_STAGING's value parser: decimal with an optional binary K/M/G suffix.
    SetEnvironmentVariableA("PALMETTO_DS_STAGING", "64K");
    check(env_size("PALMETTO_DS_STAGING") == 64u * 1024u, "staging: the size parser reads 64K");
    SetEnvironmentVariableA("PALMETTO_DS_STAGING", "2M");
    check(env_size("PALMETTO_DS_STAGING") == 2u * 1024u * 1024u, "staging: the size parser reads 2M");
    SetEnvironmentVariableA("PALMETTO_DS_STAGING", "4096");
    check(env_size("PALMETTO_DS_STAGING") == 4096, "staging: the size parser reads a plain byte count");
    SetEnvironmentVariableA("PALMETTO_DS_STAGING", "bogus");
    check(env_size("PALMETTO_DS_STAGING") == 0, "staging: an unparsable size is 0");
    SetEnvironmentVariableA("PALMETTO_DS_STAGING", nullptr);
    check(env_size("PALMETTO_DS_STAGING") == 0, "staging: an unset size is 0");
}

// ---- pinning -------------------------------------------------------------------------------------------------------

bool page_guarded(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof info) != sizeof info) return false;
    return (info.Protect & PAGE_GUARD) != 0;
}

void test_pinning() {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::size_t page = system.dwPageSize;
    auto* const memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (memory == nullptr) {
        check(false, "pinning: VirtualAlloc failed");
        return;
    }
    std::memset(memory, 0x11, page * 2);

    // A whole page armed with the same PAGE_GUARD the stream consumer's diagnostics use.
    DWORD old_protect = 0;
    check(VirtualProtect(memory, page, PAGE_READWRITE | PAGE_GUARD, &old_protect) != FALSE,
          "pinning: a page can be guarded");
    DestinationPinner pinner;
    check(pinner.pin(memory, page * 2), "pinning: a write-watched destination pins");
    check(!page_guarded(memory), "pinning: the guard over the write range is released");
    const auto& stats = pinner.stats();
    check(stats.calls == 1 && stats.pinned == 1 && stats.write_watched == 1 && stats.guards_released == 1,
          "pinning: the pin is counted as write-watched");
    check(stats.pages == 2, "pinning: the pinned pages are counted");
    std::memset(memory, 0x22, page * 2);  // the write the pin exists for must not take a guard fault
    pinner.unpin();
    check(page_guarded(memory), "pinning: unpin re-arms the guard the consumer watches");
    check(pinner.stats().guards_rearmed == 1, "pinning: the re-armed region is counted");
    DWORD restored = 0;
    VirtualProtect(memory, page, PAGE_READWRITE, &restored);

    // No guard on the range: nothing to hand back, so the pin reports false but does not refuse.
    DestinationPinner plain;
    check(!plain.pin(memory, page), "pinning: an unguarded destination pins nothing");
    check(plain.stats().refusals == 0, "pinning: an unguarded destination is not a refusal");

    // Reserve-only memory is not pinned (there is no protection to hand back).
    auto* const reserved = VirtualAlloc(nullptr, page, MEM_RESERVE, PAGE_NOACCESS);
    DestinationPinner unwritten;
    check(reserved != nullptr && !unwritten.pin(reserved, page), "pinning: reserved memory is not pinned");
    if (reserved) VirtualFree(reserved, 0, MEM_RELEASE);

    // Residency is a separate account: it may succeed (pages kept) or fail on the working-set quota, and the write
    // still runs either way.
    DestinationPinner resident;
    resident.set_residency(true);
    const bool locked = resident.pin(memory, page);
    check(resident.stats().lock_failures == 0 || !locked, "pinning: a failed VirtualLock is counted and not pinned");
    if (locked) check(resident.stats().pinned == 1 && resident.stats().pages == 1, "pinning: residency pins the range");
    resident.unpin();

    // Null and empty ranges are not pinned and not refusals.
    DestinationPinner none;
    check(!none.pin(nullptr, page) && !none.pin(memory, 0), "pinning: a null or empty range does nothing");
    VirtualFree(memory, 0, MEM_RELEASE);
}

// ---- page lists ----------------------------------------------------------------------------------------------------

struct PageTable {
    std::vector<std::uint64_t> numbers;
    std::vector<std::vector<unsigned char>> bytes;
    static const unsigned char* resolve(void* context, std::uint64_t page_number) {
        auto& self = *static_cast<PageTable*>(context);
        for (std::size_t i = 0; i < self.numbers.size(); ++i)
            if (self.numbers[i] == page_number) return self.bytes[i].data();
        return nullptr;
    }
};

void test_page_lists() {
    const std::uint64_t pages[4] = {16, 32, 48, 64};
    PagePlan plan;

    check(plan_page_transfer(pages, 4, 0, kPageListPageBytes, plan) == PagePlanError::ok, "pages: one whole page plans");
    check(plan.count == 1 && plan.spans[0].page == 16 && plan.spans[0].offset == 0 &&
              plan.spans[0].bytes == kPageListPageBytes,
          "pages: the single span is the whole first page");

    // 20 bytes starting 6 bytes before the end of the first page: 6 bytes of page 16, 14 of page 32.
    check(plan_page_transfer(pages, 4, 65530, 20, plan) == PagePlanError::ok, "pages: a straddling transfer plans");
    check(plan.count == 2 && plan.spans[0].page == 16 && plan.spans[0].offset == 65530 && plan.spans[0].bytes == 6 &&
              plan.spans[1].page == 32 && plan.spans[1].offset == 0 && plan.spans[1].bytes == 14,
          "pages: the straddling spans carry the offset and the tail");

    check(plan_page_transfer(pages, 4, 0, 0, plan) == PagePlanError::empty, "pages: an empty transfer is refused");
    check(plan_page_transfer(nullptr, 0, 0, 16, plan) == PagePlanError::too_short,
          "pages: a null/short list is refused");
    const std::uint64_t with_zero[2] = {16, 0};
    check(plan_page_transfer(with_zero, 2, 0, kPageListPageBytes + 16, plan) == PagePlanError::zero_page,
          "pages: a 0 page number is an unmapped entry");
    check(plan_page_transfer(pages, 2, 0, kPageListPageBytes * 3, plan) == PagePlanError::too_short,
          "pages: a list shorter than the transfer is refused");
    std::vector<std::uint64_t> many(PagePlan::kMaxSpans + 2);
    for (std::size_t i = 0; i < many.size(); ++i) many[i] = 16 + 16 * i;
    check(plan_page_transfer(many.data(), many.size(), 0, kPageListPageBytes * many.size(), plan) ==
              PagePlanError::too_many_pages,
          "pages: more spans than the fixed table is refused");

    // The copy math, both ways, through a resolver over three synthetic pages.
    PageTable table;
    table.numbers = {16, 32, 48};
    for (std::size_t i = 0; i < table.numbers.size(); ++i) {
        table.bytes.emplace_back(kPageListPageBytes);
        for (std::size_t at = 0; at < kPageListPageBytes; ++at)
            table.bytes.back()[at] = static_cast<unsigned char>((i * 31 + at) & 0xff);
    }
    const std::size_t bytes = 100000;
    check(plan_page_transfer(table.numbers.data(), table.numbers.size(), 1234, bytes, plan) == PagePlanError::ok,
          "pages: a 100,000-byte transfer plans");
    std::vector<unsigned char> gathered(bytes);
    check(copy_from_pages(plan, &PageTable::resolve, &table, gathered.data()), "pages: copy out of the page list");
    bool gathered_ok = true;
    for (std::size_t at = 0; at < bytes; ++at) {
        const std::size_t page = (1234 + at) / kPageListPageBytes;
        const std::size_t offset = (1234 + at) % kPageListPageBytes;
        if (gathered[at] != table.bytes[page][offset]) gathered_ok = false;
    }
    check(gathered_ok, "pages: the gathered bytes are the page bytes in order");

    std::vector<unsigned char> scattered(bytes);
    for (std::size_t at = 0; at < bytes; ++at) scattered[at] = static_cast<unsigned char>((at * 7) & 0xff);
    check(copy_to_pages(plan, &PageTable::resolve, &table, scattered.data()), "pages: copy into the page list");
    bool scattered_ok = true;
    for (std::size_t at = 0; at < bytes; ++at) {
        const std::size_t page = (1234 + at) / kPageListPageBytes;
        const std::size_t offset = (1234 + at) % kPageListPageBytes;
        if (table.bytes[page][offset] != scattered[at]) scattered_ok = false;
    }
    check(scattered_ok, "pages: the scattered bytes land at the planned page offsets");

    PageTable missing;
    missing.numbers = {16};
    missing.bytes.emplace_back(kPageListPageBytes);
    check(plan_page_transfer(missing.numbers.data(), 1, 0, kPageListPageBytes, plan) == PagePlanError::ok,
          "pages: a one-page plan for the unresolved case");
    const std::uint64_t other = 32;
    PagePlan one;
    plan_page_transfer(&other, 1, 0, 16, one);
    check(!copy_from_pages(one, &PageTable::resolve, &missing, gathered.data()),
          "pages: an unresolved page number refuses the copy");
    check(!copy_from_pages(plan, nullptr, &table, gathered.data()), "pages: a null resolver refuses the copy");
}

// ---- unshuffle -----------------------------------------------------------------------------------------------------

void test_unshuffle() {
    for (unsigned mode = 1; mode <= 4; ++mode) {
        SwizzleLayout layout;
        check(swizzle_layout(mode, layout), "unshuffle: mode " + std::to_string(mode) + " has a layout");
        check(layout.group_bytes == (mode == 1 ? 16384u : 32768u),
              "unshuffle: mode " + std::to_string(mode) + "'s group is " + std::to_string(layout.group_bytes));
        const std::size_t groups = 2;
        const std::size_t bytes = layout.group_bytes * groups;
        std::vector<unsigned char> source(bytes);
        for (std::size_t i = 0; i < bytes; ++i) source[i] = static_cast<unsigned char>((i * 13 + 7) & 0xff);
        std::vector<unsigned char> destination(bytes);
        check(unshuffle(mode, source.data(), bytes, destination.data(), bytes),
              "unshuffle: mode " + std::to_string(mode) + " unshuffles");
        std::vector<unsigned char> sorted_source = source;
        std::vector<unsigned char> sorted_destination = destination;
        std::sort(sorted_source.begin(), sorted_source.end());
        std::sort(sorted_destination.begin(), sorted_destination.end());
        check(sorted_source == sorted_destination, "unshuffle: mode " + std::to_string(mode) + " is a permutation");

        // The first stripe starts at the first group's offset 0 for every group, in source order (the table's rule).
        bool stripes_ok = true;
        std::size_t read = 0;
        std::size_t offset = 0;
        for (unsigned stripe = 0; stripe < layout.stripes; ++stripe) {
            const std::size_t stripe_bytes = std::size_t(layout.pages[stripe]) * 4096u;
            for (std::size_t group = 0; group < groups; ++group) {
                for (std::size_t at = 0; at < stripe_bytes; ++at)
                    if (destination[group * layout.group_bytes + offset + at] != source[read + at]) stripes_ok = false;
                read += stripe_bytes;
            }
            offset += stripe_bytes;
        }
        check(stripes_ok && read == bytes && offset == layout.group_bytes,
              "unshuffle: mode " + std::to_string(mode) + " places each stripe at its group offset");
    }
    SwizzleLayout layout;
    check(!swizzle_layout(0, layout) && !swizzle_layout(5, layout), "unshuffle: mode 0 and 5 have no layout");
    std::vector<unsigned char> bytes(16384);
    std::vector<unsigned char> out(16384);
    check(!unshuffle(1, bytes.data(), 16384 - 4096, out.data(), out.size()),
          "unshuffle: a partial group is refused");
    check(!unshuffle(1, bytes.data(), bytes.size(), out.data(), out.size() - 1),
          "unshuffle: a destination under the request size is refused");
    check(!unshuffle(1, nullptr, bytes.size(), out.data(), out.size()), "unshuffle: a null source is refused");
    check(!unshuffle(1, bytes.data(), bytes.size(), nullptr, out.size()), "unshuffle: a null destination is refused");
}

// ---- predictor -----------------------------------------------------------------------------------------------------

std::string join_ints(const std::vector<int>& values) {
    std::string text = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) text += ",";
        text += std::to_string(values[i]);
    }
    text += "]";
    return text;
}

std::string repeat_rows(const std::string& row, unsigned count) {
    std::string text = "[";
    for (unsigned i = 0; i < count; ++i) {
        if (i) text += ",";
        text += row;
    }
    text += "]";
    return text;
}

// A valid BC6H section: endpointHistory 256 x [3][4] of 16-bit values, indexHistory 256 x 16 of 0..15, and the class
// tables the calibration tool writes. 256 rows x (12 + 16) small integers is a few tens of KiB of JSON, which is the
// same order as the tool's own bc6h.json without any of its data.
std::string bc6h_section(bool valid = true) {
    std::string endpoint_row = "[[1,2,3,4],[5,6,7,8],[9,10,11,12]]";
    if (!valid) endpoint_row = "[[1,2,3,4],[5,6,7,8],[9,10,11,70000]]";
    std::vector<int> index_row(16);
    for (int i = 0; i < 16; ++i) index_row[i] = i & 15;
    std::vector<int> classes(256, 0);
    std::string text = "{";
    text += "\"endpointHistory\":" + repeat_rows(endpoint_row, 256);
    text += ",\"indexHistory\":" + repeat_rows(join_ints(index_row), 256);
    text += ",\"endpointClass\":" + join_ints(classes);
    text += ",\"indexClass\":" + join_ints(classes);
    text += "}";
    return text;
}

// A valid BC7 section: endpoints are 24 values 0..255, indices 32 values 0..15, plus the bits tables.
std::string bc7_section() {
    std::vector<int> endpoints(24, 1);
    std::vector<int> indices(32, 2);
    std::vector<int> bits(24, 0);
    std::vector<int> index_bits(32, 0);
    std::string text = "{";
    text += "\"historyEndpoints\":" + repeat_rows(join_ints(endpoints), 256);
    text += ",\"historyEndpointBits\":" + repeat_rows(join_ints(bits), 256);
    text += ",\"historyEndpointConfirmedBits\":" + repeat_rows(join_ints(bits), 256);
    text += ",\"historyIndices\":" + repeat_rows(join_ints(indices), 256);
    text += ",\"historyIndexBits\":" + repeat_rows(join_ints(index_bits), 256);
    text += ",\"historyIndexConfirmedBits\":" + repeat_rows(join_ints(index_bits), 256);
    text += ",\"historyEndpointLastResort\":[]";
    text += "}";
    return text;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void test_predictor(const testfiles::ScratchDir& scratch) {
    const std::filesystem::path& dir = scratch.path();
    PredictorReport report;

    check_predictor(load_dstorage_predictor((dir / L"absent.json").wstring().c_str(), report), PredictorError::missing,
                    "predictor: a missing file is named");
    check_predictor(load_dstorage_predictor(nullptr, report), PredictorError::missing, "predictor: a null path is missing");
    check_predictor(load_dstorage_predictor(dir.wstring().c_str(), report), PredictorError::missing,
                    "predictor: a directory with no codec files is missing");

    write_file(dir / "bc6h.json", bc6h_section());
    check_predictor(load_dstorage_predictor((dir / L"bc6h.json").wstring().c_str(), report), PredictorError::ok,
                    "predictor: a single-codec file loads");
    check(report.bc6h && !report.bc7 && report.bc6h_endpoint_rows == 256 && report.bc6h_index_rows == 256,
          "predictor: the BC6H rows are counted");
    check(report.source_files == 1 && report.file_bytes > 0, "predictor: the file size and count are reported");

    // The calibration tool's --out directory: bc6h.json and bc7.json together.
    write_file(dir / "bc7.json", bc7_section());
    check_predictor(load_dstorage_predictor(dir.wstring().c_str(), report), PredictorError::ok,
                    "predictor: a directory with both codec files loads");
    check(report.bc6h && report.bc7 && report.bc7_endpoint_rows == 256 && report.bc7_index_rows == 256,
          "predictor: the directory's BC7 rows are counted");
    check(report.source_files == 2, "predictor: the directory's two files are counted");

    // A combined file: the codec sections under one object.
    write_file(dir / "combined.json", "{\"bc6h\":" + bc6h_section() + ",\"bc7\":" + bc7_section() + "}");
    check_predictor(load_dstorage_predictor((dir / L"combined.json").wstring().c_str(), report), PredictorError::ok,
                    "predictor: a combined file loads both sections");

    // The sparse index-keyed form the decoder also accepts: one named row is enough for an optional table.
    write_file(dir / "sparse.json",
               "{\"bc6h\":{\"endpointHistory\":{\"0\":[[1,2,3,4],[5,6,7,8],[9,10,11,12]]}}}");
    check_predictor(load_dstorage_predictor((dir / L"sparse.json").wstring().c_str(), report), PredictorError::ok,
                    "predictor: a sparse index-keyed table loads");
    check(report.bc6h && report.bc6h_endpoint_rows == 1, "predictor: the sparse table's row is counted");

    write_file(dir / "bad_json.json", "{\"bc6h\":");
    check_predictor(load_dstorage_predictor((dir / L"bad_json.json").wstring().c_str(), report),
                    PredictorError::malformed, "predictor: broken JSON is malformed");
    write_file(dir / "no_codec.json", "{\"other\":1}");
    check_predictor(load_dstorage_predictor((dir / L"no_codec.json").wstring().c_str(), report),
                    PredictorError::no_codec_section, "predictor: a file with no codec section is named");
    write_file(dir / "empty_section.json", "{\"bc6h\":{}}");
    check_predictor(load_dstorage_predictor((dir / L"empty_section.json").wstring().c_str(), report),
                    PredictorError::malformed, "predictor: an empty codec section is malformed");
    write_file(dir / "out_of_range.json", "{\"bc6h\":" + bc6h_section(false) + "}");
    check_predictor(load_dstorage_predictor((dir / L"out_of_range.json").wstring().c_str(), report),
                    PredictorError::out_of_range, "predictor: an endpoint outside 16 bits is out of range");
    write_file(dir / "bad_length.json",
               "{\"bc6h\":{\"indexHistory\":[[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14]]}}");
    check_predictor(load_dstorage_predictor((dir / L"bad_length.json").wstring().c_str(), report),
                    PredictorError::bad_length, "predictor: an index row of 15 integers is bad_length");
    // A class table with a value the decoder does not define (0..3): the sparse endpoint table is valid, the class
    // table is not.
    std::vector<int> bad_classes(256, 9);
    write_file(dir / "bad_class.json",
               "{\"bc6h\":{\"endpointHistory\":{\"0\":[[1,2,3,4],[5,6,7,8],[9,10,11,12]]},\"endpointClass\":" +
                   join_ints(bad_classes) + "}}");
    check_predictor(load_dstorage_predictor((dir / L"bad_class.json").wstring().c_str(), report),
                    PredictorError::out_of_range, "predictor: a class outside 0..3 is out of range");

    // The bounded file size: a file over the limit is refused before it is parsed.
    {
        std::ofstream large(dir / "huge.json", std::ios::binary | std::ios::trunc);
        const std::string block(1024 * 1024, ' ');
        for (unsigned i = 0; i <= kMaxPredictorFileBytes / block.size(); ++i) large.write(block.data(), block.size());
    }
    check_predictor(load_dstorage_predictor((dir / L"huge.json").wstring().c_str(), report),
                    PredictorError::too_large, "predictor: a file over the bounded size is refused");

    // The values are validated, never retained: a valid file's report is the only output.
    write_file(dir / "truncated.json", "{\"bc6h\":{\"endpointHistory\":[[1,2,3,4],[5,6,7,8],[9,10]]}}");
    check_predictor(load_dstorage_predictor((dir / L"truncated.json").wstring().c_str(), report),
                    PredictorError::bad_length, "predictor: a short endpoint row is bad_length");
}

}  // namespace

int main() {
    testfiles::ScratchDir scratch("palmetto_dstorage_test");
    test_staging();
    test_pinning();
    test_page_lists();
    test_unshuffle();
    test_predictor(scratch);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
