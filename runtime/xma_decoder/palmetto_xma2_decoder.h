#ifndef PALMETTO_XMA2_DECODER_H
#define PALMETTO_XMA2_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif
