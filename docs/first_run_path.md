# The first-run and package-splash path: measured, and the one object it asks for

**Status: measured.** The post-30.8 s idle wait is not a first-run, splash, prompt or package-splash stage. This boot
asks the platform for nothing after +29.39 s (22 platform-trace lines between +1.14 s and +29.39 s, then zero for the
remaining 100.8 s of the 130 s window), and of the first-run/UI runtime classes the image links it asks for none: no
`XGameUiImpl`, no `XLauncherImpl`, no `XAppCaptureImpl`, no `XUserAddAsync`/`XUserGetState`, no `XPackageMount`/mount
path and no `XGameRuntimeIsFeatureAvailable`. **The one user-facing object it does ask for is the accessibility speech
synthesizer**: one `XSpeechSynthesizerCreate` at +29.39 s, which the default answer refuses (`E_FAIL`, null handle).
This page records the negative for the stage hypothesis, then implements the smallest honest answer to that one call -
an opt-in silent synthesizer (`PALMETTO_SILENT_SPEECH=1`) - and reports one A/B pair of 130 s boots.

**Short answer of the A/B: the speech path advances one call and the stage does not.** With the switch on, the title
goes past the create for the first time and calls `XSpeechSynthesizerEnumerateInstalledVoices` (+1 new request at
+24.92 s, answered `S_OK` with one silent voice), then makes no further platform call in the boot. The loader is not
re-opened after its last small batch, the pipeline population is identical (10,241 slot-57 records, 41,545 slot-55
states, 51,786 graphics and 95 compute pipelines), and all six presenter captures in both arms are uniformly black.

## 1. Why these imports/calls are needed

The boot's own timeline is measured: the title's work ends by ~30.8 s and it idles for 570+ s in a stationary black
two-pass loop ([long_boot.md](long_boot.md)); input is not polled ([input_gate.md](input_gate.md)) and the package and
platform path has been served without moving the loader ([platform_gate_ab.md](platform_gate_ab.md),
[loading_gate.md](loading_gate.md)). The remaining class of hypothesis is an early user-facing stage that a console
runtime completes and this port does not: the public fork's story-load integration carries a package-splash shim, a
first-run UI profile and a silent speech voice, and its healthy-boot table lists a package splash and a first-run
prompt before its story load. If such a stage gated this title, the calls that create or await its objects would be in
the platform trace before the wait.

## 2. Measured shapes

### 2.1 What the image links (from the file, no execution)

`tooling/gdk_runtime_calls.py` on the retail executable reads the class and interface ids and the slots the image's
161 runtime thunks call, with the argument count each thunk passes after `this`. The first-run/UI families it links:

| class | slots linked (argument count after `this`) | what the slot is | calls in this boot |
|---|---|---|---|
| `XGameUiImpl` | 7 (3), 8 (1), 17 (6), 18 (2), 19 (4) | profile card, text entry | **0** |
| `XLauncherImpl` | 3 (2), 4 (1), 5 (1) | `XLaunchUri`, display-timeout deferral | **0** |
| `XAppCaptureImpl` | 5 (2) | `XAppCaptureTakeScreenshot` | **0** |
| `XUserImpl` | 21 thunks: 7 (2) `XUserAddAsync`, 8 (2) `AddResult`, 14 (2) `GetState`, 33 (4) `RegisterForChangeEvent`, 34 (2) `UnregisterForChangeEvent` | sign-in, user state, change event | 1: the change-event registration |
| `XAccessibilityImpl` | 10 thunks: 8 (2), 9 (1), 10 (1), 11 (1), 12 (2), 13 (3), 15 (2), 16 (4), 21 (3) | speech synthesizer | 1: `Create` (slot 9) |
| `XPackageImpl` | 18 thunks: 3-7, 9, 12, 14, 16-19, 25-26, 34, 37-39 | package identity, installation monitor, mount (37 `MountWithUiAsync`, 38 `MountWithUiResult`), `CloseMountHandle` (26) | 3 class requests, refused |
| `XSystemImpl`, `XGameImpl`, `XGameRuntimeFeatureImpl` | 3, 2, 1 thunks | console id, title id, feature availability | **0** |

The argument counts are the tool's register estimate, and every one matches the parameter list of the function at that
slot in Microsoft's GDK reference (the slot order is `tooling/gdk_runtime_classes.json`, from
the public xgameruntime-docs). So the shapes are known before a call; the table above is what a first-run stage would
use. In this boot it uses only two of them.

