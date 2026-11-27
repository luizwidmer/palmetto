# Audio decode: measured XMA2/Opus readiness; title stream not yet measured

**Status: decoder libraries and the opt-in boundary tested; no title-stream decode measured.**

This page is the parity slice for audio decode (rank 8 in the clean-room programme,
[cleanroom_peers.md](cleanroom_peers.md)). It answers three questions with our own measurements: which audio paths the
retail title actually reaches in our boot, where a decode can plug into that path, and what happens when it does. The
short answer is a negative that the boot now states with numbers instead of a hypothesis: the title brings its audio
stack up at +22.25 s, allocates 350 hardware XMA contexts, and then never programmes a context, never enqueues a command
and never sends a stream command, so there is no compressed audio to decode at this stage and the output stays silent.
The decoder libraries are real and their C ABI is tested with synthetic packets. The opt-in shim boundary remains
readiness only because this boot provides no measured title packet or context to feed; the default shims remain silent.

The peer claim being tested (quoted in [cleanroom_peers.md](cleanroom_peers.md)): XCL documents "Opus 0x704F decoded
via MSOpusDecoder.dll; XMA1 / XMA2 decoded in pure Rust", VIctory documents "Menu, video and loading sound; XMA
voices and Opus streams decode", five-star documents XMA contexts that "decode on the caller's thread" and Opus at
48 kHz, and g6shim lists "XMA2 decoding, Opus decoding, DSP queues". Every one of those is a claim about a peer whose
title has reached its menus. Ours has not left its loading state, and this slice measures the distance that leaves.

## 1. Why this path is needed

The title imports ten console audio functions and we serve them with honest stand-ins
([audio_shims.md](audio_shims.md)): `XApuConnect`, `XApuEnqueueCommand`, `XApuGetResultCount`, `XApuDequeueResult`,
`XApuDisconnect` from `xapu.dll`, and `XDspConnectWithMaximumStreamLimit`, `XDspActivate`, `XDspSubmitCommand`,
`XDspDeactivate`, `XDspDisconnect` from `chad.dll`; plus the `acphal.dll` HAL that allocates the SHAPE context arrays
and manages the APU heap. Today every command is accepted and dropped, the result queue is always empty, and the
title's shared buffer is handed over zeroed.

The audio the title would play has two codec halves in its package: AWC banks with XMA2 streams (codec 5) and AWC
banks with Opus streams (codec 0x0B at 48 kHz), documented and already parsed offline by
[audio.md](audio.md). On the console, the SHAPE engine decodes the XMA contexts and the DSP mixes; on this PC the
title still runs its own WASAPI spatial output stream and its own mixer, which the host serves
([silent_audio.md](silent_audio.md)). The decode half is what is missing between "the title has a mix" and "the
title has samples to mix".

## 2. Measured shapes

All from our own 130 s harness boots of the retail executable (1.0.2.1) with the startup trace on, and from the
call-site disassembly recorded in [audio_shims.md](audio_shims.md). The times in the table are the trace boot's
offsets; this slice's own boot (section 5) put the same sequence at +28.31 s to +28.32 s, the few-second start-up
spread the other pages also record. The sequence and the call shapes are the same in both.

