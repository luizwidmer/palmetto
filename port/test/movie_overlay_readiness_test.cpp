// Synthetic tests for the bounded movie frame -> overlay readiness contract. No title memory, game data or GPU.
#include "../src/movie_overlay_readiness.h"

#include <cstdio>
#include <cstring>

using namespace palmetto::movie_overlay_readiness;

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

Evidence complete_evidence() {
    Evidence e{};
    e.opt_in = true;
    e.registration = {true, true, 10, 41};
    e.decode = {true, true, 20, 41, 71};
    e.upload = {true, true, 30, 41, 71, 91};
    e.present = {true, true, 40, 71, 91, 2, 1};
    return e;
}

int expect_refusal(const Evidence& e, Stage stage, Refusal refusal) {
    const Decision got = evaluate(e);
    if (got.stage != stage || got.refusal != refusal || got.ready() ||
        std::strcmp(got.reason(), refusal_name(refusal)) != 0) {
        std::fprintf(stderr, "line %d: expected %s at stage %u, got %s at stage %u\n",
                     __LINE__, refusal_name(refusal), static_cast<unsigned>(stage), got.reason(),
                     static_cast<unsigned>(got.stage));
        return 1;
    }
    return 0;
}

int test_default_is_off() {
    const Evidence empty{};
    EXPECT(expect_refusal(empty, Stage::off, Refusal::opt_in_off) == 0);

    Evidence otherwise_complete = complete_evidence();
    otherwise_complete.opt_in = false;
    EXPECT(expect_refusal(otherwise_complete, Stage::off, Refusal::opt_in_off) == 0);
    return 0;
}

int test_registration_gate() {
    Evidence e{};
    e.opt_in = true;
    EXPECT(expect_refusal(e, Stage::registration, Refusal::registration_not_observed) == 0);

    e.registration = {true, false, 10, 41};
    EXPECT(expect_refusal(e, Stage::registration, Refusal::registration_unconfirmed) == 0);
    e.registration = {true, true, 0, 41};
    EXPECT(expect_refusal(e, Stage::registration, Refusal::event_order_missing) == 0);
    e.registration = {true, true, 10, 0};
    EXPECT(expect_refusal(e, Stage::registration, Refusal::decoder_identity_missing) == 0);
    return 0;
}

int test_decode_gate() {
    Evidence e = complete_evidence();
    e.decode.observed = false;
    EXPECT(expect_refusal(e, Stage::decode, Refusal::decode_not_observed) == 0);
    e = complete_evidence();
    e.decode.confirmed = false;
    EXPECT(expect_refusal(e, Stage::decode, Refusal::decode_unconfirmed) == 0);
    e = complete_evidence();
    e.decode.order = 0;
    EXPECT(expect_refusal(e, Stage::decode, Refusal::event_order_missing) == 0);
    e = complete_evidence();
    e.decode.order = e.registration.order;
    EXPECT(expect_refusal(e, Stage::decode, Refusal::decode_before_registration) == 0);
    e = complete_evidence();
    e.decode.decoder_key = 0;
    EXPECT(expect_refusal(e, Stage::decode, Refusal::decoder_identity_missing) == 0);
    e = complete_evidence();
    e.decode.decoder_key = 99;
    EXPECT(expect_refusal(e, Stage::decode, Refusal::decoder_identity_mismatch) == 0);
    e = complete_evidence();
    e.decode.frame_key = 0;
    EXPECT(expect_refusal(e, Stage::decode, Refusal::frame_identity_missing) == 0);
    return 0;
}

