use palmetto_xma2_decoder::*;
use std::ptr;
use xma::bits::BitWriter;
use xma::Xma2Stream;

const PACKET_BYTES: usize = 2048;

fn synthetic_silent_packet(channels: usize) -> [u8; PACKET_BYTES] {
    assert!(channels == 1 || channels == 2);

    let mut frame = BitWriter::default();
    frame.put(1, 1); // Fixed-size tile header.
    frame.put(1, 0); // One full-length subframe.
    if channels == 2 {
        frame.put(1, 0); // No optional stereo decorrelation matrix.
    }
    frame.put(8, 0); // No dynamic range adjustment.
    frame.put(1, 0); // No trim metadata.
    frame.put(1, 0); // No fill data.
    frame.put(1, 0); // No corrupt-subframe marker.
    if channels == 2 {
        frame.put(1, 0); // No stereo transform error.
        frame.put(1, 1); // Two-channel group without decorrelation.
        frame.put(1, 0);
    }
    for _ in 0..channels {
        frame.put(1, 0); // No transmitted coefficients: silence.
    }
    frame.put(1, 0); // Align to the next frame boundary.
    frame.put(1, 0); // This packet contains one frame.

    let frame_bits = 15 + frame.bits;
    let mut packet_writer = BitWriter::default();
    packet_writer.put(6, 1);
    packet_writer.put(15, frame_bits as u32);
    packet_writer.put(3, 1);
    packet_writer.put(8, 0);
    packet_writer.put(15, frame_bits as u32);
    packet_writer.copy_from(&frame.buf, 0, frame.bits);

    let mut packet = [0u8; PACKET_BYTES];
    packet[..packet_writer.buf.len()].copy_from_slice(&packet_writer.buf);
    packet
}

fn make_decoder(rate: u32, channels: u32) -> *mut PalmettoXma2Decoder {
    let mut decoder = ptr::null_mut();
    let result = unsafe { palmetto_xma2_decoder_create(rate, channels, &mut decoder) };
    assert_eq!(result, PALMETTO_XMA2_OK);
    assert!(!decoder.is_null());
    decoder
}

#[test]
fn creation_rejects_invalid_outputs_and_formats() {
    assert_eq!(
        unsafe { palmetto_xma2_decoder_create(48_000, 2, ptr::null_mut()) },
        PALMETTO_XMA2_INVALID_ARGUMENT
    );

    let mut decoder = ptr::dangling_mut::<PalmettoXma2Decoder>();
    assert_eq!(
        unsafe { palmetto_xma2_decoder_create(48_000, 3, &mut decoder) },
        PALMETTO_XMA2_INVALID_FORMAT
    );
    assert!(decoder.is_null());

    assert_eq!(
        unsafe { palmetto_xma2_decoder_create(0, 1, &mut decoder) },
        PALMETTO_XMA2_INVALID_FORMAT
    );
    assert!(decoder.is_null());
}

#[test]
fn synthetic_silent_packet_returns_owned_pcm_block_after_decoder_destruction() {
    let decoder = make_decoder(48_000, 2);
    let packet = synthetic_silent_packet(2);
    let mut input = [0u8; PACKET_BYTES * 3];
    input[PACKET_BYTES..PACKET_BYTES * 2].copy_from_slice(&packet);
    input[PACKET_BYTES * 2..].copy_from_slice(&packet);
    let mut block = ptr::null_mut();

    assert_eq!(
        unsafe { palmetto_xma2_decode_buffer(decoder, input.as_ptr(), input.len(), &mut block,) },
        PALMETTO_XMA2_OK
    );
    assert!(!block.is_null());

    let samples = unsafe { palmetto_xma2_pcm_sample_count(block) };
    assert_eq!(samples % 2, 0);
    assert!(samples > 0);
    let pcm = unsafe { palmetto_xma2_pcm_data(block) };
    assert!(!pcm.is_null());
    let decoded = unsafe { std::slice::from_raw_parts(pcm, samples) };
    assert!(decoded.iter().all(|sample| *sample == 0));

    unsafe { palmetto_xma2_decoder_destroy(decoder) };
    assert_eq!(unsafe { palmetto_xma2_pcm_sample_count(block) }, samples);
    assert_eq!(
        unsafe { std::slice::from_raw_parts(palmetto_xma2_pcm_data(block), samples) },
        decoded
    );
    unsafe { palmetto_xma2_pcm_destroy(block) };
}

