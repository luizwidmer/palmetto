// Conservative contract boundary for the console DSP effect path.
//
// The local boot has not called XDspActivate or XDspSubmitCommand, and the host endpoint
// observation is a single measured snapshot (docs/audio_shims.md, docs/silent_audio.md).
// This header records only that evidence: the measured spatial PCM request can be matched;
// the host shared-mix subtype and any effect descriptor/transform are refused by name. It
// never interprets or dereferences a title descriptor and does not render audio.
#ifndef PALMETTO_XAPU_EFFECTS_H
#define PALMETTO_XAPU_EFFECTS_H

#include <cstdint>

namespace rt::xapu_effects {

constexpr const char* kSwitchName = "PALMETTO_AUDIO_EFFECTS";
constexpr const char* kEndpointFormatUnmeasured = "endpoint_format_unmeasured";
constexpr const char* kEffectDescriptorUnmeasured = "effect_descriptor_unmeasured";
constexpr const char* kEffectKindUnimplemented = "effect_kind_unimplemented";

enum class EndpointKind : std::uint8_t {
    SharedMix,
    SpatialObject,
};

enum class SampleEncoding : std::uint8_t {
    Unmeasured,
    PcmInteger,
    IeeeFloat,
};

struct AudioFormat {
    std::uint16_t format_tag = 0;  // WAVEFORMATEX::wFormatTag
    std::uint32_t sample_rate_hz = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    SampleEncoding encoding = SampleEncoding::Unmeasured;
};

struct FormatAnswer {
    bool accepted = false;
    // The observed host shape may be named even when it is refused because part of the format is unmeasured.
    const char* observed_format = nullptr;
    const char* refusal = nullptr;
};

// Pure exact-match negotiation against this host's measured WASAPI answers only. GetMixFormat was recorded as
// WAVEFORMATEXTENSIBLE (tag 65534), 8 channels, 48 kHz and 32 bits; the retained evidence has no subtype GUID, so a
// concrete PCM/float interpretation must be refused. The spatial pre-mix check was measured as WAVE_FORMAT_PCM (tag 3),
// mono, 48 kHz, 32-bit. This is not a host query, resampler, channel mapper or claim about other devices.
constexpr FormatAnswer negotiate_endpoint_format(EndpointKind endpoint, AudioFormat requested) noexcept {
    if (endpoint == EndpointKind::SharedMix && requested.format_tag == 65534 && requested.sample_rate_hz == 48000 &&
        requested.channels == 8 && requested.bits_per_sample == 32) {
        return {false, "shared_mix_tag_65534_8ch_48k_32bit_subtype_unmeasured", kEndpointFormatUnmeasured};
    }
    if (endpoint == EndpointKind::SpatialObject && requested.format_tag == 3 && requested.sample_rate_hz == 48000 &&
        requested.channels == 1 && requested.bits_per_sample == 32 &&
        requested.encoding == SampleEncoding::PcmInteger) {
        return {true, "spatial_object_pcm_tag3_mono_48k_pcm32", nullptr};
    }
    return {false, nullptr, kEndpointFormatUnmeasured};
}

enum class LifecycleState : std::uint8_t {
    Connected,
    ActivationObserved,
    DeactivationObserved,
    Disconnected,
};

constexpr const char* lifecycle_state_name(LifecycleState state) noexcept {
    switch (state) {
        case LifecycleState::Connected: return "connected";
        case LifecycleState::ActivationObserved: return "activation_observed";
        case LifecycleState::DeactivationObserved: return "deactivation_observed";
        case LifecycleState::Disconnected: return "disconnected";
    }
    return "unknown";
}

// Tracks only the calls surrounding the unmeasured DSP effect contract. Activation is
// deliberately a named refusal: the pointer's layout and contents have not been measured.
// Submissions likewise do not identify a measured effect kind, so no input/output page is
// read and no status, command id or audio buffer is modified.
class EffectContractTracker {
public:
    constexpr LifecycleState state() const noexcept { return state_; }

    const char* observe_activation(const void* opaque_descriptor) noexcept {
        (void)opaque_descriptor;  // Opaque by design: never dereference unmeasured title memory.
        if (state_ == LifecycleState::Disconnected) return kEffectKindUnimplemented;
        state_ = LifecycleState::ActivationObserved;
        return kEffectDescriptorUnmeasured;
    }

    static constexpr const char* refuse_submission() noexcept { return kEffectKindUnimplemented; }

    void observe_deactivation() noexcept {
        if (state_ != LifecycleState::Disconnected) state_ = LifecycleState::DeactivationObserved;
    }

    void observe_disconnect() noexcept { state_ = LifecycleState::Disconnected; }

private:
    LifecycleState state_ = LifecycleState::Connected;
};

}  // namespace rt::xapu_effects

#endif  // PALMETTO_XAPU_EFFECTS_H
