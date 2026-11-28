#pragma once

// Pure, bounded readiness and plane-identity contracts for a title-owned movie frame -> PresentX observation.
// This deliberately does not allocate, inspect title memory, or call Bink/PresentX. The existing opt-in present gate has no
// Bink, upload, or plane-role evidence producer yet; it supplies an empty chain and conservatively refuses plane 1.

#include <cstdint>

namespace palmetto::movie_overlay_readiness {

enum class Stage : std::uint8_t {
    off,
    registration,
    decode,
    upload,
    overlay_present,
    ready,
};

// The runtime switch is deliberately exact: unset/empty/0 preserve the old plane gate, 1 requires
// a complete evidence chain, and any other nonempty spelling is invalid and also preserves the old gate.
enum class RuntimeGate : std::uint8_t {
    legacy,
    require_ready,
    invalid,
};

inline constexpr RuntimeGate parse_runtime_gate_switch(const char* value) noexcept {
    if (!value || value[0] == '\0' || (value[0] == '0' && value[1] == '\0')) return RuntimeGate::legacy;
    if (value[0] == '1' && value[1] == '\0') return RuntimeGate::require_ready;
    return RuntimeGate::invalid;
}

enum class Refusal : std::uint8_t {
    none,
    opt_in_off,
    registration_not_observed,
    registration_unconfirmed,
    event_order_missing,
    decoder_identity_missing,
    decode_not_observed,
    decode_unconfirmed,
    decode_before_registration,
    decoder_identity_mismatch,
    frame_identity_missing,
    upload_not_observed,
    upload_unconfirmed,
    upload_before_decode,
    upload_decoder_mismatch,
    upload_frame_mismatch,
    resource_identity_missing,
    present_not_observed,
    present_unconfirmed,
    present_before_upload,
    overlay_plane_not_observed,
    unsupported_plane_count,
    unexpected_movie_plane_index,
    present_frame_mismatch,
    present_resource_mismatch,
    plane_identity_not_observed,
    plane_identity_unconfirmed,
    plane_identity_order_mismatch,
    primary_plane_index_mismatch,
    primary_plane_role_mismatch,
    secondary_plane_index_mismatch,
    secondary_plane_role_unknown,
    secondary_plane_role_invalid,
    second_plane_not_movie,
    movie_plane_resource_mismatch,
};

inline constexpr const char* refusal_name(Refusal refusal) noexcept {
    switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::opt_in_off: return "opt-in-off";
    case Refusal::registration_not_observed: return "frame-registration-not-observed";
    case Refusal::registration_unconfirmed: return "frame-registration-unconfirmed";
    case Refusal::event_order_missing: return "event-order-missing";
    case Refusal::decoder_identity_missing: return "decoder-identity-missing";
    case Refusal::decode_not_observed: return "frame-decode-not-observed";
    case Refusal::decode_unconfirmed: return "frame-decode-unconfirmed";
    case Refusal::decode_before_registration: return "decode-before-registration";
    case Refusal::decoder_identity_mismatch: return "decoder-identity-mismatch";
    case Refusal::frame_identity_missing: return "frame-identity-missing";
    case Refusal::upload_not_observed: return "plane-upload-not-observed";
    case Refusal::upload_unconfirmed: return "plane-upload-unconfirmed";
    case Refusal::upload_before_decode: return "plane-upload-before-decode";
    case Refusal::upload_decoder_mismatch: return "plane-upload-decoder-mismatch";
    case Refusal::upload_frame_mismatch: return "plane-upload-frame-mismatch";
    case Refusal::resource_identity_missing: return "resource-identity-missing";
    case Refusal::present_not_observed: return "overlay-present-not-observed";
    case Refusal::present_unconfirmed: return "overlay-present-unconfirmed";
    case Refusal::present_before_upload: return "overlay-present-before-upload";
    case Refusal::overlay_plane_not_observed: return "overlay-plane-not-observed";
    case Refusal::unsupported_plane_count: return "unsupported-plane-count";
    case Refusal::unexpected_movie_plane_index: return "unexpected-movie-plane-index";
    case Refusal::present_frame_mismatch: return "present-frame-mismatch";
    case Refusal::present_resource_mismatch: return "present-resource-mismatch";
    case Refusal::plane_identity_not_observed: return "plane-identity-not-observed";
    case Refusal::plane_identity_unconfirmed: return "plane-identity-unconfirmed";
    case Refusal::plane_identity_order_mismatch: return "plane-identity-order-mismatch";
    case Refusal::primary_plane_index_mismatch: return "primary-plane-index-mismatch";
    case Refusal::primary_plane_role_mismatch: return "primary-plane-role-mismatch";
    case Refusal::secondary_plane_index_mismatch: return "second-plane-index-mismatch";
    case Refusal::secondary_plane_role_unknown: return "second-plane-role-unknown";
    case Refusal::secondary_plane_role_invalid: return "second-plane-role-invalid";
    case Refusal::second_plane_not_movie: return "second-plane-not-movie";
    case Refusal::movie_plane_resource_mismatch: return "movie-plane-resource-mismatch";
    }
    return "unknown-refusal";
}