| Measured call | When | Shape |
|---|---|---|
| `ApuCreateHeap(cachedSize, nonCachedSize)` | +22.250995 s | `0x1C700000` cached, `0x226E00` non-cached |
| `AcpHalAllocateShapeContexts(ctx)` | +22.251008 s | `numSrc=0, numEqComp=0, numFiltVol=0, numDma=0, numXma=350, numPcm=0`; the shim allocates **350 × 64 = 22,400 bytes** of zeroed XMA contexts and publishes synthetic APU addresses |
| `AcpHalCreate`, `AcpHal::Connect`, `AcpHal::SubmitCommand` | +22.251421..+22.251436 s | one each; the submit command is `cmd=2, d1=0xF0F0F0F0, d2=0xFFFFFFFF, config, 0` |
| `XApuConnect(block24, out16, handle)` | +22.251488 s | the buffer pair comes back `{base, 0x104000}`; the 24-byte block is the title's own state and is untouched |
| `XDspConnectWithMaximumStreamLimit(buf, len, 1, 0x80, 1, handle)` | +22.251622 s | the title's `0x60000`-byte buffer, 30 s aggregate, 128 streams, 1 channel |
| `XApuGetResultCount(handle, 0)` | +22.25 s to the end of the window | 20,032 calls in the 130 s A/B boot of [audio_shims.md](audio_shims.md), about 188/s; the standalone audio-trace boot of the same class is the source of the timing column |
| `XApuEnqueueCommand` | **never** | none of the four measured command sites runs |
| `XApuDequeueResult` | **never** | the empty result queue is never drained |
| `XDspActivate`, `XDspSubmitCommand`, `XDspDeactivate` | **never** | the DSP side never starts |
| disconnects | **never** | the audio graph stays connected to the end |

That is the whole measured program-side path: **allocate, connect, submit one ACP command, connect the DSP, poll.**
The paper shapes behind the table are the ones already recorded in [audio_shims.md](audio_shims.md) sections 2.1 to
2.3 (the `0x14000 + 128 * 0x1E00` shared buffer, the 0x280/0x1E00 per-stream strides, the result-count gate at
`game+0x4EAA86B`); no new call site appears in this slice.

### 2.1 The measured decode plug-in point, and the reason it is latent

The only buffer the title hands the APU client is the shared `0x104000`-byte buffer. Its per-stream input region is
`base + slot * 0x280` (0x280 = 640 bytes) and its output region is `base + 0x14000 + slot * 0x1E00`. An XMA2 packet
is 2048 bytes, so **the measured command input region cannot be a packet buffer**: the compressed audio it names
lives in the title's APU heap, reached through the XMA context the title would programme next. No call site measures
which command programmes a context, and the XMA context array is not written in the boot (section 3.3 measures that).
That is the honest boundary: the decode can be wired to the path we measured, and when the title finally submits a
command the decoder refuses the 0x280-byte descriptor by name instead of inventing a stream.

### 2.2 The crate's state before this slice

`port/xma_decoder` was a C ABI around the `xma` crate with six exports and its own tests, but it was **not usable by
the runtime**, for two reasons:

1. **It could not build in this checkout.** `port/xma_decoder/Cargo.toml` pinned the `xma` dependency to a git URL
   that is a placeholder ("a public emulator project"), and `vendor/xma` contains only that package's licence and
   NOTICE, not its source, so `cargo test --manifest-path port/xma_decoder/Cargo.toml` stopped at "invalid url" while
   parsing the manifest.
2. **Nothing could link it.** There was no CMake target that builds the Rust crate, no `cdylib` output for a runtime
   load, and no caller in the shims.

The dependency is pinned again to the public repository the crate's own NOTICE names
(`https://github.com/omonkeyz/xbox-emulator`, revision `27122f7dfcceb9a899555362ee0dde2d530e033c`, the same
revision the lock file carried), and the Opus decoder is taken from the same revision. `cargo test` now runs.

## 3. What we implemented

### 3.1 The crate (the real decoders)

`port/xma_decoder` still exposes the XMA2 C ABI (`palmetto_xma2_decoder_create/decode_buffer/decoder_destroy`,
`palmetto_xma2_pcm_data/sample_count/destroy`) and now also the Opus half:

| Opus ABI | Behaviour |
|---|---|
| `palmetto_opus_decoder_create(rate, channels, out)` | one mono or stereo stream; the decoder's own rates are 8/12/16/24/48 kHz, so a 32 kHz request is refused by name |
| `palmetto_opus_decode(decoder, packet, len, max_frames_per_channel, out)` | one packet, at most 1275 bytes (RFC 6716), decoded into interleaved 32-bit float PCM; the caller's frame room is bounded at 5760 (120 ms at 48 kHz) |
| `palmetto_opus_pcm_data/sample_count/frame_count/destroy` | the block owns its samples and outlives its decoder |
| `palmetto_audio_decoder_abi_version()` | `1`; the runtime loader refuses any other version |

