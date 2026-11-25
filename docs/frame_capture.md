# Frame capture: the order and targets of one real frame

**Status: measured increment.** `docs/page_content.md` section 4 concluded that the draw's own render target read back
black while being "one of the ids the present names as its plane". This page records the diagnostic that reads
*whether that was true*: for one real frame it prints the commands in replay order with the resource ids each one
reads and writes, and compares the draw's target with the resource the present blits. The measurement corrects section
4 (the one-shot readback armed at the first/even draw reads the frame's **intermediate** target, not the display
resource), shows the frame's record is clean (one clear of the display resource, before the draws; the last writer is
the draw; no copy or resolve anywhere), and then draws the project's own known-good full-screen triangle into the very
same display resource: it reads back exactly its colour. So the replay's raster, target binding, viewport, subresource
and readback path are proven for the presented target, and the title's own draw is what leaves it black. Two further
controls then separate the remaining causes: the title's own vertices through its own pipeline state with only a
constant-output pixel shader still leave the window black (section 6: the vertex/geometry path produces no coverage),
and the first texture that six-image pixel shader samples reads back black as well (section 7: the content is empty).
Ids, counts, recorded argument dwords and the project's own wording only; no package bytes, paths or extracted tables.

## 1. What the diagnostic records

`PALMETTO_FRAME_DUMP=1` (`port/src/vulkan_replay.h`, `frame_dump`; called from `VulkanQueue::execute_now` in
`port/src/vulkan_backend.h` after every list of the submission is prepared) prints, for the submission that holds the
draw `PALMETTO_FRAME_DUMP_AT_DRAW` names (else the draw `PALMETTO_READBACK_AT_DRAW` names, else the first draw):

- the submission's lists in replay order, one line per command: its kind (`barrier`, `clear`, `draw`, `copy`,
  `resolve`, `dispatch`, `query`, `marker`, ...), the resource ids it names, and for a clear its target subresource and
  colour; for a draw its pipeline pointer, the translated pipeline handle, the pixel stage's image-sample and vertex
  load counts, every colour target's id/image/mip/layer range/format/extent, the depth target, area, viewport, scissor,
  topology, vertex/index counts, the bound vertex views' VAs with their resolved buffer and offset, and the first 16
  recorded user-data dwords;
- then the ordered writers of every resource a draw writes in that submission: `writes to 0x…: #1 clear #7 draw; last
  writer #7 draw`, and an explicit `WRITTEN AFTER THE LAST DRAW` marker when a non-draw write follows the last draw;
- the next draw-bearing submission the same way, then a bounded window (default 16 submissions, `PALMETTO_FRAME_DUMP_
  WINDOW`) counting which kinds write the armed draw's target afterwards.

`PALMETTO_FRAME_DUMP_CONTROL=1` adds the known-good pipeline control of section 5: for the armed submission only, it
draws the project's own full-screen triangle into the armed draw's colour target after the frame's commands, so the
existing one-shot readback copies it. The triangle is the existing test pair `test/glsl/vs_tri.vert` +
`test/glsl/fs_solid.frag` (compiled by the build; the runtime reads the two SPIR-V files
`PALMETTO_FRAME_DUMP_CONTROL_VS`/`_PS` name, no new shader). The control pipeline is created once for the target's
format and lives with the context.

Both switches are read once and off by default: with them unset no recorded command changes, the queue only takes a
false branch per switch per submission. Everything is bounded (256 commands per list, two submissions, a
64-submission cap on the window) and prints ids, counts and recorded dwords only.

The pure helpers the printout relies on (`frame_dump::kind_name`, `command_writes`, `writers`,
`first_writer_after`) have a GPU-free test: `port/test/frame_dump_test.cpp` builds a list whose target is cleared,
drawn twice, then resolved, and pins the kind names, the writer order and the "written after the last draw" answer.

## 2. The presented resource and the readback, by id

D3D12 resources are identified by their `ID3D12Resource` pointer throughout this layer (`resource_id` in
`port/shims/d3d12_com.h`; `docs/d3d12_vulkan.md` section 3.1), and that pointer **is** the replay layer's resource id.
The PresentX trace prints each presented plane's resource pointer, and the replay diagnostic prints the draw targets in
the same form, so the comparison is exact and needs no extra hook:

