# Frame capture: the order and targets of one real frame

**Status: measured increment.** `docs/page_content.md` section 4 concluded that the draw's own render target read back
black while being "one of the ids the present names as its plane". This page records the diagnostic that reads
*whether that was true*: for one real frame it prints the commands in replay order with the resource ids each one
reads and writes, and compares the draw's target with the resource the present blits. The measurement corrects section
4 (the one-shot readback armed at the first/even draw reads the frame's **intermediate** target, not the display
resource), shows the frame's record is clean (one clear of the display resource, before the draws; the last writer is
the draw; no copy or resolve anywhere), and then draws the project's own known-good full-screen triangle into the very
same display resource: it reads back exactly its colour. So the replay's raster, target binding, viewport, subresource
and readback path are proven for the presented target, and the title's own draw is what leaves it black. Ids, counts,
recorded argument dwords and the project's own wording only; no package bytes, paths or extracted tables.

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
explanation is the draw's own pixel output: empty sampled textures and/or root constants (the measured content gap of
`docs/page_content.md` sections 6-8), a pixel shader that computes black from them, or fixed-function state that
discards its fragments.

## 6. What was added, and what is next

Added: the `frame_dump` diagnostic in `port/src/vulkan_replay.h` (`PALMETTO_FRAME_DUMP`,
`PALMETTO_FRAME_DUMP_AT_DRAW`, `PALMETTO_FRAME_DUMP_WINDOW`), its call in `VulkanQueue::execute_now`
(`port/src/vulkan_backend.h`), the known-good pipeline control (`PALMETTO_FRAME_DUMP_CONTROL` with
`PALMETTO_FRAME_DUMP_CONTROL_VS`/`_PS`, the reserved pipeline id and the lazily created `vs_tri`/`fs_solid` pipeline in
`VulkanContext`), and `port/test/frame_dump_test.cpp`. Everything is diagnostic-only and off by default.

Measured runs (120 s harness boots, same build, 0 exceptions, 0 failed/refused submissions; killed by the timeout as
usual):

| Boot | Draws / presents | Display-resource readback | Control readback | Present capture |
|---|---:|---|---|---|
| dump at draw 20000 | 22,322 / 11,162 | intermediate target, mean 0 0 0 | - | - |
| readback at draw 20001 | 22,186 / 11,093 | **mean 0 0 0, non-black 0/16384** | - | mean rgb 0.00 0.00 0.00 (8/8) |
| control at draw 20001 | 23,366 / 11,683 | frame's content then overwritten by the control | **mean 32 160 240, non-black 16384/16384** | - |

Limits and next falsifiers:

1. The draw index that selects the submission assumes no list is refused in between (every observed boot has 0
   refused); a refused list would offset it.
2. The dump prints D3D12 resource pointers, the command's own console VAs and the first 16 recorded user-data dwords.
   That is the project's id/count/dword convention; no package bytes, shader bytes or extracted tables are printed.
3. The control draw covers the whole target, so with it on the readback's first window is the control's colour (the
   frame's own black is the readback-at-20001 boot's measurement, not that boot's).
4. Next falsifier: **the title draw's own pixel output.** Mid-submission readback (a copy of A inserted between draw
   `#7` and the list-1 barrier) would separate "the pixel shader writes black" from "the fragments never run"; a
   second control that keeps the title's vertex shader and pipeline state but substitutes a constant-output pixel
   shader would tell whether its geometry produces coverage at all. Both are one-shot diagnostics on the same switches.
5. `port/src/vulkan_replay.h`'s incremental replay path is unchanged: with every switch unset the recorded command
   stream, submission count and present path are byte-identical to the build before this increment.
