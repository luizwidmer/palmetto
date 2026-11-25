# First light: the re-measured frame path at the tree whose last assert cleared

**Status: measured negative on the presented frame, with the missing producer named.** Two 150 s boots at HEAD - the
shipping path with the read-only frame diagnostics and one writers-recipe late-frame control - measured the frame with
`PALMETTO_FRAME_DUMP`, `PALMETTO_FRAME_DUMP_SAMPLE`, `PALMETTO_READBACK_AT_DRAW`, `PALMETTO_READBACK_FULL`,
`PALMETTO_VERTEX_PATH` and `PALMETTO_CAPTURE`, and it is exactly the frame [frame_capture.md](frame_capture.md)
modelled: two draws per list, a zero-image constant pass into a sibling 4K target and a six-image compose pass into the
presented resource that samples that sibling. At full 4K resolution and in all four channels **both targets are
uniformly zero**: the constant pass's target is written only by its own draw and reads 0 of 8,294,400 pixels, the
compose pass's target (the present plane) is cleared to zero and then drawn and also reads 0 of 8,294,400, and all eight
presenter captures are black. The compose pass's only sampled T# resolves, by exact resource id, to the sibling pass's
target - so its source is produced by our replay and is empty, not a placed/streamed texture whose pages are missing.
The content is lost at the first draw's own inputs: the draw's 32-dword user data carries only the descriptor-table pair,
the translated vertex shader's four transform scalars (`s8`, `s9`, `s10`, `s28`) are unset, and the console driver block
it fetches its vertex stream through lands on the descriptor table itself. That is the slot-57 record/root-argument
decode gap named in [vertex_path.md](vertex_path.md) sections 5-6, not a page, stream, copy or pass-order fault.

Ids, counts, formats, offsets, bytes and the project's own wording only; no package bytes, paths, shader bytes or
extracted tables.

## 1. Why this measurement, and what changed since the last frame page

`ebe181e14` cleared the title's last assert (the display-surface window check, [range_assert.md](range_assert.md)) and
`8daa4a6c3` measured a 300 s boot that goes further than any before ([story_load_probe.md](story_load_probe.md)): the
loader completes 302 pairs, 8,272 DirectStorage requests, repaired first-touch reads and the first user sign-in at
115 s - and every presenter capture is still black. The frame analysis in [frame_capture.md](frame_capture.md) predicted
the stationary two-pass loop and left the colour path open; [page_content.md](page_content.md) and
[texture_source.md](texture_source.md) showed no copies and no page producer for the sampled textures;
[streamed_image_backing.md](streamed_image_backing.md) and [bcpack_runtime.md](bcpack_runtime.md) added runtime
machinery that is off by default. This page re-measures the frame at the current tree with those mechanisms present, and
answers two questions: which target's content is missing, and whether the compose pass's source is produced now.

## 2. What ran

