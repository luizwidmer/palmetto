// ui_draw_probe.h: pure validation of the measured 2D-batch sites and the bounded caller ring, driven
// with synthetic bytes and synthetic return addresses only.
// No game file is read; tests copy the short signatures from the metadata and mutate those copies.
#include "../src/ui_draw_probe.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

namespace ui = palmetto::ui_draw_probe;

void test_site_table() {
    check(ui::kSiteCount == 4, "four measured 2D-batch sites are in the table");
    check(ui::kStageWordRva == 0x7446510u, "the stage split uses the measured stage word");
    const std::uint32_t expected_rvas[ui::kSiteCount] = {0x517A290u, 0x3A1B860u, 0x58C8950u, 0x2AACF0u};
    std::set<std::uint32_t> rvas;
    std::set<std::string> names;
    for (unsigned site = 0; site < ui::kSiteCount; ++site) {
        const ui::SiteShape& shape = ui::kSites[site];
        check(shape.name && *shape.name, "every site has a name");
        check(shape.rva == expected_rvas[site], "the site RVA matches the measured table");
        check(rvas.insert(shape.rva).second, "the measured site RVAs are unique");
        check(names.insert(shape.name).second, "the measured site names are unique");
        check(shape.signature != nullptr, "every site has a signature");
        check(shape.signature_length >= 7 && shape.signature_length <= 10,
              "every signature is a short measured instruction prefix");
    }
}

void test_validation() {
    check(ui::validate_site(ui::kSiteCount, 0x10000000u, nullptr, 0) == ui::Validation::InvalidSite,
          "an out-of-table site is refused");
    check(std::strcmp(ui::validation_name(ui::Validation::InvalidSite), "unknown site") == 0,
          "invalid-site refusal is named");

    for (unsigned site = 0; site < ui::kSiteCount; ++site) {
        const ui::SiteShape& shape = ui::kSites[site];
        const std::size_t image_size = static_cast<std::size_t>(shape.rva) + shape.signature_length;
        std::uint8_t bytes[16] = {};
        std::memcpy(bytes, shape.signature, shape.signature_length);
        check(ui::validate_site(site, image_size, bytes, shape.signature_length) == ui::Validation::Ok,
              "the measured synthetic shape validates at its RVA");
        check(ui::validate_site(site, image_size - 1, bytes, shape.signature_length) == ui::Validation::ImageTooSmall,
              "a signature that crosses the mapped image end is refused");
        check(ui::validate_site(site, image_size, nullptr, shape.signature_length) == ui::Validation::MissingBytes,
              "unreadable site bytes are refused by name");
        check(ui::validate_site(site, image_size, bytes, shape.signature_length - 1) == ui::Validation::ShortBytes,
              "a short read is refused");
        for (std::size_t at = 0; at < shape.signature_length; ++at) {
            std::memcpy(bytes, shape.signature, shape.signature_length);
            bytes[at] ^= 0xFFu;
            check(ui::validate_site(site, image_size, bytes, shape.signature_length) == ui::Validation::SignatureMismatch,
                  "any changed signature byte is refused");
        }
    }
}

// Synthetic image geometry for the caller-ring tests: a plausible mapped-image base and size, no title bytes.
constexpr std::uint64_t kSyntheticBase = 0x140000000ull;
constexpr std::size_t kSyntheticImageSize = 0x10000000u;  // 256 MiB

std::uint64_t synthetic_image_address(std::uint32_t rva) { return kSyntheticBase + rva; }

