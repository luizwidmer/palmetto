// xapu.dll and chad.dll: the console's Audio Processing Unit (APU) and audio DSP command queues.
//
// On a PC neither device exists. This file gives the title what its own code needs from them to keep running: a handle
// and a zeroed shared buffer from XApuConnect, a handle and a zeroed status block from XDspConnect*/XDspActivate, and a
// success for every command, which is then dropped. Nothing is decoded, mixed or resampled, and every sample the title
// would have played stays silent.
//
// DECODE BOUNDARY (docs/audio_decode.md). With PALMETTO_AUDIO_DECODE=1 only, the command path routes a submitted
// command's measured 0x280-byte stream-input region to the XMA2 decoder boundary in port/shims/audio_decode.h, the
// result-count poll scans the measured XMA context array a bounded number of times, and the DSP submit is a named
// refusal. The measured boot sends no command and programmes no context, so this is latent; with the switch off (the
// default) none of it runs and the behaviour above is unchanged.
//
// WHY THESE IMPORTS MATTER (docs/audio_shims.md section 1). The executable imports ten functions from these two DLLs
// (five each). Before this shim they bound to the unimplemented thunk, which returns 0 and writes no out parameter, so
// the title's XApu/XDsp objects never existed. Its 130 s baseline boot reaches exactly three of the ten (XApuConnect,
// XDspConnectWithMaximumStreamLimit and XApuGetResultCount, each logged once because the thunk logs each import once)
// and no command, activation, submit or dequeue is ever called. With the shims the same boot gets real handles from the
// two connects and its audio thread polls the APU result count against a live (always empty) queue; it still never
// enqueues a command or activates the DSP, and nothing downstream of the audio bring-up moves. The A/B numbers,
// including that negative, are in the doc.
//
// THE CALL SHAPES ARE MEASURED, NOT GUESSED. Call-site counts and RVAs come from tooling/import_call_sites.py; the
// argument registers and the memory each call site reads or writes come from disassembling the callers (recorded in
// docs/audio_shims.md section 2). In particular:
//   XApuConnect(block24*, out16*, handle**)            block24 is the title's own state (see below), out16 receives
//                                                      {buffer base, buffer size}, handle receives the client.
//   XApuEnqueueCommand(handle, cmd*, flags)            4 sites; cmd[0] is a command type (1, 2, 3 and 5 measured), the
//                                                      return value is never read.
//   XApuGetResultCount(handle, flags)                  returns a count; the caller skips its dequeue loop when 0.
//   XApuDequeueResult(handle, out*)                    called at most count times; a negative HRESULT skips the result.
//   XApuDisconnect(handle)                             the caller then clears its handle global.
//   XDspConnectWithMaximumStreamLimit(buf, len, ...,   6 arguments: the title's XMemAlloc buffer, its length, a third
//                                     maxStreams, channels, handle**) value, 0x80 streams, 1 channel, and the out handle.
//   XDspActivate(handle, desc*, out*, out*)            4 arguments; the title reads status dwords at +4 and +0xC
//                                                      through one output and passes the other to XDspSubmitCommand.
//   XDspSubmitCommand(a, b, id*)                       3 arguments; the title's wrapper swaps the first two. The
//                                                      caller zeroed *id and then waits for status[+0xC] == 0 and
//                                                      status[+4] == *id.
//   XDspDeactivate(handle, ...) / XDspDisconnect(handle) tail calls; the caller returns what they return.
//
// CROSS-CHECK, NOT COPY (policy: docs/upstream_port.md). The public GPL-3.0 fork ubiquitarianism/leonida serves the same
// ten imports (runtime/shims/xapu.cpp, chad.cpp and later xapu_chad.cpp); its measurements and ours agree on the shapes
// and on the outcomes above. It is an evidence source only: this file is written from our own call-site measurements and
// our own style. Two places where our measurement decided differently from that reference are named here and in the
// doc: the 24-byte block is left untouched (below), and the result queue stays empty (below). Attribution is in
// NOTICE.md.
//
// HONEST LIMITS, in one place:
//  - No APU and no DSP exists, so every command is accepted and dropped; no audio is produced and nothing is mixed.
//    With the decode switch on, a submitted command buffer is offered to the decoder boundary and refused by name when
//    it is not a whole number of packets; the measured boot never submits one.
//  - XApuConnect's first argument is the title's own input state, not an output buffer. Our measurement: the title's
//    audio init writes 16 bytes from a template and the qword 0x0000000100000000 there, and the dword at +0xC is
//    0x104000 - the size of the buffer the title expects back. No title read of that address was found after the call.
//    This shim therefore reads nothing and writes nothing there; upstream's first revision zeroed the block, its later
//    revision ("XApuConnect leaves the title's block alone") agrees with our measurement.
//  - The result queue is always empty: XApuGetResultCount returns 0 and XApuDequeueResult returns the measured pending
//    results HRESULT, so the title's dequeue loop does not run. If the title's stream-id state machine needs a
//    completion result for an open or close command, it never receives one; upstream's later revision added those
//    results as a hypothesis. That is the named next slice in docs/audio_shims.md, not a claim of this one.
//  - The shared buffer is PC process memory (VirtualAlloc), not APU physical memory; it is zeroed at connect and then
//    belongs to the title, which writes stream input into it and reads output from it. This shim never looks inside
//    unless the decode switch is on, in which case it reads the measured 0x280-byte per-stream input region only and
//    offers it to the decoder (the decoder refuses it by name: that region is a descriptor, not a packet buffer).
//  - The DSP status block is zeroed and never changes, so a submitted command reads as "command 0 done, no error"
//    against the id the title wrote (0) - which is what the title's wait loop compares - but nothing was executed.
//  - The chad base buffer is the title's XMemAlloc memory; this shim never reads, writes or frees it. The length and
//    the stream/channel limits are recorded and not validated: no call site measures what the console refuses, so a
//    refusal here would be invented.
//  - Handles are process-local. Connect/disconnect check a live set, so a double or stale disconnect is refused instead
//    of freed twice; the hot calls trust the handle they are given, exactly as the title does (it keeps one global per
//    queue and clears it on disconnect).
//  - Threading: the shared buffer and status blocks are per connection and stable for its lifetime; a caller must not
//    disconnect while another thread is inside a call on that connection. That is the console's own lifetime rule.
//
// The trace (--trace FILE) records every call with the caller as module+RVA, so a boot can say which title code drives
// the audio path; there is no other effect and no event when the trace is off.

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <set>
#include <string>

