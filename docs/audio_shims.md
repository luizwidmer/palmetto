# The console audio shims: `xapu.dll` and `chad.dll`

**Status: base shim measured, implemented, tested, and measured again; effect-contract addendum is source/test readiness
only and has not been built or run.** Two console DLLs that the retail title imports are now
served by [`port/shims/xapu_chad.cpp`](../port/shims/xapu_chad.cpp): the Audio Processing Unit command queue
(`xapu.dll`, five exports) and the audio DSP command queue (`chad.dll`, five exports). The shims are the honest PC
answer - a handle, a zeroed shared buffer, a zeroed status block and a success for every command, which is then dropped
- and the A/B boot in section 5 measures what that changes: the three unimplemented audio lines disappear and the two
connects return real handles, the title's audio thread polls the APU result count against a live queue, and the rest of
the audio path still does not run (no command is ever enqueued, no DSP is activated), so the loading state, the
pipeline population and the presented image do not move. Nothing is decoded or mixed, and the title stays silent
through this path.

This is the first slice under the porting policy in [upstream_port.md](upstream_port.md): the shapes were measured from
the executable with this repository's own tools, the implementation is ours, and the public GPL-3.0 fork
`ubiquitarianism/leonida` was used only as a cross-check (section 4). The call-site survey that first recorded these
imports is [audio_hal_exe_calls.md](audio_hal_exe_calls.md); its "no shim" framing is superseded for these ten by this
page, its measured call sites and constants stand.

## 1. Why these imports are needed

The title brings the audio stack up in its load window. In the 130 s harness boot at `HEAD` (`boot-timeline1`, the
baseline of section 5), its startup trace records the AcpHal bring-up and then exactly three unimplemented audio calls,
all at +25.42 s:

```text
[rt] acphal: command accepted and dropped: no audio DSP hardware on PC
[rt] unimplemented: xapu.dll!XApuConnect
[rt] unimplemented: chad.dll!XDspConnectWithMaximumStreamLimit
[rt] unimplemented: xapu.dll!XApuGetResultCount
```

An unimplemented import returns 0 and **writes no out parameter**, so `XApuConnect` never set the title's queue handle
or the shared buffer, and `XDspConnectWithMaximumStreamLimit` never set the DSP handle. The audio-thread poll of
`XApuGetResultCount` ran against that null handle. What never runs at all in the whole 130 s baseline boot is
`XApuEnqueueCommand`, `XDspActivate`, `XDspSubmitCommand`, `XApuDequeueResult` and any disconnect: those imports appear
in no baseline trace, so the title's audio path stops at the two connects. That is what this slice fixes first - and
section 5 measures that the connects becoming real is all it fixes.

The executable imports ten functions from these DLLs, reached directly from 13 call sites (8 for `xapu`, 5 for `chad`;
`tooling/import_call_sites.py`). One of them is a **fatal** dependency: the audio bring-up tests `XApuConnect`'s
`SUCCEEDED` and jumps to its own `int3` panic at `game+0x43AD94E` when it fails, so a shim that refuses here would stop
the boot outright. The success answers are therefore mandatory; the honest part is what happens to the commands.

## 2. Measured shapes

Measured from the executable's own code with `tooling/import_call_sites.py` (counts and thunk RVAs) and a capstone
disassembly of every call site and its wrapper (argument registers, the memory each site reads or writes, and how the
return value is used). RVAs are for the 1.0.2.1 executable; the method is repeatable on another copy.

