# The movie path: the Bink 2 library at the loading frontier

**Status: a measured negative, with a diagnostic slice.** The package's Bink 2 decoder is mapped and fully bound by
this runtime, its file and allocator imports are served, and the title opens the movie archive during startup - but
in a 130 s boot the title never enters the decoder: no entry point of the library runs, and probes at the movie
wrapper sites the public reference measured never fire. Nothing this slice can measure in the runtime blocks the
movie; the title has not started playback by the time its boot reaches the stationary loop. The slice adds the two
opt-in trace switches that make that state observable and testable.

The format and the offline decode are recorded elsewhere ([movies.md](movies.md), `port/test/bink_decode_test.md`
and its siblings). This page is only the runtime question: what the boot does with the library, and why nothing is
played.

## 1. Why this is the stage to measure

- The boot's own work ends at about +30.8 s. The loader's begin/end entries stop, the pipeline population freezes at
  10,241 slot-57 records and 51,786 graphics pipelines, and from +31.9 s the frame loop presents the same black frame
  for as long as it is left running (the 600 s boot of this working tree shows no new stage, and the A boot in
  section 5 repeats the frozen counters at 130 s).
- The public reference's boot goes further: it plays a launch movie before its first-run prompt, and its movies page
  measures that path in detail - the title opened the archive twice shortly after entering the loader, the package's
  decoder opened metadata at `game+0x442ebe6`, playback retained the handle at `game+0x442f056` and registered its
  mapped frame buffers at `game+0x442f9b8`, with a frame wrapper at `game+0x2ef0e0` and three linear planes through
  the slot-133 uploader ([reference movies page](https://github.com/ubiquitarianism/leonida/blob/273b3bdf9d6e736442ad584ad464df6e86ee928b/docs/movies.md)).
- The retail image imports 23 functions from that library and the package ships no other Bink decoder, so a started
  movie would have to run this library's code.
- The question this slice answers is therefore: **does our runtime fail a requirement of that path** - module
  mapping, import binding, allocator semantics, file resolution - **or does the title never get there?**

## 2. Measured shapes

### 2.1 The title's import of the library

The retail image has 23 imports from the package's Bink 2 library. The open entry is `BinkOpenWithOptions` (there is
no `BinkOpen` import), and the rest of the set is the GPU/async player the title uses rather than the CPU copy path:
`BinkRegisterFrameBuffers`, `BinkGetFrameBuffersInfo`, `BinkDoFrameAsyncMulti`, `BinkDoFrameAsyncWait`,
`BinkStartAsyncThread`, `BinkRequestStopAsyncThreadsMulti`, `BinkWaitStopAsyncThreadsMulti`, `BinkWait`,
`BinkShouldSkip`, `BinkNextFrame`, `BinkGoto`, `BinkPause`, `BinkSetWillLoop`, `BinkSetVolume`,
`BinkSetSoundSystem`, `BinkSetMemory`, `BinkSetOSFileCallbacks`, `BinkGetKeyFrame`, `BinkGetPlatformInfo`,
`BinkFreeGlobals`, `BinkClose`.

| Measurement | Result |
|---|---|
| direct call sites, `tooling/import_call_sites.py <retail image> --dll <the library>` | **0** for every import; the one IAT thunk in the image is `BinkSetSoundSystem` and no instruction calls it |
| how the title reaches the library | through function pointers it builds at run time, not through direct calls to the import thunks |

A static call count therefore cannot prove reach. The runtime measurement below uses the library's **own** import
calls - its allocator and file entry points - as the reach signal, and the title wrapper sites as a second one.

### 2.2 The library's imports through our shims

`palmetto --dry-run` on the package's library maps it (SizeOfImage `0x44000`, entry `+0x2ac1c`; the boot relocates it
because another bundled image holds its preferred base) with **8 shim / 52 host / 0 module / 0 unimplemented**
imports. The eight shim imports are the split that matters:

| Import | Binding | Role in the movie path |
|---|---|---|
| `xmem.dll!XMemAlloc`, `xmem.dll!XMemFree` | `port/shims/xmem.cpp` | the library's only allocator; it imports no CRT `malloc`/`free` at all |
| `api-ms-win-core-file-l1-2-0.dll!CreateFile2` | `port/shims/file_system.cpp` | opens the stream/archive through the console-path mapping and the write overlay |
| `api-ms-win-crt-runtime-l1-1-0.dll!_configure_narrow_argv`, `_cexit` | `port/shims/process.cpp` | CRT startup/teardown behind the library's `DllMain` |
| `api-ms-win-core-com-l1-1-0.dll!CoCreateInstance`, `CoTaskMemFree`, `PropVariantClear` | `port/shims/com_shim.cpp` | the audio device; reached only by a sound system, not by video decode |

`ReadFile`, `SetFilePointerEx`, `GetFileSizeEx` and `CloseHandle` are bound to the host, which is correct: they act on
the handle the file shim returns. In the boot trace the library is mapped at +0.86 s, before the image, and its
`DllMain(DLL_PROCESS_ATTACH)` returns 1; it has no TLS directory. **Module mapping, import binding and image init are
not the gap.**

### 2.3 Reach signal one: the allocator entry points

The library allocates only through `XMemAlloc`/`XMemFree`, so if any of its code runs past `DllMain` it must call the
xmem shim. New switch `PALMETTO_XMEM_CALLERS=1` records the first call of each allocator entry point from each distinct
caller as an `xmem_caller` event (the API and the caller as module+RVA), 256 records at most.

The existing `xmem_attributes` event cannot answer this: it dedupes on the attribute word, so a later caller using a
word already seen is invisible. Boot B below shows exactly that blind spot and the new switch closing it.

| Boot B, 130 s, `PALMETTO_XMEM_CALLERS=1` | XMemAlloc callers | XMemFree callers |
|---|---|---|
| measured | `game+0x1d6be2e` at +25.82 s; `game+0x5b52575` at +25.82 s | none |
| callers inside the Bink library's mapped range | **none** | **none** |

So no code of the library executes beyond its `DllMain` in the 130 s window. The second title caller is the one the
old attribute-word dedupe hid: it passed the same attribute word as the first.

### 2.4 Reach signal two: the library's file entry point

The library opens the stream through `CreateFile2`, which the file shim serves. New switch `PALMETTO_FILE_TRACE=1`
records every open of a guest path as a `file_open` event (the API, the path the title passed, `"result":"ok"` or the
Win32 error, and the caller as module+RVA), 1,024 records at most. The existing failures-only event
(`file_open_failed`) only answers "why did this file not open"; this one answers "which files does the title open at
all".

| Boot B, 130 s, `PALMETTO_FILE_TRACE=1` | Result |
|---|---|
| guest opens in the whole boot | **24**, all between +2.738 and +2.748 s, none later |
| what they are | the 12 archives the title mounts, **each opened twice**: once by the title (`CreateFileW`, caller `game+0xf43645`) and once through the runtime's DirectStorage file interface (`host:palmetto.exe+0x109204`, the shim's `CreateFileW` called by the storage layer's `OpenFile`) |
| the movie archive among them | **yes**, opened twice like the rest |
| opens whose caller is inside the Bink library | **none** - the decoder's own `CreateFile2` of a stream never happens |
| failed opens | **0** in either boot |

This is the same startup shape the reference reports as the title opening the archive twice about 1.2 s after entering
the loader. The archive mount/metadata step is present in our boot; a decoder open is not.

### 2.5 Reach signal three: probes at the reference's movie wrapper

The reference pins the title's movie start to three sites: metadata open at `game+0x442ebe6`, retained handle at
`game+0x442f056`, frame-buffer registration at `game+0x442f9b8`. The runtime's existing one-shot probe switch
`PALMETTO_POKE_LOG` writes an `int3` over one byte of each named RVA, logs the registers, restores the byte,
single-steps it and re-arms, so the guest's behaviour is unchanged.

| Boot C, 130 s, `PALMETTO_POKE_LOG=442EBE6,442F056,442F9B8` | Result |
|---|---|
| probes installed | 3 of 3, each over the byte `0xff` at its site |
| probes hit | **0** |
| verdict / exceptions | ALIVE 130.2 s, 0 exceptions |