Both halves catch panics at the FFI boundary, map allocation failure to an explicit status, and keep all state
behind a per-handle mutex. The crate-type is now `["staticlib", "rlib", "cdylib"]`: the staticlib feeds the C ABI
smoke test, and the cdylib (`palmetto_xma2_decoder.dll`) is what the runtime loads; there is no link-time dependency
on Rust in the C++ build.

### 3.2 The runtime boundary: `port/shims/audio_decode.h`

One new header (included by the two audio shim translation units and by their tests) is the whole boundary:

- **Switch.** `PALMETTO_AUDIO_DECODE=1` turns decode on; it is read per call and the default (unset or `0`) is
  inert. Every decode request with the switch off is refused with the code `switch_off`.
- **Decoder library.** The boundary loads `palmetto_xma2_decoder.dll` from `PALMETTO_XMA2_DECODER_DLL` when set, else
  from next to `palmetto.exe`, resolves all thirteen exports, and calls `palmetto_audio_decoder_abi_version()`; a
  missing library is the named refusal `backend_missing`, a mismatched or incomplete one is refused and logged, and
  neither is a link error.
- **Bounded sink.** Decoded PCM is counted, never played: `xma2_decodes`, `xma2_frames`, `xma2_bytes`, `opus_decodes`,
  `opus_frames` and `opus_samples`. The title's own WASAPI spatial stream is already served by the host
  ([silent_audio.md](silent_audio.md)); there is no measured site that hands us the title's mix buffers, so writing
  into them would be invention. The sink is where the content pass plugs the title's own output in.
- **Named refusals.** Each refusal has a stable code (`switch_off`, `null_buffer`, `packet_size`, `format`,
  `backend_missing`, `decoder_create`, `decoder_failure`, `opus_packet_size`, `opus_frame_room`, `opus_format`,
  `opus_decoder_failure`, `stream_slot`, `dsp_transform`), written to stderr for the first eight and always to the
  startup trace as an `audio_decode` event.
- **XMA context bookkeeping.** `note_xma_contexts(array, count)` records the measured 350-context array;
  `scan_xma_contexts()` reads (never writes) all 5,600 dwords and counts the non-zero ones. That is how the boot
  proves whether the title programmes a context, instead of assuming it.
- **Trace.** Every decode, refusal, backend load and context scan is an `audio_decode` JSON event with the caller
  site, codec, frame and byte counts, so one boot explains itself.

### 3.3 The two shims

`port/shims/acphal.cpp`:

- `AcpHalAllocateShapeContexts` hands the XMA context array to the boundary (350 contexts in the measured call);
  `AcpHalReleaseShapeContexts` clears it before freeing.
- The ACP submit command is recorded (`note_acp_command`) and its trace event now carries `cmd`, `d1` and `d2`; it
  is **not** treated as a decode site, because no SHAPE/DSP transform is carried.

`port/shims/xapu_chad.cpp`:

- `XApuConnect` loads the decoder boundary when the switch is on (so the boot reports whether the DLL is there).
- `XApuEnqueueCommand` still returns `S_OK` and drops the command as before; with the switch on it then routes the
  command's measured 0x280-byte input region through `decode_xma2`. The decoder refuses it as `packet_size` (640 is
  not a whole number of 2048-byte packets), and a stream slot outside the measured 128-slot area is refused as
  `stream_slot`.
- `XApuGetResultCount` keeps returning 0 and, with the switch on plus the trace on, takes five bounded scans of the
  XMA context array on polls 1, 10, 100, 1000 and 10,000.
- `XDspSubmitCommand` stays `S_OK` and is a named `dsp_transform` refusal: the chad stream carries the DSP transforms
  and the Opus stream commands, neither of which is carried, and the wrapper's swapped outputs mean no call site
  shows which pointer is the stream block.

