# The console compute pipeline state: 115 of the title's requested pipelines were refused for a missing translation

**Status: measured, corpus completed, measured again; the refusals are a translation-coverage gap, not a decode or
validation bug, and the boot's named failure line falls by exactly the programs published.** The boot that crossed the
null read ([frontier_null_read.md](frontier_null_read.md)) runs its frame loop with no exception and stops only on the
130 s timeout; its next named failure is the console compute-pipeline slot, device slot 67,
`DeserializeComputePipelineStateX`. This slice measured what the failing calls carry, proved the proxy decodes them
correctly (each failing record's program bytes are byte-identical to a program already extracted from the title's
pipeline library), named the refusal site - the runtime's own named "no translated compute shader" answer, not a
record/root-signature validation - and completed the missing piece: the translations of the requested library
programs. One A/B boot pair (same binary, corpus the only intended change) moves the slot-67 failure lines from 326
to 209 and creates 115 of the previously refused pipelines through slot 67; 145 of the remaining refused programs are
translatable with the same tooling and 60 are genuinely untranslatable today and stay refused by name.

## 1. Why this call is worth a slice

The null-read repair took the boot from a fatal title null read to a live 130 s run, and the new frontier it named is
a repeating, non-fatal `0x80070057` (`E_INVALIDARG`) from device slot 67: 147 failed calls in that boot, matching
`compute calls 381 created 226` at the `+120 s` marker. A refusal on the frame loop's pipeline-creation path is
where a rendering pipeline the title expects is missing, so it is the next measured thing to clear - if the refusal
is in our structure. This page measures the request, the refusal and the missing piece, and shows where the fix
belongs (the shader corpus' compute-library coverage, plus the build tooling that makes it reproducible), not in the
record path.

## 2. Measured shapes

Everything below was measured on the 1.0.2.1 target image and its pipeline library, in one control boot and one after
boot (section 5; same binary, SHA-256 `0BCE1CD8...754D188`, built at the `9732aaa8a` tree). The work folder is
outside the repository; only counts, sizes, RVAs, HRESULTs and library program indices appear here (no shader bytes,
no hashes, no package path).

### 2.1 The failing request

| property | measurement (control boot) |
|---|---|
| failure line | `[rt] d3d12_xs: slot 67 call N failed with 0x80070057 (kind 0 28 B, kind 1 20 B, kind 6 S B, kind 7 T B)` |
| HRESULT | `0x80070057` (`E_INVALIDARG`) on **every** failing call |
| call numbers | 43 .. 368 (the first 42 slot-67 calls succeed, then failures are interleaved with successes) |
| failure lines / distinct requests | 326 lines / 322 distinct program keys (4 requests were repeated) |
| kind 0 / kind 1 sizes | 28 B / 20 B on every call - the dispatch-state and record words of a library compute record |
| kind 6 size (the program) | 256 .. 102,976 B, median 2,432 B |
| kind 7 size (the layout blob) | 56 .. 6,072 B |
| requester | the title image, one call site: `game+0x24800db`, with `(desc, IID*, out, 0x10)` (`PALMETTO_XS_CALLERS=1`; the same call site [console_slots.md](console_slots.md) records for slot 67) |
| successful calls | the 42 published library programs `lib_cs_00000..00041`, one creation each |

The failing records are therefore not malformed: the same 28/20-byte state words, a program in the library's size
range and a layout blob are exactly the shape of the 42 that create.

### 2.2 What the failing requests name

The runtime builds an FSPX payload from the description's four pointer/size pairs and looks the program up by
`fspx_code_sha256` (kind 6) and `fspx_state_sha256` (kind 0). Measured against the pipeline library extracted from
`shaders.arc` ([shader_library.md](shader_library.md)):

* all 322 failing program keys are byte-identical to an extracted compute program; **0 of 322 are in the shader
  corpus the boot loaded**;
* the requested programs are `lib_cs_00846..01163` (318 programs) plus `lib_cs_01166`, `lib_cs_21483`,
  `lib_cs_21490` and `lib_cs_21491` - the title's own precompiled compute pipelines, exactly the record family
  [boot_frontier.md](boot_frontier.md) identified for the earlier 42;
* the 42 successes are the 42 library programs that boot's corpus did publish (`lib_cs_00000..00041`), which is why
  they work: the decode is correct and the same description shape creates when its program is in the corpus.

### 2.3 The refusal site

`port/shims/d3d12_vulkan_device.cpp`, `PipelineState::create_compute`: after the record was decoded and its program
parsed, the lookup

```
no translated compute shader for FSPX kind-6 and kind-0 sha256 <code key>
```

returns `E_INVALIDARG`. That is the runtime's named answer for a program the shader corpus has no SPIR-V module for
(shared by slot 62 and slot 67). The refusal is **not** a root-signature/descriptor-table/user-data validation and not
a malformed console record: the same call with the same 28/20-byte shape succeeds when the program's translation is
published.

### 2.4 Translation coverage of the requested set

Translating the requested programs with the repository's own tooling (`tooling/rdna2spirv.py`, `--wave64-emulate`,
`spirv-val` clean):