void test_caller_classification() {
    std::uint32_t rva = 0xFFFFFFFFu;
    check(ui::classify_return_address(0, kSyntheticBase, kSyntheticImageSize, &rva) == ui::CallerKind::None,
          "a missing return address classifies as none");
    check(rva == 0, "a none classification clears the RVA output");
    check(std::strcmp(ui::caller_kind_name(ui::CallerKind::None), "none") == 0,
          "the none classification is named");
    check(std::strcmp(ui::caller_kind_name(ui::CallerKind::Image), "image") == 0,
          "the image classification is named");
    check(std::strcmp(ui::caller_kind_name(ui::CallerKind::External), "external") == 0,
          "the external classification is named");
    check(ui::classify_return_address(kSyntheticBase - 1, kSyntheticBase, kSyntheticImageSize, &rva) ==
              ui::CallerKind::External,
          "an address below the image is external");
    check(ui::classify_return_address(kSyntheticBase, kSyntheticBase, kSyntheticImageSize, &rva) ==
              ui::CallerKind::Image &&
              rva == 0,
          "the image base itself is image RVA 0");
    check(ui::classify_return_address(synthetic_image_address(0x63AD7D), kSyntheticBase, kSyntheticImageSize, &rva) ==
              ui::CallerKind::Image &&
              rva == 0x63AD7Du,
          "a measured-style return address reports its image RVA");
    check(ui::classify_return_address(kSyntheticBase + kSyntheticImageSize - 1, kSyntheticBase, kSyntheticImageSize,
                                      &rva) == ui::CallerKind::Image &&
              rva == static_cast<std::uint32_t>(kSyntheticImageSize - 1),
          "the last mapped byte is inside the image");
    check(ui::classify_return_address(kSyntheticBase + kSyntheticImageSize, kSyntheticBase, kSyntheticImageSize, &rva) ==
              ui::CallerKind::External,
          "the exclusive image end is outside");
    check(ui::classify_return_address(0x7FFB00001234ull, kSyntheticBase, kSyntheticImageSize, &rva) ==
              ui::CallerKind::External,
          "a host-module return address is external");
    check(ui::classify_return_address(synthetic_image_address(0x63AD7D), kSyntheticBase, kSyntheticImageSize, nullptr) ==
              ui::CallerKind::Image,
          "the RVA output is optional");
}

