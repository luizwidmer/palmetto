// Synthetic decode backend for the C++ audio-decode tests.
//
// The C++ tests exercise the wiring in port/shims/audio_decode.h: validation, frame accounting, the bounded
// sink and the named refusals. They do that with this in-process backend, which returns known frames, so the
// tests need neither the Rust toolchain nor the real decoder library. The real decoders are covered by
// port/xma_decoder's own Rust tests (and its C ABI smoke test) with synthetic packets only.
//
// This header is included by acphal_test.cpp and xapu_chad_test.cpp; every function is inline.

#ifndef PALMETTO_TEST_AUDIO_DECODE_FAKE_BACKEND_H
#define PALMETTO_TEST_AUDIO_DECODE_FAKE_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio_decode.h"

namespace audio_decode_test {

constexpr std::uint32_t kFakeXmaFrames = 480;   // 20 ms at 24 kHz? no: a known frame count for the test only
constexpr std::uint32_t kFakeOpusFrames = 960;  // 20 ms at 48 kHz, the measured Opus frame duration
constexpr std::int16_t kFakeXmaSample = 0x1234;

struct FakeXmaDecoder {
    std::uint32_t channels;
};

struct FakeXmaBlock {
    std::vector<std::int16_t> samples;
};

struct FakeOpusDecoder {
    std::uint32_t channels;
};

struct FakeOpusBlock {
    std::vector<float> samples;
};

inline std::int32_t xma_create(std::uint32_t, std::uint32_t channels, void** out_decoder) {
    *out_decoder = new FakeXmaDecoder{channels};
    return 0;
}

inline std::int32_t xma_decode(void* decoder, const std::uint8_t* input, std::size_t input_len, void** out_block) {
    const auto* stream = static_cast<const FakeXmaDecoder*>(decoder);
    auto* block = new FakeXmaBlock();
    const std::int16_t value = (input != nullptr && input_len != 0) ? kFakeXmaSample : 0;
    block->samples.assign(static_cast<std::size_t>(kFakeXmaFrames) * stream->channels, value);
    *out_block = block;
    return 0;
}

inline void xma_destroy(void* decoder) { delete static_cast<FakeXmaDecoder*>(decoder); }

inline const std::int16_t* xma_pcm_data(const void* block) {
    return static_cast<const FakeXmaBlock*>(block)->samples.data();
}

inline std::size_t xma_pcm_sample_count(const void* block) {
    return static_cast<const FakeXmaBlock*>(block)->samples.size();
}

inline void xma_pcm_destroy(void* block) { delete static_cast<FakeXmaBlock*>(block); }

inline std::int32_t opus_create(std::uint32_t sample_rate, std::uint32_t channels, void** out_decoder) {
    // Mirror the real decoder's format rule: Opus decodes at 8, 12, 16, 24 or 48 kHz only.
    if (!(sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 || sample_rate == 24000 ||
          sample_rate == 48000) ||
        !(channels == 1 || channels == 2)) {
        return 2;
    }
    *out_decoder = new FakeOpusDecoder{channels};
    return 0;
}

inline std::int32_t opus_decode(void* decoder, const std::uint8_t* packet, std::size_t packet_len,
                                std::uint32_t max_frames_per_channel, void** out_block) {
    const auto* stream = static_cast<const FakeOpusDecoder*>(decoder);
    if (packet == nullptr || packet_len == 0) return 3;
    if (max_frames_per_channel < kFakeOpusFrames) return 5;
    auto* block = new FakeOpusBlock();
    block->samples.assign(static_cast<std::size_t>(kFakeOpusFrames) * stream->channels, 0.0f);
    *out_block = block;
    return 0;
}

inline void opus_destroy(void* decoder) { delete static_cast<FakeOpusDecoder*>(decoder); }

inline const float* opus_pcm_data(const void* block) {
    return static_cast<const FakeOpusBlock*>(block)->samples.data();
}

inline std::size_t opus_pcm_sample_count(const void* block) {
    return static_cast<const FakeOpusBlock*>(block)->samples.size();
}

inline std::size_t opus_pcm_frame_count(const void* block) {
    const auto* pcm = static_cast<const FakeOpusBlock*>(block);
    return pcm->samples.size() / 2;  // the test always creates a stereo block
}

inline void opus_pcm_destroy(void* block) { delete static_cast<FakeOpusBlock*>(block); }

inline rt::audio_decode::Backend backend() {
    rt::audio_decode::Backend value;
    value.xma2_create = &xma_create;
    value.xma2_decode = &xma_decode;
    value.xma2_destroy = &xma_destroy;
    value.xma2_pcm_data = &xma_pcm_data;
    value.xma2_pcm_sample_count = &xma_pcm_sample_count;
    value.xma2_pcm_destroy = &xma_pcm_destroy;
    value.opus_create = &opus_create;
    value.opus_decode = &opus_decode;
    value.opus_destroy = &opus_destroy;
    value.opus_pcm_data = &opus_pcm_data;
    value.opus_pcm_sample_count = &opus_pcm_sample_count;
    value.opus_pcm_frame_count = &opus_pcm_frame_count;
    value.opus_pcm_destroy = &opus_pcm_destroy;
    return value;
}

}  // namespace audio_decode_test

#endif  // PALMETTO_TEST_AUDIO_DECODE_FAKE_BACKEND_H