| Import | Sites | Call site (game+0x...) | Measured arguments | Result used as |
|---|---:|---|---|---|
| `xapu!XApuConnect` | 1 | `0x43AEE69` | `rcx` = title audio state +0xF30 (24-byte block), `rdx` = +0xF48 (16-byte output), `r8` = queue handle global | `SUCCEEDED`; failure is the `int3` above |
| `xapu!XApuEnqueueCommand` | 4 | `0x483D5`, `0x4EE9978`, `0x4F2092D`, `0x4F20BB8` | `rcx` = queue handle, `rdx` = stack command (`cmd[0]` = type: 5, 1, 2, 3 at the four sites; `cmd[1]` = stream slot), `r8` = 0 | not read |
| `xapu!XApuGetResultCount` | 1 | `0x4EAA86B` | `rcx` = queue handle, `edx` = 0 | count; 0 skips the dequeue loop |
| `xapu!XApuDequeueResult` | 1 | `0x4EAA89A` | `rcx` = queue handle, `rdx` = 0x50-byte zeroed result local | negative skips the result (loop) |
| `xapu!XApuDisconnect` | 1 | `0x43AE930` | `rcx` = queue handle, read from its global and null-checked (the global is cleared after) | not read |
| `chad!XDspConnectWithMaximumStreamLimit` | 1 | `0x5B5262E` | `rcx` = title's XMemAlloc buffer, `edx` = its length, `r8d` = a caller value, `r9d` = 0x80 streams, stack = 1 channel, stack = &handle | 0 = success (`sete al` on `test eax,eax`) |
| `chad!XDspActivate` | 1 | `0x11D475D` | 4 arguments; the wrapper swaps the 3rd and 4th, so the two outputs arrive swapped | 0 = success, anything else returns -1 |
| `chad!XDspSubmitCommand` | 1 | `0x4E5B20D` | 3 arguments; the wrapper swaps the 1st and 2nd; the 3rd is the id pointer | 0 = success |
| `chad!XDspDeactivate` | 1 | `0x1166020` (tail `jmp`) | 4 arguments, handle first | the caller returns it |
| `chad!XDspDisconnect` | 1 | `0x43AEA00` (tail `jmp`) | 1 argument, the handle | the caller returns it |

### 2.1 `XApuConnect`'s buffer, and the 24-byte block that is the title's own

`XApuConnect` is called once, from the audio bring-up function `game+0x43AD8B0..0x43ADA64`, through the two-instruction
wrapper at `game+0x43AEE50`. The wrapper only loads three addresses and tests the result:

```text
game+0x43AEE54  lea rcx, [rip+...]   ; title audio state + 0xF30, 24 bytes
game+0x43AEE5B  lea rdx, [rip+...]   ; title audio state + 0xF48, 16 bytes
game+0x43AEE62  lea r8,  [rip+...]   ; the queue handle global (the one XApuDisconnect later clears)
game+0x43AEE69  call xapu!XApuConnect
game+0x43AEE6E  test eax, eax
game+0x43AEE70  setns al
```

