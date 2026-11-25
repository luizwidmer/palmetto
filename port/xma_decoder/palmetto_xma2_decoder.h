#ifndef PALMETTO_XMA2_DECODER_H
#define PALMETTO_XMA2_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decode boundary shared by the XMA2 and Opus halves. The runtime loads this library as a plugin
// (port/shims/audio_decode.h); it compares the ABI version below and refuses an unknown one.
#define PALMETTO_AUDIO_DECODER_ABI_VERSION 1

#define PALMETTO_XMA2_PACKET_BYTES 2048u

typedef struct PalmettoXma2Decoder PalmettoXma2Decoder;
typedef struct PalmettoXma2PcmBlock PalmettoXma2PcmBlock;

enum PalmettoXma2Status {
  PALMETTO_XMA2_OK = 0,
  PALMETTO_XMA2_INVALID_ARGUMENT = 1,
  PALMETTO_XMA2_INVALID_FORMAT = 2,
  PALMETTO_XMA2_BAD_PACKET_SIZE = 3,
  PALMETTO_XMA2_OUT_OF_MEMORY = 4,
  PALMETTO_XMA2_DECODER_FAILURE = 5,
  PALMETTO_XMA2_PANIC = 6,
};

int32_t palmetto_xma2_decoder_create(uint32_t sample_rate, uint32_t channels,
                                    PalmettoXma2Decoder **out_decoder);
int32_t palmetto_xma2_decode_buffer(PalmettoXma2Decoder *decoder,
                                   const uint8_t *input, size_t input_len,
                                   PalmettoXma2PcmBlock **out_block);
void palmetto_xma2_decoder_destroy(PalmettoXma2Decoder *decoder);
const int16_t *palmetto_xma2_pcm_data(const PalmettoXma2PcmBlock *block);
size_t palmetto_xma2_pcm_sample_count(const PalmettoXma2PcmBlock *block);
void palmetto_xma2_pcm_destroy(PalmettoXma2PcmBlock *block);

// ---- Opus decode boundary ----

// RFC 6716: one Opus packet is at most 1275 bytes; the longest frame is 120 ms (5760 frames at
// 48 kHz). The runtime passes the room it has for the packet's duration.
#define PALMETTO_OPUS_MAX_PACKET_BYTES 1275u
#define PALMETTO_OPUS_MAX_FRAME_SAMPLES 5760u

typedef struct PalmettoOpusDecoder PalmettoOpusDecoder;
typedef struct PalmettoOpusPcmBlock PalmettoOpusPcmBlock;

enum PalmettoOpusStatus {
  PALMETTO_OPUS_OK = 0,
  PALMETTO_OPUS_INVALID_ARGUMENT = 1,
  PALMETTO_OPUS_INVALID_FORMAT = 2,
  PALMETTO_OPUS_BAD_PACKET = 3,
  PALMETTO_OPUS_OUT_OF_MEMORY = 4,
  PALMETTO_OPUS_DECODER_FAILURE = 5,
  PALMETTO_OPUS_PANIC = 6,
};

int32_t palmetto_audio_decoder_abi_version(void);
int32_t palmetto_opus_decoder_create(uint32_t sample_rate, uint32_t channels,
                                     PalmettoOpusDecoder **out_decoder);
int32_t palmetto_opus_decode(PalmettoOpusDecoder *decoder, const uint8_t *packet,
                             size_t packet_len, uint32_t max_frames_per_channel,
                             PalmettoOpusPcmBlock **out_block);
void palmetto_opus_decoder_destroy(PalmettoOpusDecoder *decoder);
const float *palmetto_opus_pcm_data(const PalmettoOpusPcmBlock *block);
size_t palmetto_opus_pcm_sample_count(const PalmettoOpusPcmBlock *block);
size_t palmetto_opus_pcm_frame_count(const PalmettoOpusPcmBlock *block);
void palmetto_opus_pcm_destroy(PalmettoOpusPcmBlock *block);

#ifdef __cplusplus
}
#endif

#endif
