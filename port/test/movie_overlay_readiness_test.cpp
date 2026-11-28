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
    e.present.primary_plane = {true, true, 40, 0, PlaneRole::primary, 81};
    e.present.second_plane = {true, true, 40, 1, PlaneRole::movie, 91};
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

int expect_identity_refusal(const Evidence& e, Refusal refusal) {
    const IdentityGateDecision got = gate_identified_second_plane(RuntimeGate::require_ready, true, e);
    if (got.allow_overlay || got.action != PlaneGateAction::refused || got.readiness.refusal != refusal ||
        std::strcmp(got.readiness.reason(), refusal_name(refusal)) != 0) {
        std::fprintf(stderr, "line %d: expected identity refusal %s, got %s (allow=%d, action=%u)\n",
                     __LINE__, refusal_name(refusal), got.readiness.reason(), got.allow_overlay ? 1 : 0,
                     static_cast<unsigned>(got.action));
        return 1;
    }
    return 0;
}

int test_runtime_gate_parser_and_legacy_passthrough() {
    EXPECT(parse_runtime_gate_switch(nullptr) == RuntimeGate::legacy);
    EXPECT(parse_runtime_gate_switch("") == RuntimeGate::legacy);
    EXPECT(parse_runtime_gate_switch("0") == RuntimeGate::legacy);
    EXPECT(parse_runtime_gate_switch("1") == RuntimeGate::require_ready);
    EXPECT(parse_runtime_gate_switch("true") == RuntimeGate::invalid);
    EXPECT(parse_runtime_gate_switch("01") == RuntimeGate::invalid);

    Evidence empty{};
    empty.opt_in = true;
    const GateDecision legacy = gate_second_plane(RuntimeGate::legacy, true, empty);
    EXPECT(legacy.allow_overlay);
    EXPECT(legacy.readiness.refusal == Refusal::opt_in_off);
    const GateDecision invalid = gate_second_plane(RuntimeGate::invalid, true, empty);
    EXPECT(invalid.allow_overlay);
    EXPECT(invalid.readiness.refusal == Refusal::opt_in_off);
    const GateDecision no_overlay = gate_second_plane(RuntimeGate::require_ready, false, empty);
    EXPECT(!no_overlay.allow_overlay);
    return 0;
}