| | |
|---|---|
| Tree | `D:\leonida` at `2ff15fed1` (the frame-path code is `ebe181e14`'s; the later commit is a document); `port/shims/xmem.cpp` dirty from another agent's memory slice and not in this build |
| Binary | frozen HEAD build copied before other slices rebuilt `port/build`: SHA-256 `45DF506F3009A76156F40D9103B07EA63F811BE3BEFC3649D4540E497B1003BF`, `palmetto.exe` 13:50:06, `palmetto_tls_host.dll` beside it |
| Boots | two, one at a time under `_setup\boot_lock.ps1`, each killed by the 150 s timeout as usual (launcher exit code 1): arm D (the shipping-path measurement below) and arm E (the writers-recipe late-frame control of section 4) |
| Environment | `PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, `PALMETTO_PIPELINE_TIMELINE=10`, `PALMETTO_DSTORAGE_TRACE=1`, `PALMETTO_FRAME_DUMP=1`, `PALMETTO_FRAME_DUMP_AT_DRAW=4000`, `PALMETTO_FRAME_DUMP_SAMPLE=1`, `PALMETTO_READBACK_AT_DRAW=2000,2001,4000,4001`, `PALMETTO_READBACK_FULL=1`, `PALMETTO_VERTEX_PATH=1`, `PALMETTO_CAPTURE` every 300 presented frames, at most 8 |
| Control boot (arm E) | the same read-only set, `PALMETTO_READBACK_FULL=0`, armed at draws 6000/6001 (later frame), plus [frame_capture.md](frame_capture.md)'s three experiment writers `PALMETTO_GRAPHICS_CONSOLE_ROOT=1`, `PALMETTO_VERTEX_STREAM_DESCRIPTOR=1`, `PALMETTO_SLOT70_PUBLISH=1` |
| Trace | 0 exceptions in both boots, no device-lost line, title side alive to the kill (arm D's audio trace events at 129.8 s) |
| Counters at +140 s | arm D: 4,423 submissions, 2,211 presents, 51,786 graphics pipelines created, 10,241 slot-57 converted / 0 refused, 95 threads; arm E: 11,786 submissions, 5,892 presents with the same 51,786 / 10,241 population |
| Evidence | `D:\leonida-work\firstlight\arm-d` (shipping path, `boot\stderr.log` 11.0 MB, `boot\boot.jsonl`, `capture\frame_*.ppm`) and `arm-e` (writers recipe, 20.9 MB, 8 captures), and the analysis script `D:\leonida-work\firstlight\analyze.py` |

The writer/experiment switches ([frame_capture.md](frame_capture.md)'s "all switches on" recipe:
`PALMETTO_GRAPHICS_CONSOLE_ROOT`, `PALMETTO_VERTEX_STREAM_DESCRIPTOR`, `PALMETTO_SLOT70_PUBLISH`) are **off** in arm
D: it is the shipping path. Arm E runs them as the control, and section 7 records what the two extra boots of this
session showed about their cost (a load-stage crawl that did not reproduce).

## 3. The measured frame, command by command

The submission that holds draw 4000 (dump armed at draw 4000; draw indices are the replay's global draw counter) has
two lists: list 0 with 8 commands and both draws, list 1 with the present barrier. `A` is the display resource of this
frame (the present plane), `B` the sibling 4K target:

```text
#0 barrier A layout 31->2                      #5 draw 4000 -> B (ps present, 0 image(s), 3 vertices)
#1 clear A (mip 0, layers 0+1, format 0,       #6 barrier B layout 2->6
   3840x2160) colour 0 0 0 0                   #7 draw 4001 -> A (ps present, 6 image(s), 3 vertices)
#2 barrier B layout 6->2
#3 barrier B layout 2->6                       list 1: #0 barrier A layout 2->31 (present)
#4 barrier B layout 6->2
```

and the writers the diagnostic derives:

```text
writes to B (0x1e49c917170): #5 draw; last writer #5 draw
writes to A (0x1d68303b030): #1 clear #7 draw; last writer #7 draw; last draw writing it #7
over 16 later draw submission(s), writes to B: draw 16, clear 0, copy 0, resolve 0, other 0
```

The two draws:

| | draw 4000 (constant pass) | draw 4001 (compose pass) |
|---|---|---|
| translated pair | `fspx/lib_vs_00821` + `fspx/lib_ps_00820` | `fspx/lib_vs_00823` + `fspx/lib_ps_00822` |
| pixel stage | present, **0 image samples** (constant-output path) | present, **6 image samples** (reads sampled images) |
| vertex stage | 77 loads | 213 loads |
| target | `B` 0x1e49c917170, mip 0, layers 0+1, format 37, 3840x2160 | `A` 0x1d68303b030, same shape |
| viewport / scissor | 3840x2160 at 0,0 / 0,0..3840,2160 | same |
| topology, counts | 17, 3 vertices / 1 instance, first 0 | same |
| vertex buffer | vb0 va 0x1e46e06ef30, 4080 B, stride 24, 170 records | vb0 va 0x1e46e06df10, 4080 B, stride 24, 170 records |
| user data | `s0..s15 = 6e06ef00 000001e4 0 0 0 0 ...`, **2 of 32 non-zero** | `s0..s15 = 6e06dea0 000001e4 0 0 0 0 ...`, **2 of 32 non-zero** |

The present plane is the compose target by exact id: PresentX call 1 names `0x1D68303B030` (draw 4001's target),
call 2 names `0x1D68303A190` (the next submission's draw 4003 target), and call 1000 names `0x1D68303A190`; the display
resource therefore alternates between two 4K images and each frame's compose pass draws into the one the present names.
The frame's own clear at `#1` is the only clear of `A`; `B` is never cleared, and no copy or resolve appears in the list,
in the 16-submission window or in the whole boot (section 5).

