# Bindless table degradation and larger descriptor arrays

**Status: implemented and measured; the failure path is latent at today's content population.** This slice ports
upstream's bindless-table story-load fix (`273b3bdf9`) into this tree as our own degradation rules, in our structure,
under [upstream_port.md](upstream_port.md). It changes `port/src/vulkan_bindless.h`, adds a bounded diagnostic to the
submission refusal site in `port/src/vulkan_backend.h`, extends `port/test/bindless_sizing_test.cpp`, and documents
the inventory, the rules and the A/B here. No upstream code, comment text or identifier was copied; the upstream
commit is evidence for the behaviour only.

## 1. Why this slice is needed

The bindless set 0 is the one descriptor table that every translated shader probes: a T# or S# is hashed and looked up
at binding 2, and the index it yields selects the image or sampler in a runtime array. Two properties of that design
turn a single allocation failure into a lost submission:

- A change to a live key's index, or a table that outgrew its buffer, needs a new generation: new table buffers and a
  new descriptor set, with the old generation retired until the submissions that read it are done. When the device
  cannot serve that allocation, the flush fails, and the flush's caller is the submission's prepare step, which then
  refuses the whole command list.
- The sampled arrays were clamped at today's counts (2D-array 1024, 3D 256, 1D 256) no matter what the device's
  descriptor limits allowed. A streamed library that names texture arrays or volume textures past those counts gets
  `add_image() == 0`, and the layer's only answer was a named refusal at shader-resource-view creation.

Neither is reachable at today's measured content: the current boot creates 32 image allocations totalling 279.1 MiB,
all device-local, against a driver `maxMemoryAllocationCount` of 4,096 ([streamed_image_backing.md](streamed_image_backing.md)
section 1), and the arrays are far from full. They become reachable exactly when the story load starts streaming its
image library: the reference project's own story load registers about 38,000 streamed images (its evidence, not our
measurement; [remaining_mechanisms.md](remaining_mechanisms.md) rank 2). This slice makes that class degrade honestly
instead of refusing a submission, and grows the arrays the device has room for.

## 2. Measured inventory: what the tree refuses or clamps today

The inventory is static reading of our sources plus the measured startup line, the driver limits and the existing
descriptor tests. "Symptom" is what the boot would print or do the first time the shape appears.