### 2.2 The platform call trace (`PALMETTO_USER_TRACE=1`)

One 130 s boot of `port/build/palmetto.exe`; the title's calls, in the order the shim answered them. Times are the
shim's process-relative clock, the caller is the title's own thunk or shared helper.

| class / entry point | calls | when (s) | caller (`game+RVA`) | answered |
|---|---:|---|---|---|
| `XThreadingImpl` (XAsync/XTaskQueue plumbing) | 9 | +1.14 to +29.39 | `+0x55DE08C` | `S_OK` |
| `XNetworkingImpl` | 1 | +1.14 | `+0x55DE08C` | `S_OK` |
| `XUserImpl` (`QueryApiImpl`) | 1 | +2.58 | `+0x55DE08C` | `S_OK` |
| `XUserRegisterForChangeEvent` | 1 | +2.58 | `+0x55DCDCB` | `S_OK` (registered, never invoked) |
| `XPackageImpl` with `IXPackageImpl4` id | 3 | +3.58 | `+0x55DE08C` | `E_NOINTERFACE` (0x80004002): v4 is opt-in, no package slot runs |
| `XPersistentLocalStorageImpl` | 1 | +3.60 | `+0x55DE08C` | `S_OK` |
| `XDisplayImpl` (HDR query) | 4 | +3.67 to +29.39 | `+0x55DE08C` | `S_OK`, HDR disabled |
| `XAccessibilityImpl` (`IXAccessibilityImpl2`) | 1 | +29.39 | `+0x55DE08C` | `S_OK` |
| - `XSpeechSynthesizerCreate` (slot 9) | 1 | +29.39 | `+0x55DCED0` | **`E_FAIL`** (0x80004005), null handle |

**No platform call follows +29.39 s**, so none follows the +30.8 s boundary; every class in the table is asked for
once or not at all. The same single create was measured at +23.74 s, +25.35 s and +29.39 s in three control boots: the
slot, the argument count and the answer are stable, the clock is boot speed (the shared host varies).

The classes a first-run/splash stage would use never appear: `XGameUiImpl`, `XLauncherImpl`, `XAppCaptureImpl`,
`XGameImpl`, `XSystemImpl` and `XGameRuntimeFeatureImpl` are served by the shims and answered `S_OK` if asked, and none
is requested in 130 s.

### 2.3 Does the title create or await a splash or prompt object?

No. A prompt, dialog, text entry, screenshot or package-mount object would be a request to one of the classes with zero
calls above; none is made. The only user-facing sequence the title starts is the speech synthesizer, and under the
default answer it stops inside it: the create is refused and no other speech slot is called. The one asynchronous
user object that does exist, the `XUserRegisterForChangeEvent` registration, is never invoked because an offline user
never changes - and the title never re-reads user state afterwards.

## 3. What we implemented

### 3.1 The measured negative on the stage hypothesis

The post-30.8 s wait is not gated by a first-run, splash, prompt or package-splash object this runtime fails to
complete, because the title never asks for one. The first presented frame is already the black loading loop (31.9 s in the 600 s boot of
[long_boot.md](long_boot.md)); nothing in it names a movie, a prompt or a splash; and the call trace ends at +29.39 s.
The public fork's package splash is a host-side paint, not a title wait (section 4), and its first-run pages are the
title's own rendering. This page records that negative rather than adding a splash the title does not read.

### 3.2 The silent speech answer (`PALMETTO_SILENT_SPEECH=1`)

The one object on the measured path that the runtime never hands out is the speech synthesizer: the title asks
`XSpeechSynthesizerCreate` and the shim answers `E_FAIL` with a null handle. The public GDK pages list Windows as a
supported platform for the speech API, so the console answer is a synthesizer, and this runtime has no voice to
synthesize with. `port/shims/xgameruntime_offline.cpp` now answers the surface honestly in one way when the switch is
on, and with exactly the old answers when it is off:

- slot 8 `XSpeechSynthesizerEnumerateInstalledVoices` enumerates **one** voice - description, display name, gender
  `Female = 0`, id `palmetto-silent`, language `en-US` - and calls the title's callback once with the caller's context.
  The callback's `bool` return ends the enumeration as it does on a console.
- slot 9 `XSpeechSynthesizerCreate` writes a process-lifetime synthesizer handle to the caller's output pointer.
- slots 10-12 and 14 accept that handle and the enumerated voice id; any other handle or id is refused (`E_INVALIDARG`).
- slots 13 and 21 create a stream from text or SSML; the stream is a handle too.
- slots 15 and 16 answer with a **44-byte RIFF/WAVE container, 16 kHz mono 16-bit PCM with no sample data**: a valid
  silent stream. A buffer smaller than the container is refused with `HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)`,
  a null buffer with `E_POINTER`.
