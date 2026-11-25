# The second compute-library corpus publish: 668 refused programs measured, 444 translated and published, 224 untranslatable

**Status: measured; corpus published additively; measured again.** The boot that runs its frame loop to the 130 s
timeout still fails on device slot 67, `DeserializeComputePipelineStateX`, with the runtime's named "no translated
compute shader" answer. The first publish ([compute_slot67.md](compute_slot67.md)) took the slot-67 successes from the
title's 42 published library programs to 157; as the tree moved and renderer setup got further, the title asks slot 67
for a larger part of its precompiled compute-pipeline library, so the refusal stock is again large. This slice
measured that stock across the recent boots, split it with the repository's own translator into translatable and
genuinely untranslatable programs, published the 444 translatable ones additively into the shared boot corpus, and
measured the result in one A/B boot pair: **the slot-67 failure lines fall from 675 to 116 and 444 previously refused
library programs now have a translation in the corpus**; the remaining 224 programs of the measured set are refused by
the translator for the reasons in section 2.3 and stay refused by the runtime's named message.

## 1. Why this slice

[compute_slot67.md](compute_slot67.md) proved that the slot-67 refusals are a translation-coverage gap in the shader
corpus, published 115 measured programs, and named the next additive step (145 more translatable requests). The tree
has moved since: the display-window carve and the console command signatures got further into renderer setup, so the
title now deserializes more of its compute-pipeline library, and a fresh boot of the current tree refuses more slot-67
calls than the first slice's control did. The refusal is the same one: the runtime's own named answer for a program
the corpus has no SPIR-V module for, not a malformed console record (the same 28/20-byte state words and the same
kind-6/kind-7 shape as the programs that create). Clearing it is a corpus change, not port code.

## 2. Measured shapes

Everything below was measured on the 1.0.2.1 target image with the shared boot corpus
(`D:\leonida-work\shaders\spirv`, 1,452 entries / 157 `lib_cs_*` at the start) and one frozen copy of the current
runtime binary. Only counts, sizes, indices and program names appear here: no shader bytes, no program hashes, no
package path.

### 2.1 The failing requests in the recent boots

Five boots were read for the census: four recent boots of this tree made by other slices with the census switches
(`PALMETTO_XS_CALLERS`, `PALMETTO_XS_CALLERS_PROFILE`; their pages are [user_event_probe.md](user_event_probe.md) and
[story_load_probe.md](story_load_probe.md)) and this slice's own control boot. All five ran the shared corpus. The
failing call shape is identical in all five:

| property | measurement (five boots) |
|---|---|
| HRESULT on every failing call | `0x80070057` (`E_INVALIDARG`) |
| kind 0 / kind 1 size | 28 B / 20 B on every failing call - the dispatch-state and record words of a library compute record |
| kind 6 size (the program) | 256 .. 145,216 B, median 3,264 .. 5,376 B per boot |
| kind 7 size (the layout blob) | 120 .. 6,712 B |
| failure lines / distinct refused programs | 551/544, 589/582, 608/601, 638/631, 675/668 |
| repeated requests per boot | 7 (a program asked twice fails twice) |
| first / last failing call number | 54/55 .. 708, 746, 765, 795, 832 |
| successful calls before the first failure | 53 (the published library programs the boot asks for first) |

The refusal is the same named frontier as the first slice, on a larger requested set.

### 2.2 What the requested set is

