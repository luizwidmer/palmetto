#![deny(unsafe_op_in_unsafe_fn)]

use std::alloc::Layout;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;
use std::sync::Mutex;

use opusdec::OpusDecoder;
use xma::{Xma2Stream, XMA_FRAME_SAMPLES, XMA_PACKET_BYTES};

/// ABI version of the whole decode boundary (XMA2 and Opus). The runtime's loader
/// (`port/shims/audio_decode.h`) refuses a library whose version it does not know.
pub const PALMETTO_AUDIO_DECODER_ABI_VERSION: i32 = 1;

pub const PALMETTO_XMA2_OK: i32 = 0;
pub const PALMETTO_XMA2_INVALID_ARGUMENT: i32 = 1;
pub const PALMETTO_XMA2_INVALID_FORMAT: i32 = 2;
pub const PALMETTO_XMA2_BAD_PACKET_SIZE: i32 = 3;
pub const PALMETTO_XMA2_OUT_OF_MEMORY: i32 = 4;
pub const PALMETTO_XMA2_DECODER_FAILURE: i32 = 5;
pub const PALMETTO_XMA2_PANIC: i32 = 6;

const MAX_FRAMES_PER_PACKET: usize = 64;

#[repr(C)]
pub struct PalmettoXma2Decoder {
    stream: Mutex<Xma2Stream>,
    channels: usize,
}

#[repr(C)]
pub struct PalmettoXma2PcmBlock {
    samples: Vec<i16>,
}

#[cfg(not(test))]
use std::alloc::alloc as allocate_handle;
#[cfg(test)]
use tests::allocate_handle;

fn try_new_handle<T>(value: T) -> Result<Box<T>, i32> {
    const { assert!(std::mem::size_of::<T>() > 0) };
    // Both the allocation and Box's eventual deallocation use the global allocator
    // with T's exact layout. Initialize the storage before handing it to Box.
    let storage = unsafe { allocate_handle(Layout::new::<T>()) }.cast::<T>();
    if storage.is_null() {
        return Err(PALMETTO_XMA2_OUT_OF_MEMORY);
    }
    unsafe {
        storage.write(value);
        Ok(Box::from_raw(storage))
    }
}

/// Creates a decoder for one mono or stereo XMA2 stream.
///
/// # Safety
/// `out_decoder` must be writable for one pointer. The returned decoder must be destroyed once,
/// after all calls using it have finished.
#[no_mangle]
pub unsafe extern "C" fn palmetto_xma2_decoder_create(
    sample_rate: u32,
    channels: u32,
    out_decoder: *mut *mut PalmettoXma2Decoder,
) -> i32 {
    if out_decoder.is_null() {
        return PALMETTO_XMA2_INVALID_ARGUMENT;
    }
    unsafe { *out_decoder = ptr::null_mut() };
    if sample_rate == 0 || !(channels == 1 || channels == 2) {
        return PALMETTO_XMA2_INVALID_FORMAT;
    }

    match catch_unwind(AssertUnwindSafe(|| {
        let stream = Xma2Stream::new(sample_rate, channels as usize)
            .map_err(|_| PALMETTO_XMA2_INVALID_FORMAT)?;
        try_new_handle(PalmettoXma2Decoder {
            stream: Mutex::new(stream),
            channels: channels as usize,
        })
    })) {
        Ok(Ok(decoder)) => {
            unsafe { *out_decoder = Box::into_raw(decoder) };
            PALMETTO_XMA2_OK
        }
        Ok(Err(status)) => status,
        Err(_) => PALMETTO_XMA2_PANIC,
    }
}