## 4. The contents: both targets are uniformly zero

Full-target readbacks (the whole 3840x2160 subresource, four channels) at two frames, one list apart in the same boot:

| armed draw | target | full-target readback |
|---|---|---|
| 2000 (constant) | `B` 0x1e49c917170 | `mean 0 0 0, non-black 0/8,294,400, peak 0, alpha mean 0, alpha non-black 0/8,294,400` |
| 2001 (compose) | `A` 0x1d68303b030 (= present plane call 1) | same: `0/8,294,400`, alpha `0/8,294,400` |
| 4000 (constant) | `B` 0x1e49c917170 | same: `0/8,294,400` |
| 4001 (compose) | `A` 0x1d68303b030 | same: `0/8,294,400` |

The 8x8 tile maps are all-zero on both targets, so this is not an origin-window artefact. The two full-target copies are
the frame's own content; with `PALMETTO_READBACK_FULL=1` the one-shot write control is not recorded (it only runs for the
128x128 window), and the line says so honestly; the independent presenter path agrees - 8 presenter captures of the
swapchain image (frames 1 through 2,101, extent 2564x1421) each read `mean rgb 0.00 0.00 0.00`, non-black
0/3,643,444, peak 0. The sampled content is measured separately: the compose pass's only T# image is copied before the
submission and after it, both `mean 0 0 0, non-black 0/16,384` over its 128x128 window.

The late-frame control boot (arm E) adds the write control on the **presented** resource and measures the later frame:
armed at draws 6000/6001 (a later frame than arm D's), 128x128 at the target's origin,

```text
render-target readback [draw 6000 target resource 0x2ad977107c0]: mean 0 0 0, non-black 0/16,384, alpha 0/16,384
render-target readback [draw 6001 target resource 0x29ff06e7050]: mean 0 0 0, non-black 0/16,384, alpha 0/16,384
write control [draw 6000 target resource 0x2ad977107c0]: skipped, the image is an armed sampled input
write control [draw 6001 target resource 0x29ff06e7050]: mean 51 102 204, non-black 16,384/16,384, peak 204
frame dump sample readback [1/1] image 0x2b0d2705ad0: mean 0 0 0, non-black 0/16,384
pre-draw sample readback [1/1] image 0x2b0d2705ad0 (resource 0x2ad977107c0): mean 0 0 0, non-black 0/16,384
```

`0x29ff06e7050` is PresentX call 1's plane 0 resource and draw 6001's target, and the sampled image 0x2b0d2705ad0 is
draw 6000's target (`resource 0x2ad977107c0`) - the same parent/compose identity as arm D, at the later frame. So the
copy and host-mapping path writes (the control read 51 102 204 exactly, 16,384/16,384), the presented resource's own
window is black at the late frame, and the compose source is still the sibling target and still zero. The window is at
the origin, though, and the constant pass's own triangle does not cover the origin corner, so the constant target's
window is not by itself a statement about the whole late target; arm D's full-target readbacks carry that statement for
its frame, and all 8 presenter captures (including the ones taken late in both boots) carry it for the whole run.

So the constant pass's value is **zero over its whole target**, and the presented resource's value is **zero over its
whole target**. The constant pass's target is never cleared, so its zero is the state the draw left (the layer zero-fills
a committed image at first use, so a pass that writes no fragment leaves zero); `B` is written 16 times in the window and
every one is that same draw.

## 5. The compose pass's source: produced, resolved, and empty