The default is unchanged: with the switch off, neither shim makes a decode call, and the committed measurements of
[audio_shims.md](audio_shims.md) describe the default boot.

## 4. Cross-check against the peers

The mechanism claims in [cleanroom_peers.md](cleanroom_peers.md) were read as evidence, not copied. What agrees:

- **Which codecs matter.** XCL (Opus "0x704F" via `MSOpusDecoder.dll`, XMA1/XMA2 "in pure Rust"), five-star (XMA
  contexts decoding on the caller's thread, Opus at 48 kHz) and VIctory ("XMA voices and Opus streams decode") all
  name the same two decode paths for the same title. Our package survey in [audio.md](audio.md) finds the same two
  codecs in the AWC banks (codec 5 XMA2 and codec 0x0B Opus at 48 kHz), so the peer split is the right structure.
- **Decode separate from DSP.** g6shim's own split ("XMA2 decoding, Opus decoding, DSP queues | Left: DSP transforms,
  SHAPE units other than XMA") is what this slice implements: the two decoders are carried, the DSP transforms are a
  named refusal. Our measured boot never activates the DSP, so the refusal costs nothing today.

Where we differ, and why:

- **No in-process graphics/audio device.** XCL decodes Opus through `MSOpusDecoder.dll`, the console system DLL. That
  library is not on this host, so the Opus half is an independently licensed Rust port of libopus behind our own C
  ABI, and it is credited in section 8. Our measurement, not the peer's, decides which file this runtime loads.
- **The decode is latent at our stage.** The peer claims are about sound through menus, video and loading; our boot
  reaches the loading state and stops, and the measured audio graph stops at the two connects. The peer claim is a
  hypothesis for the content pass, and this slice records the measurement that keeps it honest rather than a working
  menu sound.
- **The output sink is ours, not a silent endpoint.** The peers' audio paths run into their own mixers; we do not
  have a measured handover from the title's mixer to us, so the sink counts frames. [silent_audio.md](silent_audio.md)
  already measured and declined a silent endpoint for this host, and that decision stands.

## 5. Boot measurement

One 130 s harness boot of this repository with the slice built in, the decode switch on and the startup trace on:
boot lock held, one boot at a time, `tooling/boot_report.py --timeout 130`, standard environment
`PALMETTO_SHADERS=<translated corpus>`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_PIPELINE_TIMELINE=5`, plus `PALMETTO_AUDIO_DECODE=1` and
`PALMETTO_XMA2_DECODER_DLL=<the cdylib cargo built>` (the cdylib path lives outside the repository). The boot was
`ALIVE 130.3 s` with **0 exceptions**, 19,142 trace events, and 8 `audio_decode` events.

| Measurement | Value in this boot |
|---|---|
| verdict | `ALIVE 130.3 s`, 0 exception events, killed by the 130 s timeout as intended |
| audio bring-up | `ApuCreateHeap` (`0x1C700000` cached, `0x226E00` non-cached), `AcpHalAllocateShapeContexts` `src=0, xma=350, pcm=0`, `AcpHalCreate`, `AcpHal::Connect` and one `AcpHal::SubmitCommand` (`cmd=2`) at +28.31 s, then `XApuConnect` (+28.315 s) and `XDspConnectWithMaximumStreamLimit` (+28.317 s, `0x60000`-byte buffer, 30 s, 128 streams, 1 channel) |
| decode boundary | the loader loaded `palmetto_xma2_decoder.dll` and accepted **ABI 1** at +28.317 s (`audio_decode: backend loaded`) |
| XMA context array | `xma_contexts` count **350** recorded at +28.315 s; five scheduled scans at +28.320, +28.374, +28.852, +33.652 and +81.712 s, each reading **5,600 dwords with 0 non-zero**: the title never programmes a context |
| decode attempts | **0** — no `XApuEnqueueCommand`, no `XDspSubmitCommand`, no `audio_decode` `decoded` or `refused` event |
| audio calls by import | `XApuConnect` 1, `XDspConnectWithMaximumStreamLimit` 1, `XApuGetResultCount` **19,094** (from +28.320 s to +130.213 s, about 186/s); `XApuEnqueueCommand`, `XApuDequeueResult`, `XDspActivate`, `XDspSubmitCommand`, `XDspDeactivate` and both disconnects: **0** |
| frame loop at +125 s | 10,241 slot-57 records (0 refused), 41,545 slot-55 calls, 51,786 graphics pipelines, 464 compute calls / 396 created, 8,771 submits, 4,385 presents, 1,736 console calls; 219,669 D3D12 calls recorded (max `#222,289`) |
| presented image | not captured in this arm (no visual change is expected or claimed) |

**Verdict: the slice does what it says and the boot does not advance, for the measured reason.** The decode boundary
is present, the real decoder library loads and its ABI check passes, and the measured XMA context array is scanned
five times with a zero result: the title allocates its 350 hardware contexts and then never programmes one, so there
is no compressed audio anywhere in the path for this boot and nothing is decoded. The boot is silent as before; the
difference from the committed baseline of [audio_shims.md](audio_shims.md) is that *why* is now a measurement
(0 of 5,600 context dwords, 0 submitted commands) rather than an inference, and the decode path is ready for the
first boot that does programme a context.

Two honest caveats about the frame counters: the working tree this boot was built from also holds other slices'
in-flight changes (the DirectStorage, allocator, card-copy, task-queue and deferred-compile work), which is why its
submits (8,771) and presents (4,385) are lower than the committed baseline's 24,863/12,431 and why the stderr carries
another slice's `d3d12_xs: slot 67 ... failed with 0x80070057` lines. The only audio-path change between the two
builds is this slice, and the audio event counts above are its effect; the frame loop is not claimed to be unchanged
by this page.

## 6. Limits, and the next falsifier

- **The measured boot does not decode anything.** No `XApuEnqueueCommand`, no `XDspSubmitCommand`, and no context
  programming: there is no compressed audio to decode before the title reaches content, so the output stays silent.
  The boot's numbers say so (section 5); the slice is readiness plus the measurement, not menu sound.
- **The command region is not the packet stream.** The 0x280-byte per-stream input region is a descriptor; the XMA2
  packet stream lives in the title's APU heap behind a programmed XMA context whose address semantics no call site
  measures. Wiring the decoder to the APU heap (rather than to the command descriptor) needs the title to programme
  at least one context first; that is the next falsifier: a boot whose context scan counts a non-zero dword in the
  350-context array, then the address fields of that context become measurable.
- **The sink is counters, not sound.** Feeding decoded PCM into the title's own spatial stream needs a measured site
  where the title asks us for mix input; none exists today. The host output endpoint itself works
  ([silent_audio.md](silent_audio.md)), so this is a wiring step, not a new endpoint.
- **Opus has no measured trigger.** The boundary and the decoder are real and tested, but no command or stream
  call in the boot carries an Opus packet; the chad submit path is refused by name because its swapped outputs hide
  which pointer is the stream block.
- **32 kHz Opus streams are refused by name.** The package has 32 kHz Opus streams ([audio.md](audio.md)); the
  underlying decoder's API is 8/12/16/24/48 kHz, so a 32 kHz request is a named `opus_format` refusal rather than an
  invented resample. A resampler is a later slice if a measured call asks for 32 kHz.
- **No CMake target for the crate.** The runtime loads the cdylib at run time, so this slice needed no
  `port/CMakeLists.txt` edit; but nothing builds the crate or copies the DLL next to `palmetto.exe` yet. The
  required target change (a custom target that runs `cargo build --locked --release` and copies
  `palmetto_xma2_decoder.dll` beside the `palmetto` target) is stated to the lead, who owns `port/CMakeLists.txt`.
- **Decoder-package attribution is recorded.** The existing `NOTICE.md` row for `xma` and `opusdec` (LGPL-2.1-or-later /
  BSD-3-Clause) credits the decoder dependencies and our wrapper. This status update adds no decoder or peer implementation,
  so it requires no new NOTICE row.
- **Performance with the switch on.** The switch-on boot pays one DLL load, one 22,400-byte scan for each of five
  polls, and one decode attempt per submitted command (none in this boot). With the switch off it pays one
  environment read per call and nothing else.

## 7. Tests

Synthetic bitstreams only; no fixture, no game data, no extracted asset.

| Test | Covers | Result |
|---|---|---|
| `port/xma_decoder` Rust unit + FFI tests | XMA2 creation/refusals/ownership plus the existing six FFI tests, and seven new Opus FFI tests: ABI version, creation refusals (including 32 kHz), a one-byte synthetic TOC packet that decodes to exactly 960 silence frames at 48 kHz, mono/stereo frame counts, null/oversize/short-room refusals, block ownership, deterministic synthetic packets without unwinding across the FFI | 15 tests pass (`cargo test --locked`), `cargo fmt --check` clean |
| `port/xma_decoder/tests/ffi_cpp_smoke.cpp` | the C ABI of both halves through MSVC, including the Opus packet path and the 960-frame count | builds and runs, exit 0 |
| `port/test/acphal_test.cpp` | the boundary itself with an in-process synthetic backend (`port/test/audio_decode_fake_backend.h`): switch off, a synthetic two-packet XMA2 decode (frame and byte counts, sink counters), the 0x280-byte `packet_size` refusal, the three-channel `format` refusal, a synthetic Opus decode and its 32 kHz / zero-room refusals, the 350-context registration and scan (0 then 1 programmed dword, cleared on release), and the `backend_missing` refusal | 82 checks, 0 failures |
| `port/test/xapu_chad_test.cpp` | the runtime wiring: connect with decode on, the command path's `packet_size` refusal, a direct XMA2 and Opus decode through the boundary, the poll-scheduled context scans (0 then 1 programmed dword), the `dsp_transform` refusal, and the `audio_decode` trace events by name and count | 87 checks, 0 failures |

Both C++ tests are registered with ctest (`acphal`, `xapu_chad`) and pass there. The fake backend stands in only for
the *wiring*; the real decoders are the crate's own tests. That split is deliberate: the C++ build must not need a
Rust toolchain, and the Rust tests must not need the runtime.

## 8. Attribution

This slice was implemented from this project's own measurements (the call sites and boot traces of
[audio_shims.md](audio_shims.md) and the package survey of [audio.md](audio.md)). The behaviour was cross-checked
against the public peers named in [cleanroom_peers.md](cleanroom_peers.md) as evidence only: XCL (GPL-2.0-only),
VIctory and victory-fork (GPL-3.0-or-later), five-star (GPL-3.0-or-later) and g6shim (GPL-3.0-or-later); no peer code,
comment text, identifier or table was copied.

