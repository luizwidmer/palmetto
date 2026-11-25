# Pipeline compilation: where a boot's pipeline time goes, and a persistent pipeline cache

**Status: measured, opt-in and off by default.** This page records where the Vulkan layer's pipeline creation time
goes in one boot, and a persistent `VkPipelineCache` that a warm boot starts from (`PALMETTO_PIPELINE_CACHE`). The
cache removes **81.2%** of the driver's compile time and **all 4,571** driver cache misses on a second warm boot, at
unchanged pipeline counts, 0 pipeline refusals and the same presented frames. It is off unless the switch names a
file, so the default path is the one the layer had before. The remaining per-call work — the layer builds a fresh
`VkPipeline`, two shader modules and a pipeline layout for every derived state the title asks for — is **45%** of a
cold boot's pipeline time and is not something a `VkPipelineCache` can remove; the next increment for that is at the
end of this page.

All numbers are from the same machine (an NVIDIA RTX 4090 on a Vulkan 1.3 driver, one boot at a time, harness
`tooling/boot_report.py`, 130 s, the standard boot environment of the other runtime pages, profile and capture switches
as noted). The evidence logs and the cache file are under the local work folder (`<work>/boot-pipecache-*`); nothing of
the game is in the repository.

## 1. What creates pipelines

The title reaches the PC device through the console proxy, which maps two console calls onto
`ID3D12Device::CreateGraphicsPipelineState`:

- **slot 55** `CreateDerivedPipelineStateX(base, count, patches, riid, out)`: a copy of a base state's description
  with patches applied. In one boot's last histogram checkpoint it made **40,000 calls over 6,036 distinct bases**,
  reached the PC create path 40,000 times, created 40,000 pipeline states and refused 0.
- **slot 57**: a deserialized graphics pipeline record built into a state; **10,000 of 10,000 records created a PSO,
  0 refusals**.

The layer's `PipelineState::create_graphics` also serves the runtime's own pipelines (present compose, frame capture and
the BVH builder's compute pipelines). One boot therefore makes **51,786 graphics pipeline-creation calls and 95
compute calls**, and every one of them is a *new* `PipelineState`: the layer does not cache a pipeline recipe across
calls, so `vkCreateShaderModule`, `vkCreatePipelineLayout` and `vkCreateGraphicsPipelines` all run again even when the
recipe is one it created a few thousand calls earlier.

The driver's own pipeline-creation feedback (`VK_EXT_pipeline_creation_feedback`, core in Vulkan 1.3, read only under
the profile switch) confirms how many of those calls are repeats: on a cold run, of the boot's 51,881 driver
pipeline-creation calls (51,786 graphics + 95 compute), **47,310 were already-cached recipes inside the same process
(91.2%)** and only **4,571 were real compiles** (at most 95 of them compute).

## 2. The measured profile of a cold boot

`PALMETTO_PIPELINE_PROFILE=1` prints cumulative lines for the layer's own work (shader modules, pipeline layouts) and
for the driver's `vkCreate{Graphics,Compute}Pipelines` calls separately, the cache feedback, four wall-clock windows
relative to the first creation, and the slowest few recipes. A boot killed at its timeout still leaves the last line
(the switch starts one 3 s background tick when the first pipeline is seen, so the totals are final).

One cold boot (cache switch unset) on the final tree:

| | graphics | compute |
|---|---:|---:|
| creation calls / pipelines created | 51,786 / 51,786 | 95 / 95 |
| total creation time (the layer's call wall time) | 20,545.9 ms | 1,950.7 ms |
| driver `vkCreate*Pipelines` calls / driver time | 51,786 / **7,165.2 ms** | 95 / **216.6 ms** |
| `vkCreateShaderModule` calls / time | 103,568 / 1,710.2 ms | 95 / 6.0 ms |
| `vkCreatePipelineLayout` calls / time | 51,786 / 2,439.7 ms | 95 / 9.2 ms |
| everything else in `create_*` (validation, recipe, arena) | 9,230.8 ms | 1,718.9 ms |
| driver cache hits / misses within the process | 47,310 / 4,571 (all driver calls) | — |

The same boot by creation-time window (relative to the first creation): 0-5 s 1,292 graphics pipelines (0.39 s of
creation time), 5-30 s 50,494 pipelines (20.15 s), nothing later; all 95 compute pipelines in the first window. So the
boot's pipeline work is one burst in the first tens of seconds and there are no late or repeated creations after it.

Where the time goes, on the cold boot: **driver compile 34.9%** (7,165.2 ms), **the layer's own per-call work 44.9%**
(9,230.8 ms), **pipeline layouts 11.9%** (2,439.7 ms) and **shader modules 8.3%** (1,710.2 ms). The single slowest
creation is not a driver compile at all: the first BVH compute pipeline takes 1.70 s of layer time (program resolution
and the first touch of that shader family — its `vkCreateComputePipelines` is 1.1 ms). The slowest graphics creation is
9.3 ms and the average graphics creation is 396.7 µs, which matches a corpus of small translated vertex/pixel
programs: the compile cost is spread thin over a very large number of calls, not concentrated in a few.

**The SPIR-V corpus is not translated at boot.** The runtime resolves a program's `FSSM`/`FSPX` bytes to a
hash-indexed corpus entry and creates a module from the translated `.spv` file; nothing is re-translated or
re-validated per boot (the one 1.7 s layer cost above is reading/resolving, not translating). The offline translation
pipeline is untouched.

## 3. The persistent cache

### Switch and file

| switch | effect |
|---|---|
| `PALMETTO_PIPELINE_CACHE=<file>` | opt-in. The device's `VkPipelineCache` is created with this file's blob as initial data, and the current blob is written back to it. Unset: today's behaviour exactly (an empty cache, or the build stage's `pipeline_cache.bin` seed beside the executable when one exists). |
| `PALMETTO_PIPELINE_CACHE_MAX_MB=<1..16384>` | the file's bound, default 512 MiB. A blob over the bound is never read or written; the file is left as it was. |
| `PALMETTO_PIPELINE_PROFILE=1` | the measurement switch of section 2; also chains the driver's creation feedback, which is what says whether a creation was a cache hit. |