void test_caller_ring_bounded() {
    check(ui::kCallerRingCapacity == 16, "the caller ring is a fixed 16-entry capacity");
    ui::CallerRing ring{};
    check(ring.count == 0 && ring.samples == 0 && ring.evicted == 0 && ring.unreadable == 0,
          "a fresh caller ring is empty and zeroed");
    check(ui::caller_at(ring, 0) == 0, "an empty ring has no caller entry");
    check(ui::caller_at(ring, ui::kCallerRingCapacity) == 0, "an out-of-range caller position reads 0");
    check(ui::record_caller(ring, 0) == ui::CallerRecord::Unreadable,
          "an unreadable return address is counted, not stored");
    check(ring.samples == 1 && ring.unreadable == 1 && ring.count == 0,
          "an unreadable sample changes counters only, never the ring");

    const std::uint64_t callers[ui::kCallerRingCapacity] = {
        synthetic_image_address(0x63AD7D), synthetic_image_address(0x1E40BF3),
        synthetic_image_address(0x21B8A43), synthetic_image_address(0x5A75F80),
        synthetic_image_address(0x5BB5390), synthetic_image_address(0x2234A40),
        synthetic_image_address(0x3DAF310), synthetic_image_address(0x23495A0),
        synthetic_image_address(0x2349D80), synthetic_image_address(0x51B45B0),
        synthetic_image_address(0x2355DD0), synthetic_image_address(0x22310D0),
        synthetic_image_address(0x442EA70), synthetic_image_address(0x199EF20),
        synthetic_image_address(0x2EF0E0), synthetic_image_address(0x442F760),
    };
    for (unsigned i = 0; i < ui::kCallerRingCapacity; ++i)
        check(ui::record_caller(ring, callers[i]) == ui::CallerRecord::Recorded,
              "a distinct caller is recorded in the ring");
    check(ring.count == ui::kCallerRingCapacity && ring.evicted == 0,
          "the ring holds exactly its capacity and evicts nothing");
    bool ordered = true;
    for (unsigned i = 0; i < ui::kCallerRingCapacity; ++i) ordered = ordered && ui::caller_at(ring, i) == callers[i];
    check(ordered, "ring positions follow recording order until the ring is full");
    check(ring.samples == ui::kCallerRingCapacity + 1, "every call is counted as a sample");
    check(ui::record_caller(ring, callers[0]) == ui::CallerRecord::Duplicate,
          "a repeated caller is a duplicate");
    check(ui::record_caller(ring, callers[ui::kCallerRingCapacity - 1]) == ui::CallerRecord::Duplicate,
          "a repeated late caller is a duplicate");
    check(ring.count == ui::kCallerRingCapacity && ring.evicted == 0,
          "duplicates do not move or evict the ring");

    const std::uint64_t newest = synthetic_image_address(0x2AB000);
    check(ui::record_caller(ring, newest) == ui::CallerRecord::RecordedEvicted,
          "a new caller replaces the oldest entry once the ring is full");
    check(ring.evicted == 1, "one evicted caller is counted");
    check(ui::caller_at(ring, 0) == callers[1], "the oldest position moved to the second recorded caller");
    check(ui::caller_at(ring, ui::kCallerRingCapacity - 1) == newest, "the newest caller is the last position");
    check(ui::caller_at(ring, ui::kCallerRingCapacity) == 0, "the ring never exposes more than its capacity");

    ui::CallerRing wrap{};
    for (unsigned i = 0; i < ui::kCallerRingCapacity; ++i) ui::record_caller(wrap, callers[i]);
    for (unsigned i = 0; i < 3; ++i)
        check(ui::record_caller(wrap, synthetic_image_address(0x900000 + i * 0x10)) ==
                  ui::CallerRecord::RecordedEvicted,
              "each later distinct caller evicts the oldest entry");
    check(wrap.evicted == 3, "three replacements count three evictions");
    check(ui::caller_at(wrap, 0) == callers[3], "after three replacements the oldest held entry is the fourth");
    check(ui::caller_at(wrap, ui::kCallerRingCapacity - 1) == synthetic_image_address(0x900020),
          "the newest entry closes the ring order");

    ui::CallerRing hot{};
    for (unsigned i = 0; i < 4096; ++i) ui::record_caller(hot, callers[0]);
    check(hot.count == 1 && hot.samples == 4096 && hot.evicted == 0,
          "4096 hits from one caller hold one entry and a bounded sample count");
    check(ui::record_caller(hot, callers[1]) == ui::CallerRecord::Recorded,
          "a second caller still fits after a hot loop");

    ui::CallerRing saturated{};
    saturated.samples = 0xFFFFFFFFu;
    saturated.evicted = 0xFFFFFFFFu;
    saturated.unreadable = 0xFFFFFFFFu;
    check(ui::record_caller(saturated, 0) == ui::CallerRecord::Unreadable,
          "a saturated ring still classifies a missing address");
    check(saturated.samples == 0xFFFFFFFFu && saturated.unreadable == 0xFFFFFFFFu,
          "the sample counters saturate instead of wrapping");
    for (unsigned i = 0; i < ui::kCallerRingCapacity; ++i) ui::record_caller(saturated, callers[i]);
    check(ui::record_caller(saturated, newest) == ui::CallerRecord::RecordedEvicted,
          "a saturated ring still accepts a new distinct caller");
    check(saturated.evicted == 0xFFFFFFFFu, "the evicted counter saturates");

    ui::CallerRing first{};
    ui::CallerRing second{};
    for (unsigned i = 0; i < 40; ++i) {
        const std::uint64_t address = synthetic_image_address(0x100000u + (i % 7) * 0x1000u);
        check(ui::record_caller(first, address) == ui::record_caller(second, address),
              "the same synthetic sequence gives the same ring decision");
    }
    check(std::memcmp(&first, &second, sizeof first) == 0,
          "the same synthetic sequence gives an identical ring state");
}