The title's **own audio initialisation** (`game+0x5A87480`, called from `game+0x1D0B7A0`) writes the 24-byte block it
passes: a 16-byte template from `.rdata` and the qword `0x0000000100000000`, with `0x104000` at +0xC. It pre-zeroes the
16-byte second argument. The first template dword at +0 is 1 and the one at +8 is 2; the measured per-stream layout
below makes `0x104000` the buffer size the title expects back. No title read of the 24-byte block was found after the
call (the only references to its exact address are that init write and the wrapper's `lea`). It is title state, not an
out-parameter, so this shim **leaves it untouched**.

The 16-byte output is an out-parameter and the title reads it back. Two sites use it, and they give the buffer layout:

| Where | What the title does |
|---|---|
| `game+0x4F20AF8..0x4F20B17` | input pointer = `[out16]` + slot * 0x280; the command's input is `memcpy`'d there |
| `game+0x4F20B62..0x4F20B79` | output pointer = `[out16]` + 0x14000 + slot * 0x1E00 |

With 128 stream slots (`XDspConnectWithMaximumStreamLimit` asks for 0x80) that is `0x14000 + 128 * 0x1E00 = 0x104000`:
the same number the title puts in its connect block. The shim allocates exactly that many zeroed bytes and reports the
pair `{base, size}`.

### 2.2 The `xapu` queue: commands in, results out

The four enqueue sites build their command on the stack with a type in `cmd[0]` (5, 1, 2 and 3) and a stream slot in
`cmd[1]`, and none of them reads the return value. `XApuGetResultCount` gates the only dequeue loop
(`game+0x4EAA840..0x4EAA931`): a count of 0 skips it, otherwise the loop calls `XApuDequeueResult` that many times into
a zeroed 0x50-byte local and treats a negative HRESULT as "no result". A result record is read as a type at +0, a
stream slot at +4 and a dword at +0x34; the loop updates the title's per-slot state through a table at the handle
global. The queue state lives in the title's own structure (the same global that holds the handle).

### 2.3 The `chad` queue: connect, two outputs, and the status wait

`XDspConnectWithMaximumStreamLimit` is called through two wrappers (`game+0x5B525D0` calls the inner
`game+0x5B525F0` and passes its result as the third argument, then tail-jumps into `game+0x5B52600`). It is the DSP
equivalent of the APU connect: the title hands it its own XMemAlloc'd base buffer and its length, with `0x80` as the
maximum stream count and `1` as the channel count; the handle goes to a field of the calling object and the return
value is converted to a boolean that is true for 0. The startup trace shows an `XMemAlloc` of `0x60000` bytes between
the two connects, which is the buffer this call is given.

`XDspActivate` writes two pointers the title uses as its command status and stream objects. The title reads status
dwords at +4 and +0xC: before it enqueues, `XDspSubmitCommand`'s caller zeroes the command id and then waits for
`status[+0xC] == 0` and `status[+4] == id`. `XDspDeactivate` and `XDspDisconnect` are tail calls whose return values
the caller passes through; none of them is reached in the baseline boot.

## 3. What we implemented

One file, [`port/shims/xapu_chad.cpp`](../port/shims/xapu_chad.cpp), with both `SHIM_TABLE`s. It is registered in
[`port/CMakeLists.txt`](../port/CMakeLists.txt) and serves `xapu.dll` and `chad.dll` as whole modules (the registry's
`owns_dll` then answers a run-time `LoadLibrary` of either with the same table).

| Piece | Behaviour |
|---|---|
| `XApuConnect` | Allocates a zeroed `0x104000`-byte shared buffer, writes `{base, size}` to the 16-byte output, returns an opaque client handle. Reads and writes nothing at the 24-byte block. Returns `E_POINTER` for a null handle or null output, `E_OUTOFMEMORY` if the allocation fails. |
| `XApuEnqueueCommand` | Validates the handle and command pointer (`E_INVALIDARG`), counts the command as dropped and returns `S_OK`. The type is recorded in the trace; nothing is executed. |
| `XApuGetResultCount` | Always 0. There is no result queue. |
| `XApuDequeueResult` | Always the measured pending-results HRESULT `0x8004E801`, which the title's loop reads as "nothing here". |
| `XApuDisconnect` | Refuses a null or already-disconnected handle (`E_INVALIDARG`), frees the buffer and the client otherwise. |
| `XDspConnectWithMaximumStreamLimit` | Stores the title's buffer length and stream/channel limits, returns an opaque connection handle. The buffer stays the title's: the shim never reads, writes or frees it. |
| `XDspActivate` | Hands back the connection's two zeroed 256-byte status/stream blocks (the title reads at least +4 and +0xC through one, and passes the other to `XDspSubmitCommand`). |
| `XDspSubmitCommand` | Returns `S_OK` and touches no memory: the zeroed status block already reads as "command 0 done, no error" against the id the title wrote, so its wait completes. |
| `XDspDeactivate` / `XDspDisconnect` | `S_OK` / free the connection; null or stale handles are refused. |

Details that are deliberate rather than accidental:

- **Live-handle sets.** Connect/disconnect keep a small set of live handles, so a double or stale disconnect returns
  `E_INVALIDARG` instead of freeing twice. The hot calls (enqueue, submit, deactivate) trust the handle the title owns,
  as the console's own lifetime rule does.
- **Measured numbers, not magic ones.** The buffer size is the `0x104000` derived in section 2.1; the pending-results
  HRESULT is the one the title's dequeue loop is written against; `0x80`/`1` are what the title asks for, recorded in
  the trace rather than validated (no call site measures what the console would refuse).
- **One honest stderr line per first event**, so an untraced boot says what happened: the xapu buffer is handed over,
  xapu commands are accepted and dropped, chad is connected/activated, and chad commands are dropped.
- **`--trace FILE` records every audio call** as one `audio` event with `dll`, `api`, `caller` as module+RVA and the
  measured fields (command type, flags, buffer length, stream/channel counts, deactivate arguments). This is how
  section 5 counts the calls *by site*; with the trace off it costs one atomic read per call.

## 4. Cross-check against the upstream reference

The public GPL-3.0 fork `ubiquitarianism/leonida` serves the same ten imports in `runtime/shims/xapu.cpp`,
`runtime/shims/chad.cpp` and (later) `runtime/shims/xapu_chad.cpp`, documented in its `docs/runtime_imports.md`. Its
measurements and ours agree on the call shapes, on the 128-slot buffer (`0x14000 + 128 * 0x1E00`), on the zeroed DSP
status being what completes the title's wait, and - in its later revision - on leaving the `XApuConnect` block alone.
Two differences are real and are recorded here rather than smoothed over:

| Topic | Upstream reference | This slice | Why |
|---|---|---|---|
| `XApuConnect`'s 24-byte block | First revision zeroed it; later revision (commit `eaeee26b3`) left it alone and said why | Left untouched | Our measurement shows the title's own init writes the block and nothing reads it back (section 2.1); zeroing would overwrite title configuration |
| Result queue | First revision: empty, like this slice. Later revision (`eaeee26b3`): a type-1 open and a type-2 close command each queue a result, "because the title frees a stream id only when the close result arrives" - stated there as a hypothesis | Empty | This slice's scope is the measured shapes and the connects. Section 5 shows the title never enqueues a command yet, so there is no completion to miss; whether its stream-id state machine needs those results is a hypothesis that cannot be settled until something makes it open a stream |
| Buffer size | `0x14000 + 128 * 0x1E00` (a hypothesis in the reference) | The same number, derived from the title's own connect block (`0x104000` at +0xC) and the two stride sites | Independent agreement on the number; the derivation is ours |
| Handles | Plain allocation, no liveness check | A live-handle set refuses stale/double disconnects | Quality choice: the title's teardown can be called once per global, and a double free is the one avoidable crash |
| Trace | First call of each API and first command of each type (`audio_queue` events) | Every call (`audio` events) | This slice needs call counts *per site* for its A/B; the reference's dedupe cannot produce them |

The reference's later revision, the silent WASAPI output endpoint (`docs/runtime_imports.md` there; a host with no
render endpoint stops its title on an `int3` ~47 s in), is **not** part of this slice: it answers a different question
(the host's audio endpoint), and on this host the boot is ALIVE to its timeout, so the endpoint failure the reference
fixes has not been measured here. It is the second candidate for a later slice (section 6); a silent-endpoint workstream
was started elsewhere in this tree while this slice was written.

## 5. A/B measurement

Two 130 s harness boots of this repository, one on the `HEAD` build before this slice and one on the shared working
tree with the slice built in: `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_PIPELINE_TIMELINE=5`, `PALMETTO_XS_CALLERS=1`, boot lock held, one boot
at a time, `tooling/boot_report.py --timeout 130`.

A is `boot-timeline1`, the boot of the same tree at `HEAD` before this slice (its binary was the pre-shim build). It
carried the first group of switches only, so its loader and capture rows are the same tree's committed measurements
named there rather than switches on that boot. B is `boot-uplift-b2`, the same harness with the shims built in, plus
the logging-only loader switches `PALMETTO_DS_LOAD_TRACE=1`, `PALMETTO_DS_ARRAYS=1`, `PALMETTO_DS_STACK=1` and the
presenter capture (`PALMETTO_CAPTURE=<dir>`, `PALMETTO_CAPTURE_EVERY=2000`); each is documented as changing no call.
The B tree also holds other slices' in-progress changes (DirectStorage, the streamed image memory pool, the Vulkan
device). None of them is on the audio path this page is about, but they are part of B's binary and are why its
submit/present counts can differ from A's by more than nothing. The only audio-path change between the two builds is
`port/shims/xapu_chad.cpp` plus its registration in `port/CMakeLists.txt`. A's binary was built at 08:47 local time;
B's was this slice's rebuild immediately before the run.