| outcome | programs | reason |
|---|---:|---|
| translated, `spirv-val` clean | **260** | - |
| refused | 62 | 36 `fall-through into undecodable words`; 16 `CS work-group-info SGPR use` (RSRC2 `TG_SIZE_EN`); 7 `s_bitreplicate_b64_b32 on a lane mask`; 1 `undecodable words (cannot re-anchor)`; 1 `ds_append`; 1 `reachable instruction with normalized encoding` |

The earlier frontier's set (the null-read boot's 147 requests, `lib_cs_00846..00992`) translates 115 of 147, with
32 refused for the same classes (15 / 10 / 7). Those 115 are the programs this slice published for the A/B; the
remaining 145 translatable requests are named in section 6 as the next additive step.

### 2.5 Where the corpus came from, and why it was short

The build's shaders stage already has a pipeline-library step (`library_shaders` in
[`build_port.py`](../build_port.py), `LIBRARY_STAGES = "cs"`): it extracts the library's compute programs with
`tooling/shader_library.py`, translates them and publishes them into the shader corpus the runtime reads. The boot's
work corpus (`D:\leonida-work\shaders\spirv`, outside the repository) was built before that step and carries only a
42-program compute subset; a later full extraction stopped before its disassembly phase (2,684 program sidecars,
851 listings, no staged translation). So a fresh build of this tree translates the whole compute stage, while the
work corpus the frontier boot used did not - the front is corpus coverage, not port code.

## 3. What we implemented

### 3.1 The measurement corpus (the boot fix)

The 115 translatable programs of the measured set were translated and published **additively** into the shared shader
work corpus, keeping every existing entry and module:

* the listings were rebuilt from the extraction's `fspx/<name>.code`/`.json` sidecars with the repository's
  `tooling/shader_disasm.py` (the call `shader_library.py` makes), because the stopped extraction had no listings for
  them; the modules were made with the repository's translator:
  `python -I tooling/rdna2spirv.py <staged>/disasm --out <out> --wave64-emulate --no-asm`;
* the publish ran under the shared boot lock (`_setup\boot_lock.ps1 -Acquire -Who compute67`) with a backup of the
  index first: **backup `D:\leonida-work\shaders\backup-compute67-20261125-161129`** (`fspx-index.json` and a
  manifest), plus the pre-work safety copy `backup-compute67-20261125-155728`;
* the swap script is `D:\leonida-work\compute67\swap_corpus.py` (run with its apply flag after a dry run): it only
  adds names the published index
  does not already have, writes `fspx/index.json` through a temp file and `os.replace`, and then verifies that the
  index parses, that all 1,337 pre-existing keys are still there with their modules, and that every added key
  resolves to a SPIR-V module whose `fspx_code_sha256`/`fspx_state_sha256` match the extracted `.code`/`.state`.
  Measured result: **1,337 -> 1,452 entries, 1,337 pre-existing intact, 115 added and resolving**;
* **the shared corpus now carries the 115 programs** (`lib_cs_00846..00991` except two), so boots in this work folder
  see them; the boot lock was held from the swap through the after boot.

### 3.2 Repo tooling: the compute wanted list and a per-program coverage report

Completing the corpus by hand is not reproducible, so the build's compute-library step gained both the selection and
the report (no runtime change):

* **Wanted compute program list** (`build_port.py`): `PALMETTO_COMPUTE_PROGRAMS`, else
  `<work>/compute_programs.json`, a JSON object of stage hash lists (`{"cs": [...]}`), the compute counterpart of the
  graphics selection. With one, the step extracts and translates only the library programs whose bytes have those
  SHA-256 hashes (`shader_library.py --stages none --wanted ...`) instead of the whole 2,684-program stage; without
  one, the step behaves exactly as before and a work folder built before the option keeps its recorded settings (the
  wanted digest joins the extraction/translation dependency record only when a list is present). The list itself is
  measured data and stays outside the repository, like `graphics_programs.json`.
* **Per-program refusals** (`tooling/rdna2spirv.py --refusals PATH` and `build_port.py`): the translator writes
  `{shader: reason}` for every shader it could not translate (with `--shard` each shard writes a part and `--merge`
  combines them; the writer is atomic). A refusal is no longer only a count in the summary.
