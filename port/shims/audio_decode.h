// The PC decode boundary for the console audio codecs (XMA2 and Opus).
//
// WHERE THIS PLUGS IN (measured, docs/audio_decode.md). The retail title's audio bring-up reaches our
// acphal/xapu/chad shims at about +22.25 s: it creates the APU heap, allocates SHAPE contexts with
// 350 XMA contexts and 0 of the other four types, creates the ACP HAL, submits one ACP command, opens
// the XApu client and the DSP connection, and then polls the XApu result count for the rest of the
// boot. It never enqueues an XApu command, never activates the DSP, and never writes the 350 XMA
// context slots (the scan in xapu_chad.cpp measures the last part in the live boot). So no compressed
// audio is ever submitted to us at this stage: there is nothing to decode yet, and the honest default
// stays the silent stand-in. This file is the boundary the measured paths will use when they arrive:
// the XApu command path submits a stream region, and the decoder turns XMA2 packets or Opus packets
// into PCM.
//
// SWITCH. Everything here is behind PALMETTO_AUDIO_DECODE=1; with the switch off (the default) the
// functions are inert and every decode request is refused by name with the switch as the reason. The
// switch is read from the environment at each call, so a boot and a test can turn it on and off.
//
// DECODER LIBRARY. The decoders themselves are the crate in port/xma_decoder, built as a cdylib
// (`palmetto_xma2_decoder.dll`). This boundary loads it at run time from PALMETTO_XMA2_DECODER_DLL, or
// from next to the runtime image when that variable is not set. Loading it at run time keeps a build
// without the Rust toolchain working: decode is then a named refusal, not a link error. The loader
// checks the boundary's ABI version and refuses an unknown one.
//
// SINK. Decoded PCM is counted, not played. The title's own output path is its WASAPI spatial stream,
// which the host already serves (docs/silent_audio.md); we have no measured site that hands ITS mix
// buffers to us, so writing into them would be invention. The bounded sink here counts frames, bytes
// and decodes per codec and keeps the numbers in the startup trace, which is what the boot can show.
// Wiring the sink into the title's spatial stream is a content-pass step (docs/audio_decode.md limits).
//
// TEST INJECTION. install_backend_for_test() lets the C++ tests drive this boundary with a synthetic
// decoder that returns known frames, so the wiring (validation, frame counts, sink, refusals) is
// tested without the Rust crate. The crate's own tests cover the real decoders with synthetic packets.

#ifndef PALMETTO_AUDIO_DECODE_H
#define PALMETTO_AUDIO_DECODE_H

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "startup_trace.h"

namespace rt::audio_decode {

constexpr uint32_t kXma2PacketBytes = 2048;
constexpr uint32_t kOpusMaxPacketBytes = 1275;
constexpr uint32_t kOpusMaxFrameSamples = 5760;
constexpr int kAbiVersion = 1;
constexpr const char* kDecoderDllName = "palmetto_xma2_decoder.dll";

// The function pointer types are the C ABI in port/xma_decoder/palmetto_xma2_decoder.h.
using Xma2CreateFn = int32_t (*)(uint32_t sample_rate, uint32_t channels, void** out_decoder);
using Xma2DecodeFn = int32_t (*)(void* decoder, const uint8_t* input, size_t input_len, void** out_block);
using Xma2DestroyFn = void (*)(void* decoder);
using Xma2DataFn = const int16_t* (*)(const void* block);
using Xma2SampleCountFn = size_t (*)(const void* block);
using Xma2BlockDestroyFn = void (*)(void* block);
using AbiVersionFn = int32_t (*)();

using OpusCreateFn = int32_t (*)(uint32_t sample_rate, uint32_t channels, void** out_decoder);
using OpusDecodeFn = int32_t (*)(void* decoder, const uint8_t* packet, size_t packet_len,
                                 uint32_t max_frames_per_channel, void** out_block);
using OpusDestroyFn = void (*)(void* decoder);
using OpusDataFn = const float* (*)(const void* block);
using OpusSampleCountFn = size_t (*)(const void* block);
using OpusFrameCountFn = size_t (*)(const void* block);
using OpusBlockDestroyFn = void (*)(void* block);

struct Backend {
    Xma2CreateFn xma2_create = nullptr;
    Xma2DecodeFn xma2_decode = nullptr;
    Xma2DestroyFn xma2_destroy = nullptr;
    Xma2DataFn xma2_pcm_data = nullptr;
    Xma2SampleCountFn xma2_pcm_sample_count = nullptr;
    Xma2BlockDestroyFn xma2_pcm_destroy = nullptr;
    OpusCreateFn opus_create = nullptr;
    OpusDecodeFn opus_decode = nullptr;
    OpusDestroyFn opus_destroy = nullptr;
    OpusDataFn opus_pcm_data = nullptr;
    OpusSampleCountFn opus_pcm_sample_count = nullptr;
    OpusFrameCountFn opus_pcm_frame_count = nullptr;
    OpusBlockDestroyFn opus_pcm_destroy = nullptr;