**Recommended for repeated boots:** set `PALMETTO_PIPELINE_CACHE` to a file in a local work folder (a machine-local
path, never inside the repository) and keep it between boots. That is the configuration the warm-boot numbers of
section 4 are for. The default (switch unset) is **byte-identical to the previous tree**: no file is opened, no
background tick starts, no clock is read, and the device creates its cache exactly as before (an empty cache, or the
build stage's `pipeline_cache.bin` seed beside the executable when one is compatible). The switch is opt-in because the
first run pays the extra save writes and because a cache file is a machine-local artefact, not something a checkout or
a report should carry.

### Load, validate, fall back

On the first pipeline creation the device creates its `VkPipelineCache` with the file's bytes when they are usable, and
otherwise falls back to the path the layer had before:

1. The file is read whole only if it is at most the bound and at least a 32-byte `VkPipelineCacheHeaderVersionOne`
   (`src/pipeline_cache.h`): header size 32 and header version 1, then the **compatibility rule Vulkan itself defines**
   — same `vendorID`, `deviceID` and `pipelineCacheUUID` as this physical device. That UUID changes with the driver and
   its configuration, so a driver update invalidates the file without a version field of this layer's own.
2. A file that is missing, unreadable, shorter than its header, corrupt (not a version-one header), from another
   GPU/driver, or over the bound is **ignored**: the layer logs one line and creates the cache exactly as before — from
   the build stage's seed if `pipeline_cache.bin` beside the executable is compatible, else empty. Every failure is a
   status, never an exception; nothing here can fail the boot. (Unit-tested in `port/test/pipeline_cache_test.cpp`;
   measured on a boot with a deliberately corrupt file: `ignoring <file>: incompatible`, then a normal ALIVE boot that
   saved a fresh cache.)
3. If `vkCreatePipelineCache` still declines the seeded data (a driver that rejects the whole blob), the layer logs it
   and takes the same seed/empty fallback.

The runtime writes nothing but the named file and its temporary sibling: the save creates the parent directory when it
is missing, writes a unique `<file>.tmp.<stamp>.<n>` beside the destination, flushes and closes it, and renames it over
the destination in one step (a Windows sharing violation from a reader of the previous file is retried, then the temp
file is removed). A failed save leaves the previous file untouched.

### Save

A boot that runs to its harness timeout has no exit hook, so the cache is saved while it grows and again after the burst
has stopped:

- after every driver pipeline creation, `vkGetPipelineCacheData` is asked for its size (cheap); a full snapshot and
  write happens only if the last save was at least 12 s ago and the blob grew by at least 1 MiB. A 3 s background tick
  (started only when one of the two switches is on) also asks, which is what catches the pipelines created after the
  burst's last save.
- the device's final write happens in its destructor, before `vkDestroyPipelineCache`, and an `atexit` handler covers
  the normal `exit()` path if the device object was still alive.
- the snapshot retries up to four times with headroom when a concurrent creation makes `vkGetPipelineCacheData` return
  `VK_INCOMPLETE`.

Measured save cost at 68.7 MiB: 21-27 ms per write. The load cost is inside `vkCreatePipelineCache` and is not
separately timed by the runtime. The measured boot wrote the file twice (57.1 MiB mid-burst, 68.7 MiB after the
burst, 43 ms of writes); the next warm boot loaded 68.7 MiB and saved back the same 68.7 MiB byte count.

### Keys and determinism

A `VkPipelineCache` keys a pipeline by the pipeline state the driver builds from the create info, so the recipe fields
that enter that key are the ones that matter. This layer's recipes are deterministic for the same title request:

- the SPIR-V code is the corpus file's bytes (offline-translated, not regenerated per boot) and the entry point is
  always `"main"`; no specialization constants are used;
- the pipeline layout is the push-constant range plus, when the shaders declare set-0 bindings, the device's bindless
  set-0 layout (same device, same layout);
- formats, sample count, vertex input, raster/depth/blend state, dynamic state, the unbound-attachment variant bitmask
  and the required-subgroup-size chain all come from the title's description;
- no clock, random value, thread id or allocation address enters a create info; the `pNext` chains carry only content
  the driver reads (rendering formats, vertex divisors, conservative raster, required subgroup size), never pointer
  values that select different state.

The only `pNext` item the profile adds is `VkPipelineCreationFeedbackCreateInfo`, an output-only struct that exists
only while `PALMETTO_PIPELINE_PROFILE` is set (both the cold and the warm runs in section 4 had it on). The measured
result of that determinism is the warm run's **0 misses out of 51,881 driver calls**: the same boot, off the same
file, asked the driver for exactly the recipes the file holds.

### Multiple devices and invalidation rules

- One file belongs to one device identity. The switch names one process-wide path, so the **first device owns the
  file**: a second device in the same process still loads it when the header matches (its own in-memory cache) but does
  not save it, and the registry logs that once — two identities cannot share one file, and one writer cannot replace
  the other's blob.
- Invalidation is by the file itself: driver/device change (new `pipelineCacheUUID`), a corrupt or truncated file, or a
  blob over the bound all mean "ignore and start empty"; the file is overwritten with a blob from the new device once
  the boot has pipelines. Translated recipes that change (a re-translated shader) produce a different driver key and
  simply compile once more.
- The build stage's full-corpus `pipeline_cache.bin` seed is still used when no persistent file loads; the persistent
  file is written from the merged cache the device then holds, so a warm boot does not need the build-stage file again.

## 4. Cold/warm A/B

Three 130 s boots, same tree and machine, one boot at a time: cold (switch unset), warm 1 (empty file: compiles and
saves), warm 2 (loads 68.7 MiB). Profile on in all three so the driver feedback and the profile totals are comparable,
and the same presenter capture switch so the frame numbers are comparable. The first-presented-frame time is the first
capture's file time against the harness's stderr log creation (the harness opens that log, then starts the process);
the harness itself does not timestamp presents.

| measurement | cold (no cache) | warm 1 (creates + saves) | warm 2 (loads) |
|---|---:|---:|---:|
| verdict / exceptions | ALIVE 130.2 s, 0 | ALIVE 130.2 s, 0 | ALIVE 130.1 s, 0 |
| D3D12 calls traced | 556,735 | 553,092 | 561,131 |
| graphics creation calls / created | 51,786 / 51,786 | 51,786 / 51,786 | 51,786 / 51,786 |
| graphics creation time | 20,545.9 ms | 20,519.4 ms | **14,207.9 ms** |
| graphics driver compiles / time | 51,786 / 7,165.2 ms | 51,786 / 7,037.5 ms | 51,786 / **1,346.1 ms** |
| driver feedback hits / misses (all driver calls) | 47,310 / 4,571 | 47,325 / 4,556 | 51,881 / **0** |
| shader modules / time | 103,568 / 1,710.2 ms | 103,568 / 1,692.7 ms | 103,568 / 1,680.3 ms |
| pipeline layouts / time | 51,786 / 2,439.7 ms | 51,786 / 2,442.8 ms | 51,786 / 2,340.8 ms |
| compute pipelines created / driver time | 95 / 216.6 ms | 95 / 210.8 ms | 95 / **66.4 ms** |
| cache file | — | 57.1 MiB mid-burst + 68.7 MiB after (2 writes, 43 ms) | 68.7 MiB load, 68.7 MiB save |
| first presented frame at | 22.6 s | 22.0 s | 20.6 s |
| submission 8,192 reached at | 37.6 s | 37.9 s | 36.5 s |
| submission 16,384 reached at | 70.7 s | 71.8 s | 70.2 s |
| submission 24,576 (last; the loading loop) reached at | 103.9 s | 105.3 s | 103.7 s |
| presents / captured frames | 13,292 / frame_00001..12001 | 13,201 / same | 13,410 / same |
| presented capture mean rgb | 0.00 0.00 0.00 | same | same |
| slot 55 derived states (last checkpoint) | 40,000, 6,036 bases, 0 refused | same | same |
| slot 57 records | 10,000/10,000 PSOs, 0 refused | same | same |

Per-pipeline and per-frame derivatives:

- driver compile time per graphics pipeline: **138.4 µs cold → 26.0 µs warm 2 (-81.2%)**, and the boot's 4,571 real
  compiles become **0**;
- every other number of the boot (create calls, created pipelines, module and layout counts, refused states, captured
  frames and their numbers) is identical between the three runs — the cache changes only how the driver produces the
  same pipelines;
- graphics creation time per pipeline: 396.7 µs → 274.4 µs (-30.8%); the warm run still spends 8.8 s in the layer's
  own per-call work plus 1.7 s of modules and 2.3 s of layouts;
- compute: identical 95 pipelines, driver time 216.6 → 66.4 ms (-69.3%);
- warm 1 is the control for the empty-cache path: it matches the cold run within 1.8% on the graphics driver time, so
  the switch does not change the cache-empty behaviour, and only the second warm boot gets the saving.

The user-visible effect in this boot window is small and uneven: 5.8 s of driver compile time is removed, the first
presented frame comes **2.0 s earlier** (20.6 s against 22.6 s), submission 8,192 is reached 1.1 s earlier, but the
loading loop's last submission moves by only 0.2 s (103.7 s against 103.9 s; warm 1, which also pays the two cache
writes, is 1.4 s later). The pipeline burst overlaps other loading work, so it is not the boot's only critical path.
That is the loading-gate question, a separate workstream; the pipeline side's remaining cost is the per-call layer work
below.