This is the strongest single fact on the page: the title's own movie-start code, at the sites the reference
measured, **is not executed in the boot window**.

## 3. What we implemented

Two opt-in trace switches, both off by default, plus a shared helper; no change to any default path.

- `port/shims/xmem.cpp`: `PALMETTO_XMEM_CALLERS=1` adds `xmem_caller` events - the first call of `XMemAlloc`,
  `XMemFree`, `XMemAllocDefault` and `XMemFreeDefault` from each distinct caller, 256 records, keyed on
  (entry point, caller module+RVA). This is the switch that answers "does a separately mapped module that imports the
  allocator run at all", which the attribute-word event could not.
- `port/shims/file_system.cpp`: `PALMETTO_FILE_TRACE=1` adds `file_open` events for every open of a guest path
  (`CreateFileW` and `CreateFile2`; `CreateFileA` routes through the wide path), success and failure, 1,024 records,
  with the caller as module+RVA. The UTF-8 conversion of the event's path is now one helper shared with the existing
  `file_open_failed` event.

Why this is the smallest honest slice: the measurement shows the library's front end already works - mapped, bound,
file-shim served, archive mounted. The missing stage is a decision inside the title's own state machine, which no shim
of this runtime makes. Implementing decode, faking a player or opening streams for the title would be inventing the
stage; the switches are what makes the negative checkable now and the first real decoder call visible the moment a
later slice finds the trigger. Nothing is decoded by this slice, and the package's library remains the only decoder.

## 4. Cross-check against the reference

The reference is the public GPL-3.0 project `ubiquitarianism/leonida`. Its pages consulted for this slice, each at
the commit last touching it: `docs/movies.md` at `273b3bdf9` (Moriz82), `docs/xmem_exe_calls.md` at `a847ee646`
(Mathéo Delbarre), `runtime/test/bink_decode_test.md` at `004620ec5` (ItsNotRexed), `runtime/test/bink_abi_test.md`
at `3a4bac037` (mrp1no), `runtime/test/bink_play.md` and `runtime/test/bink_audio_test.md` at `11a1f88c0`
(bobcatchris); fork HEAD `ed94fbef9` (2026-11-25).

| Topic | Reference | This measurement | Why |
|---|---|---|---|
| library load | Its bink_decode page maps the library with the loader, binds `xmem.dll!XMemAlloc`/`XMemFree`, 0 unimplemented, and its movies page says the DLL "loads on Windows only after the missing `xmem.dll` imports are supplied; a proper port needs an allocator shim with the Xbox semantics, not necessarily `malloc`" | The boot's loader maps it at +0.86 s, `DllMain` returns 1, dry-run has 0 unimplemented and both xmem imports bound to this repository's shim | **Agree.** The one requirement the reference names for the library to load at all is already met, so it is not this boot's gap |
| startup opens | Archive opened twice about 1.2 s after entering the loader; decoder opens metadata at `game+0x442ebe6` | Movie archive opened twice at +2.74 s (title + storage layer); no decoder open and no decoder execution | Same mount shape; the decoder's own open is the part missing, and it comes with the movie start, not with the mount |
| movie start sites | `game+0x442ebe6`, `game+0x442f056`, `game+0x442f9b8`; frame wrapper `game+0x2ef0e0`; slot-133 uploader; `VS_Bink`/`PS_BinkNoAlpha` plane descriptors at table offsets 0x20/0x40/0x60; a capture after an A press matched an independently CPU-decoded frame at 0.9994 spatial RGB correlation | None of the three probed sites executes in 130 s; no decoder entry point runs | **Differ where the boot is, not in the shim.** The reference's boot reaches the movie stage; ours is still in the stationary shell loop the reference also had before its later work. Nothing this slice measured in the allocator, file or loader layers accounts for that difference |
| offline decode | 515/515 streams open and decode through the library; the 4K stinger plays; audio tracks decode | Not re-measured here; the repository's own offline pages (`movies.md`, `port/test/bink_*`) cover decode through the loader | Not duplicated: the decoder is not the gap, so the next useful measurement is about the trigger, not about decode |
| xmem title imports | The xmem page counts 12 xmem imports of the retail image and 7 unshimmed at the time | Dry-run on this tree reports 0 unimplemented for the retail image and its bundled DLLs | Later work in this tree closed that set; it is not on the movie path measured here |

