// The Opus half of the decode boundary, against synthetic packets only (no audio fixtures, no
// game data). A one-byte packet that is only a TOC (RFC 6716 packet with no frame data for a
// 20 ms CELT frame) decodes deterministically to 960 silence frames at 48 kHz; the tests use that
// to prove the FFI's frame accounting, ownership and refusal cases.

use palmetto_xma2_decoder::*;
use std::ptr;

const PACKET_BYTES_SILENCE: [u8; 1] = [0xF8];
const FRAMES_20MS_48K: u32 = 960;

fn make_decoder(rate: u32, channels: u32) -> *mut PalmettoOpusDecoder {
    let mut decoder = ptr::null_mut();
    let status = unsafe { palmetto_opus_decoder_create(rate, channels, &mut decoder) };
    assert_eq!(status, PALMETTO_OPUS_OK);
    assert!(!decoder.is_null());
    decoder
}

#[test]
fn abi_version_is_the_one_the_runtime_knows() {
    assert_eq!(
        palmetto_audio_decoder_abi_version(),
        PALMETTO_AUDIO_DECODER_ABI_VERSION
    );
}

#[test]
fn creation_rejects_invalid_outputs_formats_and_rates() {
    assert_eq!(
        unsafe { palmetto_opus_decoder_create(48_000, 2, ptr::null_mut()) },
        PALMETTO_OPUS_INVALID_ARGUMENT
    );

    let mut decoder = ptr::dangling_mut::<PalmettoOpusDecoder>();
    assert_eq!(
        unsafe { palmetto_opus_decoder_create(48_000, 3, &mut decoder) },
        PALMETTO_OPUS_INVALID_FORMAT
    );
    assert!(decoder.is_null());

    assert_eq!(
        unsafe { palmetto_opus_decoder_create(0, 1, &mut decoder) },
        PALMETTO_OPUS_INVALID_FORMAT
    );
    assert!(decoder.is_null());

    // 32 kHz is not an Opus decoder rate (the decoder works at 8/12/16/24/48 kHz and resamples
    // internally); the boundary refuses it by name rather than guessing.
    assert_eq!(
        unsafe { palmetto_opus_decoder_create(32_000, 1, &mut decoder) },
        PALMETTO_OPUS_INVALID_FORMAT
    );
    assert!(decoder.is_null());
}

#[test]
fn synthetic_opus_packet_decodes_to_twenty_ms_of_silence() {
    let decoder = make_decoder(48_000, 2);
    let mut block = ptr::null_mut();
    assert_eq!(
        unsafe {
            palmetto_opus_decode(
                decoder,
                PACKET_BYTES_SILENCE.as_ptr(),
                PACKET_BYTES_SILENCE.len(),
                FRAMES_20MS_48K,
                &mut block,
            )
        },
        PALMETTO_OPUS_OK
    );
    assert!(!block.is_null());
    assert_eq!(
        unsafe { palmetto_opus_pcm_frame_count(block) },
        FRAMES_20MS_48K as usize
    );
    assert_eq!(
        unsafe { palmetto_opus_pcm_sample_count(block) },
        FRAMES_20MS_48K as usize * 2
    );
    let pcm = unsafe { palmetto_opus_pcm_data(block) };
    assert!(!pcm.is_null());
    let samples = unsafe { std::slice::from_raw_parts(pcm, FRAMES_20MS_48K as usize * 2) };
    assert!(samples.iter().all(|sample| sample.is_finite()));
    assert!(samples.iter().all(|sample| *sample == 0.0));

    unsafe { palmetto_opus_pcm_destroy(block) };
    unsafe { palmetto_opus_decoder_destroy(decoder) };
}

#[test]
fn mono_and_stereo_have_the_frame_count_the_packet_carries() {
    for channels in [1u32, 2u32] {
        let decoder = make_decoder(48_000, channels);
        let mut block = ptr::null_mut();
        assert_eq!(
            unsafe {
                palmetto_opus_decode(
                    decoder,
                    PACKET_BYTES_SILENCE.as_ptr(),
                    PACKET_BYTES_SILENCE.len(),
                    FRAMES_20MS_48K,
                    &mut block,
                )
            },
            PALMETTO_OPUS_OK
        );
        assert_eq!(unsafe { palmetto_opus_pcm_frame_count(block) }, 960);
        assert_eq!(
            unsafe { palmetto_opus_pcm_sample_count(block) },
            960 * channels as usize
        );
        unsafe {
            palmetto_opus_pcm_destroy(block);
            palmetto_opus_decoder_destroy(decoder);
        }
    }
}

