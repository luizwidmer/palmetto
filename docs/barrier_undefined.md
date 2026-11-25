# The release barrier: an enhanced texture barrier into LAYOUT_UNDEFINED

**Status: a measured latent gap, closed; the A/B boot is a measured negative.** Our recorder used to refuse the
enhanced texture barrier whose target layout is `D3D12_BARRIER_LAYOUT_UNDEFINED` from a defined layout. D3D12 allows
it - the contents are no longer needed, as before the memory is aliased - and WARP closes such a list; this machine
measured that directly through the close-parity test. The refusal was a latent fatal-`Close` class: a list that
recorded the shape closed with `E_INVALIDARG`, which fails the whole `ExecuteCommandLists` submission. Neither our 600
s boot nor either 130 s arm of this slice reaches the shape, so the boot verdict is unchanged; the slice removes a
dropped submission that the reference's boot meets on its way through the story load, and keeps every genuinely
invalid texture-barrier call refused by name.

## 1. Why this case is worth closing

- The public reference's story-load report lists, among the stops it hit, "enhanced barriers into `LAYOUT_UNDEFINED` (a
  failed `Close` the title treats as fatal)", and the fork's commit `37c5af12a4` says the retail title ends a depth
  buffer's use this way. A failed `Close` is not local to one list: the whole `ExecuteCommandLists` call fails, so one
  release barrier silently drops a submission.
- Our recorder carried the same prohibition: `CommandList::barrier(const TextureBarrier&)` failed the call when
  `layout_after == d3d12_layout::undefined` and `layout_before != d3d12_layout::undefined`
  ([command_list.h](../port/src/command_list.h)). The caller is the COM layer's `ID3D12GraphicsCommandList7::Barrier`,
  which marks the list invalid on a refused call, and `Close` then returns `E_INVALIDARG`; the D3D12 trace prints
  `ID3D12GraphicsCommandList::Close failed` for it.
- It is **latent in this tree today**: the shape is not emitted before the stationary frame loop. Section 2.2 shows
  110,956 `Close` calls and 0 failed in the 600 s boot, so no barrier of any kind was refused in that window. The
  class sits on the streamed-content path the reference travels after its story load, which this tree does not reach
  yet ([long_boot.md](long_boot.md), [streamed_consumer.md](streamed_consumer.md)). The cost of closing it now is one
  rule and one replay branch; the cost of meeting it later is a dropped submission with no diagnostic naming the
  barrier.
- There is also a correctness argument independent of the reference: on this machine, Windows' own D3D12 runtime over
  WARP closes a list that records this barrier (section 2.3), so refusing it was a parity gap with the platform.

## 2. Measured shapes

### 2.1 The recorded shape and the rule that refused it

`D3D12_TEXTURE_BARRIER`, all subresources (`IndexOrFirstMipLevel = 0xffffffff`, no mip/layer/plane counts), flags 0.
Values are the public `d3d12.h` numbers this tree's IR carries; the first row is the shape the reference names for a
depth buffer's release, the second is the generic colour-texture row the close-parity test also measures.

| field | depth release | texture release |
|---|---|---|
| `SyncBefore` | `DEPTH_STENCIL` 0x20 | `ALL` 0x1 |
| `SyncAfter` | `NONE` 0 | `NONE` 0 |
| `AccessBefore` | `DEPTH_STENCIL_WRITE` 0x20 | `COMMON` 0 |
| `AccessAfter` | `NO_ACCESS` 0x80000000 | `NO_ACCESS` 0x80000000 |
| `LayoutBefore` | `DEPTH_STENCIL_WRITE` 4 | `COMMON` 0 |
| `LayoutAfter` | `UNDEFINED` 0xffffffff | `UNDEFINED` 0xffffffff |

The removed rule was exact: `layout_after == undefined && layout_before != undefined` returns false. A false return
from the recorder is not a no-op - the COM layer calls it through `check(...)`, sets `invalid_`, and the list's
`Close` reports `E_INVALIDARG` (the shim's `Close` maps an invalid recording to `E_INVALIDARG`, where the PC runtime
would also use `E_FAIL`). One such barrier fails all lists in that `ExecuteCommandLists`.

### 2.2 What our boots emit and refuse

The boot's D3D12 trace records call names and outcomes, not barrier arguments, so the shape cannot be counted
directly from it; the refusal count can, because a refused barrier is always reported at `Close`. The count of the
shape that was *emitted and refused* is therefore the count of failed `Close` calls that the trace attributes to this
rule - zero, in every window measured. One 600 s boot (the evidence of [long_boot.md](long_boot.md), re-read for this
slice) and the two 130 s arms of this slice:

| measurement | 600 s boot | A: base 130 s | B: slice 130 s |
|---|---:|---:|---:|
| `[d3d12-trace]` calls | 2,144,626 | 534,579 | 545,599 |
| `ID3D12GraphicsCommandList7::Barrier` calls | 275,830 | 63,647 | 65,061 |
| `ID3D12GraphicsCommandList::ResourceBarrier` calls (legacy) | 0 | 0 | 0 |
| `Close` ok | 110,956 | 26,084 | 26,649 |
| `Close` failed | **0** | **0** | **0** |
| `ExecuteCommandLists` ok / failed | not counted / 0 | 25,458 / 0 | 26,023 / 0 |

So the release shape was **never emitted** in the sampled windows: any emission would have been refused by the old
rule, and no list failed to close. The split of the 275,830 enhanced `Barrier` calls between global, buffer and
texture groups is not in the trace; a texture-barrier counter would be the instrument to add if a later slice needs
the shape mix, and this slice does not add one (it would be a second, unrelated switch).

### 2.3 D3D12's verdict, measured on this machine's WARP

`port/test/d3d12_close_parity_test.cpp` records one call on a fresh list of this tree's D3D12 layer and the same call
on a live WARP device, then compares `Close`. Three rows were added: a defined-to-defined depth transition as a
control, and the two release shapes above. The live WARP column was measured on this machine (Windows 11 26200) on
the pre-slice layer and again on the post-slice layer; the test uses a live WARP answer when it can make one, and the
`warp_ok` column is the Windows 11 WARP measurement the file records.

| case | layer before the slice | layer after the slice | live WARP | expected after |
|---|---|---|---|---|
| depth `DEPTH_STENCIL_WRITE -> COPY_SOURCE` (control) | `S_OK` | `S_OK` | `S_OK` | match |
| depth `DEPTH_STENCIL_WRITE -> UNDEFINED` | `E_INVALIDARG` | `S_OK` | `S_OK` | match |
| texture `COMMON -> UNDEFINED` | `E_INVALIDARG` | `S_OK` | `S_OK` | match |

Pre-slice the test passes with those two rows marked `layer_stricter` (116 cases, 83 match, 18 known layer-stricter,
15 known layer-looser, 0 failures); post-slice the same rows are `match` (116 cases, 85 match, 16 known
layer-stricter, 15 known layer-looser, 0 failures). This is our own measurement of the rule, not only the reference's
report.

## 3. What we implemented

Two files, in our own structure (no code from the reference was copied):

- `port/src/command_list.h` - the rule goes. The texture-barrier recorder now refuses only the named invalid cases:
  a barrier with no resource; a barrier whose two sync halves are both `SPLIT`; `Flags` other than
  `D3D12_TEXTURE_BARRIER_FLAG_DISCARD`; a mip-count range that names no layers or planes; a closed list. A comment on
  the function states the measured reason `LayoutAfter = UNDEFINED` is accepted, so the next reader does not restore
  the prohibition.
- `port/src/vulkan_replay.h` - the `TextureBarrier` branch of `emit` learns the **release**: Vulkan has no transition
  into `UNDEFINED` (`VUID-VkImageMemoryBarrier2-newLayout-01198`), so each piece of the range is emitted in the layout
  the tracker already has for it (`oldLayout == newLayout`) and the barrier only orders the accesses
  (`SyncBefore`/`AccessBefore` to `SyncAfter`/`AccessAfter`). The tracker keeps those layouts. A piece still in
  `UNDEFINED` was never used: it gets no barrier and is not zero-filled for the release. If every piece was unused the
  `vkCmdPipelineBarrier2` call is skipped entirely. The later barrier whose `LayoutBefore` is `UNDEFINED` takes the
  existing discard path, which tells the driver the contents may go - the same behaviour as before this slice, and the
  reason the tracker is deliberately not moved to `UNDEFINED` here.
- The default path is otherwise unchanged: a discard (`Flags & DISCARD` or `LayoutBefore = UNDEFINED`), a
  defined-to-defined transition and every other barrier path record and replay exactly as before. The A/B in section 5
  is the check that the other counters do not move.

Not touched: `UNDEFINED` to `UNDEFINED` (target `UNDEFINED` from `UNDEFINED`) still goes through the discard path and
would emit `oldLayout = newLayout = UNDEFINED`; that is the reference's separate open case (#600), it was not part of
`37c5af12a4` there either, and no boot of ours emits it (zero failed `Close`). It remains a named gap in section 6.

## 4. Cross-check against the reference

The reference is the public GPL-3.0 project `ubiquitarianism/leonida`. Consulted: `runtime/src/command_list.h`,
`runtime/src/vulkan_replay.h`, `docs/d3d12_vulkan.md` and the three test files it extends, at commit `37c5af12a4`
(milk4sale, co-authored Moriz82), the topic commit of PR #1055 merged as `cdb003ea19` (aslan2142); fork HEAD when this
page was written was `ed94fbef9`.

