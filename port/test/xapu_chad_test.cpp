// xapu.dll and chad.dll shim ABI test, against the call shapes measured from the retail executable
// (docs/audio_shims.md; tooling/import_call_sites.py and the call-site disassembly).
//
// All ten exports are resolved through rt::shims::find, so this exercises the exact tables and calling convention the
// runtime installs. No GPU, no boot and no game data: the functions above are pure process-memory shims.
//
// The trace section at the end is what proves the per-call ("audio") events: it opens the startup trace, drives the
// shims, and reads the file back, so a shim that stops logging fails here instead of silently changing a measurement.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "audio_decode.h"
#include "audio_submit_probe.h"
#include "shims.h"
#include "startup_trace.h"
#include "xapu_effects.h"

#include "audio_decode_fake_backend.h"

namespace {

using XApuConnectFn = HRESULT(__stdcall*)(void*, void*, void**);
using XApuDisconnectFn = HRESULT(__stdcall*)(void*);
using XApuEnqueueFn = HRESULT(__stdcall*)(void*, const uint32_t*, uint64_t);
using XApuGetResultCountFn = uint32_t(__stdcall*)(void*, uint64_t);
using XApuDequeueFn = HRESULT(__stdcall*)(void*, void*);

// Measured: XDspConnectWithMaximumStreamLimit(buffer, length, aggregateSeconds, maxStreams, maxChannels, handle**).
using XDspConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void**);
using XDspActivateFn = HRESULT(__stdcall*)(void*, const void*, void**, void**);
using XDspDeactivateFn = HRESULT(__stdcall*)(void*, uint64_t, uint64_t, uint64_t);
using XDspDisconnectFn = HRESULT(__stdcall*)(void*);
using XDspSubmitFn = HRESULT(__stdcall*)(void*, void*, uint32_t*);

constexpr uint32_t kXApuBufferSize = 0x104000;      // 0x14000 input + 128 * 0x1E00 output (docs/audio_shims.md)
constexpr HRESULT kXApuEPendingResults = static_cast<HRESULT>(0x8004E801u);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