/// Decodes a buffer containing one or more complete 2048-byte XMA2 packets.
///
/// # Safety
/// `decoder` must be a live handle returned by `palmetto_xma2_decoder_create`.
/// `input` must reference `input_len` readable bytes and `out_block` must be writable.
/// Do not destroy a decoder concurrently with this call. The returned PCM block remains valid
/// until `palmetto_xma2_pcm_destroy` is called.
#[no_mangle]
pub unsafe extern "C" fn palmetto_xma2_decode_buffer(
    decoder: *mut PalmettoXma2Decoder,
    input: *const u8,
    input_len: usize,
    out_block: *mut *mut PalmettoXma2PcmBlock,
) -> i32 {
    if out_block.is_null() {
        return PALMETTO_XMA2_INVALID_ARGUMENT;
    }
    unsafe { *out_block = ptr::null_mut() };

    if decoder.is_null() || input.is_null() || input_len > isize::MAX as usize {
        return PALMETTO_XMA2_INVALID_ARGUMENT;
    }
    if input_len == 0 || !input_len.is_multiple_of(XMA_PACKET_BYTES) {
        return PALMETTO_XMA2_BAD_PACKET_SIZE;
    }

    let decoded = catch_unwind(AssertUnwindSafe(|| {
        let handle = unsafe { &*decoder };
        let bytes = unsafe { slice::from_raw_parts(input, input_len) };
        let mut stream = handle
            .stream
            .lock()
            .map_err(|_| PALMETTO_XMA2_DECODER_FAILURE)?;
        let max_packet_samples = MAX_FRAMES_PER_PACKET
            .checked_mul(XMA_FRAME_SAMPLES)
            .and_then(|samples| samples.checked_mul(handle.channels))
            .ok_or(PALMETTO_XMA2_INVALID_ARGUMENT)?;
        let mut block = try_new_handle(PalmettoXma2PcmBlock {
            samples: Vec::new(),
        })?;
        let samples = &mut block.samples;

        let (packets, _) = bytes.as_chunks::<XMA_PACKET_BYTES>();
        for packet in packets {
            let requested = samples
                .len()
                .checked_add(max_packet_samples)
                .ok_or(PALMETTO_XMA2_INVALID_ARGUMENT)?;
            if requested > isize::MAX as usize / std::mem::size_of::<i16>() {
                return Err(PALMETTO_XMA2_INVALID_ARGUMENT);
            }
            samples
                .try_reserve(max_packet_samples)
                .map_err(|_| PALMETTO_XMA2_OUT_OF_MEMORY)?;
            stream.decode_packet(packet, samples);
        }

        if !samples.len().is_multiple_of(handle.channels) {
            return Err(PALMETTO_XMA2_DECODER_FAILURE);
        }
        Ok(block)
    }));

    match decoded {
        Ok(Ok(block)) => {
            unsafe { *out_block = Box::into_raw(block) };
            PALMETTO_XMA2_OK
        }
        Ok(Err(status)) => status,
        Err(_) => PALMETTO_XMA2_PANIC,
    }
}

/// Destroys a decoder. Passing null is a no-op.
///
/// # Safety
/// A non-null pointer must be a live handle returned by `palmetto_xma2_decoder_create`,
/// and no calls may be using it concurrently.
#[no_mangle]
pub unsafe extern "C" fn palmetto_xma2_decoder_destroy(decoder: *mut PalmettoXma2Decoder) {
    if !decoder.is_null() {
        unsafe { drop(Box::from_raw(decoder)) };
    }
}

/// Returns the immutable interleaved signed-16 PCM pointer for a block.
///
/// # Safety
/// A non-null pointer must be a live block returned by `palmetto_xma2_decode_buffer`.
/// The returned pointer is valid only until the block is destroyed.
#[no_mangle]
pub unsafe extern "C" fn palmetto_xma2_pcm_data(block: *const PalmettoXma2PcmBlock) -> *const i16 {
    unsafe { block.as_ref() }.map_or(ptr::null(), |pcm| pcm.samples.as_ptr())
}

/// Returns the number of signed-16 samples in a block.
///
/// # Safety
/// A non-null pointer must be a live block returned by `palmetto_xma2_decode_buffer`.
#[no_mangle]
pub unsafe extern "C" fn palmetto_xma2_pcm_sample_count(
    block: *const PalmettoXma2PcmBlock,
) -> usize {
    unsafe { block.as_ref() }.map_or(0, |pcm| pcm.samples.len())
}