#[test]
fn synthetic_silent_mono_stream_decodes_to_silent_pcm() {
    let decoder = make_decoder(44_100, 1);
    let packet = synthetic_silent_packet(1);
    let mut input = [0u8; PACKET_BYTES * 3];
    input[PACKET_BYTES..PACKET_BYTES * 2].copy_from_slice(&packet);
    input[PACKET_BYTES * 2..].copy_from_slice(&packet);
    let mut block = ptr::null_mut();

    assert_eq!(
        unsafe { palmetto_xma2_decode_buffer(decoder, input.as_ptr(), input.len(), &mut block) },
        PALMETTO_XMA2_OK
    );
    assert!(!block.is_null());
    let samples = unsafe { palmetto_xma2_pcm_sample_count(block) };
    assert!(samples > 0);
    let pcm = unsafe { palmetto_xma2_pcm_data(block) };
    assert!(!pcm.is_null());
    assert!(unsafe { std::slice::from_raw_parts(pcm, samples) }
        .iter()
        .all(|sample| *sample == 0));

    unsafe {
        palmetto_xma2_pcm_destroy(block);
        palmetto_xma2_decoder_destroy(decoder);
    }
}

#[test]
fn decode_rejects_nulls_and_non_packet_aligned_buffers() {
    let decoder = make_decoder(44_100, 1);
    let packet = [0u8; PACKET_BYTES];
    let mut block = ptr::dangling_mut::<PalmettoXma2PcmBlock>();

    assert_eq!(
        unsafe { palmetto_xma2_decode_buffer(decoder, packet.as_ptr(), 0, &mut block) },
        PALMETTO_XMA2_BAD_PACKET_SIZE
    );
    assert!(block.is_null());

    assert_eq!(
        unsafe {
            palmetto_xma2_decode_buffer(decoder, packet.as_ptr(), PACKET_BYTES - 1, &mut block)
        },
        PALMETTO_XMA2_BAD_PACKET_SIZE
    );
    assert!(block.is_null());

    assert_eq!(
        unsafe { palmetto_xma2_decode_buffer(decoder, ptr::null(), PACKET_BYTES, &mut block) },
        PALMETTO_XMA2_INVALID_ARGUMENT
    );
    assert!(block.is_null());

    unsafe { palmetto_xma2_decoder_destroy(decoder) };
}

#[test]
fn deterministic_synthetic_packets_do_not_unwind_across_ffi() {
    let decoder = make_decoder(48_000, 2);
    for seed in 1..17u32 {
        let mut state = seed.wrapping_mul(2_654_435_761).wrapping_add(1);
        let mut packet = [0u8; PACKET_BYTES];
        for byte in &mut packet {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            *byte = state as u8;
        }

        let mut block = ptr::null_mut();
        let result = unsafe {
            palmetto_xma2_decode_buffer(decoder, packet.as_ptr(), packet.len(), &mut block)
        };
        assert!(
            result == PALMETTO_XMA2_OK
                || result == PALMETTO_XMA2_DECODER_FAILURE
                || result == PALMETTO_XMA2_PANIC,
            "unexpected status {result}"
        );
        if !block.is_null() {
            unsafe { palmetto_xma2_pcm_destroy(block) };
        }
    }

    unsafe { palmetto_xma2_decoder_destroy(decoder) };
}

#[test]
fn multiple_packet_buffer_matches_serial_decoder_calls() {
    let decoder = make_decoder(48_000, 2);
    let packet = synthetic_silent_packet(2);
    let mut input = [0u8; PACKET_BYTES * 3];
    input[PACKET_BYTES..PACKET_BYTES * 2].copy_from_slice(&packet);
    input[PACKET_BYTES * 2..].copy_from_slice(&packet);

    let mut reference = Xma2Stream::new(48_000, 2).unwrap();
    let mut expected = Vec::new();
    for packet in input.as_chunks::<PACKET_BYTES>().0 {
        reference.decode_packet(packet, &mut expected);
    }

    let mut block = ptr::null_mut();
    assert_eq!(
        unsafe { palmetto_xma2_decode_buffer(decoder, input.as_ptr(), input.len(), &mut block) },
        PALMETTO_XMA2_OK
    );
    assert!(!block.is_null());
    let samples = unsafe { palmetto_xma2_pcm_sample_count(block) };
    assert!(samples > 0);
    assert_eq!(samples, expected.len());
    assert_eq!(samples % 2, 0);
    let pcm = unsafe { palmetto_xma2_pcm_data(block) };
    let actual = unsafe { std::slice::from_raw_parts(pcm, samples) };
    assert_eq!(actual, expected.as_slice());

    unsafe {
        palmetto_xma2_pcm_destroy(block);
        palmetto_xma2_decoder_destroy(decoder);
    }
}

