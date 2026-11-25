#include "../palmetto_xma2_decoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "ffi_cpp_smoke.cpp:%d: %s\n", line, expression);
    std::exit(EXIT_FAILURE);
  }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

template <size_t Size>
void put_bits(std::array<uint8_t, Size>& bytes, size_t& position, uint32_t width,
              uint32_t value) {
  for (uint32_t bit = width; bit > 0; --bit, ++position) {
    if (((value >> (bit - 1)) & 1u) != 0) {
      bytes[position / 8] |= static_cast<uint8_t>(1u << (7 - (position % 8)));
    }
  }
}

std::array<uint8_t, PALMETTO_XMA2_PACKET_BYTES> make_silent_stereo_packet() {
  std::array<uint8_t, PALMETTO_XMA2_PACKET_BYTES> packet{};
  constexpr uint32_t frame_bits = 36;
  size_t position = 0;
  put_bits(packet, position, 6, 1);
  put_bits(packet, position, 15, frame_bits);
  put_bits(packet, position, 3, 1);
  put_bits(packet, position, 8, 0);
  put_bits(packet, position, 15, frame_bits);
  put_bits(packet, position, 1, 1);  // Fixed-size tile header.
  put_bits(packet, position, 1, 0);  // One full-length subframe.
  put_bits(packet, position, 1, 0);  // No optional stereo decorrelation matrix.
  put_bits(packet, position, 8, 0);  // No dynamic range adjustment.
  put_bits(packet, position, 1, 0);  // No trim metadata.
  put_bits(packet, position, 1, 0);  // No fill data.
  put_bits(packet, position, 1, 0);  // No corrupt-subframe marker.
  put_bits(packet, position, 1, 0);  // No stereo transform error.
  put_bits(packet, position, 1, 1);  // Two-channel group without decorrelation.
  put_bits(packet, position, 1, 0);
  put_bits(packet, position, 2, 0);  // No transmitted coefficients: silence.
  put_bits(packet, position, 1, 0);  // Frame alignment bit.
  put_bits(packet, position, 1, 0);  // No additional frame in this packet.
  CHECK(position == 32 + frame_bits);
  return packet;
}

int main() {
  PalmettoXma2Decoder* decoder = nullptr;
  CHECK(palmetto_xma2_decoder_create(48000, 2, &decoder) == PALMETTO_XMA2_OK);
  CHECK(decoder != nullptr);

  const auto packet = make_silent_stereo_packet();
  std::array<uint8_t, PALMETTO_XMA2_PACKET_BYTES * 3> input{};
  std::copy(packet.begin(), packet.end(), input.begin() + PALMETTO_XMA2_PACKET_BYTES);
  std::copy(packet.begin(), packet.end(), input.begin() + PALMETTO_XMA2_PACKET_BYTES * 2);
  PalmettoXma2PcmBlock* pcm = nullptr;
  CHECK(palmetto_xma2_decode_buffer(decoder, input.data(), input.size(), &pcm) ==
        PALMETTO_XMA2_OK);
  CHECK(pcm != nullptr);
  const size_t samples = palmetto_xma2_pcm_sample_count(pcm);
  CHECK(samples > 0);
  CHECK(samples % 2 == 0);
  const int16_t* data = palmetto_xma2_pcm_data(pcm);
  CHECK(data != nullptr);

  PalmettoXma2PcmBlock* invalid = reinterpret_cast<PalmettoXma2PcmBlock*>(1);
  CHECK(palmetto_xma2_decode_buffer(decoder, input.data(), input.size() - 1, &invalid) ==
        PALMETTO_XMA2_BAD_PACKET_SIZE);
  CHECK(invalid == nullptr);

  palmetto_xma2_decoder_destroy(decoder);
  CHECK(palmetto_xma2_pcm_sample_count(pcm) == samples);
  CHECK(palmetto_xma2_pcm_data(pcm) == data);
  for (size_t i = 0; i < samples; ++i) {
    CHECK(data[i] == 0);
  }
  palmetto_xma2_pcm_destroy(pcm);
  palmetto_xma2_decoder_destroy(nullptr);

  // ---- The Opus half of the boundary, with the same synthetic-packet rule. ----
  CHECK(palmetto_audio_decoder_abi_version() == PALMETTO_AUDIO_DECODER_ABI_VERSION);

  PalmettoOpusDecoder* opus = nullptr;
  CHECK(palmetto_opus_decoder_create(48000, 2, &opus) == PALMETTO_OPUS_OK);
  CHECK(opus != nullptr);

  // RFC 6716 TOC-only packet for a 20 ms CELT frame: no frame data, decodes deterministically
  // to silence. It is built here in memory; no fixture is opened.
  const std::array<uint8_t, 1> opus_packet{0xF8};
  PalmettoOpusPcmBlock* opus_pcm = nullptr;
  CHECK(palmetto_opus_decode(opus, opus_packet.data(), opus_packet.size(), 960, &opus_pcm) ==
        PALMETTO_OPUS_OK);
  CHECK(opus_pcm != nullptr);
  CHECK(palmetto_opus_pcm_frame_count(opus_pcm) == 960);
  CHECK(palmetto_opus_pcm_sample_count(opus_pcm) == 960 * 2);
  const float* opus_data = palmetto_opus_pcm_data(opus_pcm);
  CHECK(opus_data != nullptr);
  for (size_t i = 0; i < palmetto_opus_pcm_sample_count(opus_pcm); ++i) {
    CHECK(opus_data[i] == 0.0f);
  }

  PalmettoOpusPcmBlock* bad_opus = reinterpret_cast<PalmettoOpusPcmBlock*>(1);
  const std::array<uint8_t, 1> empty_packet{};
  CHECK(palmetto_opus_decode(opus, empty_packet.data(), 0, 960, &bad_opus) ==
        PALMETTO_OPUS_BAD_PACKET);
  CHECK(bad_opus == nullptr);

  palmetto_opus_decoder_destroy(opus);
  CHECK(palmetto_opus_pcm_frame_count(opus_pcm) == 960);
  palmetto_opus_pcm_destroy(opus_pcm);
  palmetto_opus_decoder_destroy(nullptr);
  return 0;
}