#include "shims.h"
#include "audio_decode.h"
#include "startup_trace.h"

namespace {

// The buffer XApuConnect hands out. Measured from the title's own side: its per-stream input pointer is
// base + slot * 0x280 and its output pointer is base + 0x14000 + slot * 0x1E00 (both read in the callers at
// game+0x4F20AF8..0x4F20B17 and game+0x4F20B62..0x4F20B79), and the input block it passes XApuConnect carries
// 0x104000 in its dword at +0xC. 128 slots fit that exactly: 0x14000 + 128 * 0x1E00 = 0x104000.
constexpr uint32_t kXApuBufferSize = 0x14000 + 128 * 0x1E00;
static_assert(kXApuBufferSize == 0x104000, "the size the title's own connect block names");

// Measured: one stream's command input region inside the shared buffer is 0x280 bytes (the stride the title's
// own input pointer computes) and the output region is 0x1E00 bytes. Compressed audio itself lives in the
// APU heap the title allocates, not in this command region; docs/audio_decode.md records that.
constexpr uint32_t kXApuStreamInputStride = 0x280;
constexpr uint32_t kXApuStreamSlots = 128;
constexpr uint32_t kXApuStreamInputBytes = 0x14000;
constexpr uint32_t kDecodeSampleRate = 48000;

// The console streams decode at 48 kHz (docs/audio.md: all Opus streams in the package declare 48 kHz).
constexpr uint32_t kDecodeChannels = 2;

// XAPU_E_PENDING_RESULTS: what a dequeue of the (always empty) result queue returns. The title's loop treats a
// negative HRESULT as "no result here" and continues.
constexpr HRESULT kXApuEPendingResults = static_cast<HRESULT>(0x8004E801u);

// A set of live handles. Connect adds, disconnect removes and frees only a handle it removed, so a double or stale
// disconnect is refused rather than freed twice. The hot calls (enqueue, submit) do not take this lock: the title owns
// the handle and its lifetime.
template <typename T>
class LiveHandles {
public:
    void add(T* handle) {
        const std::lock_guard lock(mutex_);
        handles_.insert(handle);
    }
    bool contains(T* handle) {
        const std::lock_guard lock(mutex_);
        return handles_.count(handle) != 0;
    }
    bool remove(T* handle) {
        const std::lock_guard lock(mutex_);
        return handles_.erase(handle) != 0;
    }

private:
    std::mutex mutex_;
    std::set<T*> handles_;
};

struct XApuClient {
    void* buffer = nullptr;
    uint32_t size = 0;
    std::atomic<uint64_t> commands_dropped{0};
};

struct XDspConnection {
    // The title reads at least the dwords at +4 and +0xC of one activate output and passes the other to
    // XDspSubmitCommand. Both are zeroed for the connection's lifetime; 256 bytes cover the measured reads with room.
    alignas(16) uint8_t status[256] = {};
    alignas(16) uint8_t stream[256] = {};
    uint32_t base_buffer_length = 0;
    uint32_t aggregate_impulse_response_seconds = 0;
    uint32_t maximum_stream_count = 0;
    uint32_t maximum_channel_count = 0;
};

LiveHandles<XApuClient> g_xapu_handles;
LiveHandles<XDspConnection> g_chad_handles;

// The measured command path: a command names a stream slot, and the title's own input pointer for that slot is
// shared_buffer + slot * 0x280. That region is the only buffer the APU client ever sees from the title, so it is
// where this boundary offers the submitted bytes to the XMA2 decoder. The measured region is 0x280 bytes, which
// is not a whole number of 2048-byte XMA2 packets, so a boot that does send a command gets a named "packet_size"
// refusal instead of a silent zero: the compressed audio it names lives in the title's APU heap, which this shim
// cannot describe until the title programmes an XMA context (it programmes none in the measured boot).
void route_command_input(const XApuClient* client, const uint32_t* command) {
    const uint32_t slot = command[1];
    if (slot >= kXApuStreamSlots) {
        rt::audio_decode::refuse("xapu.enqueue", "stream_slot",
                                 "stream slot " + std::to_string(slot) + " is outside the measured 128-slot input area");
        return;
    }
    const auto* input = static_cast<const uint8_t*>(client->buffer) + slot * kXApuStreamInputStride;
    const rt::audio_decode::Decode result =
        rt::audio_decode::decode_xma2("xapu.enqueue", input, kXApuStreamInputStride, kDecodeSampleRate,
                                      kDecodeChannels);
    (void)result;
}

// ---- Trace and the once-per-process notes ----
//
// Every call is a "audio" trace event with the caller the title called from, so a boot log can say which title code
// drives the audio path. --trace is a diagnostic switch; with it off this costs one atomic read per call.
//
// The caller has to be captured by the export itself: _ReturnAddress() read inside this helper names this file's own
// return address (it did in the first measured boot, and the trace events then carried host:palmetto.exe+0x... instead
// of the title call site). Each export therefore passes what it was called from.

void trace_audio(const char* dll, const char* api, const std::string& fields, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    std::string event = "\"dll\":" + rt::startup_trace::quote(dll) + ",\"api\":" + rt::startup_trace::quote(api) +
                        ",\"caller\":" + rt::startup_trace::quote(rt::startup_trace::where(
                                              reinterpret_cast<uint64_t>(caller)));
    if (!fields.empty()) event += "," + fields;
    rt::startup_trace::event("audio", event);
}

// The trace is JSON, so every measured number that is written in hex is a quoted string, as the other shims write
// their attribute words. The first measured boot wrote them bare and its audio events were not valid JSON.
std::string quote_hex(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%X", value);
    return rt::startup_trace::quote(text);
}

// One stderr line per first event, so a normal (untraced) boot says plainly what the shim did and does not do. The
// lines are informational; nothing depends on them.
void note_once(std::atomic<bool>& flag, const char* text) {
    if (!flag.exchange(true, std::memory_order_relaxed)) std::fprintf(stderr, "[rt] audio: %s\n", text);
}

std::string hex32(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%X", value);
    return text;
}

}  // namespace