#[test]
fn decode_rejects_nulls_bad_lengths_and_impossible_frame_room() {
    let decoder = make_decoder(48_000, 1);
    let mut block = ptr::dangling_mut::<PalmettoOpusPcmBlock>();

    assert_eq!(
        unsafe {
            palmetto_opus_decode(
                decoder,
                PACKET_BYTES_SILENCE.as_ptr(),
                0,
                FRAMES_20MS_48K,
                &mut block,
            )
        },
        PALMETTO_OPUS_BAD_PACKET
    );
    assert!(block.is_null());

    assert_eq!(
        unsafe {
            palmetto_opus_decode(
                decoder,
                ptr::null(),
                PACKET_BYTES_SILENCE.len(),
                FRAMES_20MS_48K,
                &mut block,
            )
        },
        PALMETTO_OPUS_INVALID_ARGUMENT
    );
    assert!(block.is_null());

    assert_eq!(
        unsafe {
            palmetto_opus_decode(
                decoder,
                PACKET_BYTES_SILENCE.as_ptr(),
                PACKET_BYTES_SILENCE.len(),
                0,
                &mut block,
            )
        },
        PALMETTO_OPUS_INVALID_ARGUMENT
    );

    // The RFC's 1275-byte packet cap, one byte over it: refused by name before decoding.
    let oversized = vec![0u8; PALMETTO_OPUS_MAX_PACKET_BYTES + 1];
    assert_eq!(
        unsafe {
            palmetto_opus_decode(
                decoder,
                oversized.as_ptr(),
                oversized.len(),
                FRAMES_20MS_48K,
                &mut block,
            )
        },
        PALMETTO_OPUS_BAD_PACKET
    );

    // A packet whose duration does not fit the caller's room: the decoder reports it as a
    // decoder failure rather than overrunning the caller's buffer.
    assert_eq!(
        unsafe {
            palmetto_opus_decode(
                decoder,
                PACKET_BYTES_SILENCE.as_ptr(),
                PACKET_BYTES_SILENCE.len(),
                120,
                &mut block,
            )
        },
        PALMETTO_OPUS_DECODER_FAILURE
    );

    unsafe { palmetto_opus_decoder_destroy(decoder) };
}

#[test]
fn pcm_block_owns_its_samples_after_the_decoder_is_destroyed() {
    let decoder = make_decoder(48_000, 1);
    let mut block = ptr::null_mut();
    assert_eq!(
        unsafe {
            palmetto_opus_decode(
                decoder,
                PACKET_BYTES_SILENCE.as_ptr(),
                PACKET_BYTES_SILENCE.len(),
                FRAMES_20MS_48K,
                &mut block,
            )
        },
        PALMETTO_OPUS_OK
    );
    let samples = unsafe { palmetto_opus_pcm_sample_count(block) };
    let data = unsafe { palmetto_opus_pcm_data(block) };

    unsafe { palmetto_opus_decoder_destroy(decoder) };
    assert_eq!(unsafe { palmetto_opus_pcm_sample_count(block) }, samples);
    assert_eq!(unsafe { palmetto_opus_pcm_data(block) }, data);

    unsafe { palmetto_opus_pcm_destroy(block) };
    assert_eq!(unsafe { palmetto_opus_pcm_sample_count(ptr::null()) }, 0);
    unsafe { palmetto_opus_pcm_destroy(ptr::null_mut()) };
}

#[test]
fn deterministic_synthetic_packets_do_not_unwind_across_ffi() {
    let decoder = make_decoder(48_000, 2);
    for seed in 1..17u32 {
        let mut state = seed.wrapping_mul(2_654_435_761).wrapping_add(1);
        let mut packet = [0u8; 64];
        for byte in &mut packet {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            *byte = state as u8;
        }

        let mut block = ptr::null_mut();
        let status = unsafe {
            palmetto_opus_decode(
                decoder,
                packet.as_ptr(),
                packet.len(),
                FRAMES_20MS_48K,
                &mut block,
            )
        };
        assert!(
            status == PALMETTO_OPUS_OK
                || status == PALMETTO_OPUS_DECODER_FAILURE
                || status == PALMETTO_OPUS_PANIC,
            "unexpected status {status}"
        );
        if !block.is_null() {
            unsafe { palmetto_opus_pcm_destroy(block) };
        }
    }

    unsafe { palmetto_opus_decoder_destroy(decoder) };
}