* **Coverage report** (`library_coverage()`): after the translation (and on a reused one) the step writes
  `<work>/shaders/pipeline_library/coverage.json` - every extracted program's status (`translated`, `refused: <reason>`
  or `untranslated`), whether it is published in the spoke corpus, and the counts - and logs one line:
  `pipeline library compute coverage: T of N translated, U untranslated (reasons)`; for a wanted-list extraction it
  additionally prints one line per refused requested program, so those stay **refused by name** with the reason and
  the difference between "no request" and "no translation" is readable. `shaders: N of M pipeline library compute
  programs translated` is kept.

The genuinely unsupported shapes are still refused by the runtime with its named message; the tooling now records
which they are and why, instead of leaving them as a count.

### 3.3 Tests

New `tests/test_build_port_library_coverage.py` (7 checks) and three checks added to
`tests/test_rdna2spirv_merge.py` (33 in that file): see section 7.

## 4. Cross-check against the reference

The reference is the public GPL-3.0 project `ubiquitarianism/leonida`, consulted at its HEAD `ed94fbef9888` for this
slice through its `docs/fspx.md` and `docs/shader_library.md` and its build script; no code was copied.

| Topic | Reference | This measurement | Why |
|---|---|---|---|
| Mechanism | Its `library_shaders` step extracts the library's compute programs, translates them and the runtime finds them by `fspx_code_sha256`/`fspx_state_sha256`; the executable passes each compute record's four parts to device slot 67 | **Agree**: measured here independently - the failing requests name extracted library programs by those keys, and published programs of the same shape create | Our measurement and the reference's description of the same ABI agree |
| Where the entries live | The reference loads a separate `library/index.json` beside `fspx/` and `bvh/` | **Differ by structure**: our shader library merges the library entries into the FSPX index under `lib_<stage>_` prefixes (`merge_library_corpus`), which is where our runtime's single `fspx/index.json` lookup reads them | Both are internal layouts; ours predates this slice and the merged index is what the boot measures |
| Selection | The reference translates the whole compute stage | **Differ**: this slice adds an optional wanted compute program list so a measured request set can be translated (and reported) without the whole 2,684-program stage; without a list the step still translates the whole stage | The wanted list is our own structure for reproducible measured corpora, the same shape our graphics selection already uses |
| Failure reporting | Its translator batch writes a `failures.json` keyed by both SHA-256 keys | **Differ by layout, agree on intent**: our translator writes `refusals.json` keyed by program name with the translator's reason, and the build's coverage report reads it | It keeps the reason visible without putting hashes into the repository or the report |

## 5. A/B measurement

One 130 s pair of boots, one at a time, under `D:\leonida\_setup\boot_lock.ps1`; both use the *same* runtime binary
(snapshot `D:\leonida-work\compute67\bin\palmetto.exe`, SHA-256 `0BCE1CD8...754D188`, built at the `9732aaa8a` tree,
copied so a concurrent rebuild could not change the after arm), the same environment and work folder; the **only
intended difference is the shader corpus** (old 1,337 entries / 42 compute-library programs vs 1,452 / 157). The
before arm is a fresh control boot with the old corpus, not the null-read page's boot, because the executable moved
when another slice landed mid-task.