int test_upload_gate() {
    Evidence e = complete_evidence();
    e.upload.observed = false;
    EXPECT(expect_refusal(e, Stage::upload, Refusal::upload_not_observed) == 0);
    e = complete_evidence();
    e.upload.confirmed = false;
    EXPECT(expect_refusal(e, Stage::upload, Refusal::upload_unconfirmed) == 0);
    e = complete_evidence();
    e.upload.order = 0;
    EXPECT(expect_refusal(e, Stage::upload, Refusal::event_order_missing) == 0);
    e = complete_evidence();
    e.upload.order = e.decode.order;
    EXPECT(expect_refusal(e, Stage::upload, Refusal::upload_before_decode) == 0);
    e = complete_evidence();
    e.upload.decoder_key = 0;
    EXPECT(expect_refusal(e, Stage::upload, Refusal::decoder_identity_missing) == 0);
    e = complete_evidence();
    e.upload.decoder_key = 42;
    EXPECT(expect_refusal(e, Stage::upload, Refusal::upload_decoder_mismatch) == 0);
    e = complete_evidence();
    e.upload.frame_key = 0;
    EXPECT(expect_refusal(e, Stage::upload, Refusal::frame_identity_missing) == 0);
    e = complete_evidence();
    e.upload.frame_key = 72;
    EXPECT(expect_refusal(e, Stage::upload, Refusal::upload_frame_mismatch) == 0);
    e = complete_evidence();
    e.upload.resource_key = 0;
    EXPECT(expect_refusal(e, Stage::upload, Refusal::resource_identity_missing) == 0);
    return 0;
}

int test_present_gate() {
    Evidence e = complete_evidence();
    e.present.observed = false;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::present_not_observed) == 0);
    e = complete_evidence();
    e.present.confirmed = false;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::present_unconfirmed) == 0);
    e = complete_evidence();
    e.present.order = 0;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::event_order_missing) == 0);
    e = complete_evidence();
    e.present.order = e.upload.order;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::present_before_upload) == 0);
    e = complete_evidence();
    e.present.plane_count = 1;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::overlay_plane_not_observed) == 0);
    e = complete_evidence();
    e.present.plane_count = 3;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::unsupported_plane_count) == 0);
    e = complete_evidence();
    e.present.movie_plane_index = 0;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::unexpected_movie_plane_index) == 0);
    e = complete_evidence();
    e.present.frame_key = 0;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::frame_identity_missing) == 0);
    e = complete_evidence();
    e.present.frame_key = 72;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::present_frame_mismatch) == 0);
    e = complete_evidence();
    e.present.resource_key = 0;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::resource_identity_missing) == 0);
    e = complete_evidence();
    e.present.resource_key = 92;
    EXPECT(expect_refusal(e, Stage::overlay_present, Refusal::present_resource_mismatch) == 0);
    return 0;
}

int test_refusal_names_are_stable() {
    struct NameCase {
        Refusal refusal;
        const char* name;
    };
    const NameCase cases[] = {
        {Refusal::opt_in_off, "opt-in-off"},
        {Refusal::registration_not_observed, "frame-registration-not-observed"},
        {Refusal::registration_unconfirmed, "frame-registration-unconfirmed"},
        {Refusal::decode_before_registration, "decode-before-registration"},
        {Refusal::upload_frame_mismatch, "plane-upload-frame-mismatch"},
        {Refusal::overlay_plane_not_observed, "overlay-plane-not-observed"},
        {Refusal::present_resource_mismatch, "present-resource-mismatch"},
    };
    for (const NameCase& c : cases) EXPECT(std::strcmp(refusal_name(c.refusal), c.name) == 0);
    return 0;
}

int test_correlated_two_plane_path_ready() {
    const Decision got = evaluate(complete_evidence());
    EXPECT(got.ready());
    EXPECT(got.stage == Stage::ready);
    EXPECT(got.refusal == Refusal::none);
    EXPECT(std::strcmp(got.reason(), "none") == 0);
    return 0;
}

}  // namespace

int main() {
    struct TestCase {
        const char* name;
        int (*run)();
    };
    const TestCase tests[] = {
        {"default opt-in off", test_default_is_off},
        {"registration evidence and named refusals", test_registration_gate},
        {"decode ordering and identity", test_decode_gate},
        {"upload correlation", test_upload_gate},
        {"two-plane present restrictions", test_present_gate},
        {"stable named refusal strings", test_refusal_names_are_stable},
        {"correlated movie overlay readiness", test_correlated_two_plane_path_ready},
    };

    int failures = 0;
    for (const TestCase& test : tests) {
        const int result = test.run();
        std::printf("%s: %s\n", test.name, result == 0 ? "ok" : "FAIL");
        failures += result;
    }
    std::printf("%u cases, %d failures\n", static_cast<unsigned>(sizeof(tests) / sizeof(tests[0])), failures);
    return failures == 0 ? 0 : 1;
}
