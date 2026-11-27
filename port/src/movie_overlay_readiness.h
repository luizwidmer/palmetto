#pragma once

// Pure, bounded readiness contract for a future title-owned movie frame -> overlay observation.
// This deliberately does not allocate, inspect title memory, or call Bink/PresentX. The live path has not yet produced a
// registered or decoded movie frame, so callers must not use it to synthesize one.

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

struct PresentEvidence {
    bool observed = false;
    bool confirmed = false;
    std::uint64_t order = 0;
    std::uint64_t frame_key = 0;
    std::uint64_t resource_key = 0;
    std::uint32_t plane_count = 0;
    std::uint32_t movie_plane_index = 0;
};

struct Evidence {
    // Explicitly false by default. No runtime caller is wired to this readiness-only helper.
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

// This gate answers only whether a complete, correlated two-plane movie observation exists. It does not validate the
// undocumented Bink buffer ABI, transform/upload contents, plane flags, blend mode, or presentation correctness.
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

}  // namespace palmetto::movie_overlay_readiness