/// Destroys a PCM block. Passing null is a no-op.
///
/// # Safety
/// A non-null pointer must be a live block returned by `palmetto_xma2_decode_buffer`.
#[no_mangle]
pub unsafe extern "C" fn palmetto_xma2_pcm_destroy(block: *mut PalmettoXma2PcmBlock) {
    if !block.is_null() {
        unsafe { drop(Box::from_raw(block)) };
    }
}

// ---- Opus decode boundary ----
//
// The console title's Opus streams (AWC codec 0x0B, 48 kHz) are decoded by the console's own
// MSOpusDecoder on that platform. On the PC runtime the decode boundary is this FFI, backed by the
// `opusdec` package pinned in Cargo.toml: a Rust port of the libopus float decoder (BSD-3-Clause).
// Like the XMA2 half, this is only a decode boundary; nothing here mixes, resamples or outputs.

pub const PALMETTO_OPUS_OK: i32 = 0;
pub const PALMETTO_OPUS_INVALID_ARGUMENT: i32 = 1;
pub const PALMETTO_OPUS_INVALID_FORMAT: i32 = 2;
pub const PALMETTO_OPUS_BAD_PACKET: i32 = 3;
pub const PALMETTO_OPUS_OUT_OF_MEMORY: i32 = 4;
pub const PALMETTO_OPUS_DECODER_FAILURE: i32 = 5;
pub const PALMETTO_OPUS_PANIC: i32 = 6;

/// RFC 6716 limits one Opus packet to 1275 bytes.
pub const PALMETTO_OPUS_MAX_PACKET_BYTES: usize = 1275;

/// The longest Opus frame the decoder can be asked for: 120 ms at 48 kHz.
pub const PALMETTO_OPUS_MAX_FRAME_SAMPLES: usize = 5760;

#[repr(C)]
pub struct PalmettoOpusDecoder {
    decoder: Mutex<OpusDecoder>,
    channels: usize,
}

#[repr(C)]
pub struct PalmettoOpusPcmBlock {
    samples: Vec<f32>,
    channels: usize,
}

/// Version of the decode boundary this library implements. The runtime loader compares it with the
/// version it was compiled against and refuses a mismatch instead of guessing at the layout.
#[no_mangle]
pub extern "C" fn palmetto_audio_decoder_abi_version() -> i32 {
    PALMETTO_AUDIO_DECODER_ABI_VERSION
}

/// Creates a decoder for one mono or stereo Opus stream.
///
/// # Safety
/// `out_decoder` must be writable for one pointer. The returned decoder must be destroyed once,
/// after all calls using it have finished.
#[no_mangle]
pub unsafe extern "C" fn palmetto_opus_decoder_create(
    sample_rate: u32,
    channels: u32,
    out_decoder: *mut *mut PalmettoOpusDecoder,
) -> i32 {
    if out_decoder.is_null() {
        return PALMETTO_OPUS_INVALID_ARGUMENT;
    }
    unsafe { *out_decoder = ptr::null_mut() };
    if sample_rate == 0 || !(channels == 1 || channels == 2) {
        return PALMETTO_OPUS_INVALID_FORMAT;
    }

    match catch_unwind(AssertUnwindSafe(|| {
        let decoder = OpusDecoder::new(sample_rate as i32, channels as usize)
            .map_err(|_| PALMETTO_OPUS_INVALID_FORMAT)?;
        try_new_handle(PalmettoOpusDecoder {
            decoder: Mutex::new(decoder),
            channels: channels as usize,
        })
    })) {
        Ok(Ok(decoder)) => {
            unsafe { *out_decoder = Box::into_raw(decoder) };
            PALMETTO_OPUS_OK
        }
        Ok(Err(status)) => status,
        Err(_) => PALMETTO_OPUS_PANIC,
    }
}