// `confirmed` means the corresponding stage has been independently evidenced, not inferred from an unmeasured return
// value. The keys and order are bounded observation tokens, not title pointers, frame contents, or ABI fields. A future
// instrument assigns them only after it has independently established the relevant identity/correlation.
struct RegistrationEvidence {
    bool observed = false;
    bool confirmed = false;
    std::uint64_t order = 0;
    std::uint64_t decoder_key = 0;
};

struct DecodeEvidence {
    bool observed = false;
    bool confirmed = false;
    std::uint64_t order = 0;
    std::uint64_t decoder_key = 0;
    std::uint64_t frame_key = 0;
};

struct UploadEvidence {
    bool observed = false;
    bool confirmed = false;
    std::uint64_t order = 0;
    std::uint64_t decoder_key = 0;
    std::uint64_t frame_key = 0;
    std::uint64_t resource_key = 0;
};

// Plane roles are observation labels, not PresentX ABI fields. A role is confirmed only when independent evidence
// distinguishes the title's primary plane, its movie upload, and UI/other overlays; index 1 alone never means movie.
enum class PlaneRole : std::uint8_t {
    unknown,
    primary,
    movie,
    ui_overlay,
    other_overlay,
};

struct PlaneIdentityEvidence {
    bool observed = false;
    bool confirmed = false;
    std::uint64_t order = 0;  // order of the PresentX snapshot that supplied this plane identity
    std::uint32_t index = 0;
    PlaneRole role = PlaneRole::unknown;
    std::uint64_t resource_key = 0;
};

struct PresentEvidence {
    bool observed = false;
    bool confirmed = false;
    std::uint64_t order = 0;
    std::uint64_t frame_key = 0;
    std::uint64_t resource_key = 0;
    std::uint32_t plane_count = 0;
    std::uint32_t movie_plane_index = 0;
    // Identity is captured for both positions from the same successful PresentX, not inferred from plane count/order.
    PlaneIdentityEvidence primary_plane{};
    PlaneIdentityEvidence second_plane{};
};

struct Evidence {
    // Explicitly false by default. Runtime sets this only behind PALMETTO_PRESENT_MOVIE_OVERLAY_READY_ONLY=1;
    // no Bink-registration, decode, or upload evidence producer is wired yet.
    bool opt_in = false;
    RegistrationEvidence registration{};
    DecodeEvidence decode{};
    UploadEvidence upload{};
    PresentEvidence present{};
};

struct Decision {
    Stage stage = Stage::off;
    Refusal refusal = Refusal::opt_in_off;

    constexpr bool ready() const noexcept { return refusal == Refusal::none; }
    constexpr const char* reason() const noexcept { return refusal_name(refusal); }
};

inline constexpr Decision refuse(Stage stage, Refusal reason) noexcept {
    return {stage, reason};
}

struct PlaneIdentityDecision {
    PlaneRole second_role = PlaneRole::unknown;
    Refusal refusal = Refusal::plane_identity_not_observed;

    constexpr bool classified() const noexcept { return refusal == Refusal::none; }
    constexpr const char* reason() const noexcept { return refusal_name(refusal); }
};

