# The FSR 4 console-upscaler route: 29 of 29 programs translated, the measured route state, and the picture decision

**Status: measured; the translation gap is closed in the build tooling, the route's work path is complete on our side,
and the missing input is the title's own FSR call, which the measured window does not make.** The public peer claim is
that XCL runs the game's own AMD FidelityFX FSR 4 console DLL on the PC GPU: its 29 pipelines are translated from the
console driver's own `FSPX` container and a frame's passes are recorded and run ([cleanroom_peers.md](cleanroom_peers.md),
"Upscaling and FSR" and "slice 5"). This tree's parity matrix recorded the gap as **13 of 29 upscaler programs
translate, no route**. That number is stale. Measured at HEAD, the build's own FSPX step translates **29 of 29** and
every module passes `spirv-val`; the shared shader corpus carries all 29 modules; the 130 s boot requests **0 of the
29** and loads **no package DLL**; and the console upscaler task's route through our loader and console device is
complete on the work path — a test arm drives the package's own DLL through it, creates its 29 pipelines and records its
20 passes; the one measured route defect left is its context teardown. The stretch stand-in stays the picture fallback
and is not touched by this slice.

Nothing here is game data: program counts, indices, keys, RVAs and call counts only, and the keys are this project's
own identifiers.

## 1. Why this slice

[cleanroom_peers.md](cleanroom_peers.md) ranks this as slice 5: the stand-in is done, and "the parity step is running
the title's own upscaler where its pipelines can be made". Two peer sentences are the hypothesis:

* **XCL**: "the game's AMD FSR 4 console DLL: context, the 29 pipelines ... translated, a frame's 20 passes recorded and
  run on the GPU". Its mechanism (reference only, not copied) is a FidelityFX API shim over the game's own upscaler
  programs in the console driver's container; the console-only pipeline deserialization is the blocker the peer handles.
* **PercyScarlett**: "the Series S world was black because the game upscales with the console build of AMD FSR 4 ...
  the upscale is now a stretch of the colour image". Our stand-in already covers the substitute case
  ([upscale_standin.md](upscale_standin.md)).

So this slice had two questions to answer before writing anything: **what is the exact translation gap at HEAD**, and
**is a route implementable or is the title's call itself unreached**.

## 2. The measured gap at HEAD

Everything below was measured on the 1.0.2.1 package's `amd_fidelityfx_upscaler_xbox.dll` and its extracted programs,
with the repository's own tools. The work folder is outside the repository. The measured step is the build's own FSPX
sub-step, `build_port.fspx_shaders()`, which `python -I build_port.py --stages shaders` runs after the game's shaders;
the numbers here come from a driver outside the repository that calls that step alone (no game shaders), so they are
the production code path, not a manual reproduction.

### 2.1 The programs and their translation

| measurement | result |
|---|---|
| programs the DLL carries (`tooling/fspx_unpack.py`) | **29** (the `fspx/` container set of [fspx.md](fspx.md)) |
| raw `llvm-mc` listings translated as they are | **22 of 29**; the 7 refusals are all `fall-through into undecodable words`, the console's wider scalar register file (112 SGPRs) against LLVM's 106 — `docs/boot_frontier.md` measured this class |
| listings repaired by `tooling/widen_smem_decode.py`, then translated | **29 of 29** translated, **29 of 29** pass `spirv-val --target-env vulkan1.2` |
| the repair's own counts on this set | **237 words rebuilt, 223 swallowed instructions re-emitted**, 0 listing repair failures |
| the build's own coverage line, fresh run | `shaders: FidelityFX upscaler coverage: 29 of 29 translated, 0 untranslated` |
| translator refusals recorded (`--refusals`) | **0** |

So the two old refusal classes in the parity matrix are **both fixed in the tree**: the register-file words are
reconstructed from the program bytes by `widen_smem_decode.py` (never guessed: its field layout is re-verified against
`llvm-mc --show-encoding` at start-up), and the console's packed-dot form (`v_dot4_i32_i8` VOP3P with `op_sel` 2 /
`op_sel_hi` 5) is rebuilt by the translator from the blob (`docs/fspx.md`, "What llvm-mc still rejects"). Neither needed
a code change in this slice; this slice measured them and made the result reproducible and named (section 3).

### 2.2 The published corpus

The shared shader corpus the runtime reads (`<work>/shaders/spirv/fspx/`) carries the 29 modules and their index
entries:

| measurement | result |
|---|---|
| FSPX entries in the corpus index | **29 of 29**, each with `fspx_code_sha256` and `fspx_state_sha256` |
| modules that pass `spirv-val` | **29 of 29** |
| modules byte-identical to a fresh HEAD translation | **18 of 29** |
| modules that differ, and why | **11 of 29** — they are the 11 programs with a loop, and the difference is the loop guard landed in [translator_fixes.md](translator_fixes.md): translating module 00 with `--loop-limit 0` reproduces the corpus bytes exactly (SHA-256 equal), so the corpus entries predate the guard and nothing else differs |

The 11 are the same program indices in both measurements, i.e. the corpus is a complete 29-of-29 set whose 11
loop-bearing modules were translated before the guard. Their runtime keys are unchanged, so the corpus is correct for
lookup; a fresh FSPX step replaces those 11 module *bytes* at the same keys (section 3.3).

### 2.3 What the runtime asks for, and whether the DLL runs

The boot is the other half of the measurement (section 5): in 130 s the title requests **0 of the 29** FidelityFX
programs and **no package DLL is loaded**.

| measurement | result |
|---|---|
| `no translated compute shader for FSPX kind-6 and kind-0` lines in the boot | **32 lines / 32 distinct keys** in this slice's arm (an earlier arm of the same tree logged 54 lines / 52 keys); **0 of the keys is any of the 29 upscaler programs** — they are the title's own pipeline-library compute programs ([compute_slot67.md](compute_slot67.md)) |
| FidelityFX programs requested | **0 of 29** |
| package DLLs loaded by the title (`[rt] loader: <dll> mapped from game_dir`) | **none**: the only loader lines are the game image's TLS repair and `xgameruntime.dll is served by the shims`, so the upscaler DLL, the loader DLL and the other bundled DLLs are not requested in the window |
| upscale pass in the frame | the title's **own** graphics pipeline (`fspx/lib_ps_00822`, 6 image samples), created through slot 57 — measured in [upscale_standin.md](upscale_standin.md) |
| FSR pipelines created | **0** (nothing requests them) |

That is consistent with the earlier measurement: the tree has no FSR *call* to route in the window, so there is no
runtime behaviour to change here. The route question is therefore about whether our layer *could* serve it.

### 2.4 Why the matrix said 13 of 29

The 13-of-29 row was measured before the two coverage fixes landed: the packed-dot form the translator rebuilds and the
register-file repair (`widen_smem_decode.py`). The loop guard is a third change since, but it changes module bytes, not
the count (section 2.2). The intermediate pages record the same history:
[fspx.md](fspx.md) "Translation status" measures 20 of 29 after the SHADER-813 fall-through rule and 26 before it;
[boot_frontier.md](boot_frontier.md) measures 19 of 29 after the first non-invasive repair. The number this page
replaces is **29 of 29 with the build's own repair step, 22 of 29 without it**, and the stale matrix entry is recorded
as such for the next documentation pass.

## 3. What this slice changed

### 3.1 A per-program FSPX coverage report (`build_port.py`)

The FSPX step required the whole set but only reported a count in its failure message. It now records every program's
status the way the pipeline library's step does ([compute_slot67.md](compute_slot67.md) section 3.2):