int count_occurrences(const std::string& text, const std::string& needle) {
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

// ---- The trace: every shim call is one "audio" event with its caller ----
void test_trace(XApuConnectFn connect, XApuDisconnectFn disconnect, XApuEnqueueFn enqueue,
                XApuGetResultCountFn result_count, XApuDequeueFn dequeue, XDspConnectFn dsp_connect,
                XDspActivateFn activate, XDspDeactivateFn deactivate, XDspDisconnectFn dsp_disconnect,
                XDspSubmitFn submit) {
    _putenv_s("PALMETTO_AUDIO_EFFECTS", "0");
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path =
        std::wstring(temp) + L"xapu_chad_trace_test_" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    DeleteFileW(path.c_str());

    check(rt::startup_trace::open(path.c_str()), "open the startup trace file");

    uint8_t block[24] = {};
    uint8_t out16[16] = {};
    void* handle = nullptr;
    check(connect(block, out16, &handle) == S_OK && handle != nullptr, "connect with the trace on succeeds");
    const uint32_t commands[] = {1, 2};
    for (uint32_t type : commands) {
        const uint32_t command[2] = {type, 0};
        enqueue(handle, command, 0);
    }
    result_count(handle, 0);
    result_count(handle, 0);
    uint32_t result[20] = {};
    dequeue(handle, result);
    check(disconnect(handle) == S_OK, "disconnect with the trace on succeeds");

    void* dsp = nullptr;
    uint8_t title_buffer[16] = {};
    check(dsp_connect(title_buffer, 0x60000, 1, 0x80, 1, &dsp) == S_OK && dsp != nullptr,
          "chad connect with the trace on succeeds");
    void* status = nullptr;
    void* stream = nullptr;
    const void* opaque_descriptor = reinterpret_cast<const void*>(static_cast<uintptr_t>(1));
    check(activate(dsp, opaque_descriptor, &status, &stream) == S_OK && status != nullptr && stream != nullptr,
          "effect switch off preserves the activation ABI without dereferencing an opaque descriptor");
    uint32_t id = 0xAABBCCDD;
    void* opaque_page = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    check(submit(opaque_page, opaque_page, &id) == S_OK && id == 0xAABBCCDD,
          "effect switch off preserves the submit no-op for opaque pages and id");
    deactivate(dsp, 0, 0, 0);
    check(dsp_disconnect(dsp) == S_OK, "chad disconnect with the trace on succeeds");

    rt::startup_trace::close();

    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string log = text.str();
    DeleteFileW(path.c_str());

    check(count_occurrences(log, "\"kind\":\"audio\"") == 12, "one trace event per call: 7 xapu + 5 chad");
    check(count_occurrences(log, "\"kind\":\"audio_effect\"") == 0,
          "PALMETTO_AUDIO_EFFECTS off adds no effect trace events");
    check(count_occurrences(log, "\"api\":\"XApuConnect\"") == 1, "XApuConnect is traced once");
    check(count_occurrences(log, "\"api\":\"XApuEnqueueCommand\",\"caller\"") == 2,
          "both XApuEnqueueCommand calls are traced");
    check(count_occurrences(log, "\"api\":\"XApuGetResultCount\"") == 2, "both result-count calls are traced");
    check(count_occurrences(log, "\"api\":\"XApuDequeueResult\"") == 1, "the dequeue is traced once");
    check(count_occurrences(log, "\"api\":\"XDspSubmitCommand\"") == 1, "XDspSubmitCommand is traced once");
    check(count_occurrences(log, "\"dll\":\"xapu.dll\"") == 7, "the xapu events name their DLL");
    check(count_occurrences(log, "\"dll\":\"chad.dll\"") == 5, "the chad events name their DLL");
    check(count_occurrences(log, "\"caller\":\"") == 12, "every event names its caller");
    check(count_occurrences(log, "\"type\":1") == 1 && count_occurrences(log, "\"type\":2") == 1,
          "the enqueue events carry the measured command types");
    check(count_occurrences(log, "\"dropped\":1}") == 1 && count_occurrences(log, "\"dropped\":2}") == 1,
          "the dropped-command counter increments across the traced enqueues (1, then 2)");
}

// ---- The decode boundary's runtime wiring (docs/audio_decode.md) ----
//
// With the decode switch on, a submitted command routes its measured 0x280-byte stream-input region to the XMA2
// decoder, which refuses it by name; the APU poll scans the measured XMA context array a bounded number of
// times; the chad submit is a named DSP refusal. The decoders here are the in-process synthetic backend (the real
// ones are covered by port/xma_decoder's Rust tests); every event below is checked from the trace file.
void test_decode_boundary(XApuConnectFn connect, XApuDisconnectFn disconnect, XApuEnqueueFn enqueue,
                          XApuGetResultCountFn result_count, XDspConnectFn dsp_connect, XDspActivateFn activate,
                          XDspSubmitFn submit, XDspDisconnectFn dsp_disconnect) {
    rt::audio_decode::reset_for_test();
    _putenv_s("PALMETTO_AUDIO_DECODE", "1");
    rt::audio_decode::install_backend_for_test(audio_decode_test::backend());

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path =
        std::wstring(temp) + L"xapu_chad_decode_trace_" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    DeleteFileW(path.c_str());
    check(rt::startup_trace::open(path.c_str()), "open a startup trace for the decode section");

    uint8_t block[24] = {};
    uint8_t out16[16] = {};
    void* handle = nullptr;
    check(connect(block, out16, &handle) == S_OK && handle != nullptr,
          "connect with the decode switch on succeeds");

    // A synthetic two-packet XMA2 buffer through the boundary, then the measured command region.
    uint8_t packets[2048 * 2] = {};
    const auto decoded = rt::audio_decode::decode_xma2("test.xapu", packets, sizeof packets, 48000, 2);
    check(decoded.ok && decoded.frames == audio_decode_test::kFakeXmaFrames,
          "the boundary decodes a synthetic two-packet XMA2 buffer into 480 frames");
    check(rt::audio_decode::stats().xma2_frames == audio_decode_test::kFakeXmaFrames,
          "the bounded sink counts the decoded frames");

    const uint32_t command[2] = {2, 5};
    check(enqueue(handle, command, 0) == S_OK, "a measured command type still returns S_OK with decode on");
    check(rt::audio_decode::stats().last_refusal != nullptr &&
              std::strcmp(rt::audio_decode::stats().last_refusal, "packet_size") == 0,
          "the submitted 0x280-byte command region is a named packet_size refusal, not a fake decode");

    const std::uint8_t opus_packet[1] = {0xF8};  // synthetic TOC-only 20 ms packet, no fixture
    const auto opus = rt::audio_decode::decode_opus("test.xapu", opus_packet, sizeof opus_packet, 48000, 2, 960);
    check(opus.ok && opus.frames == audio_decode_test::kFakeOpusFrames &&
              rt::audio_decode::stats().opus_decodes == 1,
          "the boundary decodes a synthetic Opus packet into 960 frames");

    // The measured XMA context array: registered, scanned on the poll schedule, and read (never written).
    std::vector<std::uint32_t> contexts(350 * 16, 0);
    rt::audio_decode::note_xma_contexts(contexts.data(), 350);
    check(result_count(handle, 0) == 0, "the result-count poll still returns 0 with the decode switch on");
    check(rt::audio_decode::stats().xma_context_scans == 1, "the first poll scans the XMA context array");
    contexts[7] = 1;
    for (int i = 0; i < 9; ++i) (void)result_count(handle, 0);  // poll 10 takes the second scan
    check(rt::audio_decode::stats().xma_context_scans == 2 &&
              rt::audio_decode::stats().xma_context_nonzero_dwords == 1,
          "the tenth poll scans again and sees exactly one programmed dword");

    // The DSP submit path is a named refusal: no SHAPE DSP transform is carried.
    void* dsp = nullptr;
    uint8_t title_buffer[16] = {};
    check(dsp_connect(title_buffer, 0x60000, 1, 0x80, 1, &dsp) == S_OK && dsp != nullptr,
          "chad connect with the decode switch on succeeds");
    void* status = nullptr;
    void* stream = nullptr;
    activate(dsp, nullptr, &status, &stream);
    uint32_t id = 0;
    check(submit(stream, title_buffer, &id) == S_OK, "the DSP submit still returns S_OK with decode on");
    check(rt::audio_decode::stats().last_refusal != nullptr &&
              std::strcmp(rt::audio_decode::stats().last_refusal, "dsp_transform") == 0,
          "the DSP submit is a named dsp_transform refusal, not a silent success");
    check(dsp_disconnect(dsp) == S_OK, "chad disconnect with the decode switch on succeeds");
    check(disconnect(handle) == S_OK, "xapu disconnect with the decode switch on succeeds");

    rt::startup_trace::close();
    rt::audio_decode::clear_xma_contexts();
    rt::audio_decode::reset_for_test();
    _putenv_s("PALMETTO_AUDIO_DECODE", "0");

    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string log = text.str();
    DeleteFileW(path.c_str());

    check(count_occurrences(log, "\"kind\":\"audio_decode\"") >= 6,
          "the decode boundary writes audio_decode trace events");
    check(count_occurrences(log, "\"event\":\"decoded\"") == 2, "the two direct decodes are traced");
    check(count_occurrences(log, "\"code\":\"packet_size\"") == 1,
          "the command-region refusal is traced with its code");
    check(count_occurrences(log, "\"code\":\"dsp_transform\"") == 1, "the DSP refusal is traced with its code");
    check(count_occurrences(log, "\"event\":\"xma_scan\"") == 2, "both scheduled context scans are traced");
    check(count_occurrences(log, "\"nonzero\":0") == 1 && count_occurrences(log, "\"nonzero\":1") == 1,
          "the scans trace the programmed-dword counts (0 then 1)");
    check(count_occurrences(log, "\"codec\":\"xma2\"") == 1 && count_occurrences(log, "\"codec\":\"opus\"") == 1,
          "the decoded events name their codec");
}

void test_endpoint_format_negotiation() {
    using rt::xapu_effects::AudioFormat;
    using rt::xapu_effects::EndpointKind;
    using rt::xapu_effects::FormatAnswer;
    using rt::xapu_effects::SampleEncoding;
    using rt::xapu_effects::negotiate_endpoint_format;

    const auto named_refusal = [](const FormatAnswer& answer) {
        return !answer.accepted && answer.refusal != nullptr &&
               std::strcmp(answer.refusal, rt::xapu_effects::kEndpointFormatUnmeasured) == 0;
    };

    const auto shared_mix = negotiate_endpoint_format(
        EndpointKind::SharedMix, AudioFormat{65534, 48000, 8, 32, SampleEncoding::Unmeasured});
    check(named_refusal(shared_mix) && shared_mix.observed_format != nullptr &&
              std::strcmp(shared_mix.observed_format,
                          "shared_mix_tag_65534_8ch_48k_32bit_subtype_unmeasured") == 0,
          "the measured 8-channel 48 kHz 32-bit extensible mix is identified but its subtype is refused by name");

    const auto guessed_float = negotiate_endpoint_format(
        EndpointKind::SharedMix, AudioFormat{65534, 48000, 8, 32, SampleEncoding::IeeeFloat});
    check(named_refusal(guessed_float) && guessed_float.observed_format != nullptr,
          "the shared-mix evidence does not promote an unmeasured subtype to float");

    const auto guessed_pcm = negotiate_endpoint_format(
        EndpointKind::SharedMix, AudioFormat{65534, 48000, 8, 32, SampleEncoding::PcmInteger});
    check(named_refusal(guessed_pcm) && guessed_pcm.observed_format != nullptr,
          "the shared-mix evidence does not promote an unmeasured subtype to integer PCM");

    const auto wrong_mix_tag = negotiate_endpoint_format(
        EndpointKind::SharedMix, AudioFormat{3, 48000, 8, 32, SampleEncoding::PcmInteger});
    check(named_refusal(wrong_mix_tag) && wrong_mix_tag.observed_format == nullptr,
          "an unmeasured shared-mix format tag gets no observed-format answer");

    const auto wrong_mix_rate = negotiate_endpoint_format(
        EndpointKind::SharedMix, AudioFormat{65534, 44100, 8, 32, SampleEncoding::Unmeasured});
    check(named_refusal(wrong_mix_rate) && wrong_mix_rate.observed_format == nullptr,
          "an unmeasured shared-mix sample rate gets a named refusal, not resampling");

    const auto wrong_mix_channels = negotiate_endpoint_format(
        EndpointKind::SharedMix, AudioFormat{65534, 48000, 2, 32, SampleEncoding::Unmeasured});
    check(named_refusal(wrong_mix_channels) && wrong_mix_channels.observed_format == nullptr,
          "an unmeasured shared-mix channel count gets a named refusal");

    const auto wrong_mix_depth = negotiate_endpoint_format(
        EndpointKind::SharedMix, AudioFormat{65534, 48000, 8, 24, SampleEncoding::Unmeasured});
    check(named_refusal(wrong_mix_depth) && wrong_mix_depth.observed_format == nullptr,
          "an unmeasured shared-mix sample depth gets a named refusal");

    const auto spatial = negotiate_endpoint_format(
        EndpointKind::SpatialObject, AudioFormat{3, 48000, 1, 32, SampleEncoding::PcmInteger});
    check(spatial.accepted && spatial.refusal == nullptr && spatial.observed_format != nullptr &&
              std::strcmp(spatial.observed_format, "spatial_object_pcm_tag3_mono_48k_pcm32") == 0,
          "the measured tag-3 mono 48 kHz PCM32 spatial-object request matches exactly");

    const auto wrong_spatial_encoding = negotiate_endpoint_format(
        EndpointKind::SpatialObject, AudioFormat{3, 48000, 1, 32, SampleEncoding::IeeeFloat});
    check(named_refusal(wrong_spatial_encoding),
          "the spatial PCM measurement is not widened to an unmeasured float format");

    const auto wrong_spatial_tag = negotiate_endpoint_format(
        EndpointKind::SpatialObject, AudioFormat{65534, 48000, 1, 32, SampleEncoding::PcmInteger});
    check(named_refusal(wrong_spatial_tag), "an unmeasured spatial-object format tag gets a named refusal");

    const auto wrong_spatial_channels = negotiate_endpoint_format(
        EndpointKind::SpatialObject, AudioFormat{3, 48000, 2, 32, SampleEncoding::PcmInteger});
    check(named_refusal(wrong_spatial_channels), "an unmeasured stereo spatial-object format gets a named refusal");

    const auto wrong_spatial_rate = negotiate_endpoint_format(
        EndpointKind::SpatialObject, AudioFormat{3, 44100, 1, 32, SampleEncoding::PcmInteger});
    check(named_refusal(wrong_spatial_rate), "an unmeasured spatial-object sample rate gets a named refusal");

    const auto wrong_spatial_depth = negotiate_endpoint_format(
        EndpointKind::SpatialObject, AudioFormat{3, 48000, 1, 24, SampleEncoding::PcmInteger});
    check(named_refusal(wrong_spatial_depth), "an unmeasured spatial-object sample depth gets a named refusal");

    const auto unknown_endpoint = negotiate_endpoint_format(
        static_cast<EndpointKind>(255), AudioFormat{3, 48000, 1, 32, SampleEncoding::PcmInteger});
    check(named_refusal(unknown_endpoint), "an unknown endpoint kind is refused without a guessed route");
}

void test_effect_contract_model() {
    using rt::xapu_effects::EffectContractTracker;
    using rt::xapu_effects::LifecycleState;

    EffectContractTracker tracker;
    check(tracker.state() == LifecycleState::Connected,
          "a new effect-contract tracker starts at the measured DSP connection boundary");

    // A deliberately invalid pointer proves that the contract guard treats the title descriptor as opaque.
    const void* opaque_descriptor = reinterpret_cast<const void*>(static_cast<uintptr_t>(1));
    const char* activation_refusal = tracker.observe_activation(opaque_descriptor);
    check(std::strcmp(activation_refusal, rt::xapu_effects::kEffectDescriptorUnmeasured) == 0,
          "activation refuses an opaque descriptor by name without dereferencing it");
    check(tracker.state() == LifecycleState::ActivationObserved,
          "the tracker records the activation attempt but does not claim an effect became active");
    check(std::strcmp(EffectContractTracker::refuse_submission(), rt::xapu_effects::kEffectKindUnimplemented) == 0 &&
              tracker.state() == LifecycleState::ActivationObserved,
          "an unmeasured submission gets a named refusal without changing lifecycle state");

    tracker.observe_deactivation();
    check(tracker.state() == LifecycleState::DeactivationObserved,
          "deactivation is tracked as an observed call, not a successful DSP stop");
    tracker.observe_disconnect();
    check(tracker.state() == LifecycleState::Disconnected,
          "disconnect closes the effect-contract tracker");
    check(std::strcmp(tracker.observe_activation(opaque_descriptor), rt::xapu_effects::kEffectKindUnimplemented) == 0 &&
              tracker.state() == LifecycleState::Disconnected,
          "activation after disconnect is refused by name without reopening the tracker");
    tracker.observe_deactivation();
    check(tracker.state() == LifecycleState::Disconnected,
          "deactivation after disconnect does not reopen the tracker");

    EffectContractTracker null_descriptor_tracker;
    check(std::strcmp(null_descriptor_tracker.observe_activation(nullptr),
                      rt::xapu_effects::kEffectDescriptorUnmeasured) == 0 &&
              null_descriptor_tracker.state() == LifecycleState::ActivationObserved,
          "a null descriptor is still named unmeasured without reading memory");
}

void test_effect_switch(XDspConnectFn dsp_connect, XDspActivateFn activate, XDspDeactivateFn deactivate,
                        XDspDisconnectFn dsp_disconnect, XDspSubmitFn submit) {
    _putenv_s("PALMETTO_AUDIO_EFFECTS", "1");
    _putenv_s("PALMETTO_AUDIO_DECODE", "0");

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path =
        std::wstring(temp) + L"xapu_chad_effect_trace_" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    DeleteFileW(path.c_str());
    check(rt::startup_trace::open(path.c_str()), "open a startup trace for the effect-contract switch");

    std::uint8_t title_buffer[16];
    std::memset(title_buffer, 0x3C, sizeof title_buffer);
    void* dsp = nullptr;
    check(dsp_connect(title_buffer, 0x60000, 1, 0x80, 1, &dsp) == S_OK && dsp != nullptr,
          "effect guard: chad connection still succeeds");

    void* status = nullptr;
    void* stream = nullptr;
    const void* opaque_descriptor = reinterpret_cast<const void*>(static_cast<uintptr_t>(1));
    check(activate(dsp, opaque_descriptor, &status, &stream) == S_OK && status != nullptr && stream != nullptr,
          "effect guard: activation preserves S_OK and both output pointers");
    std::uint32_t command_id = 0x12345678;
    void* opaque_page = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    check(submit(opaque_page, opaque_page, &command_id) == S_OK && command_id == 0x12345678,
          "effect guard: unimplemented submit preserves S_OK and the caller's command id without reading pages");

    bool status_unchanged = status != nullptr && stream != nullptr;
    if (status && stream) {
        for (std::size_t i = 0; i < 256; ++i) {
            status_unchanged = status_unchanged && static_cast<std::uint8_t*>(status)[i] == 0 &&
                               static_cast<std::uint8_t*>(stream)[i] == 0;
        }
    }
    check(status_unchanged, "effect guard: submit leaves status and stream bytes zero and unchanged");
    bool input_untouched = true;
    for (std::uint8_t byte : title_buffer) input_untouched = input_untouched && byte == 0x3C;
    check(input_untouched, "effect guard leaves the title-owned connection buffer unchanged");

    check(deactivate(dsp, 0, 0, 0) == S_OK, "effect guard: deactivation keeps its existing success answer");
    check(dsp_disconnect(dsp) == S_OK, "effect guard: disconnect keeps its existing success answer");
    rt::startup_trace::close();

    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string log = text.str();
    DeleteFileW(path.c_str());

    check(count_occurrences(log, "\"kind\":\"audio_effect\"") == 4,
          "the opt-in guard records activation, submit, deactivation and disconnect only");
    check(count_occurrences(log, "\"code\":\"effect_descriptor_unmeasured\"") == 1,
          "activation refusal is named effect_descriptor_unmeasured");
    check(count_occurrences(log, "\"code\":\"effect_kind_unimplemented\"") == 1,
          "submit refusal is named effect_kind_unimplemented");
    check(count_occurrences(log, "\"state\":\"deactivation_observed\"") == 1 &&
              count_occurrences(log, "\"state\":\"disconnected\"") == 1,
          "the opt-in trace records the observed effect call lifecycle");
    check(count_occurrences(log, "\"descriptor\"") == 0,
          "the opt-in trace does not expose the unmeasured descriptor pointer or bytes");
    _putenv_s("PALMETTO_AUDIO_EFFECTS", "0");
}

namespace audio_submit = palmetto::audio_submit_probe;

void test_audio_submit_shape_classifier() {
    const audio_submit::Arguments measured{0x8000, 0x1000, 0x1040, 0x1000, 0x2000};
    check(audio_submit::classify(measured) == audio_submit::ShapeResult::Measured,
          "submit classifier accepts context + stack descriptor + stack command-id dword");
    check(std::strcmp(audio_submit::shape_name(audio_submit::ShapeResult::Measured),
                      "measured-context-descriptor-0x20-command-id-u32") == 0,
          "submit classifier names the measured argument shape");

    auto args = measured;
    args.submit_context = 0;
    check(audio_submit::classify(args) == audio_submit::ShapeResult::ContextNull,
          "submit classifier names a null context pointer");
    args = measured;
    args.command_descriptor = 0;
    check(audio_submit::classify(args) == audio_submit::ShapeResult::DescriptorNull,
          "submit classifier names a null descriptor pointer");
    args = measured;
    args.command_id_out = 0;
    check(audio_submit::classify(args) == audio_submit::ShapeResult::CommandIdNull,
          "submit classifier names a null command-id out pointer");
    args = measured;
    args.stack_base = args.stack_limit;
    check(audio_submit::classify(args) == audio_submit::ShapeResult::InvalidStackBounds,
          "submit classifier refuses invalid stack bounds");
    args = measured;
    args.command_descriptor += 1;
    check(audio_submit::classify(args) == audio_submit::ShapeResult::DescriptorUnaligned,
          "submit classifier requires an 8-byte-aligned descriptor");
    args = measured;
    args.command_id_out += 1;
    check(audio_submit::classify(args) == audio_submit::ShapeResult::CommandIdUnaligned,
          "submit classifier requires a 4-byte-aligned command-id dword");
    args = measured;
    args.command_descriptor = 0x1FE8;
    check(audio_submit::classify(args) == audio_submit::ShapeResult::DescriptorOutsideStack,
          "submit classifier refuses a descriptor range that crosses the stack base");
    args = measured;
    args.command_id_out = 0x2000;
    check(audio_submit::classify(args) == audio_submit::ShapeResult::CommandIdOutsideStack,
          "submit classifier refuses an id dword outside the stack");
}

}  // namespace