| Measurement | A: before (no audio shims) | B: after |
|---|---|---|
| boot verdict | ALIVE 130.2 s, 0 exceptions | ALIVE 130.2 s, 0 exceptions |
| `[rt] unimplemented:` audio lines | 3: `xapu!XApuConnect`, `chad!XDspConnectWithMaximumStreamLimit`, `xapu!XApuGetResultCount` | **0** |
| audio calls by import and caller (`--trace`) | 3 events, one per import, all at +25.42 s: `XApuConnect` from `game+0x43AEE6E`, `XDspConnectWithMaximumStreamLimit` from `game+0x5B52633`, `XApuGetResultCount` from `game+0x4EAA870`. No trace event for `XApuEnqueueCommand`, `XDspActivate`, `XDspSubmitCommand`, `XApuDequeueResult` or a disconnect - those imports are never reached. The thunk logs each import once, so the repeated `XApuGetResultCount` polls are not countable in A | `XApuConnect` 1 (from `game+0x43AEE6E`, +23.278 s); `XDspConnectWithMaximumStreamLimit` 1 (from `game+0x5B52633`, +23.278 s, measured fields `bufferLength 0x60000`, `aggregateSeconds 0x1E` = 30 s, `maxStreams 0x80`, `maxChannels 0x1`); `XApuGetResultCount` **20,032** (all from `game+0x4EAA870`, +23.28 s to +130.12 s, ~188 polls/s). Still no enqueue, activate, submit, dequeue or disconnect in either boot |
| pipeline timeline at +125 s | slot57 10,241 (0 refused); slot55 41,545; graphics created 51,786; compute 95/95; submits 25,093; presents 12,546; console calls 294,680 | slot57 10,241 (0 refused); slot55 41,545; graphics created 51,786; compute 95/95; submits 24,863; presents 12,431; console calls 292,949 (the 4-5% difference is the host frame-rate variance the other pages record) |
| loader activity | same tree's committed measurement ([streamed_consumer.md](streamed_consumer.md) sections 6b-6d): the 4,199-request burst completes by +1.75 s, the small batches end with a last baseline batch at **+16.27 s**, then the loader object stays armed 0 / inflight 0 (no new read job) for the rest of a 300 s boot | measured on this boot: 4,199 requests enqueued in the first second, 4,234 total, 17 submits, **last enqueue and completion at +16.53 s** (`game+0xE8943`); the loader object is armed 1 / inflight 3,677 at +0.26 s and armed 0 / inflight 0 from +2.10 s (the 13-17 s single-request batches are shorter than the 250 ms sample), 19.4 M status polls, no re-arm to the end of the dump (+75.9 s). Same end state |
| presenter captures | same build's loading loop, measured all-black on this tree: [streamed_consumer.md](streamed_consumer.md) frames 16,001-30,001 every one `mean rgb 0.00 0.00 0.00`; [loading_gate.md](loading_gate.md) 17 captures mean 0 0 0 and a black 4K readback | 7 captures, presented frames 1-12,001, 2564x1421, every one `mean rgb 0.00 0.00 0.00` (the PPM files were re-read from disk: 0.0 0.0 0.0) |
| loading state advanced? | no | **no**: identical stationary two-pass loop, identical pipeline population (10,241 slot-57 records, 41,545 slot-55 calls, 51,786 graphics pipelines, 0 refusals), the loader is not re-armed after +18.08 s, and the presented image is black |