// ---- XApuConnect ----
//
// Measured: XApuConnect(block24*, out16*, handle**), called once from game+0x43AEE69 through a two-instruction
// wrapper. The wrapper only loads the three addresses (the block and the 16-byte output are consecutive fields of the
// title's audio state, the handle is the queue global XApuDisconnect later clears) and its result is tested: failure
// reaches the title's fatal int3 at game+0x43AD94E. Success is therefore mandatory once the arguments are sound.
//
// The 16-byte output is written as {buffer base, buffer size}; the title reads the base back before it points stream
// input and output into it. block24 is the title's own input state and is left untouched (header).

HRESULT __stdcall XApuConnect(void* block24, void* out16, void** handle) {
    trace_audio("xapu.dll", "XApuConnect", "", _ReturnAddress());
    (void)block24;
    if (!handle) return E_POINTER;
    *handle = nullptr;
    if (!out16) return E_POINTER;

    void* buffer = VirtualAlloc(nullptr, kXApuBufferSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!buffer) return E_OUTOFMEMORY;
    auto* client = new (std::nothrow) XApuClient();
    if (!client) {
        VirtualFree(buffer, 0, MEM_RELEASE);
        return E_OUTOFMEMORY;
    }
    client->buffer = buffer;
    client->size = kXApuBufferSize;
    g_xapu_handles.add(client);

    const uint64_t info[2] = {reinterpret_cast<uint64_t>(buffer), kXApuBufferSize};
    std::memcpy(out16, info, sizeof info);
    *handle = client;

    static std::atomic<bool> noted{false};
    note_once(noted, "xapu connected: zeroed shared buffer handed to the title; commands are accepted and dropped, "
                     "the result queue stays empty (no APU hardware)");
    // With the decode switch on, load the decoder library now so the boot reports whether the boundary is
    // present before any command could use it. With the switch off this is free.
    if (rt::audio_decode::switch_on()) rt::audio_decode::ensure_backend();
    return S_OK;
}