* `fspx_coverage(unpacked, spirv)` writes `<work>/shaders/fspx/coverage.json` — every program's `translated` /
  `refused` (with the translator's reason) / `untranslated` status and a one-line summary — and logs
  `shaders: FidelityFX upscaler coverage: T of N translated, U untranslated (reason xN)` on **every** run, including a
  reused one;
* the translator call now passes `--refusals <spirv>/refusals.json`, so a refused program's reason is recorded instead
  of only printed; the shared reader is `refusal_reasons()`, which the pipeline library's report uses too;
* the whole-set failure message points at the coverage file, so a regression names the program and the class before the
  stage stops. The refusal classes this names are the ones in `docs/boot_frontier.md` and `docs/fspx.md`.

### 3.2 Tests

`tests/test_fspx_shaders_stage.py` (27 tests, all pass) now also pins:

* the translator call carries `--refusals` with the staged path;
* `fspx_coverage()` names each program's status, counts the reasons, writes `coverage.json` beside the unpack, and
  treats an index entry without its module as untranslated (the runtime looks the module up beside the index);
* a step whose translator refuses one program fails the whole set **and** leaves `coverage.json` naming that program
  and its reason (zero game data: the fixtures are synthetic names and four-byte modules).

No test needs a GPU and none reads the package.

### 3.3 The corpus handoff (the count and the command)

There is **nothing new to publish**: the shared corpus already has all 29 programs, because the FSPX step has run on
this work folder. What a current tree would change is the 11 pre-guard modules (section 2.2) — same keys, guarded
bytes. The lead's additive action, when wanted, is:

```
python -I build_port.py --stages shaders
```

which reruns the FSPX sub-step when `<work>/shaders/spirv/fspx/fspx_build.json` no longer matches its inputs (the
translator's digest changed with the loop guard) and prints `FidelityFX upscaler coverage: 29 of 29 translated`. The
count to expect is **29 of 29 translated, 0 refused**; the byte changes are **11 modules**, not new entries. If the
publish is to stay strictly additive, landing the guarded modules is a replacement of those 11 and should be a
deliberate corpus swap, not an append.

## 4. The route: what the title expects and what exists

### 4.1 The console DLL and our loader

The package's upscaler is a PE32+ console DLL with five public FidelityFX entry points (`ffxConfigure`, `ffxQuery`,
`ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`) and console imports only: `xmem.dll!XMemAlloc`,
`XMemFree`, `XMemGetWorkingSetStatistics` and `d3d12_xs.dll!D3D12CreateRootSignatureDeserializer`
([runtime_imports.md](runtime_imports.md)). Every one binds to our shims; the loader maps the image from the package
directory, runs TLS and `DllMain`, and resolves the exports ([d3d12_vulkan.md](d3d12_vulkan.md) appendix A.3,
`port/shims/library_loader.cpp`, `port/test/fsr_dispatch_test.cpp`).

Its compute path is the console pipeline call we already serve: the DLL copies the container's kind-6 program and
calls device slot 67 (`DeserializeComputePipelineStateX`) with the four pointer/size pairs; the proxy builds one FSPX
payload and the Vulkan device resolves it by `fspx_code_sha256` and `fspx_state_sha256` in the corpus — the same
lookup a slot-62 `ACCC` container gets ([fspx.md](fspx.md), "Build and run-time lookup"). With all 29 programs
translated, **nothing in that path is missing on our side**.

### 4.2 The measured route test

The optional, package-gated `fsr_dispatch_test` drives the DLL through that path on the Vulkan-backed Xbox-ABI device
with the translated corpus. This is the arm that answers the only question our own code can answer today: once the
programs are translated, does the package's own upscaler create its pipelines and record its passes?

**Yes, measured.** The DLL maps with 0 unimplemented imports, `Loader::initialise` (TLS + `DllMain`) succeeds,
`ffxConfigure` and the CPU-only `ffxQuery` calls answer, and `ffxCreateContext` succeeds on the Vulkan device (the
R9G9B9E5 UAV texture is served by the mutable `R32_UINT` path already in the device). Then, on that context,
`ffxDispatch` returns **OK** and the call flow is the peer's:

| route measurement | result |
|---|---|
| device slot 67 (`DeserializeComputePipelineStateX`) calls | **29** — one per upscaler program; **0** refusals, and **0** slot-62 fallbacks (the fallback path the DLL takes when slot 67 fails was never entered) |
| device slot 16 (`CreateRootSignature`) calls | 29 (one per pipeline) |
| `ffxDispatch` result | **OK (0)** |
| recorded compute passes (console list slot 14 `Dispatch`) | **20**, with `SetPipelineState` x20, `SetComputeRootSignature` x20, `SetComputeRootDescriptorTable` x40, `SetComputeRootConstantBufferView` x20, `ResourceBarrier` x13, `SetDescriptorHeaps` x22, `CopyBufferRegion` x2 and two slot-85 console-only calls |
| computed list slots the DLL writes | measured slots 14, 15, 25, 26, 32, 33, 35, 41, 53 (the console list map of [d3d12_command_objects.md](d3d12_command_objects.md) §3.5a) |
| translated-shader lookup during the dispatch | `28449 translated shaders` loaded; every one of the 29 FSPX keys resolved |

That is XCL's "context, the 29 pipelines, a frame's 20 passes recorded" measured on this tree. The one failure in the
test is **not** the route: `ffxDestroyContext` faults with `0xc0000005` reading `0xfffffffffffffff8` after the
successful dispatch (the test's only failing check; `fsr_dispatch_test` exits 1 for that check alone). The passes are
recorded into the test's logging list, which it never executes — executing them is the next slice, not this one.

### 4.3 The title's own call is absent, and why that is not a failure of this slice

The measured boot requests none of the 29 programs and does not load the DLL (section 2.3). The frame's upscale pass is
the title's own graphics pipeline ([upscale_standin.md](upscale_standin.md) section 3), so the console FSR DLL is not
what draws the measured frame. The title's FSR call belongs to the content/picture stage that our tree has not
reached: the same gate every peer had to cross before their upscaler ran. **Implementing a "route" now would be
plumbing for a call that does not happen.**

### 4.4 Decision

1. **Keep the stretch stand-in** as the picture fallback (`PALMETTO_UPSCALE_STANDIN`, off by default, unchanged). It is
   the documented substitute case and this slice does not touch it.
2. **Do not land a new route switch.** The loader already routes a title `LoadLibrary` of the package DLL
   (`library_loader.cpp` → `rt::Loader`, logged `mapped from game_dir (run-time load)`), and the device already serves
   the DLL's slot 67/62 shapes with the translated programs. There is no call to switch today, and adding a path that
   no boot exercises would be unmeasured code.
3. **The next slice is the call, not the container**: when the title first loads the DLL, run one boot with the loader
   lines and a slot-67 call count and prove (a) the DLL is mapped and initialised, (b) its `ffxCreateContext` reaches
   the device, (c) its pipeline creations resolve from the 29-of-29 corpus, and (d) its dispatch records. The
   falsifier that would re-open this page: the DLL is loaded and its pipeline call still fails with
   `no translated compute shader for FSPX kind-6 and kind-0` for one of the 29 keys (that would make the corpus or the
   key pairing wrong, not the coverage), or the DLL's dispatch faults after the pipelines are created (the next gap is
   in the console list/backend shapes, not in translation).
4. **The route's own named next gap is teardown**: `ffxDestroyContext` faults after the successful dispatch (section
   4.2). That is a lifetime/destroy-path defect in the layer's context teardown, independent of coverage; it is the
   first item for the next FSR slice and should be measured on its own arm.

