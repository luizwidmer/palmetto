// movie_draw_probe.h: the measured movie draw-path site table, its byte-shape refusal rule and the counter verdict, on
// synthetic bytes only. No game files: the tests build the measured instruction shapes from the header's own table and
// mutate copies of them (docs/peer_port_movie.md section 13).
#include "../src/movie_draw_probe.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

namespace md = palmetto::movie_draw;

void test_site_table() {
    check(md::kSiteCount == 6, "six measured sites are in the table");
    std::set<std::uint32_t> rvas;
    std::set<std::string> names;
    std::set<std::string> signatures;
    for (unsigned site = 0; site < md::kSiteCount; ++site) {
        const md::SiteShape& shape = md::kSites[site];
        check(shape.name && *shape.name, "every site is named");
        check(rvas.insert(shape.rva).second, "the site RVAs are unique");
        check(names.insert(shape.name).second, "the site names are unique");
        check(shape.signature != nullptr, "every site has a signature");
        check(shape.signature_length >= 4 && shape.signature_length <= 16,
              "every signature is a bounded instruction shape");
        check(signatures.insert(std::string(reinterpret_cast<const char*>(shape.signature), shape.signature_length))
                  .second,
              "no two sites share a signature");
        check(shape.words != nullptr && shape.word_count > 0, "every site captures at least one control word");
    }
    // The audit's four named sites and the two inside-gate controls, by the measured RVAs.
    check(md::kSites[md::kLayerDraw].rva == 0x23495A0, "the layer entry is 0x23495A0");
    check(md::kSites[md::kPlayerDraw].rva == 0x2349D80, "the player draw is 0x2349D80");
    check(md::kSites[md::kDrawReady].rva == 0x2349DD4, "the frame-ready compare is 0x2349DD4");
    check(md::kSites[md::kAdvance].rva == 0x51B45B0, "the second advance is 0x51B45B0");
    check(md::kSites[md::kAdvanceDecode].rva == 0x51B4646, "the advance's decode preparation is 0x51B4646");
    check(md::kSites[md::kDecode].rva == 0x2EF0E0, "the decode wrapper is 0x2EF0E0");
}

void test_control_words() {
    for (unsigned site = 0; site < md::kSiteCount; ++site) {
        const md::SiteShape& shape = md::kSites[site];
        std::set<std::string> names;
        for (unsigned word = 0; word < shape.word_count; ++word) {
            const md::ControlWord& control = shape.words[word];
            check(control.name && *control.name, "every control word is named");
            check(names.insert(control.name).second, "the control-word names are unique inside a site");
            check(control.base <= 1, "the control word's base is a direct register or the component's player pointer");
            check(control.reg < md::kRegisterCount, "the control word names a dumped register");
            check(control.width == 1 || control.width == 4 || control.width == 8,
                  "the control word has a byte, dword or qword width");
            check(control.base == 0 || control.deref != 0, "a derived control word names the pointer's offset");
            check(control.offset < 0x10000, "the control word's offset is inside the measured object layout");
        }
    }
    // The frame-ready gate's word is the draw's skip decision: +0x1c7 on the player in rax.
    const md::ControlWord& ready = md::kSites[md::kDrawReady].words[1];
    check(std::strcmp(ready.name, "player+0x1c7") == 0, "the frame-ready word is player+0x1c7");
    check(ready.reg == md::kRax && ready.offset == 0x1c7 && ready.width == 1,
          "the frame-ready word is a byte at [rax+0x1c7]");
    const md::ControlWord& state = md::kSites[md::kDecode].words[0];
    check(std::strcmp(state.name, "player+0x1c4") == 0 && state.reg == md::kRcx,
          "the decode control reads the player state byte from rcx");
}

void test_shape_rule() {
    unsigned char bytes[16] = {};
    check(!md::shape_matches(0, nullptr), "the null byte pointer is refused");
    check(!md::shape_matches(md::kSiteCount, bytes), "a site index past the table is refused");

    for (unsigned site = 0; site < md::kSiteCount; ++site) {
        const md::SiteShape& shape = md::kSites[site];
        std::memcpy(bytes, shape.signature, shape.signature_length);
        check(md::shape_matches(site, bytes), "the measured shape matches its site");
        // One wrong byte anywhere in the shape is refused.
        for (unsigned at = 0; at < shape.signature_length; ++at) {
            std::memcpy(bytes, shape.signature, shape.signature_length);
            bytes[at] ^= 0xFF;
            check(!md::shape_matches(site, bytes), "a mutated instruction byte is refused");
        }
        // A shape that starts one byte later is a different instruction stream and is refused.
        std::memset(bytes, 0xCC, sizeof bytes);
        std::memcpy(bytes, shape.signature + 1, shape.signature_length - 1);
        check(!md::shape_matches(site, bytes), "a shifted instruction stream is refused");
        // A signature with bytes following it is still the measured shape: the check is a prefix.
        std::memcpy(bytes, shape.signature, shape.signature_length);
        std::memset(bytes + shape.signature_length, 0xCC, sizeof bytes - shape.signature_length);
        check(md::shape_matches(site, bytes), "the check compares the measured prefix only");
    }
}

void test_verdict() {
    check(md::verdict_of(0, 0, 0, 0) == md::Verdict::kNeverIssued, "no counters is never-issued");
    check(std::strcmp(md::verdict_name(md::Verdict::kNeverIssued), "never-issued") == 0,
          "never-issued has its name");
    check(md::verdict_of(3, 0, 0, 0) == md::Verdict::kLayerOnly, "a layer hit alone is layer-only");
    check(md::verdict_of(3, 1, 0, 0) == md::Verdict::kDrawNotReady, "a draw hit without the frame-ready compare");
    check(md::verdict_of(3, 9, 9, 0) == md::Verdict::kReadyNotDecoded,
          "the frame-ready compare without a decode");
    check(md::verdict_of(0, 0, 0, 2) == md::Verdict::kDecodeReached,
          "the decode alone (the second advance's path) is decode-reached");
    check(md::verdict_of(3, 9, 9, 2) == md::Verdict::kDecodeReached, "decode wins over the shallower counters");
    check(md::verdict_of(0, 4, 0, 0) == md::Verdict::kDrawNotReady,
          "a draw reached without the layer entry still names the draw step");
    check(md::verdict_of(0, 0, 1, 0) == md::Verdict::kNeverIssued,
          "the frame-ready site cannot fire without the draw, but the verdict stays conservative");
    for (unsigned value = 0; value <= static_cast<unsigned>(md::Verdict::kDecodeReached); ++value)
        check(md::verdict_name(static_cast<md::Verdict>(value)) != nullptr, "every verdict value has a name");
}

}  // namespace

int main() {
    test_site_table();
    test_control_words();
    test_shape_rule();
    test_verdict();
    std::printf("movie_draw_probe_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