int main() {
    _putenv_s("PALMETTO_AUDIO_EFFECTS", "0");
    std::printf("xapu_chad_test: registered xapu.dll and chad.dll shims against the measured call shapes\n");

    const auto connect = reinterpret_cast<XApuConnectFn>(rt::shims::find("xapu.dll", "XApuConnect"));
    const auto disconnect = reinterpret_cast<XApuDisconnectFn>(rt::shims::find("xapu.dll", "XApuDisconnect"));
    const auto enqueue = reinterpret_cast<XApuEnqueueFn>(rt::shims::find("xapu.dll", "XApuEnqueueCommand"));
    const auto result_count =
        reinterpret_cast<XApuGetResultCountFn>(rt::shims::find("xapu.dll", "XApuGetResultCount"));
    const auto dequeue = reinterpret_cast<XApuDequeueFn>(rt::shims::find("xapu.dll", "XApuDequeueResult"));
    const auto dsp_connect = reinterpret_cast<XDspConnectFn>(
        rt::shims::find("chad.dll", "XDspConnectWithMaximumStreamLimit"));
    const auto activate = reinterpret_cast<XDspActivateFn>(rt::shims::find("chad.dll", "XDspActivate"));
    const auto deactivate = reinterpret_cast<XDspDeactivateFn>(rt::shims::find("chad.dll", "XDspDeactivate"));
    const auto dsp_disconnect = reinterpret_cast<XDspDisconnectFn>(rt::shims::find("chad.dll", "XDspDisconnect"));
    const auto submit = reinterpret_cast<XDspSubmitFn>(rt::shims::find("chad.dll", "XDspSubmitCommand"));

    check(connect != nullptr && disconnect != nullptr && enqueue != nullptr && result_count != nullptr &&
              dequeue != nullptr,
          "all five xapu.dll exports are registered");
    check(dsp_connect != nullptr && activate != nullptr && deactivate != nullptr && dsp_disconnect != nullptr &&
              submit != nullptr,
          "all five chad.dll exports are registered");
    check(rt::shims::owns_dll("xapu.dll"), "the registry owns xapu.dll");
    check(rt::shims::owns_dll("chad.dll"), "the registry owns chad.dll");
    if (!connect || !disconnect || !enqueue || !result_count || !dequeue || !dsp_connect || !activate ||
        !deactivate || !dsp_disconnect || !submit) {
        std::printf("Fatal: missing export bindings\n");
        return 1;
    }

    // ---- XApuConnect: block24 is the title's, out16 is {base, size} ----
    uint8_t block[24];
    std::memset(block, 0xA5, sizeof block);
    uint8_t out16[16];
    std::memset(out16, 0x5A, sizeof out16);
    void* handle = nullptr;
    check(connect(block, out16, &handle) == S_OK && handle != nullptr,
          "XApuConnect returns S_OK with a non-null opaque handle");
    bool block_untouched = true;
    for (uint8_t byte : block) block_untouched = block_untouched && byte == 0xA5;
    check(block_untouched, "XApuConnect leaves the title's 24-byte input block untouched");
    uint64_t base = 0;
    uint64_t size = 0;
    std::memcpy(&base, out16, sizeof base);
    std::memcpy(&size, out16 + 8, sizeof size);
    check(base != 0, "the 16-byte output's first qword is the buffer base");
    check(size == kXApuBufferSize, "the 16-byte output's second qword is the measured 0x104000-byte buffer size");
    if (base && size == kXApuBufferSize) {
        auto* buffer = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(base));
        check(buffer[0] == 0 && buffer[size - 1] == 0, "the shared buffer is zeroed when it is handed over");
        buffer[0] = 0x11;
        buffer[size - 1] = 0x22;
        check(buffer[0] == 0x11 && buffer[size - 1] == 0x22, "the shared buffer is committed and writable end to end");
    }

    check(connect(block, out16, nullptr) == E_POINTER, "XApuConnect(nullptr handle) returns E_POINTER");
    void* rejected = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    check(connect(block, nullptr, &rejected) == E_POINTER && rejected == nullptr,
          "XApuConnect(nullptr output block) returns E_POINTER and clears the handle");

    // ---- XApuEnqueueCommand: accepted and dropped; the result queue stays empty ----
    check(result_count(handle, 0) == 0, "XApuGetResultCount is 0 before any command");
    for (uint32_t type : {1u, 2u, 3u, 5u}) {
        const uint32_t command[2] = {type, 0};
        check(enqueue(handle, command, 0) == S_OK, "XApuEnqueueCommand accepts a measured command type");
    }
    check(result_count(handle, 0) == 0, "XApuGetResultCount stays 0: commands are accepted and dropped");
    uint32_t result[20] = {};  // the title's dequeue local: a 0x50-byte zeroed buffer of result records
    check(dequeue(handle, result) == kXApuEPendingResults,
          "XApuDequeueResult reports the pending-results HRESULT on the empty queue");
    check(enqueue(nullptr, result, 0) == E_INVALIDARG, "XApuEnqueueCommand(nullptr handle) returns E_INVALIDARG");
    check(enqueue(handle, nullptr, 0) == E_INVALIDARG, "XApuEnqueueCommand(nullptr command) returns E_INVALIDARG");
    check(dequeue(nullptr, result) == E_INVALIDARG, "XApuDequeueResult(nullptr handle) returns E_INVALIDARG");
    check(dequeue(handle, nullptr) == E_INVALIDARG, "XApuDequeueResult(nullptr output) returns E_INVALIDARG");

    // ---- Handle lifetime: two connections are distinct; a stale handle is refused, not freed twice ----
    uint8_t out16b[16] = {};
    void* other = nullptr;
    check(connect(block, out16b, &other) == S_OK && other != nullptr, "a second XApuConnect succeeds");
    check(other != handle, "each connection has its own handle");
    uint64_t base_b = 0;
    std::memcpy(&base_b, out16b, sizeof base_b);
    check(base_b != 0 && base_b != base, "each connection has its own shared buffer");
    check(disconnect(handle) == S_OK, "XApuDisconnect frees the first connection");
    check(disconnect(handle) == E_INVALIDARG, "XApuDisconnect refuses the same (stale) handle a second time");
    check(disconnect(nullptr) == E_INVALIDARG, "XApuDisconnect(nullptr) returns E_INVALIDARG");
    check(disconnect(other) == S_OK, "XApuDisconnect frees the second connection");
    check(result_count(nullptr, 0) == 0, "XApuGetResultCount(nullptr handle) is a safe 0");

    // ---- chad: connect, activate, submit, deactivate, disconnect ----
    uint8_t title_buffer[16];
    std::memset(title_buffer, 0x3C, sizeof title_buffer);
    void* dsp = nullptr;
    check(dsp_connect(title_buffer, 0x60000, 1, 0x80, 1, &dsp) == S_OK && dsp != nullptr,
          "XDspConnectWithMaximumStreamLimit returns S_OK with a non-null handle");
    bool title_buffer_untouched = true;
    for (uint8_t byte : title_buffer) title_buffer_untouched = title_buffer_untouched && byte == 0x3C;
    check(title_buffer_untouched, "the chad base buffer stays the title's: the shim never touches it");
    check(dsp_connect(title_buffer, 0x60000, 1, 0x80, 1, nullptr) == E_POINTER,
          "XDspConnectWithMaximumStreamLimit(nullptr handle) returns E_POINTER");

    void* status = nullptr;
    void* stream = nullptr;
    check(activate(dsp, nullptr, &status, &stream) == S_OK, "XDspActivate succeeds");
    check(status != nullptr && stream != nullptr, "XDspActivate writes both outputs");
    if (status && stream) {
        bool zeroed = true;
        for (size_t i = 0; i < 256; ++i) zeroed = zeroed && static_cast<uint8_t*>(status)[i] == 0 &&
                                                       static_cast<uint8_t*>(stream)[i] == 0;
        check(zeroed, "the status and stream blocks are zeroed");
        check(*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(status) + 4) == 0 &&
                  *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(status) + 0xC) == 0,
              "the status dwords the title waits on (at +4 and +0xC) read 0");
    }
    check(activate(nullptr, nullptr, &status, &stream) == E_INVALIDARG, "XDspActivate(nullptr handle) returns E_INVALIDARG");

    uint32_t command_id = 0xDEADBEEFu;
    check(submit(stream, title_buffer, &command_id) == S_OK, "XDspSubmitCommand returns S_OK");
    check(command_id == 0xDEADBEEFu, "XDspSubmitCommand leaves the caller's id as it wrote it (status reads complete)");
    if (status) {
        check(*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(status) + 4) == 0 &&
                  *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(status) + 0xC) == 0,
              "the status block still reports command done, no error after a submit");
    }

    check(deactivate(dsp, 1, 2, 3) == S_OK, "XDspDeactivate succeeds");
    check(deactivate(nullptr, 0, 0, 0) == E_INVALIDARG, "XDspDeactivate(nullptr handle) returns E_INVALIDARG");
    check(dsp_disconnect(dsp) == S_OK, "XDspDisconnect frees the connection");
    check(dsp_disconnect(dsp) == E_INVALIDARG, "XDspDisconnect refuses the same (stale) handle a second time");
    check(dsp_disconnect(nullptr) == E_INVALIDARG, "XDspDisconnect(nullptr) returns E_INVALIDARG");

    // Repeated connect/disconnect cycles must not fail and must not hand out the same live handle twice.
    bool cycles_ok = true;
    for (int i = 0; i < 32; ++i) {
        void* cycle_handle = nullptr;
        uint8_t cycle_out[16] = {};
        cycles_ok = cycles_ok && connect(block, cycle_out, &cycle_handle) == S_OK && cycle_handle &&
                    disconnect(cycle_handle) == S_OK;
    }
    check(cycles_ok, "32 connect/disconnect cycles all succeed");
    bool dsp_cycles_ok = true;
    for (int i = 0; i < 32; ++i) {
        void* cycle_handle = nullptr;
        dsp_cycles_ok = dsp_cycles_ok && dsp_connect(title_buffer, 0x60000, 1, 0x80, 1, &cycle_handle) == S_OK &&
                        cycle_handle && dsp_disconnect(cycle_handle) == S_OK;
    }
    check(dsp_cycles_ok, "32 chad connect/disconnect cycles all succeed");

    // ---- The decode boundary's runtime wiring (docs/audio_decode.md) ----
    test_decode_boundary(connect, disconnect, enqueue, result_count, dsp_connect, activate, submit,
                         dsp_disconnect);

    // ---- The trace: one event per call, with the caller, and nothing before the trace is opened ----
    test_trace(connect, disconnect, enqueue, result_count, dequeue, dsp_connect, activate, deactivate, dsp_disconnect,
               submit);

    // ---- Pure endpoint-format and effect-contract boundary tests ----
    test_endpoint_format_negotiation();
    test_effect_contract_model();
    test_audio_submit_shape_classifier();
    test_effect_switch(dsp_connect, activate, deactivate, dsp_disconnect, submit);

    std::printf("xapu_chad_test complete: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