/// Decodes one Opus packet into interleaved 32-bit float PCM.
///
/// `max_frames_per_channel` is the room the caller has for the packet's duration in frames, at
/// most `PALMETTO_OPUS_MAX_FRAME_SAMPLES`. The block holds the decoded frames only.
///
/// # Safety
/// `decoder` must be a live handle returned by `palmetto_opus_decoder_create`.
/// `packet` must reference `packet_len` readable bytes and `out_block` must be writable.
/// Do not destroy a decoder concurrently with this call. The returned PCM block remains valid
/// until `palmetto_opus_pcm_destroy` is called.
#[no_mangle]
pub unsafe extern "C" fn palmetto_opus_decode(
    decoder: *mut PalmettoOpusDecoder,
    packet: *const u8,
    packet_len: usize,
    max_frames_per_channel: u32,
    out_block: *mut *mut PalmettoOpusPcmBlock,
) -> i32 {
    if out_block.is_null() {
        return PALMETTO_OPUS_INVALID_ARGUMENT;
    }
    unsafe { *out_block = ptr::null_mut() };

    if decoder.is_null() || packet.is_null() {
        return PALMETTO_OPUS_INVALID_ARGUMENT;
    }
    if packet_len == 0 || packet_len > PALMETTO_OPUS_MAX_PACKET_BYTES {
        return PALMETTO_OPUS_BAD_PACKET;
    }
    if max_frames_per_channel == 0
        || max_frames_per_channel as usize > PALMETTO_OPUS_MAX_FRAME_SAMPLES
    {
        return PALMETTO_OPUS_INVALID_ARGUMENT;
    }

    let decoded = catch_unwind(AssertUnwindSafe(|| {
        let handle = unsafe { &*decoder };
        let bytes = unsafe { slice::from_raw_parts(packet, packet_len) };
        let channels = handle.channels;
        let max_frames = max_frames_per_channel as usize;
        let sample_capacity = max_frames
            .checked_mul(channels)
            .ok_or(PALMETTO_OPUS_INVALID_ARGUMENT)?;

        let mut samples: Vec<f32> = Vec::new();
        samples
            .try_reserve(sample_capacity)
            .map_err(|_| PALMETTO_OPUS_OUT_OF_MEMORY)?;
        samples.resize(sample_capacity, 0.0f32);

        let mut opus = handle
            .decoder
            .lock()
            .map_err(|_| PALMETTO_OPUS_DECODER_FAILURE)?;
        let frames = opus.decode_float(bytes, &mut samples, max_frames_per_channel as i32, false);
        if frames < 0 {
            return Err(PALMETTO_OPUS_DECODER_FAILURE);
        }
        let frames = frames as usize;
        if frames == 0 || frames > max_frames || frames * channels > samples.len() {
            return Err(PALMETTO_OPUS_DECODER_FAILURE);
        }
        samples.truncate(frames * channels);

        try_new_handle(PalmettoOpusPcmBlock { samples, channels })
    }));

    match decoded {
        Ok(Ok(block)) => {
            unsafe { *out_block = Box::into_raw(block) };
            PALMETTO_OPUS_OK
        }
        Ok(Err(status)) => status,
        Err(_) => PALMETTO_OPUS_PANIC,
    }
}

/// Destroys an Opus decoder. Passing null is a no-op.
///
/// # Safety
/// A non-null pointer must be a live handle returned by `palmetto_opus_decoder_create`, and no
/// calls may be using it concurrently.
#[no_mangle]
pub unsafe extern "C" fn palmetto_opus_decoder_destroy(decoder: *mut PalmettoOpusDecoder) {
    if !decoder.is_null() {
        unsafe { drop(Box::from_raw(decoder)) };
    }
}