| step | measurement |
|---|---|
| distinct refused program keys across the five boots (union) | **668** |
| of those, keys byte-identical to a program in the extracted pipeline library | **668 of 668** |
| programs that are not library programs (ordinary scene compute or another wanted set) | **0** |
| distinct library programs | 668 (one program per key; no key names two programs) |
| library index ranges | `lib_cs_00857..00905`, `00955..00988` (gaps), `00992..01619`, `01622..01624`, `21483`, `21490..21491`, `21495..21497` |
| already published by the first slice among the 668 | 0 (the first slice's programs are asked first and succeed) |

The requested set varies a little boot to boot (544..668 distinct) because the frame loop reaches the pipeline burst
at a different point; the union is the target, and a fresh 130 s control boot of this tree asks for 668 of it, a
superset of the four earlier logs' 631.

### 2.3 Translation coverage of the requested set

The 668 programs were extracted into a wanted list and translated with the repository's own tooling
(`tooling/rdna2spirv.py` through `build_port.translate()`, `--wave64-emulate`, no asm), and every produced module was
re-validated with `spirv-val` (`spirv-val --target-env vulkan1.2`):

| outcome | programs | reason |
|---|---:|---|
| translated, `spirv-val` clean | **444** | - |
| refused | 224 | 165 `fall-through into undecodable words`; 25 `CS work-group-info SGPR use`; 11 `s_bitreplicate_b64_b32 on a lane mask`; 10 `ds_append`; 8 `undecodable words (cannot re-anchor)`; 2 `v_cmp destination`; 1 `mask destination`; 1 `reachable instruction with normalized encoding`; 1 `s_flbit_i32_b64 of a lane mask` |

The refusal classes are the same family the first slice measured (the translator's unsupported shapes); the larger
requested set brings more of them rather than a new shape (the first slice's set had 36 fall-through and 16
work-group-info programs; this one has 165 and 25).

## 3. What we implemented

### 3.1 The coverage measurement with the repository tooling

The wanted-list/coverage path [compute_slot67.md](compute_slot67.md) section 3.2 describes was used for this set, in a
work folder only (no repository file changed by this measurement):

* the wanted list is the measured census (`{"cs": [...]}` of the 668 programs' kind-6 SHA-256 keys), kept outside the
  repository like `graphics_programs.json`; it is the same data `PALMETTO_COMPUTE_PROGRAMS` selects for a build;
* the extraction is the command `build_port.py`'s library step runs for a wanted list:
  `python -I tooling/shader_library.py <library file> --out <work>/shaders/pipeline_library --stages none --wanted
  <list> --disasm --jobs 16`; it found 668 of 668 wanted programs (no hash named no library program) and wrote their
  sidecars, listings and source containers;
* the translation is `build_port.translate(<disasm>, <staged>, 16, emulate=True, refusals=<staged>/refusals.json)`,
  which is the step `library_shaders()` runs (16 `rdna2spirv.py --shard` jobs, `--wave64-emulate`, `--refusals`);
* the report is `build_port.library_coverage(<extracted>, <staged>, <work>/shaders/spirv/fspx)`: it wrote
  `coverage.json` with every program's status (`translated` / `refused: <reason>`) and the counts, and printed the
  refused programs by name with their reason (the wanted-list behaviour of section 3.2 of the first page).

One measured behaviour of the wanted-list path is worth writing down: `build_port.translate()` reuses a finished
shard plan when its settings match, so a driver that grows the extraction's `disasm/` folder and calls `translate()`
again keeps the shards from the smaller run and leaves the new programs **untranslated with no refusal reason
recorded** (measured here: 49 programs reported as `untranslated` / "reason not recorded"). `library_shaders()` does
not hit this because it drops the staged translation whenever the extraction or the wanted list changes; the
coverage driver in this slice therefore ran the translation from an empty staged folder. The repository tooling was
not changed by this slice.

The work-folder artefacts are `D:\leonida-work\corpus2\cov\pipeline_library` (extraction and `coverage.json`),
`D:\leonida-work\corpus2\cov\spirv\pipeline_library` (444 modules, `index.json`, `refusals.json`) and
`D:\leonida-work\corpus2\union2-keys.json` (the measured wanted list).

### 3.2 The additive corpus publish

The 444 translated programs were published **additively** into the shared shader work corpus
(`D:\leonida-work\shaders\spirv\fspx`), keeping every existing entry and module:

* the publish ran under the shared boot lock (`_setup\boot_lock.ps1 -Acquire -Who corpus2`) and the lock was held
  through the after boot;
* the index was backed up before the change: **backup `D:\leonida-work\shaders\backup-corpus2-20261125-174255`**
  (the exact `fspx-index.json` bytes and a manifest with the pre-change index SHA-256, the entry count and the added
  names' range);
* the publish script is `D:\leonida-work\corpus2\swap_corpus.py` (run with its apply flag after a dry run): it only
  adds names the published index does not already have, writes `fspx/index.json` through a temp file and
  `os.replace`, hard-links each module (copying when a link is not possible), and then verifies that the index
  parses, that all **1,452 pre-existing keys are still there with their modules**, and that every one of the **444
  added keys resolves to a SPIR-V module whose `fspx_code_sha256`/`fspx_state_sha256` match the extracted program's
  `.code`/`.state` bytes**;
* measured result: **1,452 -> 1,896 entries (157 -> 601 `lib_cs_*`), 1,452 pre-existing intact, 444 added and
  resolving**; the runtime's corpus therefore now carries every translatable program of the measured set, and a boot
  in this work folder sees them.

## 4. Cross-check against the reference

The reference is the same public GPL-3.0 project `ubiquitarianism/leonida` at the HEAD `ed94fbef9888`
[compute_slot67.md](compute_slot67.md) section 4 cross-checked through its `docs/fspx.md`,
`docs/shader_library.md` and `make_game.py`; this slice makes no new reference claim and consulted no new reference
file. The two claims it depends on were re-confirmed by our measurement and are unchanged: the runtime looks a
library compute program up by its `fspx_code_sha256`/`fspx_state_sha256` keys (the 668 refused keys are those of
library programs, and the programs this slice publishes are the same programs), and the reference translates the
whole compute stage while our measured path translates a requested subset. No code was copied.

## 5. A/B measurement

One 130 s pair of boots, one at a time, under `D:\leonida\_setup\boot_lock.ps1`; both use the *same* runtime binary
(snapshot `D:\leonida-work\corpus2\bin\palmetto.exe`, SHA-256 `DEB2A7E7...5C6B`, the HEAD build present at snapshot
time, with the TLS host DLL beside it), the same environment and work folder; the **only intended difference is the
shader corpus** (old 1,452 entries / 157 `lib_cs_*` vs 1,896 / 601).

Environment: `PALMETTO_SHADERS=D:\leonida-work\shaders\spirv`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_PIPELINE_TIMELINE=10`, `PALMETTO_LOADER_HOOK=1`,
`PALMETTO_USER_TRACE=1`, `PALMETTO_DS_STACK=1`, `PALMETTO_DS_ARRAYS=1`, `PALMETTO_DSTORAGE_TRACE=1`,
`PALMETTO_FILE_TRACE=1`, `PALMETTO_XS_CALLERS=1`, `PALMETTO_XS_CALLERS_PROFILE=1`, captures every 2,000 frames.
Logs under `D:\leonida-work\corpus2\boot-control\` and `boot-after\`.

| | control (old corpus) | after (444 published) |
|---|---|---|
| corpus | 1,452 fspx entries, 157 `lib_cs_*` | 1,896 entries, 601 `lib_cs_*` |
| boot | **ALIVE 130.2 s**, 0 exceptions, 19,903 trace events | **ALIVE 130.2 s**, 0 exceptions, 19,985 trace events |
| slot-67 failure lines | **675** | **116** |
| slot-67 distinct refused programs | **668** | **114** |
| slot-67 successes | derived: 53 before the first failure, plus interleaved | derived: 53 before the first failure, plus the published programs' calls |
| first slot-67 failure / last failing call | call 54 / call 832 | call 54 / call 588 |
| compute calls / created at +120 s | 1,560 / 873 | 744 / 619 |
| compute creation failures implied by that pair | 687 | 125 |
| submits / presents at +120 s | 6,402 / 3,200 | 8,359 / 4,179 |
| `d3d12` calls recorded | 179,894 | 217,556 |
| loader begin/end hook pairs | 400 / 400 | 400 / 400 |
| graphics created / slot 55 / slot 57 calls at +120 s | 51,786 / 41,545 / 10,241 | 51,786 / 41,545 / 10,241 |
| captures | 2 (frames 1, 2,001): mean RGB 0.00 all | 3 (frames 1, 2,001, 4,001): mean RGB 0.00 all |
| `int3_skipped` | 0 | 0 |

Reading it:

* **The named refusal collapses.** 675 -> 116 failure lines (-559, -83%) and 668 -> 114 distinct refused programs.
  554 of the control's refused programs no longer appear as refusals. Of those, 444 are the published translations
  (their module is in the corpus, so the runtime can no longer answer "no translated compute shader"), and 110 are
  programs the after boot's fixed 130 s window did not reach (see the cost below). The after boot's refused set is a
  **strict subset** of the control's (0 new programs), and none of the 114 is a published program: all 114 are from
  the translator's 224 untranslatable programs.
* **The 444 now compile real Vulkan compute pipelines, and the fixed window shows the cost.** In the control the
  pipeline burst is over by `+30 s` (compute created 868 of its 873 at the `+120 s` marker); in the after boot the
  burst is still running at `+120 s` (created 271 at `+30 s`, 619 at `+120 s`), which is what 444 module compilations
  inside the same window look like. The compute creation failures implied by the counters fall 687 -> 125, matching
  the 559 fewer slot-67 failure lines (the rest of the difference is slot-62/other compute failures).
* **The frame loop does not advance content but runs further.** Both arms are alive to the timeout with 0 exceptions
  and the loader's 400/400 hook pairs complete in both; the after boot records more d3d12 calls (179,894 -> 217,556),
  more submits and more presents (3,200 -> 4,179) and one more capture. Every capture in both arms is black (mean RGB
  0.00): the slice clears a named refusal and translates real pipelines; it does not claim content.
* **The whole-compute counters move down, not up.** `compute calls 1,560 -> 744` with `created 873 -> 619`: the
  created counter mixes both creation paths (slot 62 also creates the title's ordinary compute pipelines and the BVH
  builder's, as the first slice noted), and the after boot's window is spent compiling the published library
  pipelines, so it creates fewer - but successful - pipelines in 130 s. Read the fixed numbers as: 559 fewer refusals
  and 125 vs 687 failed creations; the total created is a mix, not a population.

## 6. Limits

* **One pair, one machine.** The two boots ran one after the other with the corpus as the only intended change; the
  requested set varies a little between boots (section 2.2) and order/cache effects are not excluded. Each arm is one
  boot.
* **The binary is a snapshot of a moving tree.** The pair uses one frozen copy of the build present at snapshot time
  (SHA-256 `DEB2A7E7...5C6B`, built at tree `2ff15fed1`); other slices were committing and rebuilding during this
  slice, so the absolute counters are not comparable with the first page's, and a fresh build of the final tree is
  not what the pair measured. The corpus is the only intended difference inside the pair.
* **The slot-67 success count is derived, not counted.** The proxy logs failed calls and each instrumented slot's
  first call only; "successes" is the reading of the first-failure call number (53 in both arms) and the published
  set, as in the first slice. A per-slot success counter would make it direct.
* **224 programs stay refused.** They are the translator's unsupported shapes (section 2.3), named per program in
  `coverage.json`; publishing a stand-in for them is not this slice.
* **A fixed corpus cannot cover boot-to-boot variation.** The measured set is a union over five boots, and the after
  boot's window reached only part of it; a later boot that asks for a library program outside this set still fails by
  name, and the tooling path (wanted list + coverage report) is how the next census turns into a next publish.
* **The shared extraction folder was not re-extracted through `library_shaders()`.** That step extracts from the
  package's shader archive; this slice's coverage run used the same `shader_library.py` wanted-list command and the
  same `build_port.translate()`/`library_coverage()` functions on the saved library in the work folder, so the shared
  2,684-program extraction and the additive corpus kept by other slices were not replaced.
* **No content claim.** The captures are compared only for the honest statement in section 5; a corpus coverage
  change is not expected to advance the image, and the slice does not claim it does.

## 7. Tests

No repository code or tooling changed in this slice (the work is the measurement, the corpus publish and this page),
so no new test was added. The tooling this slice's report depends on keeps its tests; measured on the slice's tree:

| test | checks | result |
|---|---:|---|
| `tests/test_build_port_library_coverage.py` | 7 | OK |
| `tests/test_rdna2spirv_merge.py` | 33 | OK |
| `tests/test_build_port_argparse.py`, `test_build_port_translate_cleanup.py`, `test_build_port_empty_translation.py` | 38 / 3 / 13 | OK |
| `tests/test_shader_library.py` | 13 | OK |
| `tests/test_docs_*.py` (docs checks, 12 files) | 12 files | 9 OK; 3 failed on files outside this slice's write scope (other slices' in-flight pages naming a transient flag, and a concurrent slice's new environment variables and untracked header). This page passes every docs check. |
| `_setup\build_gta6rt.cmd` / `_setup\build_tests.cmd` | build gates | `PALMETTO_OK` / `TESTS_BUILT` (the tree also carried other slices' committed and in-flight changes at that moment) |

The 444 staged modules were also validated independently: 444 of 444 pass `spirv-val --target-env vulkan1.2`.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project `ubiquitarianism/leonida` (<https://github.com/ubiquitarianism/leonida>, licence
<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), docs `docs/fspx.md` and `docs/shader_library.md` and
build script `make_game.py` at HEAD `ed94fbef9888`, as [compute_slot67.md](compute_slot67.md) section 8 records; no
code from that project was copied and no new reference file was consulted by this slice. The upstream project remains
the copyright of its contributors and its license continues to apply to its own tree.

The project's attribution rule ([upstream_port.md](upstream_port.md), section 3) puts the attribution sentence in
[NOTICE.md](../NOTICE.md). That file is outside this slice's write scope, so the lead adds the entry there; the
sentence above is its content for this slice.