// ---- XApuDisconnect ----
//
// Measured: XApuDisconnect(handle), called once; the caller loads the handle global, calls, then clears it. A null or
// already-disconnected handle is refused rather than crashing the title.

HRESULT __stdcall XApuDisconnect(void* handle) {
    trace_audio("xapu.dll", "XApuDisconnect", "", _ReturnAddress());
    if (!handle || !g_xapu_handles.remove(static_cast<XApuClient*>(handle))) return E_INVALIDARG;
    auto* client = static_cast<XApuClient*>(handle);
    VirtualFree(client->buffer, 0, MEM_RELEASE);
    delete client;
    return S_OK;
}

// ---- XApuEnqueueCommand ----
//
// Measured: 4 call sites, all with a stack-built command and flags 0; the return value is never read. Types 1, 2, 3
// and 5 are built at the sites (command[0]) with a stream slot in command[1]. The command is accepted and dropped, and
// its per-connection drop count is in the trace event, so a boot says whether the title ever commands the device at all.

HRESULT __stdcall XApuEnqueueCommand(void* handle, const uint32_t* command, uint64_t flags) {
    if (!handle || !command) {
        trace_audio("xapu.dll", "XApuEnqueueCommand",
                    "\"type\":" + std::to_string(0xFFFFFFFFu) + ",\"flags\":" + quote_hex(static_cast<uint32_t>(flags)),
                    _ReturnAddress());
        return E_INVALIDARG;
    }
    const uint32_t type = command[0];
    const uint64_t dropped =
        static_cast<XApuClient*>(handle)->commands_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
    trace_audio("xapu.dll", "XApuEnqueueCommand",
                "\"type\":" + std::to_string(type) + ",\"flags\":" + quote_hex(static_cast<uint32_t>(flags)) +
                    ",\"dropped\":" + std::to_string(dropped),
                _ReturnAddress());
    static std::atomic<bool> noted{false};
    note_once(noted, "xapu command accepted and dropped: no APU hardware on PC");
    if (rt::audio_decode::switch_on()) route_command_input(static_cast<XApuClient*>(handle), command);
    (void)flags;
    return S_OK;
}

// ---- XApuGetResultCount ----
//
// Measured: returns a count; the caller (game+0x4EAA86B) skips its dequeue loop when it is zero. Nothing is ever
// queued, so the answer is always 0.