// Classify the two supported PresentX positions without guessing that every secondary plane is a movie. Both descriptors
// must be observed and independently confirmed against this same presentation order; identity tokens are opaque.
inline constexpr PlaneIdentityDecision classify_second_plane(const PresentEvidence& p) noexcept {
    if (!p.observed) return {PlaneRole::unknown, Refusal::present_not_observed};
    if (!p.confirmed) return {PlaneRole::unknown, Refusal::present_unconfirmed};
    if (p.order == 0) return {PlaneRole::unknown, Refusal::event_order_missing};
    if (p.plane_count < 2) return {PlaneRole::unknown, Refusal::overlay_plane_not_observed};
    if (p.plane_count != 2) return {PlaneRole::unknown, Refusal::unsupported_plane_count};

    const auto& primary = p.primary_plane;
    const auto& second = p.second_plane;
    if (!primary.observed || !second.observed)
        return {PlaneRole::unknown, Refusal::plane_identity_not_observed};
    if (!primary.confirmed || !second.confirmed)
        return {PlaneRole::unknown, Refusal::plane_identity_unconfirmed};
    if (primary.order == 0 || second.order == 0)
        return {PlaneRole::unknown, Refusal::event_order_missing};
    if (primary.order != p.order || second.order != p.order)
        return {PlaneRole::unknown, Refusal::plane_identity_order_mismatch};
    if (primary.index != 0) return {PlaneRole::unknown, Refusal::primary_plane_index_mismatch};
    if (second.index != 1) return {PlaneRole::unknown, Refusal::secondary_plane_index_mismatch};
    if (primary.role != PlaneRole::primary)
        return {PlaneRole::unknown, Refusal::primary_plane_role_mismatch};
    if (second.role == PlaneRole::unknown)
        return {PlaneRole::unknown, Refusal::secondary_plane_role_unknown};
    if (second.role == PlaneRole::primary)
        return {PlaneRole::unknown, Refusal::secondary_plane_role_invalid};
    if (second.role != PlaneRole::movie && second.role != PlaneRole::ui_overlay &&
        second.role != PlaneRole::other_overlay)
        return {PlaneRole::unknown, Refusal::secondary_plane_role_invalid};
    if (primary.resource_key == 0 || second.resource_key == 0)
        return {PlaneRole::unknown, Refusal::resource_identity_missing};

    return {second.role, Refusal::none};
}

// Chain-only readiness predicate retained for the existing consumer; it does not establish semantic plane roles. Use the
// identity-aware gate when a producer can independently classify the positions. It validates no Bink ABI, pixel contents,
// transform/upload contents, plane flags, blend mode, or presentation correctness.
inline constexpr Decision evaluate(const Evidence& e) noexcept {
    if (!e.opt_in) return refuse(Stage::off, Refusal::opt_in_off);

    const auto& r = e.registration;
    if (!r.observed) return refuse(Stage::registration, Refusal::registration_not_observed);
    if (!r.confirmed) return refuse(Stage::registration, Refusal::registration_unconfirmed);
    if (r.order == 0) return refuse(Stage::registration, Refusal::event_order_missing);
    if (r.decoder_key == 0) return refuse(Stage::registration, Refusal::decoder_identity_missing);

    const auto& d = e.decode;
    if (!d.observed) return refuse(Stage::decode, Refusal::decode_not_observed);
    if (!d.confirmed) return refuse(Stage::decode, Refusal::decode_unconfirmed);
    if (d.order == 0) return refuse(Stage::decode, Refusal::event_order_missing);
    if (d.order <= r.order) return refuse(Stage::decode, Refusal::decode_before_registration);
    if (d.decoder_key == 0) return refuse(Stage::decode, Refusal::decoder_identity_missing);
    if (d.decoder_key != r.decoder_key) return refuse(Stage::decode, Refusal::decoder_identity_mismatch);
    if (d.frame_key == 0) return refuse(Stage::decode, Refusal::frame_identity_missing);

    const auto& u = e.upload;
    if (!u.observed) return refuse(Stage::upload, Refusal::upload_not_observed);
    if (!u.confirmed) return refuse(Stage::upload, Refusal::upload_unconfirmed);
    if (u.order == 0) return refuse(Stage::upload, Refusal::event_order_missing);
    if (u.order <= d.order) return refuse(Stage::upload, Refusal::upload_before_decode);
    if (u.decoder_key == 0) return refuse(Stage::upload, Refusal::decoder_identity_missing);
    if (u.decoder_key != d.decoder_key) return refuse(Stage::upload, Refusal::upload_decoder_mismatch);
    if (u.frame_key == 0) return refuse(Stage::upload, Refusal::frame_identity_missing);
    if (u.frame_key != d.frame_key) return refuse(Stage::upload, Refusal::upload_frame_mismatch);
    if (u.resource_key == 0) return refuse(Stage::upload, Refusal::resource_identity_missing);

    const auto& p = e.present;
    if (!p.observed) return refuse(Stage::overlay_present, Refusal::present_not_observed);
    if (!p.confirmed) return refuse(Stage::overlay_present, Refusal::present_unconfirmed);
    if (p.order == 0) return refuse(Stage::overlay_present, Refusal::event_order_missing);
    if (p.order <= u.order) return refuse(Stage::overlay_present, Refusal::present_before_upload);
    if (p.plane_count < 2) return refuse(Stage::overlay_present, Refusal::overlay_plane_not_observed);
    if (p.plane_count != 2) return refuse(Stage::overlay_present, Refusal::unsupported_plane_count);
    if (p.movie_plane_index != 1) return refuse(Stage::overlay_present, Refusal::unexpected_movie_plane_index);
    if (p.frame_key == 0) return refuse(Stage::overlay_present, Refusal::frame_identity_missing);
    if (p.frame_key != u.frame_key) return refuse(Stage::overlay_present, Refusal::present_frame_mismatch);
    if (p.resource_key == 0) return refuse(Stage::overlay_present, Refusal::resource_identity_missing);
    if (p.resource_key != u.resource_key) return refuse(Stage::overlay_present, Refusal::present_resource_mismatch);

    return {Stage::ready, Refusal::none};
}

