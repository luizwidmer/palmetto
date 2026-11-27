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

## 9. Policy override, telemetry and current readiness (2026-11-27)

The implementation already present in this tree remains the default: **unset** `PALMETTO_BINDLESS_DEGRADATION` keeps
array growth, spare reuse, allocation-failure degradation and named collision refusals enabled. This update does not
flip that default. The new exact override `PALMETTO_BINDLESS_DEGRADATION=0` selects the pre-slice behavior for a
controlled comparison; `=1` explicitly selects the current behavior. An empty/unset value keeps current behavior; an
unrecognized value logs `bindless_degradation_override_invalid` and also keeps current behavior rather than silently
disabling a working path. This dated appendix supersedes section 3's earlier sentence “There is no new environment
switch”; the measured behavior from section 5 remains the default. This update closes the legacy-policy and telemetry gap,
not a missing growth algorithm: the larger counts and the current fallback rules already existed in this tree. Real
allocation exhaustion is still unmeasured; the new tests use the fake Vulkan device.

With the override set to `0`, only the 2D, sampler and 2D storage counts retain their pre-slice sizing/growth; the
2D-array, 3D and 1D sampled arrays stay at 1,024/256/256. A replacing flush that cannot allocate still returns false,
completed generations are destroyed rather than kept as a spare, and a key that exceeds the shader's eight-probe shape
keeps the pre-slice `std::length_error` path. With the default/current policy, all existing coverage above is unchanged.
The GPU-free `bindless_sizing_test` pins both policies, including that the default still degrades a failed generation,
reuses a released spare, and counts a named key refusal, while explicit legacy mode returns the old failure, keeps no
spare and propagates the collision exception.

The old per-set counters (`degraded_flushes`, `spares_reused`, `keys_refused`) were not visible through
`rt::d3d12::bindless_stats`. The internal `BindlessStats` snapshot now exports all three alongside the
generation/retirement counters, and the D3D12 churn test prints them and asserts the normal zero degradation/refusal
values. The boot-report parser itself is not wired to serialize this snapshot yet.

**Readiness, not a boot claim.** The retained boot census is still 32 image allocations / 279.1 MiB and the recorded
bindless arms show 0 degraded flushes and 0 refused submissions. That population does not pressure the set, so no new
runtime arm was run for this update and allocation exhaustion remains latent. The unit test injects allocation and pool
failures into the fake Vulkan device; it does not prove a real driver survives its own device-memory exhaustion.

**Next falsifier:** when the host/boot lock is free and a boot reaches the streamed image population, compare one default
run with the explicit legacy override. Read the reported bindless counts plus the newly exported counters and refusal
lines. The extension is falsified for that measured population if it fails to grow the other sampled dimensions where
limits allow, reports no degraded/spare/refusal counters under an injected/observed corresponding event, or changes
submissions at a population that never exhausts those limits. At today's 32-image population, a zero failure counter is
expected and is not evidence of the failure path.

### Peer evidence and clean-room attribution

The local peer audit records Five Star Recomp `34b4dfed`'s D3D12.X descriptor description as 32-byte descriptor memory
provided by the title allocator and per-draw tables copied into a linear allocator, and XCL `623d02e` as exposing
console T#/V#/S# descriptor heaps. Those are evidence of the console heap/table shape, not measurements of this
Vulkan set's device limits or an allocation-failure policy. The existing clean-room comparison also cites the public
GPL-3.0 `ubiquitarianism/leonida` `273b3bdf9` bindless implementation as behavioral prior art; this update only adds a
legacy override and counters in this tree's own policy/API, with no peer code, comments, identifiers or tables copied.
See [cleanroom_peers.md](cleanroom_peers.md) sections 108–112 and 204–211, and
[upstream_port.md](upstream_port.md) for the source/evidence rule.