| Boot (armed draw) | present plane 0, call 1 | present plane 0, call 2 | draw's target (replay id) | same? |
|---|---|---|---|---|
| dump at draw 20000 | `0x1544bdd5660` | `0x1544bdd5890` | `0x1621e656ea0` (an intermediate) | **no** |
| readback at draw 20001 | `0x1bf82d056f0` | `0x1bf82d05920` | `0x1bf82d056f0` | **yes** |
| control at draw 20001 | `0x17809054d10` | `0x17809054450` | `0x17809054d10` | **yes** |

The one-shot readback copies **mip 0, layer 0, 128x128 at the target's origin**, in the pipeline's colour format
(4-byte texel here), after the whole submission completes. The dump's per-draw line says the draws write mip 0, layer
0+1 with a full viewport and scissor, so the readback window lies inside the area the draw writes.

## 3. The measured frame

Every submission of the frame loop holds two draws (even global index: a pixel shader with no images into a 4K
intermediate; odd index: the six-image pixel shader into the display buffer), and the display buffer alternates between
two 4K images (the ids the present trace names on alternate calls, and the ids consecutive submissions draw into). The
frame's dump (boot at draw 20000, list 0 = 8 commands, list 1 = 1 command):

```
#0 barrier A layout 31->2                      #5 draw draw 20000 -> intermediate B (ps present, 0 image(s), 3 vertices)
#1 clear A (mip0, layers 0+1, format 37,       #6 barrier B layout 2->6
   3840x2160) colour 0 0 0 0
#2 barrier B layout 6->2                       #7 draw draw 20001 -> display A (ps present, 6 image(s), 3 vertices)
#3 barrier B layout 2->6                           A: mip 0 layers 0+1 format 37 3840x2160, viewport 3840x2160,
#4 barrier B layout 6->2                           scissor 0,0..3840,2160, topology 17, vertices 3/1 first 0
list 1: #0 barrier A layout 2->31 (the present/common layout)
```

and the writers the diagnostic derives from it:

```
writes to 0x…(B): #5 draw; last writer #5 draw
writes to 0x…(A): #1 clear #7 draw; last writer #7 draw; last draw writing it #7
```

So the display resource is written by the frame's own clear at **#1, before both draws**, its last writer is draw
**#7**, and only a layout barrier follows. Armed at draw 20001 (the display-resource draw), the one-shot readback
measures:

```
render-target readback: 128x128 at the target's origin of 3840x2160, 4-byte texel: mean 0 0 0, non-black 0/16384, peak 0
write control: mean 51 102 204, non-black 16384/16384, peak 204
```

and the presenter's own capture of the window in the same boot reports every captured presented frame as
`mean rgb 0.00 0.00 0.00`. So the presented resource itself is black right after its last writer, and the presented
image is black: those agree, and neither is a readback artefact (the write control clears and re-copies the same image
exactly).

Over the next 16 draw-bearing submissions the display resource is written **only** by the frame loop reusing it:
`draw 8, clear 8, copy 0, resolve 0, other 0` - each reuse is cleared and then drawn, exactly as in the dumped frame,
never a clear or copy after the draw.

**Correction to `docs/page_content.md` section 4.** Armed at the first (even) draw, the readback measures the
intermediate target, not the display resource: in the dump-at-draw-20000 boot the readback's resource is
`0x1621e656ea0` while the presented planes are `0x1544bdd5660`/`0x1544bdd5890`. The ids quoted in section 4 have the same
shape (`rt0 id 0x1D0B1D39980` against `plane 0 resource 0 0x1D0B1D38EE0`). So that readback did not prove the presented
resource black; the presented resource was first read back in the armed-at-20001 boot above (where the ids do match).

## 4. Which candidate the data supports

The four candidates for a presented target that stays black although its draws execute:

| Candidate | Measurement | Verdict |
|---|---|---|
| (a) a clear of the display resource after the draws | The only clear of A is `#1`, before both draws; last writer is draw `#7` and only a layout barrier follows; the 16-submission window writes A only with its next frame's clear+draw | not supported |
| (b) a copy/resolve that overwrites it | 0 `CopyBufferRegion`, `CopyTextureRegion`, `CopyResource`, `ResolveSubresource` in the dump, in the window, and in the whole boot (also on `docs/page_content.md`'s 300 s boot) | not supported |
| (c) the draws write a different subresource/area than the readback window | Draw target: mip 0, layers 0+1, 3840x2160; readback: mip 0, layer 0, origin 0,0; viewport and scissor cover the target | not supported |
| (d) the draws are recorded but produce no visible colour on the GPU | The record is clean (above) and the display resource reads black immediately after its last writer | **supported** |

## 5. The known-good pipeline control: the replay's raster path writes this target

One boot armed the readback at draw 20001 and added `PALMETTO_FRAME_DUMP_CONTROL=1`: after the frame's commands the
replay recorded one more draw into the **same** display resource (`0x17809054d10`, 3840x2160, format 37) with the
project's `vs_tri` + `fs_solid` pipeline (cull off, depth off, blend off, full viewport/scissor, triangle list, 3
vertices, 1 instance), and the existing readback copied that target:

```
frame dump control: built the vs_tri/fs_solid pipeline for format 37; its draws write 32 160 240
frame dump control: drew the vs_tri/fs_solid triangle into 0x17809054d10 (3840x2160, format 37) after the frame's commands
render-target readback: 128x128 at the target's origin of 3840x2160, 4-byte texel: mean 32 160 240, non-black 16384/16384, peak 240
write control: mean 51 102 204, non-black 16384/16384, peak 204
```

`32 160 240` is exactly `fs_solid`'s colour on 8 bits, in the same target, subresource, viewport, scissor and format
the title's draw uses; the same boot's present plane 0 call 1 is `0x17809054d10`, so this is the presented resource.
Rasterisation, the target view, the layout transition into and out of the draw, the viewport/scissor, the format and the
readback copy all work for this target. The title's own draw writes black through the same path, so the remaining
explanation is the draw's own output: the pixel shader that computes black from empty inputs, the vertex stage that
produces no coverage, or fixed-function state that discards its fragments. Sections 6 and 7 separate the first two.

## 6. The PS-only control: the title's vertex stage produces no coverage in this window