void test_caller_formatting() {
    char buffer[512] = {};
    ui::CallerRing ring{};
    check(ui::format_callers(ring, kSyntheticBase, kSyntheticImageSize, buffer, sizeof buffer) == 2,
          "an empty ring renders as two characters");
    check(std::strcmp(buffer, "[]") == 0, "an empty ring renders as []");

    ui::record_caller(ring, synthetic_image_address(0x63AD7D));
    ui::record_caller(ring, synthetic_image_address(0x1E40BF3));
    const std::size_t length = ui::format_callers(ring, kSyntheticBase, kSyntheticImageSize, buffer, sizeof buffer);
    check(length == std::strlen(buffer), "the formatter returns the full rendered length");
    check(std::strcmp(buffer, "[image+0x63ad7d image+0x1e40bf3]") == 0,
          "image callers render as image+<rva> in recording order");

    ui::CallerRing host{};
    ui::record_caller(host, 0x7FFB00001234ull);
    const std::size_t host_length = ui::format_callers(host, kSyntheticBase, kSyntheticImageSize, buffer, sizeof buffer);
    check(std::strcmp(buffer, "[ext:0x7ffb00001234]") == 0,
          "a host-module return address renders as ext:<address>");
    check(host_length == std::strlen(buffer), "the external rendering reports its length too");

    ui::CallerRing full{};
    for (unsigned i = 0; i < ui::kCallerRingCapacity; ++i)
        ui::record_caller(full, synthetic_image_address(0x100000u + i * 0x1000u));
    const std::size_t full_length = ui::format_callers(full, kSyntheticBase, kSyntheticImageSize, buffer, sizeof buffer);
    check(full_length < sizeof buffer, "a full ring renders inside the reporter's fixed buffer");
    check(full_length == std::strlen(buffer), "the full rendering is whole inside the buffer");
    unsigned rendered = 0;
    for (const char* at = buffer; (at = std::strstr(at, "image+0x")) != nullptr; ++at) ++rendered;
    check(rendered == ui::kCallerRingCapacity, "the full ring renders every held caller");
    check(buffer[0] == '[' && buffer[full_length - 1] == ']', "the rendering is bracketed");

    char tiny[8] = {};
    const std::size_t needed = ui::format_callers(full, kSyntheticBase, kSyntheticImageSize, tiny, sizeof tiny);
    check(needed == full_length, "a truncated render still reports the full needed length");
    check(tiny[sizeof tiny - 1] == '\0', "a truncated render is NUL-terminated inside its buffer");
    check(std::strlen(tiny) == sizeof tiny - 1, "a truncated render fills only its buffer");
    check(std::strcmp(tiny, "[image+") == 0, "a truncated render keeps the leading shape it can hold");

    char one[1] = {'x'};
    check(ui::format_callers(full, kSyntheticBase, kSyntheticImageSize, one, sizeof one) == full_length &&
              one[0] == '\0',
          "a one-byte buffer receives only the terminator");
    check(ui::format_callers(full, kSyntheticBase, kSyntheticImageSize, nullptr, 0) == full_length,
          "a null buffer only measures the rendering");
}

void test_caller_rings_per_site() {
    ui::CallerRing site[ui::kSiteCount] = {};
    check(ui::record_caller(site[ui::kBatchBuild], synthetic_image_address(0x63AD7D)) ==
              ui::CallerRecord::Recorded,
          "a batch-build caller lands in the batch-build ring");
    check(ui::record_caller(site[ui::kBatchSubmit], synthetic_image_address(0x2AACF0 + 0x600)) ==
              ui::CallerRecord::Recorded,
          "a batch-submit caller lands in the batch-submit ring");
    check(site[ui::kRectangleFill].count == 0 && site[ui::kTransformCopy].count == 0,
          "the other two site rings stay empty");
    check(ui::caller_at(site[ui::kBatchBuild], 0) == synthetic_image_address(0x63AD7D) &&
              ui::caller_at(site[ui::kBatchSubmit], 0) == synthetic_image_address(0x2AACF0 + 0x600),
          "the two measured batch rings hold their own return sites");
}

}  // namespace

int main() {
    test_site_table();
    test_validation();
    test_caller_classification();
    test_caller_ring_bounded();
    test_caller_formatting();
    test_caller_rings_per_site();
    std::printf("ui_draw_probe_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