**Verdict: the slice does what it says and does not advance the boot.** The three unimplemented audio imports are
served; both connects get real handles and the measured DSP fields come back from the title's own call
(`0x60000`-byte buffer, 30 s aggregate, 0x80 streams, 1 channel); the audio thread's result-count poll now reads a live
queue and gets 0, 20,032 times. But the title never enqueues a command or activates the DSP in either boot, so no
stream is opened, nothing is submitted and nothing is mixed: the audio path still stops at the same place, and the
loading state, the pipeline population and the presented image are unchanged. That is reported as a negative result,
not as progress. A repeat boot of the same build (`boot-uplift-b2`) gave 19,730 polls and the identical call set
(connect 1, DSP connect 1, result count only), and the same timeline shape; the poll count and the loader's last
activity move by a second or two between boots.

A first measured boot of this slice (`boot-uplift-b`) exposed two defects in the slice's own diagnostic path, both
fixed before the run above: `_ReturnAddress()` was read inside the shared trace helper, so the `caller` field named
this file's return address (`host:palmetto.exe+0x1238e0`) instead of the title call site, and the hex fields were
written unquoted, so the audio lines were not valid JSON (the harness counted 39 parseable events instead of 20,034).
The counts in that first run agreed with the run above; the `caller` values and JSON validity did not. The test
asserts the caller and the event count, so this cannot regress silently.