uint32_t __stdcall XApuGetResultCount(void* handle, uint64_t flags) {
    trace_audio("xapu.dll", "XApuGetResultCount", "\"flags\":" + quote_hex(static_cast<uint32_t>(flags)),
                _ReturnAddress());
    (void)handle;
    (void)flags;
    // Bounded measurement of the XMA context array: the title allocates 350 contexts (acphal records the array)
    // and this scan counts their non-zero dwords at five points in the poll loop, so the boot says with a number
    // whether the title ever programmed one. It reads; it never writes.
    if (rt::audio_decode::switch_on() && rt::startup_trace::enabled() && rt::audio_decode::has_xma_contexts()) {
        static std::atomic<uint64_t> context_polls{0};
        const uint64_t poll = context_polls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (poll == 1 || poll == 10 || poll == 100 || poll == 1000 || poll == 10000) {
            rt::audio_decode::scan_xma_contexts();
        }
    }
    return 0;
}

// ---- XApuDequeueResult ----
//
// Measured: called at most result-count times with a 0x50-byte zeroed local; a negative HRESULT skips that result.
// The queue is empty, so every call reports the pending-results HRESULT the title's loop understands.

HRESULT __stdcall XApuDequeueResult(void* handle, void* result) {
    trace_audio("xapu.dll", "XApuDequeueResult", "", _ReturnAddress());
    if (!handle || !result) return E_INVALIDARG;
    return kXApuEPendingResults;
}

// ---- XDspConnectWithMaximumStreamLimit ----
//
// Measured at game+0x5B5262E (through the wrapper at game+0x5B52600): the six arguments are the title's XMemAlloc'd
// base buffer, its length, a third value the caller computes, 0x80 (streams), 1 (channels) and the out handle; success
// is a return of 0 and the caller turns it into a boolean that is true for 0. The buffer stays the title's: this shim
// never touches it.

HRESULT __stdcall XDspConnectWithMaximumStreamLimit(void* base_buffer, uint32_t base_buffer_length,
                                                   uint32_t aggregate_impulse_response_seconds,
                                                   uint32_t maximum_stream_count, uint32_t maximum_channel_count,
                                                   void** handle) {
    trace_audio("chad.dll", "XDspConnectWithMaximumStreamLimit",
                "\"bufferLength\":" + quote_hex(base_buffer_length) + ",\"aggregateSeconds\":" +
                    quote_hex(aggregate_impulse_response_seconds) + ",\"maxStreams\":" +
                    quote_hex(maximum_stream_count) + ",\"maxChannels\":" + quote_hex(maximum_channel_count),
                _ReturnAddress());
    (void)base_buffer;
    if (!handle) return E_POINTER;
    *handle = nullptr;
    auto* connection = new (std::nothrow) XDspConnection();
    if (!connection) return E_OUTOFMEMORY;
    connection->base_buffer_length = base_buffer_length;
    connection->aggregate_impulse_response_seconds = aggregate_impulse_response_seconds;
    connection->maximum_stream_count = maximum_stream_count;
    connection->maximum_channel_count = maximum_channel_count;
    g_chad_handles.add(connection);
    *handle = connection;

    static std::atomic<bool> noted{false};
    if (!noted.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] audio: chad connected: title buffer %s bytes, %s stream(s), %s channel(s); no DSP "
                     "hardware, every command is a no-op\n",
                     hex32(base_buffer_length).c_str(), hex32(maximum_stream_count).c_str(),
                     hex32(maximum_channel_count).c_str());
    return S_OK;
}

// ---- XDspActivate ----
//
// Measured: 4 arguments, and the title's wrapper swaps the third and fourth before the call, so this function cannot
// rely on which output is which; both are written when non-null. The outputs are this connection's zeroed status and
// stream blocks, which the title reads (status dwords at +4 and +0xC) and hands to XDspSubmitCommand.

HRESULT __stdcall XDspActivate(void* handle, const void* description, void** output_a, void** output_b) {
    trace_audio("chad.dll", "XDspActivate", "", _ReturnAddress());
    (void)description;
    auto* connection = static_cast<XDspConnection*>(handle);
    if (!connection || !g_chad_handles.contains(connection)) return E_INVALIDARG;
    if (output_a) *output_a = connection->status;
    if (output_b) *output_b = connection->stream;
    static std::atomic<bool> noted{false};
    note_once(noted, "chad activated: zeroed status handed to the title; a submitted command reads as complete");
    return S_OK;
}