#[test]
fn measured_command_region_and_partial_packets_are_refused_without_a_block() {
    let decoder = make_decoder(48_000, 2);
    // These bytes stand in for the measured 0x280-byte command descriptor region. They are
    // generated here; the measured region is only a shape, not captured title contents.
    let bytes = [0u8; PACKET_BYTES + 1];
    let mut block = ptr::dangling_mut::<PalmettoXma2PcmBlock>();

    for len in [0, 0x280, PACKET_BYTES - 1, PACKET_BYTES + 1] {
        let result =
            unsafe { palmetto_xma2_decode_buffer(decoder, bytes.as_ptr(), len, &mut block) };
        assert_eq!(result, PALMETTO_XMA2_BAD_PACKET_SIZE, "length {len}");
        assert!(
            block.is_null(),
            "failed decode must clear output for length {len}"
        );
    }

    unsafe { palmetto_xma2_decoder_destroy(decoder) };
}

#[test]
fn synthetic_silent_frames_have_exact_interleaved_size_and_no_eos_drain() {
    const CHANNELS: usize = 2;
    const EXPECTED_FRAMES: usize = 448;
    const MAX_FRAMES_PER_PACKET: usize = 64;
    const SAMPLES_PER_FRAME: usize = 512;

    let decoder = make_decoder(48_000, CHANNELS as u32);
    let packet = synthetic_silent_packet(CHANNELS);
    // The first packet has no coded frame; the next two each carry one test-generated silent
    // frame. The pinned decoder skips its startup frame and 64 more samples before returning PCM.
    let mut input = [0u8; PACKET_BYTES * 3];
    input[PACKET_BYTES..PACKET_BYTES * 2].copy_from_slice(&packet);
    input[PACKET_BYTES * 2..].copy_from_slice(&packet);
    let mut block = ptr::null_mut();

    assert_eq!(
        unsafe { palmetto_xma2_decode_buffer(decoder, input.as_ptr(), input.len(), &mut block) },
        PALMETTO_XMA2_OK
    );
    assert!(!block.is_null());
    let sample_count = unsafe { palmetto_xma2_pcm_sample_count(block) };
    let packet_count = input.len() / PACKET_BYTES;
    assert!(
        sample_count <= packet_count * MAX_FRAMES_PER_PACKET * SAMPLES_PER_FRAME * CHANNELS,
        "returned samples must fit the wrapper's per-packet reserve bound for this vector"
    );
    assert_eq!(sample_count, EXPECTED_FRAMES * CHANNELS);
    let pcm = unsafe { palmetto_xma2_pcm_data(block) };
    assert!(!pcm.is_null());
    assert!(unsafe { std::slice::from_raw_parts(pcm, sample_count) }
        .iter()
        .all(|sample| *sample == 0));

    // The FFI has no flush/end-of-stream export. Empty input is refused as a bad packet-size
    // request, and never returns an extra block that a caller could mistake for drained samples.
    let mut drained = ptr::dangling_mut::<PalmettoXma2PcmBlock>();
    assert_eq!(
        unsafe { palmetto_xma2_decode_buffer(decoder, input.as_ptr(), 0, &mut drained) },
        PALMETTO_XMA2_BAD_PACKET_SIZE
    );
    assert!(drained.is_null());

    unsafe {
        palmetto_xma2_pcm_destroy(block);
        palmetto_xma2_decoder_destroy(decoder);
    }
}

#[test]
fn zero_length_xma2_packet_header_is_not_a_codec_error_from_the_public_api() {
    let decoder = make_decoder(48_000, 2);
    // A complete synthetic 2048-byte packet whose 15-bit payload length is zero. This tests the
    // pinned API's error-reporting limit; it is not used as a claim about title packet validity.
    let packet = [0u8; PACKET_BYTES];
    let mut block = ptr::null_mut();

    assert_eq!(
        unsafe { palmetto_xma2_decode_buffer(decoder, packet.as_ptr(), packet.len(), &mut block) },
        PALMETTO_XMA2_OK
    );
    assert!(!block.is_null());
    assert_eq!(unsafe { palmetto_xma2_pcm_sample_count(block) }, 0);

    unsafe {
        palmetto_xma2_pcm_destroy(block);
        palmetto_xma2_decoder_destroy(decoder);
    }
}