The compose pass's table (`VA 0x1e46ea6dea0`, 64 slots) holds exactly **one T# key**: slot 1 at table + 0x20,
`T#[1] 01000016 c3800000 021bc3bf 90000fac 00000000 00000000 00000000 00000000`. It resolves through the layer to
`image 0x1e7d9fa5ad0 format 37 3840x2160, resource 0x1e49c917170, title VA 0x1e2e0ad0000` - the very resource the
constant pass draws into (same D3D12 resource id as the draw-4000 attachment). The other slots are not sampled inputs:
slot 0 is empty, slot 2 is a V# over the 480-byte constants buffer (V# base 0x76dc40 + 0x1e0, read 480: mean 17,
non-black 78/480, first dwords `3f800000 0 0 0 0 3f800000`), and slots 4/5 hold float data (`45070000`, `45700000`,
`bf800000`, `3f800000`), not descriptors. The constant pass's table holds **0 T# keys** (its 96-byte V# at slot 0 and
float data only).

The producer chain that is *not* implicated, with the same boot's numbers:

| Producer | Measurement |
|---|---|
| Copy/upload commands | `CopyBufferRegion` 0, `CopyTextureRegion` 0, `CopyResource` 0, `CopyTiles` 0, `ResolveSubresource` 0, `WriteToSubresource` 0 in the whole boot |
| DirectStorage streamed blocks | 8,272 request shapes, 8,192 requests / 498.9 MiB decompressed (zlib file→buffer, `memory=0`, `bcpack=0`, `swizzle=0`, `dest-pages=0`, `source-pages=0`, refusals 0); no page-list destination the device could name |
| BCPack/predictor machinery | `bcpack refused=0 decoded=0 blocks=0 bytes=0 history=0`, `predictor=off` - no request reaches it |
| Placed-texture page import | off by default (and the sampled resource is a render target of the title's own pass, not a texture the texture computer fills) |
| Page pulls | the draw-read pull count and the per-table lines (`64 slot(s), 1 V#, 1 title-named (pulled)`) cover the draws' V#/vertex ranges; no pull is a texture fill |
| Image pool/deferred backing | both opt-in and unset; the frame does not change |
| Compose pass order | constant pass is `#5` and compose is `#7` of the same list, so the compose pass samples the sibling target of the same frame (its target is barriered `#6` after the draw) |

In one line: the compose pass's source is our replay's own constant-pass render target, its descriptor is resolved and
correct, and it is black because the constant pass writes nothing visible.

## 6. Where the content is actually lost: the constant pass's own inputs

The constant pass's draw runs but its translated vertex stage produces no rasterisable vertex. The vertex-path dump for
draw 4000 (same numbers at draw 2000) reads:

```text
vertex path: pipeline 0x1e4a0fb46e0 vs "fspx/lib_vs_00821" ps "fspx/lib_ps_00820"; layout problems 0x0
vertex path: binding 0 stride 0 step rate vertex divisor 0
vertex path: attribute location 0 format 106 (12 byte(s)) offset 0 binding 0
vertex path: attribute location 1 format 37 (4 byte(s)) offset 12 binding 0
vertex path: attribute location 2 format 103 (8 byte(s)) offset 16 binding 0
vertex path: vb0 va 0x1e46e06ef30 size 4080 stride 24; resolved buffer 0x1e3d3743e50 offset 7794480 stride-at-draw 24
vertex path: vb0 read 4080 byte(s), 30 non-zero; first vertices:
  v0 bf800000 3f800000 00000000 ffffffff 00000000 00000000
  v1 bf800000 bf800000 00000000 ffffffff 00000000 3f800000
  v2 3f800000 bf800000 00000000 ffffffff 3f800000 3f800000
vertex path: clip-position scalars x 0 y 0 z 0 w 0 -> degenerate: every vertex at (0,0,0,0), nothing rasterizes
vertex path: root parameters: 1
vertex path: acceptance inputs s8 unset s9 unset s10 unset s28 unset
vertex path: user data 0+1 = 0x1e46e06ef00 readable +16; dwords 6e06ff20 000001e4 00000060 30014fac
vertex path: fetch bases: s0/s1 pair 0xc06ef00, s8/s9 pair 0x0
vertex path: fetch slot driver(s0/s1)+0x00: ... 0076ff20 00000000 00000060 30014fac -> V# buffer address 0x76ff20
             stride 0 records 96 format 20
vertex path: driver-block-shaped V# for vb0: arena-relative address 0x76ef30 stride 24 records 170
```

Read as a statement:

* the vertex buffer is bound and carries the right shape (170 records of stride 24; the first three are the
  full-screen triangle's coordinates and the rest zero), and the input layout decodes with 0 problems - the data the
  draw names is there;
* the 32-dword push-constant block holds only the descriptor-table pair (`s0`/`s1`); `s8`, `s9`, `s10` and `s28`, the
  four scalars the translated module multiplies its fetched vertex register by ([vertex_path.md](vertex_path.md)
  section 4), are printed `unset`; the title issues no `SetGraphicsRoot32BitConstant` call in the boot, so they can
  only come from the slot-57 record, and the record decode reconstructs the descriptor table but not its root constants
  ([vertex_path.md](vertex_path.md) section 5);
* the shader's driver-block fetch (`s0`/`s1`) lands on the **descriptor table itself**, and its slot 0 is a V# over a
  96-byte block at stride 0 / format 20 - the 96-byte constants block - not the vertex-stream V# the layer has bound
  (`0x76ef30`, stride 24, 170 records, format 22). The compose pass's fetch is the same story (its `s0/s1` fetch at
  +0x00 is empty, +0x20 is the sibling-target T# it samples, +0x40 is the 480-byte constants V#).

The compose pass's own vertex stage prints the same degenerate line with the same four unset scalars, so both passes are
missing the same transform values; even if the compose geometry were correct, its source `B` is the constant pass's
zero target (section 4). The constant pass's pixel shader is a zero-image constant-output module, so it has no sampled
input to be black - its colour would come from the same user-data block, of which only the table pointers are non-zero;
whether the title's own pixel shader would write a non-black background with correct transform values is not measured in
this boot (its geometry produced no fragment here, and the earlier coverage controls substitute the project's own
pixel shader, so they carry the control's colour, not the title's). Section 9 names that limit.

The control boot with the writers on (arm E) narrows this to the missing scalars: its vertex-path dump for draw 6000
prints the console feed's `s8 loose` (the table VA, a subnormal as a float) and `s9 unset s10 unset s28 unset`, while the
driver-block fetch now decodes the synthesised vertex-stream V# (`0186ef30 00180000 000000aa 00016fac` -> address
0x186ef30, stride 24, records 170, format 22) and the s8 table's slot 0 carries the published matrix V#
(`0186ff20 00000000 00000060 30014fac`, a 96-byte block, where the compose pass's table slot 0 carries the published
5768-byte matrix V# `0186de20 ... 00001688 30000fac`). The transform line still reads `clip-position scalars x 4.651e-31 y 0 z 0 w 0` and the
pass's target/sampled windows are still zero and the presenter captures still black. So the driver-block descriptor and
a matrix A are supplied by the experiment, and the three remaining scalars are the next measured gap - they are the
slot-57 record's root-constant values, which no measured path carries yet.

## 7. What the extra boots showed: a boot-to-boot spread, not a recipe property

Four 150 s boots of the same frozen binary ran in this session while other slices built, measured and booted on the same
machine (the boot lock serialises the boot slot, not the CPU). Their frame-loop and load-stage rates disagree in both
directions:

| arm | switches | frame loop | DirectStorage |
|---|---|---|---|
| A | producers + writers + read-only diagnostics | 6 submissions / 2 presents at +30 s, still 6 / 2 at +120 s | 5,120 requests by 91 s, then flat |
| C | writers + read-only diagnostics | submission 16 at 65.9 s, 168 submissions / 83 presents at +120 s | 5,120 requests by 91.2 s |
| D | read-only diagnostics (shipping path, this page) | submission 1,024 at 30.0 s, 4,096 at 106.9 s; 4,423 / 2,211 at +140 s | 8,272 requests / 498.9 MiB by 69.4 s |
| E | writers + read-only diagnostics (late-frame control) | submission 1,024 at 18.6 s, 4,096 at 56.7 s, 8,192 at 94.0 s, 12,288 at 128.2 s; 11,786 / 5,892 at +140 s | 6,144 requests / 480.1 MiB by 144.1 s |

The first two boots (A, C) crawled the load stage and the last two ran the same recipe (E) or the shipping path (D) at a
normal rate - in fact E, with the writers on, ran the frame loop about 2.6 times faster than D, with the writers off,
while its DirectStorage load was slower in the same boot. The two stages therefore vary independently under the external
load of the session; **no causal claim about the writer switches is supported by these numbers, and the early crawl is
recorded only so its timings are not reused as tree baselines.** The frame shape, the target identities and the
readbacks are the same in all four boots; the reproducible difference between D and E is the feed content (D has 2 of 32
non-zero user-data dwords and no `s8`; E has 3 of 32 and `s8 loose`), which is the subject of section 6.

The measurement this page reports is arm D because it is the shipping path (every writer switch off) and it produced
the full-target readbacks; the late-frame control (arm E) runs the writers recipe and is quoted in sections 4 and 6.

## 8. Boot numbers

| measure | arm D (shipping path) | arm E (writers-recipe control) |
|---|---|---|
| Verdict | ALIVE 150.1 s, killed by the timeout; 0 exceptions, no device-lost, 0 failed/refused submissions | ALIVE 150.2 s, same verdict |
| Submissions / presents | 4,423 / 2,211 at +140 s; 4,096 at 106.9 s, 1,000 presents at 30.0 s | 11,786 / 5,892 at +140 s; 12,288 at 128.2 s |
| Pipelines | slot 57: 10,241 converted / 0 refused; slot 55: 41,545; graphics 51,786; compute 278 / 232 created | same 10,241 / 0, 41,545, 51,786; compute 392 / 289 created |
| DirectStorage | 8,272 shapes, 8,192 requests / 498.9 MiB decompressed by 69.4 s; zlib file→buffer only; 0 refusals | 6,996 shapes, 6,144 requests / 480.1 MiB by 144.1 s; same shape, 0 refusals |
| Copies/resolves | 0 of every copy-family call in the whole boot | 0 of every copy-family call in the whole boot |
| Target contents | `B` and `A` full targets both 0 of 8,294,400 pixels, alpha 0 (draws 2000/2001 and 4000/4001); write control not recorded with FULL | `B` and `A` 128x128 origin windows 0 of 16,384 at draws 6000/6001; A write control 51 102 204, 16,384/16,384 |
| Presenter captures | 8/8 black (frames 1..2,101, 2564x1421, 0/3,643,444) | 8/8 black, same extents and zeros |
| Frame shape | 2 draws per list, 1 list per present: constant pass into `B`, compose pass into the present plane, compose's only T# = `B` | same shape and identity at draws 6000/6001 (present plane call 1 resource = draw 6001 target) |

## 9. Limits

* The full-target copies carry no write control in this boot because `PALMETTO_READBACK_FULL=1` skips it; the 8
  presenter captures are the independent control over the whole run (a different readback path, same black), and the
  frame's own copy path was write-controlled in earlier increments of [frame_capture.md](frame_capture.md).
* The constant pass's pixel shader never rasterised in any measured boot, so what colour it would write with the
  missing constants is unmeasured; the page says so rather than guessing.
* The full-target frames are draws 2000/2001 (~29 s) and 4000/4001 (~52 s) of arm D; the boot's streamed load completes
  at 69.4 s in that run, so both are "during the load window". The late-frame control (arm E, draws 6000/6001) measures
  only a 128x128 origin window, and the constant pass's triangle does not cover that corner, so that window is not a
  statement about the whole late target; the presenter captures (frames up to 2,101 in both boots) are the whole-run
  statement. The frame loop is the same stationary loop throughout (2 draws per present, same four modules,
  `presents` growing linearly to +140 s), and no boot measured a post-load full target.
* The translated module names (`fspx/lib_vs_00821` etc.) and slot ids are this boot's; the resource ids are ASLR
  per boot and are quoted only to tie the lines together.
* The boot was slower than the standard-environment 300 s probe (DS 498.9 MiB in 69.4 s here against 21.6 s there),
  consistent with the diagnostic load and other agents' work on the machine; the load finished, the frame loop ran,
  and the counters above are the boot's own.

## 10. What would change the picture

Two independent readings are supported by this boot, and they are named separately so the next slice can tell them
apart:

1. **The frame path's own gap (our structure).** The slot-57 record/root-argument decode never supplies the draw's
   transform constants (`s8`, `s9`, `s10`, `s28`) or the console driver-block vertex-stream descriptor, so both passes'
   vertex stages are degenerate. The fix lives in the slot-57 record decode and its console-feed path - the measured
   gap's exact places: `port/src/vulkan_replay.h:3084-3187` (the driver-block V# synthesis experiment and the emitted
   user data), `port/shims/d3d12_com.cpp:1169-1193` and `1324-1393` (the console root feed and the 32-dword
   `write()`/spill cut), and `port/src/graphics_pso.cpp:170-189` (`console_graphics_user_data()` packs the vertex stage
   only) plus the record decode - owned by the vertex/record slices, not edited here. Its acceptance test on the shipping
   path: the vertex-path dump's `fetch slot driver(s0/s1)`
   decodes to the bound vertex-stream V# (`stride 24`, `records 170`) and `clip-position scalars` is not degenerate;
   then the constant pass's target changes from 0 of 8,294,400 and, if it does not, the pass's colour path is next.
2. **The title not having started content.** The frame contains no content pass at all: two draws, four FSPX
   blit/upscale modules, a zero-image constant pass and an upscale of its output, at a constant rate, while the
   title's state words measured by [story_load_probe.md](story_load_probe.md) hold (`0x7446510` = 2,
   `0x98f828c` = 0, `0x7453ed0` = 1) to 298.9 s. A frame with a third graphics pipeline (a scene/content pass, a fifth
   translated graphics module) in the present loop is the **first measurable signal that would precede content**;
   before it, the title's own state step (init word leaving 2, session word 0 -> 1) is the upstream signal. Until one of
   those appears, the presented black is the stationary background/upscale loop, and the two passes' own inputs are
   the only content the replay could be asked to produce.