The decoders are third-party packages consumed as Cargo dependencies and wrapped by our own C ABI. Their attribution
is already recorded in the `NOTICE.md` row `xma / opusdec decoder packages` (LGPL-2.1-or-later / BSD-3-Clause).

- `xma` **LGPL-2.1-or-later**, from the public project `omonkeyz/xbox-emulator` at revision
  `27122f7dfcceb9a899555362ee0dde2d530e033c`: a Rust port of FFmpeg's WMA Pro / XMA decoder (`libavcodec/wmaprodec.c`,
  `wmaprodata.h`, `wma_common.c`, sine windows) from FFmpeg commit `76fa18983e3849595fdc332bf9fa583d5fe7a147`.
  Original notices: Wmapro compatible decoder, Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion; (c)
  2008-2011 Sascha Sommer, Benjamin Larsson. The licence text is retained in `vendor/xma/COPYING.LGPLv2.1`.
- `opusdec` **BSD-3-Clause**, from the same project and revision: a Rust port of the libopus float decoder (CELT,
  SILK, hybrid) taken from the official libopus repository at commit
  `316c675fa1c9982d76e9cdcba7231076f75047da`. Copyright 2001-2023 Xiph.Org, Skype Limited, Octasic, Jean-Marc Valin,
  Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo, Mozilla, Amazon. The full
  licence text and conditions for redistribution in source and binary form are in that package's `COPYING`/`NOTICE`.

