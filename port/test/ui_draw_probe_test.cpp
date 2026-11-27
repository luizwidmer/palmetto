// ui_draw_probe.h: pure validation of the measured 2D-batch sites against synthetic bytes only.
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

}  // namespace

int main() {
    test_site_table();
    test_validation();
    std::printf("ui_draw_probe_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