/// Returns the immutable interleaved 32-bit float PCM pointer for a block.
///
/// # Safety
/// A non-null pointer must be a live block returned by `palmetto_opus_decode`.
/// The returned pointer is valid only until the block is destroyed.
#[no_mangle]
pub unsafe extern "C" fn palmetto_opus_pcm_data(block: *const PalmettoOpusPcmBlock) -> *const f32 {
    unsafe { block.as_ref() }.map_or(ptr::null(), |pcm| pcm.samples.as_ptr())
}

/// Returns the number of interleaved 32-bit float samples in a block.
///
/// # Safety
/// A non-null pointer must be a live block returned by `palmetto_opus_decode`.
#[no_mangle]
pub unsafe extern "C" fn palmetto_opus_pcm_sample_count(
    block: *const PalmettoOpusPcmBlock,
) -> usize {
    unsafe { block.as_ref() }.map_or(0, |pcm| pcm.samples.len())
}

/// Returns the number of frames per channel in a block.
///
/// # Safety
/// A non-null pointer must be a live block returned by `palmetto_opus_decode`.
#[no_mangle]
pub unsafe extern "C" fn palmetto_opus_pcm_frame_count(
    block: *const PalmettoOpusPcmBlock,
) -> usize {
    unsafe { block.as_ref() }.map_or(0, |pcm| {
        if pcm.channels == 0 {
            0
        } else {
            pcm.samples.len() / pcm.channels
        }
    })
}

/// Destroys an Opus PCM block. Passing null is a no-op.
///
/// # Safety
/// A non-null pointer must be a live block returned by `palmetto_opus_decode`.
#[no_mangle]
pub unsafe extern "C" fn palmetto_opus_pcm_destroy(block: *mut PalmettoOpusPcmBlock) {
    if !block.is_null() {
        unsafe { drop(Box::from_raw(block)) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    thread_local! {
        static FAIL_NEXT_HANDLE_ALLOCATION: Cell<bool> = const { Cell::new(false) };
    }

    pub(super) unsafe fn allocate_handle(layout: Layout) -> *mut u8 {
        if FAIL_NEXT_HANDLE_ALLOCATION.replace(false) {
            ptr::null_mut()
        } else {
            unsafe { std::alloc::alloc(layout) }
        }
    }

    #[test]
    fn decoder_handle_allocation_failure_clears_output() {
        let mut decoder = ptr::dangling_mut();
        FAIL_NEXT_HANDLE_ALLOCATION.set(true);
        let status = unsafe { palmetto_xma2_decoder_create(48_000, 2, &mut decoder) };
        assert_eq!(status, PALMETTO_XMA2_OUT_OF_MEMORY);
        assert!(decoder.is_null());

        assert_eq!(
            unsafe { palmetto_xma2_decoder_create(48_000, 2, &mut decoder) },
            PALMETTO_XMA2_OK
        );
        assert!(!decoder.is_null());
        unsafe { palmetto_xma2_decoder_destroy(decoder) };
    }

    #[test]
    fn pcm_handle_allocation_failure_clears_output_and_allows_retry() {
        let mut decoder = ptr::null_mut();
        assert_eq!(
            unsafe { palmetto_xma2_decoder_create(48_000, 2, &mut decoder) },
            PALMETTO_XMA2_OK
        );
        let packet = [0u8; XMA_PACKET_BYTES];
        let mut block = ptr::dangling_mut();
        FAIL_NEXT_HANDLE_ALLOCATION.set(true);
        let status = unsafe {
            palmetto_xma2_decode_buffer(decoder, packet.as_ptr(), packet.len(), &mut block)
        };
        assert_eq!(status, PALMETTO_XMA2_OUT_OF_MEMORY);
        assert!(block.is_null());

        assert_eq!(
            unsafe {
                palmetto_xma2_decode_buffer(decoder, packet.as_ptr(), packet.len(), &mut block)
            },
            PALMETTO_XMA2_OK
        );
        assert!(!block.is_null());
        unsafe {
            palmetto_xma2_pcm_destroy(block);
            palmetto_xma2_decoder_destroy(decoder);
        }
    }
}