## 11. Files and gates

This page is diagnostic measurement; no runtime file changed, no test target changed, and the boot lock was held for
each boot and released after its kill. The boot directories and captures are outside the repository under
`D:\leonida-work\firstlight\arm-d` and `arm-e`; the measured recipe and the analysis script (`analyze.py`) stay there
too. The matching GPU-free test target (`port/test/frame_dump_test.cpp`) is another slice's file and was not edited
for this measurement; the acceptance test for the root-constant/driver-block fix is named in section 10. This slice's
uncommitted file is this page; the tree also carries other slices' uncommitted `port/shims/xmem.cpp` and
`port/src/alloc_trace.h`.

Gates on this tree: `_setup\build_gta6rt.cmd` printed `PALMETTO_OK` and `_setup\build_tests.cmd` printed `TESTS_BUILT`
(both with "ninja: no work to do", i.e. the build directory is current for the tree that includes another slice's
uncommitted `xmem.cpp`); `port/build/frame_dump_test.exe` passes (exit 0, "command kinds, ordered writers and table
windows exact"); the documentation checks pass for this page (it is untracked, so the `git ls-files` based checks scan
it only after the lead commits, and it was checked against the same rules by hand: no dead links, no orphan or duplicate
table rows, no repeated openings, no mojibake). Two documentation checks fail on other slices' files, not this page:
`test_docs_env_vars.py` reports `PALMETTO_ALLOC_TRACE` (`port/shims/xmem.cpp:210`, another slice's in-progress
instrumentation) named in no page, and `test_docs_cli_flags.py` reports `--interval` in
`docs/range_assert.md:218` and `docs/user_event_probe.md:576` named by no source script. The other ten documentation
checks pass.
[upstream_port.md](upstream_port.md) applies only to slices that port a reference here; this measurement takes no
upstream code or behaviour.