One boot (`PALMETTO_FRAME_DUMP_PS_CONTROL=1` with `PALMETTO_FRAME_DUMP_CONTROL_PS` naming the build's `fs_solid.spv`);
after the frame's commands the replay recorded one more draw with the armed draw's **own command** - same render target
`0x2c0000ccf50` (the same boot's present plane 0 call 1), same vertex view at the same offset, same vertex count, root
arguments, viewport and scissor - through the title's own pipeline recipe with only the pixel shader replaced by the
project's constant-output `fs_solid`. The shim builds that variant from the same recipe (`GraphicsPipeline::with_ps`), so
the vertex stage, input layout and every fixed-function field are the title's. The readback then measured:

```
frame dump PS control: drew the armed draw's own vertices through the title pipeline (0x2cd5f4a18d0) with fs_solid
                       as the pixel shader, into 0x2c0000ccf50
render-target readback: 128x128 at the target's origin of 3840x2160, 4-byte texel: mean 0 0 0, non-black 0/16384, peak 0
write control: mean 51 102 204, non-black 16384/16384, peak 204
```

`0x2cd5f4a18d0` is the very VkPipeline the dump prints for the title's draw `#7`, so the substituted pipeline is the
title's recipe. A same-build positive control replaced the same target with the project's own full-screen triangle
(`PALMETTO_FRAME_DUMP_CONTROL`): `mean 32 160 240, non-black 16384/16384, peak 240`. The target, subresource, format,
layout transitions, viewport/scissor and readback copy therefore do write a solid colour, and the title's vertex stage
plus state plus a constant-output pixel shader leaves the window black: **the title's vertex data/state produces no
fragment in this window.** That is either degenerate or off-screen transformed positions, a vertex-data/input-layout
mismatch, or the documented-default raster state (back-face culling is on; the state record's fixed-function fields are
still undecoded, `docs/graphics_pso.md`) culling the title vertex shader's winding. The black does not come from the
six-image pixel shader's own arithmetic; the vertex/geometry path is the frontier, and the sampled content is a second,
independent gap (section 7).

## 7. The sampled textures: the first T# resolves to a 4K image that reads black

A boot with `PALMETTO_FRAME_DUMP_SAMPLE=1` (readback armed at draw 20001) has the shim read the armed draw's descriptor
table from the console heap mirror, prints the table's slot keys, and copies the first sampled image through the layer:

```
frame dump sample: table VA 0x1718135dea0: 64 slot(s), 8 key(s):
    [00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000]
  T#[01000016 c3800000 021bc3bf 90000fac 00000000 00000000 00000000 00000000]
    [0086dc40 00000000 000001e0 30014fac 00000000 00000000 00000000 00000000]
    [00000000 00000000 00000000 00000000 00000000 00000000 00000000 ffffffff]
    [00000000 00000000 00000000 45070000 00000000 ffffffff 00000000 3f800000]
  T#[45700000 45070000 00000000 ffffffff 3f800000 3f800000 00000000 00000000]
    [00000000 ...] [00000000 ...]
frame dump sample: first T# image 0x182d1805ad0 format 37 3840x2160; readback armed
frame dump sample readback: 128x128 of a 3840x2160 image, 4-byte texel: mean 0 0 0, non-black 0/16384, peak 0
```

The first T# (`01000016 ...`) resolves to a 4K R8G8B8A8 image (format 37) and its top-left 128x128 window is uniformly
black; the sampled content carries nothing, as `docs/page_content.md` sections 6-8 predicted (the title never uploads).
The keys are printed as the title wrote them: the first slot is empty, the second is that T#, the third is a V# (base
`0x0086dc40`, 0x1E0 bytes - the constants buffer the draw names), and the rest are zero or S#/V# slots. So even with
coverage the six-image pixel shader would sample black: the vertex/geometry path and the sampled content are two
independent gaps, and neither is a fault of the replay's raster or readback path.

## 8. The cull axis and the constants: neither explains the absent coverage

Two boots on the same build close the two cheapest alternatives behind section 6's black window:

- **Culling.** The PS-only control repeated with `PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE=1`, which changes only the
  substituted variant's raster cull mode (back-face culling off; every other title field kept). Readback of the same
  presented resource (`0x14e123d0780` = present plane 0 call 1): `mean 0 0 0, non-black 0/16384, peak 0`; write control
  51/102/204. The documented-default back-face culling of the undecoded state record is therefore **not** what discards
  the title's fragments: with culling off the title's own vertex stage and state still leave the window black, so the
  cull axis of this question is closed.
- **The constants.** A boot with `PALMETTO_FRAME_DUMP_SAMPLE=1` and `PALMETTO_READBACK_CENTRE=1` read the first V# the
  armed draw's table names (base `0x86dc40`, declared `0x1E0` bytes - the constants buffer) through the layer, and moved
  the target readback window to the image's centre. Measured: `V# base 0x86dc40 + 0x1e0, read 480: mean 17, non-black
  78/480, peak 214; dwords 3f800000 00000000 00000000 00000000 00000000 3f800000 00000000 00000000`. The first dwords are
  a plausible transform matrix (1.0 on the diagonal, zeros so far: `3f800000` is 1.0f), not zeros or garbage, so the
  vertex shader is not transforming through an empty constants block. The same boot's target readback window at the
  image's centre (offset 1856,1016 of 3840x2160) is black too (`mean 0 0 0, non-black 0/16384`), so the black is not an
  origin-corner artefact, and the sampled T# image is black as in section 7.

So the absent coverage comes from the vertex stage's own inputs or outputs - the vertex bytes the pull leaves at the
view (`docs/page_content.md` measured 24 non-zero bytes in the 0xFF0-byte view for the frame's three 8-byte vertices),
the input layout the translated vertex shader reads, or the positions it writes - not from culling, not from empty
constants, and not from the pixel shader's work. The display target itself carries no content at its centre either, and
the first texture the pixel shader samples is empty: three independent gaps, each now measured rather than inferred.

## 9. What was added, and what is next

Added: the `frame_dump` diagnostic in `port/src/vulkan_replay.h` (`PALMETTO_FRAME_DUMP`,
`PALMETTO_FRAME_DUMP_AT_DRAW`, `PALMETTO_FRAME_DUMP_WINDOW`), its call in `VulkanQueue::execute_now`
(`port/src/vulkan_backend.h`), the known-good full-screen control (`PALMETTO_FRAME_DUMP_CONTROL` with
`PALMETTO_FRAME_DUMP_CONTROL_VS`/`_PS`), the PS-only control (`PALMETTO_FRAME_DUMP_PS_CONTROL` and its cull axis
`PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE`, the `GraphicsPipeline::with_ps` hook and the shim's
`build(substitute PS, cull_none)` variant of the same recipe), the sampled-texture readback and constants read-through
(`PALMETTO_FRAME_DUMP_SAMPLE`, the `SampledTable` probe and `read_title_memory` reads on the shim's descriptor state,
the `capture_sample` hook), the centre-window option for the target readback (`PALMETTO_READBACK_CENTRE`), and
`port/test/frame_dump_test.cpp`. Everything is diagnostic-only and off by default; with the switches unset no pipeline,
capture, probe or log is created, and the recorded commands are unchanged (the shim's default `build()` still points at
the recipe's own stage list, so the normal pipeline build and its cache keys are untouched).

Measured runs (120 s harness boots, 0 exceptions, 0 failed/refused submissions; killed by the timeout as usual):

| Boot | Draws / presents | Display-resource readback | Control readback |
|---|---:|---|---|
| dump at draw 20000 (previous increment) | 22,322 / 11,162 | intermediate target, mean 0 0 0 | - |
| readback at draw 20001 (previous increment) | 22,186 / 11,093 | **mean 0 0 0, non-black 0/16384** | - |
| full-screen control (previous increment) | 23,366 / 11,683 | overwritten by the control | **mean 32 160 240, 16384/16384** |
| PS-only control | 22,896 / 11,449 | overwritten by the control | **mean 0 0 0, non-black 0/16384** |
| PS-only control, cull NONE | 24,750 / 12,375 | overwritten by the control | **mean 0 0 0, non-black 0/16384** |
| sampled textures | 22,042 / 11,022 | **mean 0 0 0, non-black 0/16384** | sampled image **mean 0 0 0, 0/16384** |
| full-screen control (same build as the two above) | 24,800 / 12,400 | overwritten by the control | **mean 32 160 240, 16384/16384** |
| sampled textures + constants + centre window | 23,484 / 11,742 | **centre window mean 0 0 0, 0/16384** | sampled image mean 0 0 0, 0/16384; V# mean 17, non-black 78/480 |

Limits and next falsifiers:

1. The draw index that selects the submission assumes no list is refused in between (every observed boot has 0
   refused); a refused list would offset it.
2. The dump prints D3D12 resource pointers, the command's own console VAs and the first 16 recorded user-data dwords,
   plus the sampled T#/S#/V# keys and the V# bytes' statistics and first 8 dwords. That is the project's
   id/count/dword convention; no package bytes, shader bytes or extracted tables are printed.
3. The control draws cover the whole (full-screen) or transformed (PS-only) target, so with either on the readback's
   first window is that control's result; the frame's own black is measured in the readback-at-20001 boot.
4. The sampled-texture probe reads 64 slots and prints 8 keys, copies a 128x128 window of the *first* T# only, and
   assumes the image is in GENERAL (set 0's layout); it does not write a control window into the image, because that
   is the title's texture. A black window says that window has no content, not that the whole image does.
5. Constants read-through: the V# base is resolved like the page pull resolves it - a rebased base reads the arena
   stand-in that mirrors the title range, an unrebased one reads the title's own memory - bounded to one 480-byte window
   and reported as byte statistics plus the first 8 dwords.
6. Next falsifier (geometry): dump the positions the translated vertex shader writes (or its input reads) for the
   armed draw, and compare the vertex bytes left at the view with what the input layout expects; the cull axis is
   closed by section 8, so the remaining question is whether the 3 vertices are fetched as the shader expects and where
   it transforms them.
7. Next falsifier (content): the other T#s of the same table and a window away from the origin of the sampled image,
   so a texture that is only black where it was sampled is not mistaken for an empty one.
8. `port/src/vulkan_replay.h`'s incremental replay path is unchanged: with every switch unset the recorded command
   stream, submission count and present path are byte-identical to the build before this increment.