    bool complete() const {
        return xma2_create && xma2_decode && xma2_destroy && xma2_pcm_data && xma2_pcm_sample_count &&
               xma2_pcm_destroy && opus_create && opus_decode && opus_destroy && opus_pcm_data &&
               opus_pcm_sample_count && opus_pcm_frame_count && opus_pcm_destroy;
    }
};

// Counters behind every decode and refusal. The sink consumes the decoded frame counts; nothing here
// holds PCM beyond the call that decoded it.
struct Stats {
    std::uint64_t attempts = 0;
    std::uint64_t refusals = 0;
    std::uint64_t xma2_decodes = 0;
    std::uint64_t xma2_frames = 0;
    std::uint64_t xma2_bytes = 0;
    std::uint64_t opus_decodes = 0;
    std::uint64_t opus_frames = 0;
    std::uint64_t opus_samples = 0;
    std::uint32_t xma_contexts = 0;
    std::uint32_t xma_context_scans = 0;
    std::uint32_t xma_context_nonzero_dwords = 0;
    // The code of the most recent refusal (a stable name), so a test can match it without parsing stderr.
    const char* last_refusal = nullptr;
};

struct State {
    Backend backend{};
    HMODULE module = nullptr;
    bool load_attempted = false;
    bool loaded = false;
    std::string library;
    void* xma_contexts = nullptr;
    std::uint32_t xma_context_count = 0;
    std::atomic<bool> note_backend_missing{false};
    std::atomic<bool> note_backend_loaded{false};
    std::atomic<bool> note_contexts{false};
};

inline Stats& stats() {
    static Stats value;
    return value;
}

inline State& state() {
    static State value;
    return value;
}

inline std::mutex& mutex() {
    static std::mutex value;
    return value;
}

// ---- The opt-in switch ----

// PALMETTO_AUDIO_DECODE=1 turns decode on. Read per call, so a boot or a test can change its mind and
// the default (unset or 0) is inert.
inline bool switch_on() {
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA("PALMETTO_AUDIO_DECODE", value, sizeof value);
    return length > 0 && length < sizeof value && value[0] != '\0' && value[0] != '0';
}

// ---- One-line diagnostics, once per first event ----

inline void note(std::atomic<bool>& flag, const char* text) {
    if (!flag.exchange(true, std::memory_order_relaxed)) std::fprintf(stderr, "[rt] audio decode: %s\n", text);
}

// The trace events this file emits are `audio_decode` events, so a boot with --trace can separate them
// from the per-call `audio` events of the shims.
inline void trace_event(const std::string& fields) {
    if (!rt::startup_trace::enabled()) return;
    rt::startup_trace::event("audio_decode", fields);
}

inline std::string field_string(const char* name, const std::string& value) {
    return std::string("\"") + name + "\":" + rt::startup_trace::quote(value);
}

inline std::string field_u64(const char* name, std::uint64_t value) {
    return std::string("\"") + name + "\":" + std::to_string(value);
}

// ---- Loading the decoder library ----

inline std::string decoder_library_path() {
    char from_env[512] = {};
    const DWORD length = GetEnvironmentVariableA("PALMETTO_XMA2_DECODER_DLL", from_env, sizeof from_env);
    if (length > 0 && length < sizeof from_env) return std::string(from_env);

    char image[MAX_PATH] = {};
    const DWORD image_length = GetModuleFileNameA(nullptr, image, MAX_PATH);
    std::string path = image_length > 0 ? std::string(image, image_length) : std::string("palmetto.exe");
    const std::size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) path.resize(slash + 1);
    path += kDecoderDllName;
    return path;
}