- every caller output pointer is written only after `VirtualQuery` proves the range committed and writable; handles are
  compared by address, so a handle never handed out is refused; a text or voice-id pointer is read only inside
  committed pages and only its **length** is logged, never the content.
- `PALMETTO_USER_TRACE=1` logs each speech slot with its time, caller and answered `HRESULT`, so the A/B can see which
  call the title makes next.

The slice also corrects a measured defect found while writing it: the port had the arguments of slot 8 **swapped** -
it validated the first argument as the callback, while the GDK reference is `(void* context, callback)` - and the old
test asserted the swapped shape. The order now follows the published signature; the default path still reports no
voice and calls nothing back.

No new source file and no build file change: the work is in the existing accessibility section, and the tests extend
the existing `port/test/xgameruntime_offline_test.cpp`. The switch is off by default; unset, the lines, answers and
handles are the ones measured before the slice.

## 4. Cross-check against the reference

The public reference is the GPL-3.0 project ubiquitarianism/leonida at commit `ed94fbef9`; its files were read as
evidence only, no code was copied.

- **Package splash.** In the reference the splash is presentation, not a signal. Its commit `273b3bdf9`
  ([integration] Linux/Proton and native Windows: boot through the story load) adds
  `runtime/shims/package_splash.cpp` and its docs page linux_proton.md: the runtime reads `ShellVisuals/SplashScreenImage` from
  `MicrosoftGame.Config` (or `SplashScreen/Image` from `appxmanifest.xml`), paints the title's window through a
  `ShowWindow` hook, and creates a D3D12 texture that is presented on the same swapchain when the title creates its
  first direct queue; its `docs/boot.md` says "Startup clears keep the splash visible until the first non-black output
  replaces it". Nothing in that path is a callback or completion the title waits for, and the fork's healthy-boot table
  places the splash at 0-10 s while the title's own calls run. **This refutes the task's reading of the fork's splash
  as a stage boundary.** Our tree does not carry the host paint: it presents the title's own swapchain, whose first
  frame is already the black loading loop.
- **First-run UI.** The reference's merge `e79906a42` (PR #939, its docs page first_run_ui.md) describes "the retail
  frame-loop runtime with the shader and rendering changes needed to display the first-run controller prompts"; the
  milestone is "a solid background with legible prompts at the lower right". Those pages are the title's own rendering
  after its content loads - no shim object gates them.
- **Silent speech.** The reference serves the same surface: `runtime/shims/xgameruntime_offline.cpp` (class added by
  commit `4da521de3`, "serve XAccessibilityImpl and XLauncherImpl offline (speech synthesis, launcher)"; the silent
  voice by `d7354f2b9`, "one silent voice, drop XAppCaptureImpl and the lifecycle shim (#401 review)") hands out a
  synthesizer, enumerates one silent voice and returns an empty WAVE header, with the comment that failing the create
  "can keep a title from leaving its first-boot accessibility step". **Where we agree:** the call order and the
  parameter lists, the one-voice enumeration, the synthesizer handle and a WAVE container with no samples.
  **Where we differ:** the reference's silent voice is on by default and forwards to the host's speech when
  `LEONIDA_LOCAL_SPEECH=1`; ours is opt-in (`PALMETTO_SILENT_SPEECH=1`) pending the A/B below, has no host-speech
  forwarding, and keeps this file's `E_INVALIDARG` for an invalid handle where the published page names
  `ERROR_NOT_FOUND`. Our implementation was written from the public GDK pages and this tree's own thunk analysis, not
  from the reference's code.
- **Its own timeline.** The reference's machines reach the first-run pages and the story load with their own runtime
  advances (streamed-image backing, BCPack, frame pacing); this port's boot stops before them. That difference is a
  rendering/streaming frontier, not a first-run signal; the package-served arms of [platform_gate_ab.md](platform_gate_ab.md)
  already bounded the platform class.

## 5. A/B measurement