## 5. Boot and route measurements

### 5.1 The FSPX build step (fresh work folder)

One run of `build_port.fspx_shaders()` on the retail package into a fresh work folder outside the repository:

| measurement | result |
|---|---|
| programs unpacked | 29 |
| repair | 237 words rebuilt, 223 swallowed instructions re-emitted |
| translated / `spirv-val` clean | 29 / 29 |
| coverage line | `FidelityFX upscaler coverage: 29 of 29 translated, 0 untranslated` |
| translator refusals | 0 |

### 5.2 The route arm

One run of `fsr_dispatch_test` under the shared boot lock, on a frozen binary built from this tree's HEAD
(`palmetto.exe` snapshot `7F49FC…0016E`, tree HEAD `96ccb8e` plus the other slices' in-flight work), with
`PALMETTO_GAME_DIR` = the package, `PALMETTO_FSR_DEVICE=vulkan`, `PALMETTO_FSR_NO_STANDIN=1` and
`PALMETTO_SHADERS` = the work corpus. The device is the RTX 4090's Vulkan backend through the Xbox-ABI proxy.

| measurement | result |
|---|---|
| map / imports | `mapped amd_fidelityfx_upscaler_xbox.dll at 0x180000000, size 802816, entry +0x26780`; shim=4 host=82 unimplemented=0 |
| TLS / `DllMain` | loader TLS index 6; `Loader::initialise` succeeds |
| FidelityFX CPU queries | `ffxConfigure(GLOBALDEBUG)` OK; PERFORMANCE ratio 2.0; 3840x2160 QUALITY renders at 2560x1440 |
| `ffxCreateContext` (backend 0x1000002, version 4.1.1) | **OK (0)**; slots 14x5, 15x26, 18x10, 19x16, 27x4, 38x4, 76x10 |
| `ffxDispatch` | **OK (0)**; slots 15x141, 16x29, 18x4, 19x2, 24x47, 27x1, 38x4, **67x29** |
| console command list | 20 `Dispatch`, 20 `SetPipelineState`, 20 `SetComputeRootSignature`, 40 `SetComputeRootDescriptorTable`, 20 `SetComputeRootConstantBufferView`, 13 `ResourceBarrier`, 22 `SetDescriptorHeaps`, 2 `CopyBufferRegion`, 2 slot-85 calls |
| refusals in the dispatch | **0** `no translated compute shader for FSPX kind-6 and kind-0` lines; slot 62 (the DLL's fallback) **0** calls |
| test result | `ffxDispatch returns, or stops at a documented limit` PASS; `ffxDestroyContext succeeds` **FAIL** — the destroy path faults (`0xc0000005`, reading `0xfffffffffffffff8`) after the successful dispatch; `fsr_dispatch_test: FAILED (1 failure)` is that check alone |
| log | `D:\leonida-work\fsrroute\run\fsr_dispatch_vulkan.log` |

The teardown fault is the route's own next slice (section 4.4): the upscaler's *work* path is complete on our device,
its *destruction* path is not. Both are outside the translation question this page measured, and neither changes the
picture decision.

### 5.3 The boot arm

One 130 s boot of the frozen `palmetto-fsrroute.exe` under the shared boot lock, standard environment
(`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`) plus the
presenter-capture switch (`PALMETTO_CAPTURE`, every 500 presented frames, 4 at most).

| measurement | result |
|---|---|
| boot | **ALIVE 130.3 s** (killed by the 130 s timeout), **0 exceptions** in the JSONL trace, 141,174 `[d3d12-trace]` calls (max #143,687) |
| slot-67/62 `no translated compute shader for FSPX kind-6 and kind-0` | **32 lines / 32 distinct keys**, and **0 of the 32 is one of the 29 upscaler programs** — all are the title's own pipeline-library compute programs ([compute_slot67.md](compute_slot67.md)) |
| FidelityFX programs requested / created | **0 / 0** |
| package DLLs loaded by the title | **none**: the only loader lines are the game image's TLS repair and `xgameruntime.dll is served by the shims`; no `mapped from game_dir` line at all |
| translated shaders the device loaded | 28,449 from the corpus (the 29 FSPX entries are among them) |
| frame population at +120 s | slot 57: 10,241 calls, 10,241 converted, 0 refused; graphics pipelines 51,786; compute 267 calls / 225 created; **4,381 submits / 2,190 presents**; console calls 140,265; 87 threads |
| presenter captures (4: frames 1, 501, 1,001, 1,501, 2564x1421) | every one `mean 0 0 0, non-black 0/3,643,444, peak 0` — the frame is black by design ([colour_path.md](colour_path.md)) |
| arm meta | frozen `palmetto-fsrroute.exe` SHA-256 `7F49FC48815A602DB405C4F87402E3EA57CCE6E4A89CA43E32E7DF3A9A00016E` + `palmetto_tls_host.dll`, tree HEAD `96ccb8e` plus the other slices' in-flight work; logs under `D:\leonida-work\fsrroute\run\boot\` |

Reading it: the corpus is complete and loaded (28,449 entries, the 29 FSPX keys included), the boot is otherwise the
known live run, and **a complete upscaler translation changes no boot number**, because nothing requests an FSPX
pipeline and the DLL is never loaded. The 32 refused library keys are a different family (the title's own precompiled
compute pipelines) and were the earlier slices' subject; they are not this slice's gap.

### 5.4 Why the boot is a coverage negative and not a route regression

The boot cannot exercise the route: nothing in it loads the DLL or requests one of the 29 programs. That is the same
state the earlier pages measured ([upscale_standin.md](upscale_standin.md) section 2.2), and this slice's arm exists to
state it at HEAD with the 29-of-29 corpus in place: a fully translated upscaler set changes no boot number by itself,
because the frame's upscale pass is the title's own graphics pipeline.

## 6. Limits

* **One boot, one machine.** The boot numbers are one 130 s run of the current tree's frozen binary; the corpus is the
  only input this slice cares about, and no runtime source changed here.
* **The route test is a test, not the title.** `fsr_dispatch_test` drives the DLL through its public API on our Vulkan
  device; it records the DLL's own dispatch but does not present a frame and does not prove the title would choose the
  same configuration. That is the next slice (section 4.4).
* **The dispatch is recorded, not executed.** The passes go into the test's logging command list, which is never
  executed on the GPU. "The 20 passes recorded" is exactly that; running them is the next arm.
* **`ffxDestroyContext` faults** (`0xc0000005` reading `0xfffffffffffffff8`) after the successful dispatch, which is
  the route test's only failing check. The work path is measured; the teardown path is not fixed here and is named as
  the route's next gap.
* **No FSR picture.** The 29 translated pipelines are not dispatched by the title in the measured window, so this page
  makes no claim about a picture from them; the black frame is the colour path's measured negative
  ([colour_path.md](colour_path.md)).
* **The corpus' 11 pre-guard modules are correct for lookup** but are not the current translator's output; the
  handoff in section 3.3 makes that deliberate.
* **The boot's 32 refused compute keys are the title's library programs**, not the FSPX set; they belong to the
  earlier compute-coverage slices and are not a regression of this page.
* **The FSR DLL's PC-side texture workaround is separate.** The R9G9B9E5 UAV texture path is implemented in the
  device ([d3d12_vulkan.md](d3d12_vulkan.md) appendix A.3) and was not re-measured here.

## 7. Tests, gates and evidence

* **Tests**: `tests/test_fspx_shaders_stage.py` — 27 tests, OK (the coverage checks and the step's `--refusals`
  argument are new). `tests/test_build_port_library_coverage.py` — 7 tests, OK (the shared refusal reader is its path
  too). `tests/test_bvh_shaders_stage.py` (13), `tests/test_fspx_unpack.py` (34) and
  `tests/test_shader_build_fallbacks.py` (5) also pass, because they exercise neighbouring build steps.
* **Gates**: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, the tests
  above pass, and the documentation checks pass for this page.
* **Evidence** (outside the repository, `D:\leonida-work\fsrroute\`): the frozen binary (`bin\palmetto-fsrroute.exe`,
  SHA-256 `7F49FC48815A602DB405C4F87402E3EA57CCE6E4A89CA43E32E7DF3A9A00016E`, with `palmetto_tls_host.dll`), the route
  arm's `run\fsr_dispatch_vulkan.log`, the boot arm's `run\boot\{stderr.log,boot.jsonl,summary.txt}` and
  `run\capture\frame_*.ppm`, the fresh work folders' `shaders\fspx\coverage.json` and
  `shaders\spirv\fspx\refusals.json`, and the guard-off comparison that pins the 11-module difference.
* **No game data** in the repository, this page or the report: counts, program indices, keys, RVAs and call counts
  only.

## 8. Attribution

The behaviour was cross-checked against the public projects the parity programme cites. **XCL**
(<https://github.com/xcl-dev/xcl>, GPL-2.0-only) publishes the client that runs the game's own FSR 4 console DLL on the
PC GPU with its translated FSPX pipelines; **PercyScarlett** (<https://github.com/rpatty-dev/PercyScarlett>,
GPL-2.0-only) publishes the black-world/stretch account. The reference GPL-3.0 project **ubiquitarianism/leonida**
(<https://github.com/ubiquitarianism/leonida>) was consulted for the container/pipeline mechanism through its
`docs/fspx.md` and `docs/shader_library.md` at HEAD `ed94fbef9888`, as this project's own pages already record. No
code, comment text, identifier or table from any of them was copied; every implementation here is this project's own,
measured on this project's own package, and the projects remain the copyright of their contributors under their own
licences (the attribution rule is [upstream_port.md](upstream_port.md) section 3, and the `NOTICE.md` entry for the
referenced project is the lead's to add).

## 9. C-04: Series-S kind-7 consumer readiness

**Status: readiness only; no title-level kind-7 consumer has been observed.** This update narrows the earlier default-boot
negative in sections 2.3 and 4.3-4.4: the default route is still the title-owned six-sample kind-6 compose described in
[upscale_standin.md](upscale_standin.md), but a Series-S answer (`PALMETTO_CONSOLE_TYPE=6`) is now measured to select the
kind-7 branch and create its output shape. What remains absent is the FFX dispatch and any observation of that output
being consumed by a compose draw. This is not a translation failure or evidence that kind 7 skips the draw.

### 9.1 Measured branch and the consumer gap

| case | measurement | consequence |
|---|---|---|
| default console answer | the title selects kind 6; its existing compose draw has six pixel-stage image samples and reads the measured 3840x2160 colour image into the 3840x2160 target ([upscale_standin.md](upscale_standin.md) sections 2.1 and 4.1) | this is the current picture path and remains byte-identical when the readiness switch is unset |
| Series-S answer | the registry read maps type 6, the selector returns 7, and the title selects its kind-7 object ([console_type_branches.md](console_type_branches.md) section 2.4; [decomp_renderer.md](decomp_renderer.md) section 11.4) | the console branch is real; the default kind-6 draw is not evidence for its consumer |
| kind-7-created shape | DXGI format 67 (`R9G9B9E5_SHAREDEXP`), 1920x1080x1, one mip/layer/sample, RT+UAV flags; the branch also places 1280x720 colour/depth resources in formats 57/62 ([decomp_renderer.md](decomp_renderer.md) sections 11.4 and 12.1) | the 1920x1080 image is the candidate kind-7 output source; the 1280x720 colour/depth shapes are candidate dispatch inputs, not proof of which resources FFX consumed |
| format backing | the created shape is served by the measured mutable `R32_UINT` backing on the Series-S arm, while its render-target view remains refused by name ([decomp_renderer.md](decomp_renderer.md) sections 12.2 and 13.3) | resource creation alone does not prove that a writable FFX output or a later sampled view was used |
| FFX request / dispatch | the shape-served Series-S arms record **0 of 29** FSPX programs; the FFX DLL maps, but the first upscaler pipeline and `ffxDispatch` are not reached ([decomp_renderer.md](decomp_renderer.md) sections 12.3-12.5 and 13.1-13.5) | no output contents, resource identity at the consumer, filter, numeric transform or compose rectangle has been observed |

The shape-served arms in sections 12-13 (`fsr1`, `fsr2`, `fsr7`, `df1`, `df2`, `fsr9`) stop in machine stage 2 before
the FFX dispatch; the 0/29 count is unchanged. The older `s61`/`s62` arms reached stage 3 while the shape itself was
still refused at creation, so they are not consumer evidence either. Several later arms reported device loss, including
a control arm without the Series-S shape; the loss cause is unassigned and the host has no stable repeatable FFX window.
No result supports forcing a runtime route or treating a same-format image as kind 7.

### 9.2 Readiness planner and compose contract

`port/src/upscale_kind7.h` is a GPU-free, fail-closed planner kept separate from `upscale_standin::plan`. Its candidate
source is the created 1920x1080 DXGI-67 shape, not the 1280x720 candidate colour/depth inputs. Its candidate target is
the measured default kind-6 3840x2160 compose target; that geometry is a testable mapping hypothesis only, not an
observed kind-7 destination. The separate `Provenance::series_s_kind7_output` input must be explicitly supplied; format,
extent and layout alone never identify an FFX output.

The only switch spelling accepted by `PALMETTO_UPSCALE_KIND7_CONSUMER` is the exact token `measured` (case-insensitive,
with surrounding space, tab, CR or LF trimmed). The plan still refuses unless the caller also supplies a resolved, sampled
E5B9G9R9 view, the measured source/target formats and extents, an allowed sampled-image layout, both explicitly recorded
whole-rect bounds, an observed producer-to-compose link, point filtering and passthrough numeric semantics. Each failure returns a named refusal; the measured
current input stops at `consumer_not_observed`. `map_point_texel` exposes the candidate centre-to-texel map for tests,
but callers must require `Plan::compose` before recording any draw.

`port/src/vulkan_present_compose.h` adds only a Vulkan adapter: it maps the candidate E5B9G9R9 and RGBA16F formats,
checks `VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT`, and maps the explicit source layout to the present composer's accepted
SHADER_READ_ONLY_OPTIMAL/GENERAL states before passing evidence to the pure planner. It neither assigns kind-7 provenance
nor changes `PresentComposer::record`/`present_planes`. The generic present compositor already accepts sampled single-plane
colour images with those tracked layouts, point or linear fetch, and an explicit
SDR/HDR colour transform; none of those details has been observed for the title's FFX output. No runtime caller currently
applies the planner, so the opt-in switch cannot alter a title frame before the missing link is measured. With the switch
off, the existing command recording remains byte-for-byte untouched.

### 9.3 Tests, arms and limits

- `port/test/frame_dump_test.cpp` covers the switch token, named refusals, explicit provenance, VkFormat mapping and
  sampled-image support, measured extents, full-rectangle checks, the refusal when the consumer link is absent, and the
  candidate point-map endpoints/2x footprint. These are GPU-free checks; a synthetic complete input exercises the pure
  planner only and is not a claim that the title has made that observation.
- `port/test/vk_presenter_sync_test.cpp` adds a feature-gated synthetic E5 image through the generic PresentX compositor
  and capture. It verifies that this host can sample and compose a controlled packed-float image when the format
  advertises sampled/transfer-destination support. It does not use the title's resource, the FFX dispatch or an accepted
  kind-7 plan; the test's candidate remains refused for `consumer_not_observed`.
- No game boot or package-DLL arm was run for this readiness slice. The existing shape-served Series-S arms already end
  before FFX and the recent host arms include repeated device losses; the optional arm is deferred until the host is
  stable. The stop rule is two device losses.

**Next falsifier:** one stable light Series-S arm must survive the loading window, reach the first FFX pipeline creation
and `ffxDispatch`, then show the exact output resource being sampled by a subsequent compose draw. Record the output and
consumer resource identity, Vulkan view format/features/layout, source and destination rectangles, filter, and numeric
conversion; a nonzero FSPX count alone is not enough. If two device losses occur, stop the arm. Until the same-resource
consumer is observed, keep the route at readiness and retain the named refusal.

### 9.4 Peer credit and NOTICE handoff

The readiness decision is based on this project's own route and stage measurements, cross-checked against **XCL**
(GPL-2.0-only, reference clone `623d02e`) for the public claim that the game's console FSR DLL's 29 pipelines and 20
passes can run, and **PercyScarlett** (GPL-2.0-only, reference clone `e44c465`) for the stretch-of-colour fallback. These
are mechanism references only; no code, comments, identifiers or tables were copied. The existing attribution in section
8 and the clean-room rule in [upstream_port.md](upstream_port.md) remain in force.

**NOTICE row:** the kind-7 consumer readiness planner and synthetic compose seam were implemented from this project's own
measurements and cross-checked against XCL (`623d02e`) and PercyScarlett (`e44c465`), both GPL-2.0-only; no peer code was
copied, and each project's copyright and licence continue to apply to its own tree. The row is recorded under the
implemented-peer-port credits in `NOTICE.md`.

## 10. P-28 follow-up: Type-6 arms reached the branch, but not a kind-7 consumer

Two `-Light` 90 s arms used the same integrated binary (SHA-256
`1DD3A3E84789BDB06EDBB0D078A724F940FC1E38B0C05CF4A3A7CB1F077A28E6`), the standard content/state/capture environment,
`PALMETTO_CONSOLE_TYPE=6`, `PALMETTO_TITLE_CLASS0A_SIZE=1`, and the template's `PALMETTO_SLOT67_PROGRAM_LOG=1`. No
build, relink, code change or git write was made.

- **`p28-kind7-20261127-1011`**, 10:11:14.483–10:12:45.286 (-03), wall 90.2 s. The title's registry reads were
  answered `0x60` (type 6); the class-0xa mitigation reported three redirected call sites and row stride `width*0x10`.
  At t=84/88 s the sampled words were `init=2`, `loading=2`, `session=1`, machine `current=2 next=2`. The 1920x1080x1
  format-67 shape was placed on the mutable `R32_UINT` backing; its render-target view was refused by name. The FSR DLL
  mapped from `game_dir`, but the post-map slot-67 trace showed no FSPX request (0 of 29 upscaler programs in this
  observed window), no `ffxDispatch`-labelled event, and no resource-identity trace linking that shape to a later compose
  draw. At `t=79518.508`, stderr recorded `[rt] replay: the Vulkan device was lost`, followed by
  `ExecuteCommandLists` failure and `present failed (VkResult -4)`; the process reported access violation `0xC0000005`
  at `game+0x86f9df`, read `0x6`. Twelve captures (1904x1041) all had mean RGB `0.00 0.00 0.00`.
- **`p28-kind7-retry-20261127-1014`**, 10:14:39.257–10:16:10.232 (-03), wall 90.3 s, `ALIVE` at timeout, zero
  exceptions and no device-loss line. The type-6 override and mitigation were applied, but the last sample at t=88 s
  remained `init=2`, `loading=1`, `session=0`, machine `current=2 next=2`; no format-67 shape or FSR DLL mapping appeared.
  Twelve captures (1904x1041) again had mean RGB `0.00 0.00 0.00`.

The first arm confirms that the type-6 branch can reach the candidate shape and load the DLL, but it does **not** establish
an FFX dispatch, an upscaler output, or a compose consumer. The slot-67 trace showed no post-load program request; the
available trace does not directly instrument the `ffxDispatch` API entry or identify a resource through a later draw.
Thus this remains readiness-only, not a translation failure and not evidence that kind 7 skips composition. The one retry
followed one device loss and was loss-free; the two-consecutive-loss rule was not reached, but the task's one-retry/three-arm
budget is exhausted. Boot logs, metadata and JSONL traces remain outside the repository under
two outside-repo boot directories named for the kind-7 arms (the `-1011` run and its `-1014` retry); capture PPMs remain in the corresponding outside-repo
capture folders and were not copied here.

**Next falsifier:** only after a stable host and a separately authorized arm, observe the first post-load FFX pipeline
request/`ffxDispatch`, match the requested program against the 29-program corpus, then identify that exact output resource
in a subsequent compose draw (view format/features/layout, source/destination rectangles, filter and numeric conversion).
Until that same-resource link is measured, keep the kind-7 consumer at `consumer_not_observed`.