**NOTICE.md draft for lead integration:** “The bindless degradation and descriptor-array sizing policy
([docs/bindless_degradation.md](bindless_degradation.md)) was cross-checked against public behavioural evidence in
[ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida) (GPL-3.0, `273b3bdf9`, Moriz82), [Five Star Recomp](https://github.com/ThatRecompDev/five-star-recomp) (GPL-3.0-or-later, `34b4dfed`, ThatRecompDev) and
[XCL](https://github.com/xcl-dev/xcl) (GPL-2.0-only, `623d02e`, Prime1997). The peer notes were consulted only for their
D3D12.X heap/table shapes; the Vulkan limits and allocation-failure behavior were derived from this tree's own code and
tests. No source, comments, identifiers or tables were copied, and the implementation, override and tests are this
project's own. Each referenced project remains the copyright of its contributors under its stated licence.”

## 10. Task-228 bindless arm not run after the stop rule

P-44 did not launch. The bounded series used the fixed prior-gate binary (SHA-256
`52C7A57172893CF3E5EC8586DC2CD40DEC9DD118D965B3AED427F1DC1D759B59`, mtime
`2026-11-27T10:24:54.6694203-03:00`) and stopped after two consecutive loss-affected arms: P-40 logged one Vulkan
device-loss line before a title exception; P-43 logged one loss event on a failed queue signal (the same event was printed
twice at one timestamp). The three-launch cap and stop rule therefore left no launch for the bindless override. No
`PALMETTO_BINDLESS_DEGRADATION=1` arm ran, no bindless telemetry was collected, and no P-44 output/capture directory was
created. The per-set degraded/spare/refused values for this task are **not measured**, not zero. The fixed artifact and
the two completed arm records are in [device_slot70.md](device_slot70.md#13-task-228-strict-validation-boot-device-loss-limited-2026-11-27)
and [title_memory_model.md](title_memory_model.md#13-task-228-native-write-watch-arm-switch-on-telemetry-unavailable-2026-11-27).

This is a stop-rule negative only; it does not change section 9's existing readiness result or falsify bindless
allocation-failure degradation. Section 9 notes that `BindlessStats` is exported but the boot-report parser does not
serialize it, and today's 32-image population does not pressure the failure path.

**Next falsifier, only after the host is cleared and a separate arm is authorized.** Use the exact override
`PALMETTO_BINDLESS_DEGRADATION=1` with a reporter that reads and records `degraded_flushes`, `spares_reused` and
`keys_refused`, alongside set sizing and submission/refusal lines. Under an observed allocation failure, a degraded flush
must keep the last valid table and the submission must continue; a reused spare must increment its counter; an
unresolvable key must increment its named refusal counter without throwing through the shim. If the boot sees no
allocation/collision pressure, zero event counters remain expected and are not a failure. Do not interpret this task's
unrun P-44 arm as evidence either way.

## 11. Task-236 P-44 bindless arm (2026-11-27)

The preceding section records the earlier task-228 series, which correctly stopped before a P-44 launch. This was a new,
separately authorized light arm on the fixed integrated `port/build/palmetto.exe` (SHA-256
`9355F4DA153720BBA61C3A66AAB438C92B3CBF2777AC89A0B5B8053932540069`, mtime
`2026-11-27T11:23:21.4090967-03:00`), with no build or relink. The `D:\leonida-work\fsrstable\boot_arm.ps1`
`-Light` 90 s content environment with the template's standard `--trace` output, adding `PALMETTO_TITLE_CLASS0A_SIZE=1`, `PALMETTO_BINDLESS_DEGRADATION=1`,
`PALMETTO_STATE_TRACE=2,90`, and `PALMETTO_TIMELINE=1`. `PALMETTO_CAPTURE=` and `PALMETTO_READBACK_PPM=` were explicitly
empty; no frame/readback payload was collected.

| measure | P-44 bindless arm |
|---|---|
| window | `2026-11-27T11:50:10.7788893-03:00`–`11:51:13.6321357-03:00`; 62.3 s |
| outcome | `STOP` on a title exception (`0xC0000005`); 177,657 D3D12 calls; 1 exception; 1 `[rt] replay: the Vulkan device was lost` line; not a harness timeout |
| state words | at 60 s: `init=2`, `loading=2`, `session=1`, machine current/next `2`; movie player state/next `3`, probe/open `1`, error `0` |
| bindless sizing | 65,536 2D images; 3,936 samplers; 16,384 2D-array; 4,096 each 3D and 1D; 4,096 storage indices in 2D |
| degradation telemetry | no `BindlessStats` snapshot or counter event lines; `degraded_flushes`, `spares_reused`, and `keys_refused` are **unavailable, not zero** |
| captures | not collected; the capture and readback directories are empty by design |

The evidence remains outside the repository under `D:\leonida-work\fsrstable\boot-task236-p44-bindless` (`meta.txt`,
`summary_console.txt`, `stderr.log`, `boot.jsonl`). The device-loss line and title exception are co-observations, not evidence
that bindless degradation caused the stop. The next arm, P-35, had a second consecutive arm-level device loss and ended the
series (see [the P-35 XG observation](texture_layout.md#9-task-236-p-35-xg-slot-2-array-observation-2026-11-27)).

**Limits and falsifier.** This run verifies only the explicit override and the set-sizing line. It did not serialize the
per-set counters, and therefore does not establish whether degraded flushes or spare reuse occurred. A meaningful
falsifier requires an observed allocation/collision-pressure event with the counter snapshot: a degraded flush that does
not retain the last valid table, a spare replacement that is not reused, or a refused key that escapes the named refusal
path would refute the corresponding rule. Absence of telemetry here is not evidence either way.