The wrapper (`port/xma_decoder/src/lib.rs`, header and tests) and the runtime boundary are our own code under
GPL-3.0-or-later.

## 9. C-03 result: no measured title stream at this frontier

This task rechecked the title's own import-call-site record and the 130 s trace in section 5. Peer runtime descriptions
remain evidence of possible mechanisms, not proof of what this boot submitted. At the measured loading frontier:

| Surface | Measured evidence | What is still missing for a real decode |
|---|---|---|
| XMA2 / SHAPE | 350 XMA contexts allocated; five bounded scans read 5,600 dwords each and find 0 non-zero. The only observed `AcpHal::SubmitCommand` is `cmd=2`, `d1=0xF0F0F0F0`, `d2=0xFFFFFFFF`, a config pointer, and `d3=0`. | No programmed context identifies an APU input range, packet bounds, or output-ring cursor. The one ACP command does not establish a stream input. |
| XApu queue | `XApuConnect` succeeds and `XApuGetResultCount` is polled 19,094 times; `XApuEnqueueCommand` and `XApuDequeueResult` are both 0. Static sites show command types 5/1/2/3 with a stream slot and a 0x280-byte per-slot input area, but no live packet or length/codec value is captured. | The 0x280-byte region is not an XMA2 packet. The existing opt-in XMA2 attempt on that region is a tested `packet_size` refusal, not stream decoding; its contents are not proven to be an Opus packet either. |
| Opus / DSP | `XDspConnectWithMaximumStreamLimit` receives the title's 0x60000-byte buffer, but `XDspActivate` and `XDspSubmitCommand` are both 0. The wrapper swaps the activate outputs, and the submit-command pointer roles are not resolved by the current measurement. | No measured Opus packet, format, or stream-buffer range reaches the decoder. Direct Opus tests use synthetic packets only. |