struct GateDecision {
    bool allow_overlay = false;
    Decision readiness{};
};

// The legacy mode is an exact passthrough. The evidence gate is intentionally conservative: it permits plane 1 only when the
// full registration -> decode -> upload -> two-plane-present chain is correlated. Invalid switch spellings are handled by the
// runtime parser as a named diagnostic and are also a passthrough.
inline constexpr GateDecision gate_second_plane(RuntimeGate mode, bool overlay_requested, const Evidence& evidence) noexcept {
    if (!overlay_requested) return {false, refuse(Stage::off, Refusal::opt_in_off)};
    if (mode != RuntimeGate::require_ready) return {true, refuse(Stage::off, Refusal::opt_in_off)};
    const Decision readiness = evaluate(evidence);
    return {readiness.ready(), readiness};
}

enum class PlaneGateAction : std::uint8_t {
    not_requested,
    legacy_passthrough,
    non_movie_passthrough,
    movie_ready,
    refused,
};

struct IdentityGateDecision {
    bool allow_overlay = false;
    PlaneGateAction action = PlaneGateAction::refused;
    // Meaningful as movie readiness only for movie_ready/refused; non-movie passthrough is identified by `action`.
    Decision readiness{};
};

// Identity-aware policy for a future evidence producer. The current runtime still calls gate_second_plane with an empty chain,
// so its strict mode presently refuses every selected candidate at registration without classifying any plane role.
inline constexpr IdentityGateDecision gate_identified_second_plane(RuntimeGate mode, bool overlay_requested,
                                                                    const Evidence& evidence) noexcept {
    if (!overlay_requested)
        return {false, PlaneGateAction::not_requested, refuse(Stage::off, Refusal::opt_in_off)};
    if (mode != RuntimeGate::require_ready)
        return {true, PlaneGateAction::legacy_passthrough, refuse(Stage::off, Refusal::opt_in_off)};
    if (!evidence.opt_in)
        return {false, PlaneGateAction::refused, refuse(Stage::off, Refusal::opt_in_off)};

    const PlaneIdentityDecision plane = classify_second_plane(evidence.present);
    if (!plane.classified())
        return {false, PlaneGateAction::refused, refuse(Stage::overlay_present, plane.refusal)};
    if (plane.second_role == PlaneRole::ui_overlay || plane.second_role == PlaneRole::other_overlay)
        return {true, PlaneGateAction::non_movie_passthrough,
                refuse(Stage::overlay_present, Refusal::second_plane_not_movie)};

    const Decision readiness = evaluate(evidence);
    if (!readiness.ready()) return {false, PlaneGateAction::refused, readiness};
    if (evidence.present.second_plane.resource_key != evidence.upload.resource_key)
        return {false, PlaneGateAction::refused,
                refuse(Stage::overlay_present, Refusal::movie_plane_resource_mismatch)};
    return {true, PlaneGateAction::movie_ready, readiness};
}

}  // namespace palmetto::movie_overlay_readiness