// ---- XDspDeactivate ----
//
// Measured: a tail call with four arguments; the caller returns what this returns. Nothing was running, so nothing
// stops; the handle stays live until XDspDisconnect.

HRESULT __stdcall XDspDeactivate(void* handle, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    trace_audio("chad.dll", "XDspDeactivate",
                "\"arg1\":" + quote_hex(static_cast<uint32_t>(arg1)) + ",\"arg2\":" +
                    quote_hex(static_cast<uint32_t>(arg2)) + ",\"arg3\":" + quote_hex(static_cast<uint32_t>(arg3)),
                _ReturnAddress());
    (void)arg1;
    (void)arg2;
    (void)arg3;
    auto* connection = static_cast<XDspConnection*>(handle);
    if (!connection || !g_chad_handles.contains(connection)) return E_INVALIDARG;
    return S_OK;
}

// ---- XDspDisconnect ----
//
// Measured: a tail call with the connection handle; the caller returns what this returns. A null or already-closed
// handle is refused rather than freed twice.

HRESULT __stdcall XDspDisconnect(void* handle) {
    trace_audio("chad.dll", "XDspDisconnect", "", _ReturnAddress());
    auto* connection = static_cast<XDspConnection*>(handle);
    if (!connection || !g_chad_handles.remove(connection)) return E_INVALIDARG;
    delete connection;
    return S_OK;
}

// ---- XDspSubmitCommand ----
//
// Measured at game+0x4E5B20D through a wrapper that swaps its first two arguments; the third is the id the title
// zeroes before the call. The title then waits until the status dword at +0xC is 0 and the one at +4 equals the id.
// This connection's status is zeroed and the id is left as the title wrote it, which is that state, so the wait
// completes immediately. Nothing is read or written here.

HRESULT __stdcall XDspSubmitCommand(void* arg_a, void* arg_b, uint32_t* command_id) {
    trace_audio("chad.dll", "XDspSubmitCommand", "", _ReturnAddress());
    (void)arg_a;
    (void)arg_b;
    (void)command_id;
    static std::atomic<bool> noted{false};
    note_once(noted, "chad command accepted and dropped: no audio DSP hardware on PC");
    if (rt::audio_decode::switch_on()) {
        // The chad stream carries the DSP side of the console audio graph (SHAPE DSP transforms and the Opus
        // stream commands XCL describes). None of those transforms is carried here, and no call site measures
        // which of the two swapped outputs is the stream block, so this is a named refusal rather than a guess.
        (void)rt::audio_decode::refuse(
            "chad.submit", "dsp_transform",
            "SHAPE DSP transforms are not carried; the XMA2 and Opus decoders are wired only at the APU command "
            "path, and no submit is measured in the boot");
    }
    return S_OK;
}

SHIM_TABLE(xapu,
    {"xapu.dll", "XApuConnect", reinterpret_cast<void*>(&XApuConnect)},
    {"xapu.dll", "XApuDequeueResult", reinterpret_cast<void*>(&XApuDequeueResult)},
    {"xapu.dll", "XApuDisconnect", reinterpret_cast<void*>(&XApuDisconnect)},
    {"xapu.dll", "XApuEnqueueCommand", reinterpret_cast<void*>(&XApuEnqueueCommand)},
    {"xapu.dll", "XApuGetResultCount", reinterpret_cast<void*>(&XApuGetResultCount)},
);

SHIM_TABLE(chad,
    {"chad.dll", "XDspActivate", reinterpret_cast<void*>(&XDspActivate)},
    {"chad.dll", "XDspConnectWithMaximumStreamLimit", reinterpret_cast<void*>(&XDspConnectWithMaximumStreamLimit)},
    {"chad.dll", "XDspDeactivate", reinterpret_cast<void*>(&XDspDeactivate)},
    {"chad.dll", "XDspDisconnect", reinterpret_cast<void*>(&XDspDisconnect)},
    {"chad.dll", "XDspSubmitCommand", reinterpret_cast<void*>(&XDspSubmitCommand)},
);