Environment: `PALMETTO_SHADERS=D:\leonida-work\shaders\spirv`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_PIPELINE_TIMELINE=10`, `PALMETTO_LOADER_HOOK=1`,
`PALMETTO_XS_CALLERS=1`, `PALMETTO_XS_CALLERS_PROFILE=1`, and the usual trace switches; captures every 2,000 frames.
Logs under `D:\leonida-work\compute67\boot-control\` and `boot-after\`.

| | control (old corpus) | after (115 published) |
|---|---|---|
| corpus | 1,337 fspx entries, 42 `lib_cs_*` | 1,452 entries, 157 `lib_cs_*` |
| boot | **ALIVE 130.3 s**, 0 exceptions | **ALIVE 130.2 s**, 0 exceptions |
| slot-67 failure lines | **326** | **209** |
| slot-67 distinct refused programs | **322** | **205** |
| slot-67 successes | 42 (the published subset) | **157** (42 + the 115 published) |
| first slot-67 failure / last call | call 43 / call 368 | call 54 / call 366 |
| compute calls / created at +120 s | 745 / 408 | 628 / 407 |
| submits / presents at +120 s | 11,437 / 5,718 | 8,193 / 4,096 |
| `d3d12-trace` calls | 280,496 | 215,179 |
| loader begin/end hook pairs | 226 / 226 | **400 / 400** |
| captures | 4 (frames 1, 2,001, 4,001, 6,001): mean RGB 0.00 all | 3 (frames 1, 2,001, 4,001): mean RGB 0.00 all |
| `int3_skipped` | 0 | 0 |

Reading it:

* **The named frontier line falls by exactly the published programs** (326 -> 209 lines, 322 -> 205 programs; four
  duplicated requests stay duplicated). The 115 programs that had no translation now create through slot 67: the
  first 42 calls still succeed as before, and the first failure moves from call 43 to call 54.
* **The compute-created total does not move** (408 -> 407), because the created counter mixes both creation paths
  (slot 62 also creates the title's ordinary compute pipelines and the BVH builder's; the run's `compute calls 745 ->
  628` mostly lost slot-62 calls). What moved is where the title's library pipelines are created, not how many
  pipelines exist at the end of the window. Read the fixed number as: **+115 pipelines now create through slot 67**,
  and the mix of paths, not the population, is what the counters show.
* **The after boot spends its window differently**: the pipeline burst moves from the `+20..30 s` window to
  `+60..70 s`, presents in the 130 s window drop (5,718 -> 4,096) and the loader's hook pairs are complete (400/400)
  instead of 226/226. The refused calls used to fail immediately; the published ones now compile real Vulkan compute
  pipelines (115 module compilations), which is the likely cost inside the fixed window, and the after boot reaches
  the loader completion the null-read page's post-fix boot also had. Both effects are stated as observed; the pair is
  one boot each, so order/cache effects are not excluded (section 6).
* **No render-content advance**: every capture in both arms is black (mean RGB 0.00), 0 exceptions, the frame loop
  runs to the timeout in both. The slice clears a named refusal and creates real pipelines; it does not claim content.

## 6. Limits

* **One pair, one machine.** The two boots ran one after the other with the corpus as the only intended change; the
  loader and frame-loop phase differences above are measured in that pair and are not a controlled causal claim
  (the control's 226/226 loader pairs versus the after's 400/400 matches the null-read page's post-fix boot, so the
  control may be the outlier). A second pair would tighten it.
* **The executable moved mid-slice.** The task's stated baseline (381 asked / 226 created, 147 failures) is from the
  previous binary; another slice landed `9732aaa8a` while this one ran, so the control arm is a fresh boot of that
  binary. Both arms of this pair use the same snapshot, so the corpus comparison is clean, but the absolute numbers
  are not comparable with the previous page's.
* **The slot-67 success count is derived, not counted.** The proxy logs failed calls only and the timeline counter
  mixes slot 62 and 67; "157 successes = 42 + 115" is the reading of the failure lines and the published set (the
  first-failure call moves 43 -> 54 and every published program stops failing). A per-slot success counter would make
  it direct.
* **145 translatable requested programs are still refused.** The requested set translates 260 of 322; this slice
  published the 115 measured from the previous frontier's 147-program set. The other 145 are the obvious next
  additive publish (one more corpus swap and boot would take the failure lines to the ~60-66 genuinely untranslatable
  requests).
* **62 requested programs are genuinely untranslatable today** and stay refused by the runtime's named message; the
  build's coverage report now names each one with its reason (36 fall-through into undecodable words, 16
  work-group-info SGPR, 7 lane-mask bit-replicate, 3 other). Publishing a stand-in for them is not this slice.
* **The whole 2,684-program library translation was not completed** in this slice (a stopped extraction leaves no
  listings and the whole stage takes minutes); the reproducible path is the build's library step, now with the wanted
  list, and the coverage report says what a run covers. The corpus change itself lives in the work folder, outside
  the repository.
* **No content claim.** The captures are black in both arms; the pipelines the after boot creates are used by the
  title on its own path, and no dispatch or draw was shown to produce an image.

## 7. Tests

| test | checks | what it proves |
|---|---:|---|
| `tests/test_build_port_library_coverage.py` (new) | 7 | the coverage report names each program translated / refused-with-reason / untranslated, counts them, marks publication, writes `coverage.json`, and prints a wanted extraction's refusals by name; a build without a wanted list keeps its recorded settings, and the wanted path/digest behave |
| `tests/test_rdna2spirv_merge.py` (extended) | 3 added (33 in the file) | `--refusals` writer is atomic and sorted, the per-shard parts merge into one name -> reason report, and a missing or unreadable part is skipped rather than invented |

Run: `python -I tests/test_build_port_library_coverage.py` -> 7 tests, OK; `python -I tests/test_rdna2spirv_merge.py`
-> 33 tests, OK. The build_port argument, translate-cleanup and empty-translation suites also run (38 + 3 + 13 tests,
OK), since the changed step is on the build's shaders path. No game data, no GPU.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project `ubiquitarianism/leonida` (<https://github.com/ubiquitarianism/leonida>, licence
<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), docs `docs/fspx.md` and `docs/shader_library.md` and
build script `make_game.py` at HEAD `ed94fbef9888`; no code from that project was copied. The upstream project remains
the copyright of its contributors and its license continues to apply to its own tree.

The project's attribution rule ([upstream_port.md](upstream_port.md), section 3) also puts this sentence in
[NOTICE.md](../NOTICE.md). That file is outside this slice's write scope, so the lead adds the entry there; the
sentence above is its content.