template <typename T>
inline T resolve_proc(HMODULE module, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// Loads the decoder library and resolves the whole boundary. Must be called with mutex() held.
inline bool load_backend_locked() {
    State& s = state();
    if (s.load_attempted) return s.loaded;
    s.load_attempted = true;
    s.library = decoder_library_path();

    const HMODULE module = LoadLibraryA(s.library.c_str());
    if (!module) {
        note(s.note_backend_missing,
             "decode switch is on but palmetto_xma2_decoder.dll is not present; decode is a named refusal");
        char detail[64] = {};
        std::snprintf(detail, sizeof detail, "LoadLibrary failed (%lu)", GetLastError());
        trace_event(field_string("event", "backend") + "," + field_string("state", "missing") + "," +
                    field_string("library", kDecoderDllName) + "," + field_string("detail", detail));
        return false;
    }

    Backend backend;
    backend.xma2_create = resolve_proc<Xma2CreateFn>(module, "palmetto_xma2_decoder_create");
    backend.xma2_decode = resolve_proc<Xma2DecodeFn>(module, "palmetto_xma2_decode_buffer");
    backend.xma2_destroy = resolve_proc<Xma2DestroyFn>(module, "palmetto_xma2_decoder_destroy");
    backend.xma2_pcm_data = resolve_proc<Xma2DataFn>(module, "palmetto_xma2_pcm_data");
    backend.xma2_pcm_sample_count = resolve_proc<Xma2SampleCountFn>(module, "palmetto_xma2_pcm_sample_count");
    backend.xma2_pcm_destroy = resolve_proc<Xma2BlockDestroyFn>(module, "palmetto_xma2_pcm_destroy");
    backend.opus_create = resolve_proc<OpusCreateFn>(module, "palmetto_opus_decoder_create");
    backend.opus_decode = resolve_proc<OpusDecodeFn>(module, "palmetto_opus_decode");
    backend.opus_destroy = resolve_proc<OpusDestroyFn>(module, "palmetto_opus_decoder_destroy");
    backend.opus_pcm_data = resolve_proc<OpusDataFn>(module, "palmetto_opus_pcm_data");
    backend.opus_pcm_sample_count = resolve_proc<OpusSampleCountFn>(module, "palmetto_opus_pcm_sample_count");
    backend.opus_pcm_frame_count = resolve_proc<OpusFrameCountFn>(module, "palmetto_opus_pcm_frame_count");
    backend.opus_pcm_destroy = resolve_proc<OpusBlockDestroyFn>(module, "palmetto_opus_pcm_destroy");
    const auto abi_version = resolve_proc<AbiVersionFn>(module, "palmetto_audio_decoder_abi_version");

    const int32_t version = abi_version ? abi_version() : 0;
    if (!backend.complete() || version != kAbiVersion) {
        FreeLibrary(module);
        char detail[96] = {};
        std::snprintf(detail, sizeof detail, "%s, ABI %ld (expected %d)",
                      backend.complete() ? "complete" : "an export is missing", static_cast<long>(version),
                      kAbiVersion);
        trace_event(field_string("event", "backend") + "," + field_string("state", "refused") + "," +
                    field_string("library", kDecoderDllName) + "," + field_string("detail", detail));
        std::fprintf(stderr, "[rt] audio decode: decoder library refused: %s\n", detail);
        return false;
    }

    s.backend = backend;
    s.module = module;
    s.loaded = true;
    note(s.note_backend_loaded, "decoder library loaded: XMA2 and Opus decode boundary, ABI 1");
    trace_event(field_string("event", "backend") + "," + field_string("state", "loaded") + "," +
                field_string("library", kDecoderDllName) + "," + field_u64("abi", static_cast<std::uint64_t>(version)));
    return true;
}

inline bool ensure_backend() {
    const std::lock_guard lock(mutex());
    return load_backend_locked();
}

// ---- Test injection ----

// Replaces the loaded backend with a synthetic one. Product code never calls this; the C++ tests use
// it to prove the wiring without the Rust crate.
inline void install_backend_for_test(const Backend& backend) {
    const std::lock_guard lock(mutex());
    State& s = state();
    s.backend = backend;
    s.load_attempted = true;
    s.loaded = true;
    s.library = "test-injected";
}

inline void reset_for_test() {
    const std::lock_guard lock(mutex());
    State& s = state();
    s.backend = Backend{};
    s.module = nullptr;
    s.load_attempted = false;
    s.loaded = false;
    s.library.clear();
    s.xma_contexts = nullptr;
    s.xma_context_count = 0;
    s.note_backend_missing.store(false);
    s.note_backend_loaded.store(false);
    s.note_contexts.store(false);
    stats() = Stats{};
}

// ---- Decode results ----

struct Decode {
    bool ok = false;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    const char* refusal = nullptr;  // stable code, so a test or a trace can match it by name
    std::string detail;
};

inline Decode refuse(const char* site, const char* code, const std::string& detail) {
    ++stats().attempts;
    ++stats().refusals;
    trace_event(field_string("event", "refused") + "," + field_string("site", site) + "," +
                field_string("code", code) + "," + field_string("detail", detail));
    // Bounded stderr: the first eight refusals are named, then the trace carries the rest.
    static std::atomic<int> printed{0};
    if (printed.fetch_add(1, std::memory_order_relaxed) < 8) {
        std::fprintf(stderr, "[rt] audio decode: refusing decode at %s: %s (%s)\n", site, code, detail.c_str());
    }
    Decode result;
    result.refusal = code;
    result.detail = detail;
    stats().last_refusal = code;
    return result;
}

// The bounded sink: it counts the PCM a decode produced and keeps nothing. No mixing, no output.
inline void sink_consume(std::uint32_t frames, std::uint32_t channels, std::uint32_t bytes_per_sample,
                         bool opus) {
    std::uint64_t& frames_counter = opus ? stats().opus_frames : stats().xma2_frames;
    std::uint64_t& bytes_counter = opus ? stats().opus_samples : stats().xma2_bytes;
    frames_counter += frames;
    bytes_counter += static_cast<std::uint64_t>(frames) * channels * bytes_per_sample;
}

// ---- XMA2 ----

// Decodes a buffer of complete 2048-byte XMA2 packets into interleaved 16-bit PCM and feeds the sink.
// `site` names the measured caller so the boot can say which path asked.
inline Decode decode_xma2(const char* site, const void* packets, size_t bytes, uint32_t sample_rate,
                          uint32_t channels) {
    if (!switch_on()) return refuse(site, "switch_off", "PALMETTO_AUDIO_DECODE is not 1");
    if (!packets || bytes == 0) return refuse(site, "null_buffer", "no input buffer");
    if (bytes % kXma2PacketBytes != 0) {
        return refuse(site, "packet_size",
                      std::to_string(bytes) + " bytes is not a whole number of 2048-byte XMA2 packets");
    }
    if (sample_rate == 0 || (channels != 1 && channels != 2)) {
        return refuse(site, "format", "an XMA2 stream is mono or stereo with a non-zero sample rate");
    }

    const std::lock_guard lock(mutex());
    if (!load_backend_locked()) return refuse(site, "backend_missing", "palmetto_xma2_decoder.dll is not loaded");

    void* decoder = nullptr;
    int32_t status = state().backend.xma2_create(sample_rate, channels, &decoder);
    if (status != 0 || !decoder) {
        return refuse(site, "decoder_create", "the XMA2 decoder refused the stream format");
    }
    void* block = nullptr;
    status = state().backend.xma2_decode(decoder, static_cast<const uint8_t*>(packets), bytes, &block);
    std::uint64_t frames = 0;
    if (status == 0 && block) {
        const size_t samples = state().backend.xma2_pcm_sample_count(block);
        if (samples > 0 && samples % channels == 0) {
            frames = samples / channels;
            ++stats().xma2_decodes;
            sink_consume(static_cast<std::uint32_t>(frames), channels, 2, false);
        }
        state().backend.xma2_pcm_destroy(block);
    }
    state().backend.xma2_destroy(decoder);
    if (frames == 0) return refuse(site, "decoder_failure", "the XMA2 decoder produced no frames");
    ++stats().attempts;

    trace_event(field_string("event", "decoded") + "," + field_string("site", site) + "," +
                field_string("codec", "xma2") + "," + field_u64("frames", frames) + "," +
                field_u64("bytes", static_cast<std::uint64_t>(frames) * channels * 2));
    Decode result;
    result.ok = true;
    result.frames = frames;
    result.bytes = frames * channels * 2;
    return result;
}

// ---- Opus ----

// Decodes one Opus packet into interleaved 32-bit float PCM and feeds the sink. The runtime's Opus
// streams are the title's 48 kHz AWC banks; the boundary accepts whatever rate the decoder accepts
// (8/12/16/24/48 kHz) and refuses any other by name.
inline Decode decode_opus(const char* site, const void* packet, size_t packet_len, uint32_t sample_rate,
                          uint32_t channels, uint32_t max_frames_per_channel) {
    if (!switch_on()) return refuse(site, "switch_off", "PALMETTO_AUDIO_DECODE is not 1");
    if (!packet || packet_len == 0) return refuse(site, "null_buffer", "no Opus packet");
    if (packet_len > kOpusMaxPacketBytes) {
        return refuse(site, "opus_packet_size",
                      std::to_string(packet_len) + " bytes exceeds the 1275-byte Opus packet limit");
    }
    if (channels < 1 || channels > 2 || sample_rate == 0) {
        return refuse(site, "format", "an Opus stream is mono or stereo with a non-zero sample rate");
    }
    if (max_frames_per_channel == 0 || max_frames_per_channel > kOpusMaxFrameSamples) {
        return refuse(site, "opus_frame_room", "the sink room must be 1..=5760 frames per channel");
    }

    const std::lock_guard lock(mutex());
    if (!load_backend_locked()) return refuse(site, "backend_missing", "palmetto_xma2_decoder.dll is not loaded");

    void* decoder = nullptr;
    int32_t status = state().backend.opus_create(sample_rate, channels, &decoder);
    if (status != 0 || !decoder) {
        return refuse(site, "opus_format",
                      "the decoder does not accept " + std::to_string(sample_rate) + " Hz: Opus decodes at "
                      "8, 12, 16, 24 or 48 kHz");
    }
    void* block = nullptr;
    status = state().backend.opus_decode(decoder, static_cast<const uint8_t*>(packet), packet_len,
                                        max_frames_per_channel, &block);
    std::uint64_t frames = 0;
    if (status == 0 && block) {
        frames = state().backend.opus_pcm_frame_count(block);
        if (frames > 0) {
            ++stats().opus_decodes;
            sink_consume(static_cast<std::uint32_t>(frames), channels, 4, true);
        }
        state().backend.opus_pcm_destroy(block);
    }
    state().backend.opus_destroy(decoder);
    if (frames == 0) return refuse(site, "opus_decoder_failure", "the Opus decoder produced no frames");
    ++stats().attempts;

    trace_event(field_string("event", "decoded") + "," + field_string("site", site) + "," +
                field_string("codec", "opus") + "," + field_u64("frames", frames) + "," +
                field_u64("samples", frames * channels));
    Decode result;
    result.ok = true;
    result.frames = frames;
    result.bytes = frames * channels * 4;
    return result;
}

// ---- The measured XMA context array ----

// acphal records the SHAPE XMA context array it allocated. The title allocated 350 of them and (this
// scan measures) never programmed one; the scan counts non-zero dwords across the whole array so the
// boot can say that with a number instead of a hypothesis.
inline void note_xma_contexts(void* array, uint32_t count) {
    const std::lock_guard lock(mutex());
    state().xma_contexts = array;
    state().xma_context_count = count;
    stats().xma_contexts = count;
    if (!switch_on()) return;
    trace_event(field_string("event", "xma_contexts") + "," + field_u64("count", count));
    note(state().note_contexts,
         "350 XMA contexts allocated by the title; no decode command is sent in this boot, so nothing is decoded");
}

inline void clear_xma_contexts() {
    const std::lock_guard lock(mutex());
    state().xma_contexts = nullptr;
    state().xma_context_count = 0;
}

inline bool has_xma_contexts() {
    const std::lock_guard lock(mutex());
    return state().xma_contexts != nullptr && state().xma_context_count != 0;
}

// The ACP HAL's own command path is not a decode site: on the console that command programmes the SHAPE
// engine, and no SHAPE/DSP transform is carried here. This records the command so the boot can say the
// one measured ACP command was seen and was not mistaken for compressed audio.
inline void note_acp_command(std::uint32_t command) {
    ++stats().attempts;
    if (!switch_on()) return;
    trace_event(field_string("event", "acp_command") + "," + field_u64("cmd", command));
}

// Reads (never writes) the context array and counts non-zero dwords. Bounded: the caller decides how
// often to ask. Returns the non-zero dword count.
inline std::uint32_t scan_xma_contexts() {
    const std::lock_guard lock(mutex());
    State& s = state();
    if (!s.xma_contexts || s.xma_context_count == 0) return 0;
    const auto* words = static_cast<const std::uint32_t*>(s.xma_contexts);
    const std::uint32_t dwords = s.xma_context_count * (64 / 4);
    std::uint32_t nonzero = 0;
    for (std::uint32_t i = 0; i < dwords; ++i) {
        if (words[i] != 0) ++nonzero;
    }
    ++stats().xma_context_scans;
    stats().xma_context_nonzero_dwords = nonzero;
    if (switch_on()) {
        trace_event(field_string("event", "xma_scan") + "," + field_u64("contexts", s.xma_context_count) + "," +
                    field_u64("dwords", dwords) + "," + field_u64("nonzero", nonzero));
    }
    return nonzero;
}

}  // namespace rt::audio_decode

#endif  // PALMETTO_AUDIO_DECODE_H