## 6. Limits, and the next slice

- **No audio exists.** Every command is dropped; nothing is decoded, mixed or resampled. The title's shared buffer
  stays zeroed (it may write input into it; the shim never reads it), and whatever it would output is never produced.
- **The result queue is always empty.** The title's dequeue loop never runs. The measurement in section 5 says what
  that does *not* cost yet: the title never enqueues a command at all, so there is no open/close completion to miss.
  The reference's later hypothesis (a type-1 open and a type-2 close command each queue a same-type result, because
  the title frees a stream id only when the close result arrives) becomes testable only once the title starts
  enqueueing; **the next slice is to find out why the audio graph never opens a stream**, not to queue answers to
  commands nobody sends. Candidate measurements, cheapest first: (a) sample the audio thread's blocked site
  (`PALMETTO_WAIT_SAMPLE`) around the poll loop at `game+0x4EAA840`, which says whether it is a timer loop or a wait;
  (b) check whether the title ever asks the host for an audio endpoint (the reference's WASAPI path, section 4) and
  whether the audio device object was created; (c) compare with the upstream's story-load run, where streams do open.
  The falsifier for the current slice's scope: a boot in which `XApuEnqueueCommand` appears in the trace - then, and
  only then, does the result-queue question have an answer to measure.
- **The DSP status never changes.** A command reads as complete against id 0; a title that waits for a *specific*
  non-zero id would wait forever. The measured caller writes 0, so this slice's wait completes.
- **The 24-byte block is not an output as far as we measured.** If a console contract does write it, a title build that
  reads it back would need the shim revisited; the baseline title does not read it.
- **No validation of the DSP buffer or limits.** The title's buffer is never touched; the stream/channel limits are
  recorded. Refusing values the console might refuse would be invention.
- **Threading.** Status blocks and the shared buffer are per connection and stable for its lifetime; a caller must not
  disconnect while another thread is inside a call on that connection (the console's own rule). The shim does not take
  a global lock on the hot path.
- **Silent output endpoint.** Not part of this slice; see section 4. The falsifier is a boot that shows the title
  asking for a render endpoint (and failing) on this host - this host's boot is ALIVE to its timeout, so that failure
  has not been measured here.

## 7. Tests

`xapu_chad_test` (`port/test/xapu_chad_test.cpp`, already defined in `port/CMakeLists.txt` and registered with ctest as
`xapu_chad`): the previously recorded baseline was **65 checks, 0 failures**. The new effect/format checks are now called
from `main()`, but have not been built or run in this integration-gated slice; no post-change pass count is claimed. The
target resolves all ten exports through `rt::shims::find`, so it exercises the installed tables, and covers:

- the measured `XApuConnect` shape: the 24-byte input block is left untouched, the output pair is
  `{non-null, 0x104000}`, the buffer is zeroed and writable end to end; null-argument refusals;
- the empty result queue: count 0, the pending-results HRESULT from a dequeue, no result after commands of types
  1/2/3/5;
- handle lifetime: two connections have distinct handles and buffers, a stale handle is refused, 32
  connect/disconnect cycles, null-handle refusals;
- the chad side: connect leaves the title's buffer untouched, activate writes both zeroed outputs, the status dwords
  at +4 and +0xC read 0, a submit returns `S_OK` and leaves the caller's id (and the status) alone, deactivate and
  disconnect, stale/null refusals, 32 cycles;
- the trace: opening the startup trace and driving the calls produces exactly 12 `audio` events (7 xapu + 5 chad),
  each with its `dll` and `caller`, with the enqueue events carrying the measured command types and the incrementing
  dropped-command counter (1, then 2);
- the new pure endpoint helper cases: exact spatial PCM32 acceptance, the observed shared-mix shape with its subtype
  deliberately refused, and named refusals for unmeasured tags, rates, channels, depths, encodings and endpoint kinds;
- the effect-contract model and opt-in path: lifecycle transitions/refusals, invalid opaque descriptor/page pointers,
  preserved status/stream/id bytes, named trace events when enabled, and no `audio_effect` event when disabled.

## 8. Attribution

The behaviour in this page was established by this repository's own call-site measurement and A/B boots, and
cross-checked against the public GPL-3.0 project [ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida)
(files `runtime/shims/xapu.cpp`, `runtime/shims/chad.cpp`, `runtime/shims/xapu_chad.cpp` and `docs/runtime_imports.md`,
at commits `e6515ea41`, `b95401617`, `28f013c2f`, `eaeee26b3` by Jadiac and `273b3bdf9` by Moriz82). No code from that
project was copied; the full notice is in [NOTICE.md](../NOTICE.md). The upstream project remains the copyright of its
contributors under GPL-3.0.

## 9. Endpoint and effect/reverb contract: measured gap, no reverb implementation

This is a readiness addendum to the earlier A/B result, not a change to its measured boot. It does not add an audio
renderer or make an effect audible.

### 9.1 What our boot and static sites establish

The 130.3 s boot in section 5 of [audio_decode.md](audio_decode.md) creates `AcpHal`, connects it, submits one `cmd=2`
configuration, then connects XApu and XDsp and repeatedly polls `XApuGetResultCount`. That boot records 19,094 polls and
zero `XApuEnqueueCommand`, `XApuDequeueResult`, `XDspActivate`, `XDspSubmitCommand`, `XDspDeactivate`, `XApuDisconnect`
or `XDspDisconnect` calls. The call-site survey in section 2 above
establishes the static activation/submit call shapes, but not the `XDspActivate` descriptor layout, an effect kind, any
send-page layout, or a send's sample rate/channel/encoding. There is no title effect descriptor or DSP send to measure
in this boot, so it cannot establish reverb behavior.

The separate host-output measurement in [silent_audio.md](silent_audio.md) captured `IAudioClient::GetMixFormat` as
`WAVEFORMATEXTENSIBLE` tag 65534, 8 channels, 48 kHz and 32 bits; its retained record does not include the subtype
GUID. The host mix therefore remains subtype-unmeasured: no float32 or integer-PCM claim is made. The title's spatial
pre-mix query is separately measured as `WAVE_FORMAT_PCM` tag 3, mono, 48 kHz, 32-bit, and succeeds. These observations
are not interchangeable.

### 9.2 Peer cross-checks are not our contract

- VIctory's README at [63bcca4](https://github.com/Marvisak/VIctory/blob/63bcca4/README.md) reports menu/video/loading
  sound and XMA/Opus decoding, while listing “Reverb is silent” and Opus results as unverified against the title.
- PercyScarlett's README at [e44c465](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/README.md) names audio
  hardware among its platform services; the referenced README and prologue guide do not supply a title-specific XDsp
  descriptor/send-format contract.
- Five Star Recomp's [`docs/audio-runtime.md` at 34b4dfed](https://github.com/ThatRecompDev/five-star-recomp/blob/34b4dfed/docs/audio-runtime.md)
  documents an XDsp convolution path and a separate kind-2 path. Its own notes call the impulse-response float layout an
  inference, leave the response format unresolved, and say kind 2 is unknown.

Those peer reports are useful leads, not substitutes for this executable's evidence. Our measured boot and static
call-site boundary win for Palmetto: until this title supplies a descriptor and a send, its effect kind and format stay
unmeasured. No peer code was copied; peer claims are paraphrased or quoted briefly with their source cited.

### 9.3 Opt-in contract witness and endpoint helper

Set `PALMETTO_AUDIO_EFFECTS=1` to enable the diagnostic effect-contract witness; it is off by default. Unset/empty,
a value beginning with `0`, or a value too long for the 8-byte environment buffer keeps the old no-op path; any other
short non-empty value also enables it, so use `0` to disable. The switch does not query or reconfigure WASAPI, render
samples, or implement reverb. When enabled, the per-connection tracker records only the observed call lifecycle.
`XDspActivate` emits the named refusal `effect_descriptor_unmeasured` and never dereferences the opaque descriptor;
`XDspSubmitCommand` emits `effect_kind_unimplemented` and never reads its page pointers or changes the caller's id/status
bytes. Both retain the
existing `S_OK` no-op ABI answer; “refusal” means that effect processing is not implemented, not that the exported call
fails. Activation/deactivation/disconnect lifecycle events are diagnostic observations, not proof that a DSP effect ran.

The pure helper in `port/shims/xapu_effects.h` is currently a testable contract boundary, not wired into the WASAPI/COM
path. It recognizes the host mix's tag/channels/rate/bits tuple but returns `endpoint_format_unmeasured`, because the
subtype GUID was not recorded; it refuses both float and PCM guesses. It accepts only the separately measured spatial
pre-mix PCM tag-3 mono 48 kHz/32-bit request. Other rates, channel counts, bit depths, tags, encodings and endpoint kinds
are named refusals, not resampling or channel mapping.

### 9.4 Tests, readiness, limits and falsifier

The new pure format/tracker tests and runtime switch checks are wired into the existing `xapu_chad_test` executable; the
target and its CTest registration already exist in `port/CMakeLists.txt`, so no CMake stanza is needed. They cover the
subtype refusal, exact spatial PCM match, lifecycle and named refusals, opaque invalid descriptor/page pointers, unchanged
status/stream/id bytes, and switch-on/off trace behavior. They have **not** been built or run against these changes in this slice: the integration gate forbids a build/relink, so
there is no post-change test output to report. After integration clears, run:

```text
cmake --build port/build --target xapu_chad_test
ctest --test-dir port/build --output-on-failure -R "^xapu_chad$"
```

No new boot/arm was made for the same reason; this is source/test readiness only, backed by the earlier measured boot
above. No game data or audio bytes were read or added. The falsifier for this conservative boundary is a measured title
`XDspActivate` descriptor together with a measured `XDspSubmitCommand` send (including their size, effect kind, input/output
page formats, and lifecycle). That evidence would justify a new contract and synthetic tests; a peer's implementation or
an unobserved static call site would not.

**NOTICE draft row (not applied in this slice):** VIctory (GPL-3.0-or-later; README, commit 63bcca4; Marvisak and
contributors), PercyScarlett (GPL-2.0-only; README and its prologue documentation, commit e44c465; rpatty-dev), and Five Star
Recomp (GPL-3.0-or-later; its audio-runtime documentation, commit 34b4dfed; ThatRecompDev) — audio-effect parity evidence
consulted only; no peer source copied. `NOTICE.md` is outside this slice's write scope; integrate this draft with any
parallel notice update.