The disagreement to record: the reference's movies page names the decoder's requirement as the xmem imports and
describes playback as a reached stage. In this tree the xmem requirement is met, yet playback is not reached. The
measurement wins over the inference that satisfying the imports would move this boot.

## 5. A/B measurement

Two 130 s boots of this repository under the boot lock, one at a time, through `tooling/boot_report.py --timeout 130`,
with the standard set `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`, plus `PALMETTO_PIPELINE_TIMELINE=30`, `PALMETTO_LOADER_HOOK=1` and presenter
captures (`PALMETTO_CAPTURE`, every 6,000 frames, 2 frames).

- **A** is `port/build/palmetto.exe` as built before this slice (10:23 local time; the working tree's other
  uncommitted changes are tool and test files, which do not build into `palmetto`).
- **B** is the same target rebuilt with this slice, run with `PALMETTO_XMEM_CALLERS=1` and `PALMETTO_FILE_TRACE=1`.
  `port/shims/xmem.cpp` and `port/shims/file_system.cpp` are the only runtime sources that changed between the two
  binaries; both switches only add trace events when `--trace` is open, and the default path is unchanged. A later
  uncommitted runtime edit (another slice's `port/shims/xgameruntime_offline.cpp`, 11:15 local time) postdates both
  boots and is in neither binary.
- **C** is a third boot for section 2.5 only: the B build plus the three `PALMETTO_POKE_LOG` probes, without captures.

| Measurement | A: before | B: slice, switches on | C: probes |
|---|---|---|---|
| verdict, wall time | ALIVE 130.1 s, 0 exceptions | ALIVE 130.2 s, 0 exceptions | ALIVE 130.2 s, 0 exceptions |
| trace events | 19,755 | 19,629 | 20,258 |
| `[d3d12-trace]` calls | 527,320 | 519,150 | 512,529 |
| pipeline timeline at +120 s | slot57 10,241 (0 refused); slot55 41,545; graphics 51,786; compute 95/95; submits 23,215; presents 11,607; console calls 280,585; threads 85 | slot57 10,241 (0 refused); slot55 41,545; graphics 51,786; compute 95/95; submits 23,048; presents 11,523; console calls 279,332; threads 85 | submits 22,728; presents 11,363; console calls 276,931; the rest identical |
| loader object counters | `1/1, 17/17, 1/1, 1/1` begin/end at both the 30 s and the 60 s summary - no change after the +30.8 s burst | `1/1, 18/18, 1/1, 1/1` begin/end at both summaries - identical state | not measured |
| allocator callers | attribute-word event: 1 XMemAlloc caller (`game+0x1d6be2e`), no Bink caller | caller event: 2 XMemAlloc callers (`game+0x1d6be2e`, `game+0x5b52575`), no Bink caller; XMemFree never called | same as B |
| guest opens | not visible (failures only) | 24, the 12 archives twice, +2.74 s, 0 failed, no Bink caller | same as B |
| movie wrapper probes | not measured | not measured | 3 of 3 installed, 0 hit |
| presenter captures | 2, presented frames 1 and 6,001, 2564x1421, both `mean rgb 0.00 0.00 0.00`, 0 non-black pixels | 2, the same two frames, both `mean rgb 0.00 0.00 0.00`, 0 non-black pixels | none |
| loader activity resumed after +30.8 s? | no | no | no |
| decoder called? | no | no | no |

The A/B counter differences (submits, presents, console calls, d3d12 calls) are the host frame-rate variance the
other pages record; the pipeline population, the loader state and the presented image are identical. **Verdict: the
slice changes no behaviour (by design - the switches are off on the default path), the boot does not advance, and the
measurement is the negative of sections 2.3-2.5.** That is the honest result: the runtime was not failing a Bink
requirement that this slice could repair.

## 6. Limits, and the next slice

- **The movie is not reached, and this page does not say what gates it.** It says what the gate is not: the loader
  does not drop the library, the library is fully bound, the allocator and file shims answer, the movie archive
  resolves and opens, and nothing fails silently on the file path.
- **The reach signals have blind spots.** The `xmem_caller` event sees only `XMemAlloc`/`XMemFree`; a decoder entry
  that fails before allocating is invisible to it (the wrapper probes cover that case for the three measured sites,
  not for the whole library). The `file_open` event sees `CreateFileW`/`CreateFile2` through the console-path shim;
  a stream opened by another API would not appear. Both events are bounded (256 and 1,024 records) and were nowhere
  near their bounds in either boot.
- **The 130 s window is not the limit.** The 600 s boot of this working tree shows nothing new after +30.8 s, so
  "boot longer" is not the falsifier.
- **The GPU half of the reference's path is untested.** Slot-133 uploader, the `VS_Bink`/`PS_BinkNoAlpha` descriptors
  and the plane-to-swapchain path need frames to exist; this slice produced none, so they are a separate slice if
  playback ever starts. The reference's frame-wrapper call shape at `game+0x2ef0e0` is likewise only recorded, not
  measured here.
- **No decode, audio or subtitle work.** The package's library remains the decoder; the repository's offline pages
  remain the decode measurement.
- **The next falsifier is a boot in which any of `game+0x442ebe6`, `game+0x442f056` or `game+0x442f9b8` executes.**
  The moment one does, the two switches on that same boot report whether the decoder follows: an `xmem_caller` event
  naming the library, and a `CreateFile2` open whose caller is inside it. Until then the next measurement is what
  makes the title start playback - the reference places the movie after its first-run prompt and before its story
  load, so the candidates are the first-run/UI state and the loading state machine that stops updating at +30.8 s.
  The cheapest local step is to find the caller of `game+0x442ebe6` (one level up from the probed site) and probe
  that: if the caller runs and the site does not, the gap is a branch between them the title code takes by itself,
  which is measurable from the title side without another shim.

## 7. Tests

| Test | Checks | What it covers |
|---|---|---|
| `xmem_test` (`port/test/xmem_test.cpp`) | **238**, all pass | the new `xmem_caller` group: no event while `PALMETTO_XMEM_CALLERS` is unset; one event per (entry point, caller) with the caller named; a repeated call from the same site adds nothing; both `XMemAlloc` and `XMemFree` are recorded; existing `xmem_attributes` behaviour unchanged |
| `file_system_test` (`port/test/file_system_test.cpp`) | **496**, all pass | the extended trace group: no `file_open` event while `PALMETTO_FILE_TRACE` is unset; a successful open records `"result":"ok"`; a failed open records its Win32 error and still writes the `file_open_failed` event; an open after the switch goes off again is not recorded |

Builds: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`; `_setup\build_tests.cmd` prints `TESTS_BUILT`. The two test
targets above are built directly (`xmem_test`, `file_system_test`) and run. Documentation checks under `tests/`
(`test_docs_*.py`) pass.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project `ubiquitarianism/leonida` (<https://github.com/ubiquitarianism/leonida>, license
<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), files `docs/movies.md` (commit `273b3bdf9d6e`,
Moriz82), `docs/xmem_exe_calls.md` (`a847ee646a6e`, Mathéo Delbarre), `runtime/test/bink_decode_test.md`
(`004620ec567b`, ItsNotRexed), `runtime/test/bink_abi_test.md` (`3a4bac0373c0`, mrp1no), `runtime/test/bink_play.md`
and `runtime/test/bink_audio_test.md` (`11a1f88c0726`, bobcatchris). The fork's HEAD when this page was written was
`ed94fbef9888` (2026-11-25). No code from that project was copied. The upstream project remains the copyright of its
contributors and its license continues to apply to its own tree.

The project's attribution rule ([upstream_port.md](upstream_port.md), section 3) also puts this sentence in
`NOTICE.md`. That file is outside this slice's write scope, so the lead adds the entry there; the sentence above is
its content.