One 130 s pair, both arms on the same binary (SHA-256 `1351E961F616B8A51AE8A2ABE9E3C0EA7F9741DD864FC2D193C55B2721132CC7`,
built 2026-11-25 11:17:20), one at a time under `_setup/boot_lock.ps1`, own overlay and capture directories outside the
repository. Standard environment (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`) plus the same measurement layer in both arms: `PALMETTO_PIPELINE_TIMELINE=10`,
`PALMETTO_LOADER_HOOK=1`, `PALMETTO_DS_STACK=1`, `PALMETTO_DS_ARRAYS=1`, `PALMETTO_USER_TRACE=1`,
`PALMETTO_CAPTURE` with `PALMETTO_CAPTURE_EVERY=2000` and `PALMETTO_CAPTURE_LIMIT=8`. Arm B adds
`PALMETTO_SILENT_SPEECH=1`.

| measurement | A control | B silent speech |
|---|---|---|
| verdict | ALIVE 130.3 s, killed by the timeout, 0 exceptions | ALIVE 130.2 s, killed by the timeout, 0 exceptions |
| traced D3D12 calls (max #) | 472,755 (#475,585) | 475,716 (#477,900) |
| platform-trace lines (of which speech entries) | 22 (1: the create) | 24 (3: create, the class re-query, the enumerate) |
| last platform call | +29.39 s (`XSpeechSynthesizerCreate`, `E_FAIL`) | +24.92 s (`XSpeechSynthesizerEnumerateInstalledVoices`, `S_OK`) |
| first-run/UI classes requested | none | none |
| new requests after the switch | - | **one**: slot 8 enumerated the silent voice after the create (`S_OK`, callback called once) |
| further speech calls (close, voice, stream) | none (create refused) | none |
| loader begin/end records | 38 / 38 | 38 / 38 |
| last loader record (DirectStorage clock) | +21.379 s | +17.371 s |
| loader armed at/after the 16.5 s check | armed 0 (first sample +16.578 s), never re-armed | armed 0 (first sample +16.583 s), never re-armed |
| status array #17 enqueues / completions, last | 16 / 16 at +21.379 s | 16 / 16 at +17.371 s |
| slot-57 records / slot-55 states | 10,241 / 41,545 | 10,241 / 41,545 |
| graphics / compute pipelines | 51,786 / 95 | 51,786 / 95 |
| submits / presents at the last timeline row | 20,857 / 10,428 | 20,896 / 10,447 |
| presenter captures (frames 1..10,001) | 6, all mean rgb 0.00 0.00 0.00 | 6, all mean rgb 0.00 0.00 0.00 |

Reading the table: the switch is consumed - B asks for the voice list after the create, which A cannot reach - and it
still does not move the stage. The loader's last small batch lands 4.0 s earlier in B, and its status-array clock with
it; that is the boot-speed variable [platform_gate_ab.md](platform_gate_ab.md) section 4 measured (the last batch
tracks how fast the 41,545-state precompile finishes, not the switches), and both arms are dormant after it. The
pipeline population, the presentation and every capture are identical; **verdict: the speech path advanced one call,
the first-run/loading stage unchanged**.

## 6. Limits

- One 130 s pair on one machine (Windows 11, RTX 4090), shared with other agents' builds and boots. The boot-speed
  spread of section 5 and the create clocks of section 2.2 are host variance; the durable state (population, loader
  dormancy, black captures) does not depend on it.
- A 130 s window cannot exclude a stage after 130 s; the 600 s boot of [long_boot.md](long_boot.md) shows no title-side
  work after it either, and the 300 s platform trace of [loading_gate.md](loading_gate.md) ends by +21.56 s.
- The silent voice is **silent**: if the title ever plays the stream, nothing is heard. There is no host speech
  forwarding.
- The 44-byte WAVE container has a valid header and no samples. A title that insists on at least one sample would read
  an empty audio stream; this boot cannot tell, because the title does not get past the voice enumeration. The stream
  data page of the GDK reference does not promise non-empty data.
- One synthesizer and one stream handle exist per process, so two open streams share the same silence. Their identity
  is not distinguishable; nothing measured depends on it.
- The `XSpeechSynthesizerVoiceInformation` layout (40 bytes on x64) is the published struct, not measured from a
  caller; the B boot is the first time the title's own callback read it (no fault, process ALIVE). The same is true
  of the `(void* context, callback)` argument order of slot 8 - the published order, with only the count measured from
  the image.
- `PALMETTO_SILENT_SPEECH` is opt-in and stays off; what would make it default is a boot in which the first-run pages
  or the landing prompt render and the narration path runs end to end, with the A/B showing no regression.
- The package surface is not re-tested here: serving it was measured inert in [platform_gate_ab.md](platform_gate_ab.md),
  and this page's control boot still refuses `IXPackageImpl4` by default, which is the state that page's control arm
  measured.
- No game data: only API names, slot numbers, call times, counts, HRESULTs, the title's own RVAs and image means.

## 7. Tests

`xgameruntime_offline_test` now drives the whole speech surface twice in one process, extending the accessibility
section of `port/test/xgameruntime_offline_test.cpp`; the target runs as both `xgameruntime_offline` and
`xgameruntime_offline_hdr` (the second with `PALMETTO_HDR=1`), and both pass.

- Default: the enumerator calls back zero times and needs a callback (`E_INVALIDARG`); the create fails (`E_FAIL`) and
  nulls the handle; every other speech call refuses the null handle (`E_INVALIDARG`) and zeroes its output.
- With `PALMETTO_SILENT_SPEECH=1` set in the test process: one voice is enumerated with the caller's context and the
  silent voice's fields (Female = 0, `palmetto-silent`, `en-US`); the create hands out a handle; the default and custom
  voice calls accept the handle and the enumerated id and refuse any other id; text and SSML streams are created; the
  size is 44; the data is a RIFF/WAVE container with a zero-byte `data` chunk; a short buffer is refused with
  `ERROR_INSUFFICIENT_BUFFER` and a null buffer with `E_POINTER`; the two handles close; a handle that was never handed
  out is refused; clearing the variable restores the `E_FAIL` create.
- Result: **77 checks, 0 failures** in both runs.

The A/B boots are measurement, not tests: they are not in the repository and no test depends on game files.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project ubiquitarianism/leonida, files `runtime/shims/package_splash.cpp`, `runtime/shims/xgameruntime_offline.cpp`,
`runtime/src/main.cpp`, its docs pages boot.md, first_run_ui.md and linux_proton.md, commits
`273b3bdf9` (Moriz82), `4da521de3` (quackottu), `d7354f2b9` (andretuta) and `e79906a42` (aslan2142). The
XSpeechSynthesizer signatures and structures are Microsoft's public GDK reference (XSpeechSynthesizer.h pages on
learn.microsoft.com/gaming/gdk). No code from that project was copied. The upstream project remains the copyright of
its contributors and its license continues to apply to its own tree. The licence text of this project is GPL-3.0.

## 9. Evidence and reproduction

Outside the repository, under `D:\leonida-work`:

- `boot-firstrun-a-control` - arm A (control) of the pair: stderr log, `summary.txt`, startup trace, presenter
  captures in `firstrun\capture-a-control`; `firstrun\a2-control.json` is its analysis.
- `boot-firstrun-b-silentspeech` - arm B: the same files, captures in `firstrun\capture-b-silentspeech`;
  `firstrun\b-silentspeech.json`.
- The measurement boot before the slice (binary `5572DE508C481B59F2096B0644919D66191E0CAB25CCB28E7A8B757AF0302E84`) is
  `firstrun\a-control.json` with its startup trace kept in `boot-firstrun-a-control\boot.jsonl`.
- `firstrun\arm.ps1` runs one arm; `firstrun\analyze_arm.py` extracts the tables of this page (platform calls, loader
  state, timeline row, captures). No game bytes are read by either.
- The gate build after the pair (2026-11-25 11:24:05, SHA-256
  `8821CF5F6D64CC7B782F7048CEA2AED500F48C2979D7C23A23BD2A85E81319E7`) differs from the pair's binary only by a comment
  in the speech section, so the pair was not repeated on it.

Reproduce one arm of the pair (from the checkout root, after `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`):

```powershell
python -I tooling\boot_report.py --exe port\build\palmetto.exe --image <retail image> --game-dir <package> `
    --overlay <work>\overlay-b --out <work>\boot-firstrun-b --timeout 130 `
    --env PALMETTO_SHADERS=<work>\shaders\spirv --env PALMETTO_D3D12=vulkan --env PALMETTO_SKIP_INT3=1 `
    --env PALMETTO_SEED_GLOBAL=75DF268:D8 --env PALMETTO_PIPELINE_TIMELINE=10 --env PALMETTO_LOADER_HOOK=1 `
    --env PALMETTO_DS_STACK=1 --env PALMETTO_DS_ARRAYS=1 --env PALMETTO_USER_TRACE=1 `
    --env PALMETTO_CAPTURE=<work>\capture-b --env PALMETTO_CAPTURE_EVERY=2000 --env PALMETTO_CAPTURE_LIMIT=8 `
    --env PALMETTO_SILENT_SPEECH=1
```

Drop the last `--env` for arm A; hold `_setup/boot_lock.ps1` around each run.