**C-03 is not yet wired to a measured title stream at this frontier.** The Rust decoders and the existing opt-in boundary
are readiness; the tests in section 7 prove decoder/boundary behavior with synthetic data, not title-stream playback. No
new decoder or shim behavior is added here: mapping an unknown context or DSP pointer to compressed data would invent an
input layout and could read unrelated title memory. The default-off behavior remains unchanged.

**Falsifier for the next pass.** Re-open this path only when an own-title trace or static call-site capture provides at
least one of these:

- a live `XApuEnqueueCommand` with its type, slot, source bounds/length and framing/codec semantics established from the
title's writes (record fields and counts only, never payload bytes); or
- a non-zero XMA context plus enough measured fields to map its input address/range and output ring through the title's
  APU mapping calls; or
- an `XDspActivate`/`XDspSubmitCommand` call with the swapped output roles resolved at the title call site and the
  submitted stream's bounds, format and packet framing established.

Then implement only that measured route behind `PALMETTO_AUDIO_DECODE`, keep unsupported shapes as named refusals, add
synthetic contract tests for the measured fields, and verify that switch-off remains byte-identical. A zero-context,
zero-command boot or a peer claim alone does not unlock the path.

**Arm and data limits.** No new arm was run for this docs-only result: section 5 already records a 130 s switch-on boot
(ALIVE 130.3 s, 0 exceptions) with the decoder library loaded and no submitted commands or decode attempts. No boot lock
was acquired. No audio bytes, package bytes, or decoded samples were captured or added to the repository.

**NOTICE.** The existing `NOTICE.md` row `xma / opusdec decoder packages` (LGPL-2.1-or-later / BSD-3-Clause) credits the
third-party decoder dependencies and our wrapper. This status note adds no dependency or peer implementation, so no new
NOTICE row is needed.