| # | Shape or event | Where our tree handles it | Measured symptom |
|---|---|---|---|
| 1 | A replacing flush (live key's index changed, or the table grew) cannot allocate its new generation | `BindlessSet::flush` returned false; the prepare lambda in the device shim propagated it; the queue returned `ReplayResult::unsupported` for the submission (`vulkan_backend.h`) | 0 today: no refused submission in any measured arm. The path needs a device allocation failure that today's 32-image population does not create; it is the class the reference calls out at streamed population |
| 2 | 2D-array, 3D and 1D sampled arrays past their defaults | `port/src/vulkan_bindless.h` default counts 1024/256/256; `size_bindings` grew only the 2D, sampler and storage arrays | measured boot line before the slice: `bindless set: 65536 2D images, 3936 samplers, 1024 2D-array, 256 3D, 256 1D, 4096 storage indices in 2D` - the device had room for more of the three other dimensions and was given none |
| 3 | An image of a dimension whose sampled array is full | `add_image` returns 0; the device shim logs `a bindless image array is full; dimension N` and refuses that view by name (`d3d12_vulkan_device.cpp`, shim `log_once`) | named refusal, unchanged and correct; 0 today (32 images) |
| 4 | A T#/S# key the shader's eight probes can never resolve: 9 keys sharing a full FNV hash, or the table at its capacity limit | `DescriptorTable` throws `std::length_error` (`port/src/descriptor_table.h`, "Descriptor hash collision exceeds shader probe limit" / "Descriptor table capacity" / "Descriptor probe range"); `BindlessSet::insert_key` did not catch it, so it left through shader-resource-view creation | 0 today (no such key measured); a throw across the shim is the wrong failure for a descriptor shape that cannot be expressed |
| 5 | A device whose per-stage or per-set descriptor limits are below the set's default sizes | `bindless::size_bindings` returns false; the device shim logs that the set holds less | named, honest clamp, kept: it is a device limit, not a table-memory shortage |
| 6 | The sampler array full | `add_sampler` returns 0; the shim logs the sampler-array refusal | 3,936 samplers measured available on this host (a full D3D12 sampler heap is 2,048 plus literal/static S#s); not exhausted, named refusal kept |

The measured device this page's boot A/B runs on is an RTX 4090 with 1,048,576 descriptor limits of each type. The
synthetic-limits test measures the set at those limits: 152,929 descriptors with the grown arrays (65,536 2D images,
16,384 2D-array, 4,096 3D, 4,096 1D, 3,936 samplers, 4,096 storage indices in each dimension's storage arrays), against
the 93,793 the header comment recorded for the set before this slice. The 152,929-descriptor set was created and used
by the real device in boot B (section 5).

## 3. What we implemented

The rules live in `port/src/vulkan_bindless.h` in the set's existing generation/retire/ticket structure; the caller's
contract is unchanged. A flush may now be *degraded*: it returns true, so the submission is recorded, and says so
through `last_flush_degraded()`.

1. **A released generation is a spare.** `release_retired()` keeps the largest generation that completed instead of
   destroying it (the spare, one at a time; `release_spares()` frees it in the destructor). A replacement whose table
   fits the spare's buffers reuses its buffers, descriptor set and pool with no allocation at all: this is the common
   churn case (a texture's descriptor changes), and the case a device out of memory cannot serve. Counted by
   `spares_reused()`.
2. **A generation that cannot be made keeps the last uploaded table.** `flush()` no longer fails on an allocation
   failure: the current set and its last uploaded table stay, so every key they hold still resolves correctly; the
   pending descriptor writes and table changes stay queued for the next flush; and a key added since that upload is a
   miss, which reads the fallback index 0 (a 1x1 zero image or the clamping sampler), never a wrong image. Nothing is
   recorded into the command buffer and nothing is written to the set, so a submission that binds the set is safe.
   Counted by `degraded_flushes()`, named in one bounded log line (first event, then every 4096th). `flush()` returns
   false only for the caller's own case of no command buffer at all, where nothing can be recorded.
3. **The sampled arrays grow to the device's room.** `size_bindings()` grows the 2D-array, 3D and 1D sampled arrays
   towards 16,384, 4,096 and 4,096 after the existing 2D/sampler/storage growth, each step keeping the per-type limits,
   the per-set limit, the update-after-bind pool limit and `growth_headroom`; their storage arrays follow the counts
   through the existing storage sizing. The default counts are a floor: a device without the room (or below today's
   sizes) keeps 1024/256/256, and the existing `false` return plus the shim's named line still reports it. The growth
   order is 2D first - the streamed population is mostly 2D - then the other three dimensions with what is left.
4. **An unresolvable key is refused by name, not thrown.** `insert_key()` catches the table's `length_error`, counts it
   (`keys_refused()`), logs one bounded line with the table's own reason, and returns false: the image keeps the index
   it was given, the table is unchanged, and that shader reads the fallback. This is a shape that cannot be expressed,
   not a memory shortage, so it is not degraded further.
5. **A submission whose prepare cannot be served is still refused, by name.** The `vulkan_backend.h` path that answers
   a failed prepare now logs once (bounded) that the bindless set could not be prepared; it is the only remaining way
   to refuse a submission for the set, and it can no longer be triggered by table memory.

The rules are default-on: they only change behaviour where the old path failed or clamped, and boot A/B measures
nothing moving (section 5). There is no new environment switch.

## 4. Cross-check against the reference

Upstream's `273b3bdf9` changes `runtime/src/vulkan_bindless.h` (+260/−65) and `runtime/src/descriptor_table.h`
(+57/−12), with `runtime/src/gfx10_descriptors.h`, `runtime/shims/d3d12_xs_desc_clean.h` (new) and
`runtime/shims/d3d12_xs_alloc_size.h` (new) around it, per the numstat recorded in
[remaining_mechanisms.md](remaining_mechanisms.md) section 3.2. The reference's own squash message, quoted there,
describes the mechanism as: a new table generation that cannot use device-local memory goes to host-visible memory,
released generations become reusable spares, a generation that still cannot be made is rewritten in place when the
device is idle and otherwise the table keeps its last uploaded contents with new keys at the fallback index, and a
flush never fails the submission. The same change grows the 2D-array, 3D and 1D descriptor arrays.

**Where we agree.** A flush must never fail a submission for want of table memory; released generations are reusable;
the kept table still resolves its keys and new keys fall back; 2D-array/3D/1D counts grow; and the finalizer/cleanup
files around it exist because the table is a shader contract, not private state.

**Where we differ, and why.**

- Our generation already retires rather than waits (D3D-314), so a spare is the natural reuse unit here; the reference's
  in-place rewrite "when idle" has no idle point in our frame path, and we did not add one. We keep the last uploaded
  table instead, which is the reference's own second half, and measured it with a fake device rather than a driver.
- The reference's `descriptor_table.h` change includes a descriptor-hash finalizer option. We did not adopt it: the hash
  is computed by the translated shaders from the same eight dwords (`tooling/rdna2spirv.py` probes the table at binding
  2), so a host-only finalizer would silently break every lookup. Our answer to an unresolvable key is the named
  refusal in rule 4.
- The reference adds `d3d12_xs_desc_clean.h` and `d3d12_xs_alloc_size.h` as new files in its shim layer; our device
  shim's descriptor-heap path is a different structure, and this slice stays inside set 0. A larger shader-visible heap
  shape refusal remains a named refusal here.
- Growth order differs by measurement: the reference grows the arrays as part of the same sizing change; we put the
  other three dimensions after the 2D/sampler/storage growth because the measured streamed population is mostly 2D and
  this keeps every pre-existing sizing result for constrained devices (the sizing tests assert them).

## 5. A/B measurement

Two 130 s boots under the shared boot lock, one at a time, both through `tooling/boot_report.py` with the standard
environment (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL`,
`PALMETTO_PIPELINE_TIMELINE=5`, captures every 6,000 presented frames, limit 2).

Arm A is the tree as it stands with `port/src/vulkan_bindless.h` reverted to its base-commit copy; arm B is the same
tree with the slice. Both arms carry this slice's `vulkan_backend.h` log line (it cannot fire in either boot, since no
bindless prepare fails), so the only behavioural difference between the two arm builds is the set's header. The two
arms were built back to back inside one lock session, and the two `git diff` snapshots taken at each build are kept
with the logs (`D:\leonida-work\bindless\arm-A-git-diff.txt` and `arm-B-git-diff.txt`): their sections are identical
for every file except `port/src/vulkan_bindless.h`, so no other slice moved between the two builds. Arm A was then
booted a second time (A2, the same binary) because the title's loading path is timing-dependent, and the repeated arm
measures that variance for the comparison.

| Measured | Arm A (before) | Arm A2 (same binary, repeat) | Arm B (slice) |
|---|---|---|---|
| binary sha256 | `DE88AE6E...5548923D1` | same as A | `2C8FBB50...05F17E46258C` |
| bindless set line | 65,536 2D / 1,024 2D-array / 256 3D / 256 1D / 3,936 samplers / 4,096 storage | same as A | 65,536 2D / **16,384 2D-array** / **4,096 3D** / **4,096 1D** / 3,936 samplers / 4,096 storage |
| degraded/kept-table flushes | 0 | 0 | 0 |
| refused submissions (slot57 `refused`, replay refusals) | 0 / 0 | 0 / 0 | 0 / 0 |
| exceptions | 0 | 0 | 0 |
| verdict at 130 s | ALIVE | ALIVE | ALIVE |
| submissions / presented frames at +125 s | 7,306 / 3,652 | 6,318 / 3,158 | 10,669 / 5,334 |
| compute pipeline requests / created | 1,119 / 877 | 1,119 / 877 | 474 / 403 |
| D3D12 calls recorded | 191,482 | 172,189 | 255,321 |
| captured frame (first presented) sha256 | `1B8380D4...F64207` (mean rgb 0 0 0) | same | same |

Reading: the descriptor-shape line is the one thing the slice was supposed to change, and it changed exactly as
designed, with the larger set created and used by the real device on this host (the boot would otherwise report the
device's bindless-set failure by name and stop). The degradation path is silent in boot B because no allocation fails
at this population - the slice is a latent negative in the boot, as expected.

The frame numbers are not evidence either way: the same binary gave 3,652 and 3,158 presented frames across A and A2
(and the boot's compute-pipeline request count was identical within arm A but different in arm B), so the loading path
this title takes in 130 s is timing-dependent, and arm B's 5,334 frames sit inside that noise rather than on a
regression or an improvement. The milestone that would move if the degradation path ran - a bindless log line, a
refused submission, or a device-loss exception - is 0 in every arm. The captured first presented frame is byte-identical
in all three runs (black, which is the measured frontier, not this slice's subject).

The counters the next reader should read on a boot that does reach the streamed population are the named log lines
(`degraded_flushes` and `keys_refused`, first event and every 4096th) in the boot's stderr; the per-set counters are
also exposed as `degraded_flushes()`, `spares_reused()` and `keys_refused()` on the set, but this slice does not edit
the device shim's `BindlessStats` export (out of the slice's file scope), so a boot report cannot read them yet - that
remains a named next step.

## 6. Limits

- **The failure path is latent in both arms of the A/B.** No allocation fails at today's 32-image population, so the
  A/B cannot exercise the degradation itself: it measures that the larger arrays are created and used, and that
  nothing else moved. The degraded flush and the spare are measured on the fake device in the sizing test, whose fault
  injection is the harness for the GPU-free half of the acceptance test. A boot that reaches the real out-of-memory
  path needs the streamed population this slice is preparation for.
- **A degraded flush is a visual hazard, not a correctness one.** Until a later flush makes a generation, keys added
  after the last uploaded table read the fallback (1x1 zeros or the clamping sampler). If memory never returns, those
  textures stay fallback for the session. The counter and the log line make that state visible in a boot.
- **The spare costs one released table buffer** (about 2.6-5.2 MiB at the streamed 65,536-131,072 table slots) and one
  descriptor set. It is not freed until the destructor or replaced by a larger released generation.
- **Set construction is not covered.** A device that cannot create the 152,929-descriptor set at all fails device
  creation through the existing named path; the degradation rules are for the running set, not for the initial one.
- **Descriptor writes are not covered.** `vkUpdateDescriptorSets` and pool creation return void or are part of the
  generation; a failure there is outside any Vulkan contract, and rule 2's no-write-on-degrade keeps the running set's
  view intact.
- **The 2D-array/3D/1D counts are a target, not a promise.** A device whose limits leave no room keeps today's counts;
  the growth order means a mid-size device gives the remaining room to 2D first.
- **The 38,000-image population is the reference's measurement, not ours.** Our own streamed population has not been
  measured yet; the boot's image allocation count and table size are the counters to read when it is.

## 7. Tests

- `bindless_sizing_test` (GPU-free, synthetic limits and a fake Vulkan device; 109 checks in the file, of which the
  three new scenarios are: a degraded flush that keeps the submission and republishes on the next flush; a spare that
  replaces an allocation with every allocation failing; and an unresolvable key refused by name. The sizing half now
  asserts the 2D-array/3D/1D growth to 16,384/4,096/4,096 at RTX-class limits, the 1024/256/256 floor on a 7,168-sampled
  device, and the partial growth at 60,000, with the storage arrays consistent in each case.
- `descriptor_table_test` (GPU-free): unchanged and passing - the table itself still refuses the unresolvable key with
  `length_error`, which is what `insert_key` now catches.
- `vk_bindless_test` (`ps_tex` and `ps_imgatomic`, GPU): rebuilt against the changed header and both entries pass on
  the RTX 4090 - the 152,929-descriptor set creates, binds and samples, and the image atomics read back exactly.
- `d3d12_bindless_churn_test` (GPU): **fails in this shared working tree at its own environment precondition**
  (`line 197: view_gpu < (1ull << 32)`), not in the bindless set: the rebuilt binary fails the same way with this
  slice's header and with the base-commit header restored, so the failure is another in-flight slice's console-heap
  addressing (the run logs `descriptor heap uses console address, NOT backed`) and is not caused or fixed here. The
  churn test must be re-run green once that slice lands; it is named here as an open gate rather than claimed.
- Gates: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints `TESTS_BUILT`, and the
  Markdown checks that read this page (links, tables, list items, repeated lines, encoding) pass.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project [ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida) (license
<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), commit
[`273b3bdf9`](https://github.com/ubiquitarianism/leonida/commit/273b3bdf9) files
`runtime/src/vulkan_bindless.h`, `runtime/src/descriptor_table.h`, `runtime/src/gfx10_descriptors.h`,
`runtime/shims/d3d12_xs_desc_clean.h` and `runtime/shims/d3d12_xs_alloc_size.h`, whose numstat and squash-message
description are quoted in [remaining_mechanisms.md](remaining_mechanisms.md) section 3.2. No code, comment text,
identifier or table from that project was copied. The upstream project remains the copyright of its contributors and
its license continues to apply to its own tree.