| Topic | Reference | This tree | Why |
|---|---|---|---|
| recorder rule | removes the same `LayoutAfter = UNDEFINED` refusal; comments that the retail title releases a depth buffer this way and WARP closes such a list | same rule removed, with the measured reason beside the remaining named refusals; `port/test/command_list_test.cpp` records the release and the still-refused cases | **Agree.** The measurement that matters (D3D12 allows it, WARP closes it) is reproduced here in section 2.3, so the rule is not taken on trust |
| replay | each piece gets `oldLayout = newLayout` (the tracker's layout), the tracker keeps the layouts, an unused piece gets no barrier and no zero-fill, and the next barrier from `UNDEFINED` discards | same semantics, written in this tree's `emit` with our `LayoutPieces`/`LayoutTracker` and the existing discard path; guarded so an all-unused release emits no empty dependency | **Agree on behaviour; differ in structure and in when the empty `vkCmdPipelineBarrier2` is skipped.** Our guard is explicit because `barrier_count` can now be 0 where it never could before |
| scope | `UNDEFINED` to `UNDEFINED` is explicitly a separate case (#600), not handled there | not handled here either; the discard path still produces `UNDEFINED -> UNDEFINED` for that shape | **Agree to leave it out.** It is a recorded limit, not an omission by accident |
| tests | command-list case, three close-parity rows, CPU replay cases (kept layouts, discard flag, no tracker, unused image, following `UNDEFINED` barrier), `vk_command_replay_test` part K and a `d3d12_com_test` readback on the GPU | command-list cases, three close-parity rows, two CPU replay sections (kept layouts over mips; unused image, no barrier call, following `UNDEFINED` barrier); no GPU end-to-end test | **Differ in test cost, not in the rule.** The GPU readback (release, reuse from `UNDEFINED`, read back) is the next falsifier named in section 6 |

## 5. A/B measurement

Two 130 s boots under the boot lock, one at a time, through `tooling/boot_report.py --timeout 130`, with the standard
set `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, plus
`PALMETTO_PIPELINE_TIMELINE=5`, `PALMETTO_LOADER_HOOK=1`, `PALMETTO_DSTORAGE_TRACE=1` and presenter captures
(`PALMETTO_CAPTURE`, every 6,000 frames, 2 frames).

- **A** is `palmetto` built from the committed base `4c4efb68c` in a clean tree: SHA-256
  `886447EA017057F5661275AB4C1CAE979A3F15C007B8CE37896D3649488628FE`.
- **B** is the same tree and the same build with only `port/src/command_list.h` and `port/src/vulkan_replay.h`
  replaced by this slice: SHA-256 `E62949E812B3ECD3C4C279CBE94FA353A329D0A40DDB9D91822CE6FAEE66BDD4`. Both binary
  snapshots carry their own copy of `palmetto_tls_host.dll`, as the runtime expects.
- The arms are built from the commit, not from the live working tree, because at measurement time the working tree
  carried other slices still being measured; that build stopped at ~28-40 s with an access violation in title code and
  could not be the baseline of anything. The clean-tree pair differs only in the two files above, which is the
  isolation this slice's claim needs. The limit is recorded in section 6.

| measurement | A: base | B: slice | verdict |
|---|---|---|---|
| verdict, wall time | `ALIVE 130.2 s`, 0 exceptions | `ALIVE 130.2 s`, 0 exceptions | unchanged |
| startup-trace events | 19,429 | 19,837 | within run variance |
| `[d3d12-trace]` calls | 534,579 | 545,599 | +2.1%, run variance |
| enhanced `Barrier` calls | 63,647 | 65,061 | +2.2%, run variance |
| `Close` ok / failed | 26,084 / 0 | 26,649 / 0 | **0 refusals in both; the shape was not emitted** |
| `ExecuteCommandLists` ok / failed | 25,458 / 0 | 26,023 / 0 | no dropped submission in either arm |
| pipeline population at +125 s | slot57 10,241 (0 refused), slot55 41,545, graphics 51,786, compute 95/95 | identical | unchanged |
| loader objects at the 60 s summary (begin/end) | 1/1, 16/16, 1/1, 1/1 | 1/1, 15/15, 1/1, 1/1 | same dormant state (spinner depth 0); the one-object difference is run variance |
| DirectStorage enqueue prints (bounded at 8 per site) | 8 | 8 | no new request class |
| presents / submits / console calls at +125 s | 12,548 / 25,096 / 294,699 | 12,791 / 25,583 / 298,351 | +1.2-2.1%, run variance |
| presenter captures | frames 1 and 6,001, both 2564x1421, mean rgb 0.00 0.00 0.00 | the same two frames, both black | unchanged |

**Verdict: unchanged, a measured negative on the boot question.** The slice changes no reachable behaviour of a 130 s
boot, which is what its own evidence predicts: the shape is not emitted there. It does change the `Close` verdict for a
list that records the shape, and that is measured directly in section 2.3 rather than inferred from the reference.

## 6. Limits, and the next falsifier

- **No boot of ours reaches the shape**, so this slice's A/B cannot show a boot difference and does not claim one. The
  positive evidence is the WARP parity measurement (section 2.3) plus the CPU replay test; the negative is the A/B.
- **The replay is validated on the CPU, not end to end on the GPU.** `replay_texture_barrier_test` records the
  barriers `emit` would issue over a tracked image, but no GPU run releases a depth buffer and reuses it. The next
  falsifier is one GPU test of exactly that: a depth buffer cleared, released into `UNDEFINED`, taken from `UNDEFINED`
  again, cleared and read back (the reference's `d3d12_com_test` case; its `vk_command_replay_test` part K is the
  colour-target form). If the Vulkan driver rejects or corrupts the `oldLayout == newLayout` release, that test fails.
- **`UNDEFINED` to `UNDEFINED` (#600) is still the old discard path**, which would emit `UNDEFINED -> UNDEFINED` in a
  Vulkan image barrier. It is the reference's separate open case and no boot of ours emits it; it is not repaired
  here and is the named adjacent gap.
- **The empty `vkCmdPipelineBarrier2` skip** is a small behavioural difference from the reference's first version; the
  CPU test pins it (an all-unused release makes no call), but a driver that would tolerate the empty call is not what
  this slice measures.
- **The A/B arms are clean-tree builds of the commit, not the merged tree.** When the lead merges this slice with the
  other in-flight slices, the pair should be repeated on the merged tree; the isolation above is what makes the
  current numbers attributable to these two files.
- The 600 s boot and the 130 s arms use a trace that records call names only, so the shape mix of the 275,830
  enhanced-barrier calls (global / buffer / texture) is not measured; a texture-barrier layout counter is the
  instrument a later slice would need for that.

## 7. Tests

| target | what it covers | result |
|---|---|---|
| `command_list_test` (252 `CHECK` sites) | the release from a defined layout is recorded with its `SDK` fields; a release with the discard flag is accepted; the named refusals (no resource, both halves `SPLIT`, unknown flags, a mip range with zero layers) still fail; the closed-list rule | passes |
| `replay_texture_barrier_test` (33 `expect` calls) | section 3: three mips in three layouts released with `oldLayout == newLayout` per piece, one `vkCmdPipelineBarrier2`, tracker keeps the layouts; section 4: a never-used image released records no barrier and is not zero-filled, then the following barrier from `UNDEFINED` discards and the tracker moves to the new layout | passes |
| `d3d12_close_parity_test` (116 cases) | the three enhanced-barrier rows; the layer's `Close` is compared with a live WARP device when one can be made, and with the recorded Windows 11 WARP column otherwise | passes: 85 match WARP, 16 known layer-stricter, 15 known layer-looser, 0 failures |

Reproduce the pair: acquire `_setup/boot_lock.ps1 -Acquire -Who barrier`, build the two clean trees as described in
section 5 (or A and B from `port/build` as usual), run each arm through `tooling/boot_report.py` with the switches in
section 5, and release the lock. The evidence outside the repository: `boot-head-A`, `boot-head-B`, `head-A`,
`head-B`, `capture-head-A` and `capture-head-B` under the work directory, each boot with `stderr.log`, `summary.txt`,
`boot.jsonl` and `arm_meta.txt` (binary hash). The 600 s evidence is the `boot-long600` directory named by
[long_boot.md](long_boot.md).

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project `ubiquitarianism/leonida` (<https://github.com/ubiquitarianism/leonida>, license
<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>), files `runtime/src/command_list.h`,
`runtime/src/vulkan_replay.h`, `docs/d3d12_vulkan.md`, `runtime/test/command_list_test.cpp`,
`runtime/test/replay_texture_barrier_test.cpp` and `runtime/test/d3d12_close_parity_test.cpp`, commit
`37c5af12a45f256767e0274b0d1a2d0a858968c8` (milk4sale, co-authored Moriz82; the topic commit of PR #1055, merged as
`cdb003ea19256ab29997b2ca1e4abf0be05ae9d2`, aslan2142); fork HEAD when this page was written was
`ed94fbef9888` (2026-11-25). No code from that project was copied. The upstream project remains the copyright of its
contributors and its license continues to apply to its own tree.

The project's attribution rule ([upstream_port.md](upstream_port.md), section 3) also puts this sentence in
`NOTICE.md`. That file is outside this slice's write scope, so the lead adds the entry there; the sentence above is
its content.