int test_runtime_gate_requires_correlated_evidence() {
    Evidence incomplete{};
    incomplete.opt_in = true;
    const GateDecision refused = gate_second_plane(RuntimeGate::require_ready, true, incomplete);
    EXPECT(!refused.allow_overlay);
    EXPECT(refused.readiness.stage == Stage::registration);
    EXPECT(refused.readiness.refusal == Refusal::registration_not_observed);
    EXPECT(std::strcmp(refused.readiness.reason(), "frame-registration-not-observed") == 0);

    const GateDecision ready = gate_second_plane(RuntimeGate::require_ready, true, complete_evidence());
    EXPECT(ready.allow_overlay);
    EXPECT(ready.readiness.ready());
    EXPECT(ready.readiness.stage == Stage::ready);
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

int test_plane_identity_and_order_contract() {
    Evidence e = complete_evidence();
    PlaneIdentityDecision identity = classify_second_plane(e.present);
    EXPECT(identity.classified());
    EXPECT(identity.second_role == PlaneRole::movie);

    e = complete_evidence();
    e.present.primary_plane.observed = false;
    EXPECT(expect_identity_refusal(e, Refusal::plane_identity_not_observed) == 0);
    e = complete_evidence();
    e.present.second_plane.confirmed = false;
    EXPECT(expect_identity_refusal(e, Refusal::plane_identity_unconfirmed) == 0);
    e = complete_evidence();
    e.present.second_plane.order = e.present.order - 1;
    EXPECT(expect_identity_refusal(e, Refusal::plane_identity_order_mismatch) == 0);
    e = complete_evidence();
    e.present.primary_plane.order = 0;
    EXPECT(expect_identity_refusal(e, Refusal::event_order_missing) == 0);
    e = complete_evidence();
    e.present.primary_plane.index = 1;
    EXPECT(expect_identity_refusal(e, Refusal::primary_plane_index_mismatch) == 0);
    e = complete_evidence();
    e.present.primary_plane.role = PlaneRole::ui_overlay;
    EXPECT(expect_identity_refusal(e, Refusal::primary_plane_role_mismatch) == 0);
    e = complete_evidence();
    e.present.second_plane.index = 0;
    EXPECT(expect_identity_refusal(e, Refusal::secondary_plane_index_mismatch) == 0);
    e = complete_evidence();
    e.present.second_plane.role = PlaneRole::unknown;
    EXPECT(expect_identity_refusal(e, Refusal::secondary_plane_role_unknown) == 0);
    e = complete_evidence();
    e.present.second_plane.role = PlaneRole::primary;
    EXPECT(expect_identity_refusal(e, Refusal::secondary_plane_role_invalid) == 0);
    e = complete_evidence();
    e.present.second_plane.resource_key = 92;
    EXPECT(expect_identity_refusal(e, Refusal::movie_plane_resource_mismatch) == 0);

    e = complete_evidence();
    e.present.second_plane.role = PlaneRole::ui_overlay;
    identity = classify_second_plane(e.present);
    EXPECT(identity.classified());
    EXPECT(identity.second_role == PlaneRole::ui_overlay);
    e.present.second_plane.role = PlaneRole::other_overlay;
    identity = classify_second_plane(e.present);
    EXPECT(identity.classified());
    EXPECT(identity.second_role == PlaneRole::other_overlay);
    return 0;
}

int test_identity_aware_gate_and_legacy_contract() {
    Evidence empty{};
    const IdentityGateDecision legacy = gate_identified_second_plane(RuntimeGate::legacy, true, empty);
    EXPECT(legacy.allow_overlay);
    EXPECT(legacy.action == PlaneGateAction::legacy_passthrough);
    const IdentityGateDecision invalid = gate_identified_second_plane(RuntimeGate::invalid, true, empty);
    EXPECT(invalid.allow_overlay);
    EXPECT(invalid.action == PlaneGateAction::legacy_passthrough);
    const IdentityGateDecision not_requested = gate_identified_second_plane(RuntimeGate::require_ready, false, empty);
    EXPECT(!not_requested.allow_overlay);
    EXPECT(not_requested.action == PlaneGateAction::not_requested);

    Evidence ui = complete_evidence();
    ui.present.second_plane.role = PlaneRole::ui_overlay;
    ui.registration = {};
    ui.decode = {};
    ui.upload = {};
    const IdentityGateDecision ui_gate = gate_identified_second_plane(RuntimeGate::require_ready, true, ui);
    EXPECT(ui_gate.allow_overlay);
    EXPECT(ui_gate.action == PlaneGateAction::non_movie_passthrough);
    EXPECT(ui_gate.readiness.refusal == Refusal::second_plane_not_movie);

    Evidence other = complete_evidence();
    other.present.second_plane.role = PlaneRole::other_overlay;
    other.registration = {};
    other.decode = {};
    other.upload = {};
    const IdentityGateDecision other_gate = gate_identified_second_plane(RuntimeGate::require_ready, true, other);
    EXPECT(other_gate.allow_overlay);
    EXPECT(other_gate.action == PlaneGateAction::non_movie_passthrough);

    Evidence unknown = complete_evidence();
    unknown.present.second_plane.role = PlaneRole::unknown;
    const IdentityGateDecision unknown_gate = gate_identified_second_plane(RuntimeGate::require_ready, true, unknown);
    EXPECT(!unknown_gate.allow_overlay);
    EXPECT(unknown_gate.action == PlaneGateAction::refused);
    EXPECT(unknown_gate.readiness.refusal == Refusal::secondary_plane_role_unknown);

    Evidence missing_registration = complete_evidence();
    missing_registration.registration.observed = false;
    const IdentityGateDecision movie_unready =
        gate_identified_second_plane(RuntimeGate::require_ready, true, missing_registration);
    EXPECT(!movie_unready.allow_overlay);
    EXPECT(movie_unready.readiness.refusal == Refusal::registration_not_observed);

    const IdentityGateDecision movie_ready =
        gate_identified_second_plane(RuntimeGate::require_ready, true, complete_evidence());
    EXPECT(movie_ready.allow_overlay);
    EXPECT(movie_ready.action == PlaneGateAction::movie_ready);
    EXPECT(movie_ready.readiness.ready());

    Evidence opted_off = complete_evidence();
    opted_off.opt_in = false;
    const IdentityGateDecision off = gate_identified_second_plane(RuntimeGate::require_ready, true, opted_off);
    EXPECT(!off.allow_overlay);
    EXPECT(off.readiness.refusal == Refusal::opt_in_off);

    Evidence ui_old_gate = complete_evidence();
    ui_old_gate.present.second_plane.role = PlaneRole::ui_overlay;
    const GateDecision old_gate = gate_second_plane(RuntimeGate::require_ready, true, ui_old_gate);
    EXPECT(old_gate.allow_overlay);
    EXPECT(old_gate.readiness.ready());

    Evidence legacy_complete = complete_evidence();
    legacy_complete.present.primary_plane = {};
    legacy_complete.present.second_plane = {};
    const GateDecision unchanged_positive =
        gate_second_plane(RuntimeGate::require_ready, true, legacy_complete);
    EXPECT(unchanged_positive.allow_overlay);
    EXPECT(unchanged_positive.readiness.ready());
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
        {Refusal::plane_identity_not_observed, "plane-identity-not-observed"},
        {Refusal::plane_identity_unconfirmed, "plane-identity-unconfirmed"},
        {Refusal::plane_identity_order_mismatch, "plane-identity-order-mismatch"},
        {Refusal::secondary_plane_role_unknown, "second-plane-role-unknown"},
        {Refusal::second_plane_not_movie, "second-plane-not-movie"},
        {Refusal::movie_plane_resource_mismatch, "movie-plane-resource-mismatch"},
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
        {"runtime gate parsing and legacy passthrough", test_runtime_gate_parser_and_legacy_passthrough},
        {"runtime gate needs correlated evidence", test_runtime_gate_requires_correlated_evidence},
        {"registration evidence and named refusals", test_registration_gate},
        {"decode ordering and identity", test_decode_gate},
        {"upload correlation", test_upload_gate},
        {"two-plane present restrictions", test_present_gate},
        {"plane identity and order contract", test_plane_identity_and_order_contract},
        {"identity-aware gate and legacy contract", test_identity_aware_gate_and_legacy_contract},
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