## 5. Limits and the next increment

- **The layer still creates a pipeline per derived state.** 51,786 `create_graphics` calls reach only 4,571 driver
  compiles on a cold run; the layer builds a fresh `VkPipeline`, two shader modules and a layout for each of them. The
  persistent cache removes the compile part on warm boots; a **per-process recipe cache** (share one `VkPipeline` +
  modules + layout per identical create-info, reference-counted until the last `PipelineState` that uses it is
  released) is the next increment and is measured to be worth the remaining ~12.8 s of warm-boot creation time
  (8.8 s of layer work + 1.7 s of modules + 2.3 s of layouts). It changes what the title gets back (a shared pipeline
  object), so it needs its own pass over PSO lifetime, `SetPipelineState`, frame capture and the pipeline-library
  paths before it can be turned on.
- **The burst is not the boot's critical path.** Even with most compiles removed the first present moves ~2 s and the
  loading loop's last submission ~0.2 s; the loading gate needs the parallel workstream.
- **A killed boot keeps its last save.** Saves happen while pipelines are created and for a short while after, but a
  run killed between two saves loses the last seconds of recipes; the next warm run saves again, so a cache converges
  over runs. On the final tree one create+saves run wrote 57.1 MiB mid-burst and 68.7 MiB after the burst, and the
  next run loaded those 68.7 MiB with 0 misses; on an earlier build three consecutive runs grew the file
  48.5 → 64.8 → 66.4 MiB before it was complete.
- **A pre-1.3 device has no creation feedback.** `PALMETTO_PIPELINE_PROFILE` then reports `without feedback` for the
  hit/miss line; the cache itself works the same.
- **The bound is a hard refusal, not a truncation.** A driver blob over `PALMETTO_PIPELINE_CACHE_MAX_MB` is not
  written (and one over the bound on load is not read). The measured file is 68.7 MiB, comfortably inside the 512 MiB
  default; a driver that caches more per pipeline may need the switch raised, and the file is then bounded by the
  switch.
- **One boot at a time.** The cache file is per machine/GPU; two boots writing one file at once would race the rename
  (each write is atomic, so a reader only ever sees a complete file). The local harness serialises boots with the boot
  lock.

## 6. Tests

- `port/test/pipeline_cache_test.cpp` (CMake target `pipeline_cache_test`, run by `ctest -R pipeline_cache`): the pure
  header/bound helpers — a synthetic 32-byte header is parsed and matched against a device identity, a wrong header
  size or version is refused, a shorter-than-header file, a corrupt file, another device's file and an over-bound file
  each give their status; the atomic replace creates a missing parent directory, round-trips, replaces a longer file
  with a shorter one, refuses a blob over the bound or without a version-one header without touching the destination,
  and fails on a directory destination with no temp file left behind.
- The runtime path itself is measured by the three boots above (cold, create+saves, load: 0 misses) and by the corrupt
  file boot of section 3.
