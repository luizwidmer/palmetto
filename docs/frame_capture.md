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

### The control readback line

The one-shot readback copies the target after the frame's commands **and** after any control draw, so when a control drew
the copied window is the control's output. The line now says so: it is labelled `frame dump PS control readback:` when
the PS-only control drew, `frame dump control readback:` when the full-screen control drew, and `render-target readback:`
only when neither did, and it ends with `the control drew after the frame's commands and this window is its output`. A
boot that prints the control-draw line but no readback line would mean the one-shot capture was never armed (the armed
draw's prepare did not reach the colour-target arming); with the label the control's coverage axis is readable on its
own, without confusing it with the frame's own (black) content. The measured PS-control line was
`frame dump PS control readback: ... mean 0 0 0, non-black 0/16384` for the title's pipeline through the first geometry
increments; `docs/vertex_path.md` section 8 records what changed: with the vertex-stream V# in the s0/s1 driver block and
a matrix V# over the constants buffer at the s8 table's slot 0 (the matrix experiment), the **same control reads
`mean 23 115 172, non-black 11,776/16,384, peak 240`** - the title vertex stage and raster state produce coverage, so the
coverage axis passes; the display target is still black because the title's pixel shader samples the empty sampled
textures (`docs/texture_source.md`), which is the next frontier, not the vertex path.

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

### The colour path, named (all switches on, real title matrix A)

The boot that closes the geometry and matrix work (`PALMETTO_GRAPHICS_CONSOLE_ROOT=1`,
`PALMETTO_VERTEX_STREAM_DESCRIPTOR=1`, `PALMETTO_SLOT70_PUBLISH=1`, `PALMETTO_VERTEX_PATH=1`,
`PALMETTO_FRAME_DUMP=1`, `PALMETTO_FRAME_DUMP_SAMPLE=1`, readback at 20001, no PS control) measures the colour path
directly:

```
render-target readback: 128x128 at 0,0 of 3840x2160, 4-byte texel: mean 0 0 0, non-black 0/16384, peak 0
write control: mean 51 102 204, non-black 16384/16384, peak 204
colour path: ps present, 6 image sample(s) -> the PS reads sampled images (no constant-output path);
             non-zero push constants: s0=… s1=… s8=… (3 of 32)
frame dump sample: key 1 T# image 0x… format 37 3840x2160, resource 0x…, title VA 0x…
frame dump sample: key 5 T# image is unresolved (set 0 holds no layer image for it)
frame dump sample readback [1/1] image 0x…: 128x128 window, 4-byte texel: mean 0 0 0, non-black 0/16384, peak 0
```

So exactly which pixel-shader inputs are empty/zero today:

* the one resolved sampled T# (key 1, the 4K R8G8B8A8 placed texture) reads back uniformly black over its whole copied
  window - the population nothing writes (`docs/texture_source.md`);
* the other T# (key 5) has **no layer image in set 0 at all**: the shader samples an unresolved input, which the layer
  reads as empty rather than as a black texture;
* the PS receives no colour-path scalars: of the 32 push-constant dwords only the two table addresses (`s0`/`s1`) and the
  console-table address (`s8`) are non-zero, so the PS's blend/factor arithmetic can only use those descriptors and the
  sampled texels;
* the PS has no constant-output path (6 image-sampling instructions), so a black display with all sampled input black is
  the expected arithmetic, not a discarding state.

The two experiments that would separate "texture data is the only gap" from "the PS discards for another reason" are the
next step and are named here so the mechanism is fixed: (a) a **substitution control** that binds a known-content image
into the PS's sampled table window - the shim's `write_image_descriptor` is the single place a T# becomes a set-0 layer
image (`bindless->insert_key` after `image_entries`), so a switch can point the first resolved T# at a small solid debug
image and the display readback says whether the PS arithmetic passes it through; and (b) a **debug sampling PS** that
outputs the title's own first sampled T# (reusing the title's vertex stage and descriptor layout with a project pixel
shader that samples T# slot 1), so the measured image can be seen at the display. Neither has been built yet; both need
a solid-image/debug-module addition in the shim, which is why they are the colour-path increment, not a measurement of
this one.

### The two-pass frame, measured: the sampled image is the sibling pass's render target

`PALMETTO_READBACK_AT_DRAW` now takes a comma-separated list, so one boot copies several draws' targets and prints each
with its draw index and resource id. The measurement (all switches on: console feed, vertex-stream descriptor, slot-70
publish, vertex path, frame dump, sample mode; two frames armed, draws 20000/20001 and 24000/24001 / 28000/28001):

| draw | pipeline (PS shape) | target resource | target readback | sampled T# |
|---|---|---|---|---|
| 20000 | 6 image sample(s) | 0x2f71567f9a0 | mean 0 0 0, 0/16384 | image 0x3085d055ad0 -> resource 0x3052d112890 (black at this draw) |
| 20001 | constant-output (0 images) | 0x3052d112890 | mean 0 0 0, 0/16384 | - |
| 24000 | 6 image sample(s) | 0x2f71567f9a0 | mean 0 0 0, 0/16384 | image 0x3085d055ad0 (28% non-black by now) |
| 24001 | constant-output | 0x3085d055ad0 | **mean 14 28 57, non-black 4608/16384, peak 204** | - |
| 28000 | 6 image sample(s) | 0x15975079a60 (**= present plane 0 resource of PresentX call 2**) | mean 0 0 0, 0/16384 | image 0x16a82045ad0 -> resource 0x166c0795040, **mean 14 28 57, 4608/16384** |
| 28001 | constant-output (0 images) | 0x166c0795040 | **mean 14 28 57, non-black 4608/16384, peak 204** | - |

The identity comparison is exact: the 6-image pass's sampled T# resolves to the **sibling constant-output pass's 4K
render target** (same layer image / same D3D12 resource), and the 6-image pass's own target is the **presented plane's
resource** (`0x15975079a60` = present plane 0 resource of call 2). So the "placed 4K texture nothing writes" from
`docs/texture_source.md` is not a texture the CPU should have filled: it is a **render target the title's own constant
pass fills through the GPU**. Nothing about page writes could see it, exactly as suspected.

What the two passes are today: the constant-output pass (pipeline `fspx/lib_ps_00820`) writes a mostly-black frame with
one blue element - the measured 128x128 origin window is 28% covered (4608/16384) with `mean 14 28 57, peak 204`, i.e. a
blue shape (the write control is 51/102/204, so the element is that colour at 28% coverage). Its own constants come from
a 96-byte matrix-A block (V# address 0x86ff20, stride 0, 96 bytes; content read 96 B: mean 17, non-black 9/96, peak 255 -
sparse, not empty) published for it. The 6-image pass (`fspx/lib_ps_00822`) samples that non-black target yet its own
target (the presented resource) reads black: of its 6 image-sampling instructions only the sibling target's T# resolves
(key 1); the other T# (key 5) is unresolved, and slot70's publication log reports 60 refused "texture-shaped
description (no pinned T# encoding)" - the likely source of the remaining sampled inputs.

### The remaining sampled inputs: T#s fetched out of the published matrix block

Widening the sample probe to every descriptor slot (and classifying slots that decode as neither T# nor V#/S# as data) shows the compose pass table (draw 20001 at frame 20000/20001) holds **exactly one T#: slot 1 = table + 0x20**, resolving to the sibling pass 4K target. The table slots at +0x40/+0x60 that earlier read as unresolved T# were float data (bf800000 3f800000 ...), not descriptors. The other sampled images are therefore not table slots: the compose pixel shader fetches T# descriptors itself out of the **published matrix block** - the s8 table slot 0 is a V# over a 5768-byte block (address 0x86de20 in that boot), and scanning that block (32-byte T# windows, 8-byte stride) finds its T#s, e.g. at +0xa0: image title VA 0x100001600, format 56, 3840x2160.

### Full thread-stack sampler (PALMETTO_THREAD_STACKS): no title-side frames on native stacks

`PALMETTO_THREAD_STACKS=<seconds,...>` (new, opt-in; `port/src/main.cpp`) suspends every other thread at each named time,
captures RIP, the thread start address and the code-range words of its stack (8 pages, 32 frames), and prints one line per
distinct stack with the threads that share it. Unlike `PALMETTO_WAIT_SAMPLE` it does not filter to threads inside a named
wait, so a dispatcher that is never blocked in a wait API would appear.

One 130 s boot at 2.5/5/7.5/10/15/20 s (boot-stacks1, ALIVE 130 s, 0 exceptions): 49-71 threads and 44-64 distinct
stacks per sample. The stacks are populated - but with **host-side frames only**: ntdll/KERNELBASE/dbghelp/
gameinputredist/nvoglv64/D3D12Core, the port's own `palmetto.exe` and its `palmetto_tls_host_*` thunks, and Windows
worker threads. **No frame in the title image appears at any sampled time** (no `image+`/`game+` RVA in any stack), so
this method does not see the loader job or the stage dispatcher: the title's code runs on the emulated CPU threads, whose
guest stack pointer is not the native RSP the sampler reads (the native stack holds the host trampolines), and the loader
job was not executing on any native stack at those instants anyway.

Named next step: walk the *guest* stack for each emulated title thread (the emulator's per-thread stack pointer, i.e. what
streamtrace's loader-chain walk already does for the one job thread) and filter/annotate by thread role; the native-stack
sampler should stay as the host-side counterpart. That is where the dispatcher RVA will come from - this boot gives no RVA
candidate from native stacks.

### The 1,200 s boot: still black, steady loop, and the fallback list slots

A 1,200 s boot (all fixes on, presenter captures every 300 presented frames, full-target readbacks at four late draws,
console list fallback-slot trace on) shows the same state as 300 s and 600 s:

* presenter captures (frames 1, 301, 601, 901, 1,201, 1,501, 1,801, 2,101; swapchain 2564x1421) - **every one**
  `mean 0 0 0, non-black 0/3,643,444, peak 0`;
* full-target readbacks of the composed 4K target at draws 150,000/180,000/210,000/240,000 - all the 0-image-sample
  constant pass, each `mean 0 0 0, non-black 0/8,294,400, alpha 0`;
* population: 2,758,217 calls / 139,264 submissions at 600 s grew to 5,741,116 calls / 299,008 submissions at 1,200 s
  with the same cadence (~4,096 submissions per ~16 s); submission 229,376 at 908 s and 299,008 at 1,180 s. No new
  pipeline, module, present content or target content appears; the stationary two-pass blit/clear loop persists.

Console list fallback slots (the inline root-argument writers' vtable fallback at the user-data limit; shape only,
opt-in `PALMETTO_LIST_SLOT_TRACE`): in the 1,200 s window slots **20, 24 and 47** are called; 35/43/45 never are.
Measured shapes, repeating:
* slot 20: `(0x11, 1, 0, 0x43)` - an index/count shape with index 17, count 1 and a value in the fourth argument, and
  `(4, 0, 0x20d54df0000, 0)` alternating with it;
* slot 47: `(0, 0x20, 0, 0)` - index 0 with count 32;
* slot 24: all-zero arguments.

So a root-32-bit-constant-like payload is present on the fallback path during the load window (index/count shapes on
slots 20 and 47), but no forwarding is implemented: whether the sibling pass's signature declares that constant is
rootargs' question, and the numbers above are the measured shape to compare against. The title remains in the same
stationary state at 1,200 s, which points at a loading stall (streamtrace's investigation), not at missing present
content.

### 300 s and 600 s: the presented swapchain image never becomes non-black

Two long boots measured the displayed resource directly through the presenter path plus full-target readbacks of the
composed 4K target, all fixes on (console root feed, vertex-stream descriptor, slot-70 publish):

* **Presenter captures** (`PALMETTO_CAPTURE`, the swapchain image copied at present): 6 captures over a 300 s boot
  (frames 1, 61, 121, 181, 241, 301) and 5 over a 600 s boot (frames 1, 121, 241, 361, 481), each 2564x1421,
  every one `mean 0 0 0, non-black 0/3,643,444, peak 0`. The presented image is uniformly black for the whole window.
* **Full-target readbacks of the composed 4K target** at late draws: 300 s boot draws 36,000/44,000/52,000/56,000 and
  600 s boot draws 80,000/95,000/110,000/120,000, all the same 0-image-sample constant pass (`fspx/lib_ps_00820`,
  pipeline 0x1d51c49f640 in the 600 s boot), all `mean 0 0 0, non-black 0/8,294,400, alpha 0` - no content anywhere in
  the target at any sampled frame.
* **Population over time**: 1,305,811 traced calls / 65,536 submissions at 300 s and 2,758,217 calls / 139,264
  submissions at 600 s - the submission cadence is steady (~4,096 submissions every ~16 s in both windows), the slot-70
  publish count grows in step (32,693 then 69,557), and the sampled late draws are the same constant pass. No new
  pipeline or shader module appeared late in either boot; the only translated modules ever seen on these draws are the
  four FSPX blit modules (`lib_vs_00820`/`lib_ps_00820` for the constant pass, `lib_vs_00823`/`lib_ps_00822` for the
  compose pass).

Plainly: **the title has not reached content rendering in 300-600 s**. The frame is a stationary two-pass blit/clear
loop (constant pass + compose pass, two draws per submission, both writing the same 4K target) that presents a black
swapchain image; the constant pass's own pixel shader is a zero-image-sample module whose colour comes from a user-data
block its one-table root signature does not define, so the replay has nothing to feed it. This looks like a
pre-content/loading state of the title (assets loading, the same pair of passes run at a constant rate with no scene
draws), not a translation artefact of a content frame. What would advance it: a much longer boot (the loading phase may
take minutes), or an input/loading gate the harness does not drive (a title screen waiting for a button). Neither is
cheap enough for this increment; the 600 s boot shows the same state as the 300 s one.

### Root-data trace: the carrier search for the sibling pixel shader's user data

Instrumentation added (bounded, presence classes and counts only): `PALMETTO_ROOT_DATA_TRACE=1` prints one line per
draw with a root-constant parameter (plus a few others): the bound layout's parameters (type, console slot, dword count,
non-zero count in the recorded block), the recorded block's s0..s31 zero/non-zero map, whether any argument was written
since the signature bind, and how many non-zero dwords a later signature rebind cleared. A root-signature rebind that
drops recorded arguments logs once, with the count.

One boot with it resolves the three candidates:

* **(a) cleared by a rebind - no.** The traced draws report `rebinds 0 (cleared 0 dword(s))`; no non-zero recorded
  argument was ever dropped by a signature rebind.
* **(b) written but not inherited - no.** Every traced draw reports `written since bind 1`, and the recorded block is
  exactly the console feed's content for the signature's parameter.
* **(c) the title never provides it as root arguments - yes.** The sibling pass's pipeline is bound to the same
  one-parameter root signature the two FSPX pipelines use: `p0 t2 s8 dw1 nz1` (one descriptor table, one dword, at console
  slot s8), and the recorded block is `s0..s31 = xx......x.......` - s0, s1 (the translated table pair) and s8 (the
  table) non-zero, everything else zero. There is **no root-constant parameter** in that signature, so no colour
  constants could travel through the root-argument system at all.

The two other carriers are also negative:

* **Slot 55 (CreateDerivedPipelineStateX)**: measured histogram over a boot: all 40,000 calls reached the PC
  CreateGraphicsPipelineState with `S_OK`, 0 refused, and the only patch parts carried were 0 (depth-stencil) and
  2 (blend). The decoder refuses any part id outside 0..3 with E_NOTIMPL (it is not silently dropped or ignored), so a
  user-data patch part does not occur in this title's derivations.
* **Slot 57 record**: the record's pairs are pointer/size pairs (program, blob, state blob, input layout); the stage-slot
  pairs name program pointers, and the validated 0x108-byte region has no user-data field; the pipeline-creation path
  never receives a user-data array.

Conclusion: of the carriers this tree models, none holds the sibling pixel shader's user-data dwords; its only recorded
input is the one-table root signature. The next question is therefore not "which carrier is missing" but "what is the
sibling constant pass for": a zero-image-sample module whose colour chain is built from the user-data block it is given
(with a one-table signature that block carries only the table pointer) may be a utility/flag pass rather than the
content pass, and the visible frame may come from different draws/targets. The next instrumentation is to full-target
scan the targets of the draws that present (which draws write the present plane resource across frames), not to forward
more root arguments.

### Where the user data lives: the pixel-stage console pack is dropped before the draw

The fix's reconnaissance (data path, no raw values):

* **console device slot 55 (CreateDerivedPipelineStateX) does not carry it.** The shim's patch-part histogram shows the
  derived states are patched only in fixed-function parts (0 depth-stencil, 1 rasterizer, 2 blend, 3 sample mask), i.e.
  pipeline-state parts, not user-data registers.
* **The recorded per-draw block is `Draw::user_data` (32 dwords)**, produced by the API layer from the root-argument
  state: the console-layout pack (`console_graphics_user_data`, `pack_stage(signature, Stage::Vertex)`) plus the
  console feed's table pointers, and the replay copies it into push-constant member 0 and rebases the table VAs. Its
  provenance shows 3 of 32 non-zero - and those three are the console feed's **synthetic table pointers (s0/s1/s8)**, not
  title constants.
* **The graphics pack is vertex-stage only.** `console_graphics_user_data()` sets `placement.stage = Stage::Vertex` and
  `pack_stage(signature, Stage::Vertex)`; the pixel stage's parameters sit at a different console base
  (`rootsig::stage_base(Stage::Pixel)`), and the root-argument writer stores only dwords below the 32-user-SGPR limit:
  `write(RootState&, dword, value)` keeps `dword < kMaxUserSgprs` and otherwise hits the
  "root arguments beyond 32 user-data dwords (spill not modelled)" stub. So the **pixel stage's user data is dropped
  by the recording path**, and the translated pixel shader's push-constant member 0 (which is exactly the pixel stage's
  32 user SGPRs) receives nothing but the common block - the sibling PS's colour constants among them.

Fix candidate (concrete, and it crosses files): record the pixel-stage console pack alongside the vertex one (a second
32-dword block in the root-argument state, placed with `stage_base(Stage::Pixel)` instead of the current
`dword < 32` cut), carry it on the draw, and emit it as the pixel stage's push-constant member 0 while the vertex stage
keeps the current block. That is the "forward the recorded user data" change the sibling PS needs; until then the shim
keeps filling only the synthetic table pointers and the constant pass writes black.

### The sibling pixel shader's colour source: the push-constant user-data dwords, which the layer feeds as zero

The constant-output sibling PS (`fspx/lib_ps_00820`) has no image samples, no fetches and no kill. Its single output
store is `OpConstantSelect`-style: `OpSelect %true (v4 built from UnpackHalf2x16 words) (old)`, so the colour is the
packed-half result of a scalar chain. Walking that chain backwards gives the source:

* the two words the final colour unpacks are private-slot values (`%160[5]`, `%160[6]`, via `%283`/`%284`);
* those slots are filled by packed-half computations over values copied from **push-constant member 0** - the module
  copies `member0[0..31]` (all 32 dwords) into a private array at entry (`%161[i] = load %5.member0[i]`, i = 0..31) and
  then packs derived values back into the colour slots;
* the module accesses push-constant members 1 and 2 as well (the translated table pointers), reads no vertex attribute
  into the colour chain (the only interpolated input `%4` is untouched by the traced chain), and contains only the
  ordinary float->half packer literals.

Our feed (the provenance line, sibling draw): `non-zero push constants: s0=9bd5ef00 s1=00000178 s8=0cf6ef00 (3 of 32)`.
The compose PS of the same frame feeds the same shape (`3 of 32`) but reads **zero** member-0 data dwords, so the zeros
cost it nothing; the sibling PS reads all 32 and depends on them for its colour. So the named zero input is:

> **push-constant member 0 (the console user-data dwords) for the constant-output pass**: 29 of 32 dwords are fed as
> zero, and that is exactly the data the sibling PS's colour is computed from, so it writes black.

Vertex-attribute classification of the armed 170-record range (boot-sibling3; that boot's armed draw turned out to be the compose pass, 6 image samples, so this is the compose pass's vertex buffer; the sibling pass's first vertices in boot-sibling2 are the same full-screen NDC triangle shape): only 24 bytes are non-zero;
component 2 is all zero, components 0/1/3/4/5 have 1/2/3/1/2 non-zero records respectively - the vertex buffer is the
full-screen NDC triangle's coordinates with the rest zero, no colour attribute. This is consistent with the traced
colour chain and rules the vertex data out as the colour source.

Fix candidate: forward the draw's recorded console user SGPRs into the translated push-constant member 0 (for at least
the indices the shader reads) instead of zero-filling everything except the three table pointers. The next concrete step
is to dump the sibling draw's recorded root-argument provenance (the user-data values the console feed carries at that
draw) and compare it with the zero-filled block; if the recorded feed has the values, the fix is a forwarding change in
the push-constant emission, and if it does not, the console user-data capture needs extending for that pipeline.

### Sibling pass first: coverage works, its own PS output is the gap

The sibling constant pass is the draw that writes the 4K target the compose pass samples. Arming it alone
(boot-sibling2, draw 28001 this boot) gives its identity and its control at full resolution:

```
readback armed at draw 28001: pipeline 0x1869f058650 target resource 0x18639e57870 image 0x18603115ad0 format 37 3840x2160
colour path: ps present, 0 image sample(s) -> constant-output path; push constants s0=9bd5ef00 s1=00000178 s8=0cf6ef00
vertex path: pipeline 0x1869f058650 vs "fspx/lib_vs_00821" ps "fspx/lib_ps_00820"; layout problems 0x0
vertex path: vb0 va 0x1789bd5ef30 size 4080 stride 24; resolved buffer ..., stride-at-draw 24
vertex path:   v0 bf800000 3f800000 ...   v1 bf800000 bf800000 ...   v2 3f800000 bf800000 ...
matrix A slot (s8+0): 0166ff20 00000000 00000060 30014fac -> V# address 0x166ff20 stride 0 records 96 format 20
matrix A content: read 96 of 96 B: mean 18, non-black 9/96, peak 255; dwords ... 00000000 ffffffff 0000ffff b2392200
PS control readback [draw 28001]: 3840x2160: mean 16 80 120, non-black 4,147,200/8,294,400, peak 240, alpha mean 127
full-target tile map: half-screen lower-left triangle (64800, then 129600 + 64800 per row)
```

Read as a statement:

* the sibling pass's geometry is a full-screen NDC triangle in its vertex buffer (v0 -1,1 / v1 -1,-1 / v2 1,-1,
  stride 24) fetched through the console vertex-stream descriptor fix (vb0 va 0x1789bd5ef30), viewport 3840x2160;
* with fs_solid substituted for its own pixel shader, the sibling target **does** receive content over the same
  half-screen triangle the compose pass covers - so the sibling pass's rasterisation, target binding, viewport and
  vertex path all work;
* with its own pixel shader (`fspx/lib_ps_00820`, a constant-output module with zero image samples and no kill) the same
  target reads uniformly black at full resolution, so the gap is inside the sibling pass's own PS output, not its coverage;
* the concrete matrix-A difference: the compose pass's s8 slot 0 is a V# over a 5,768-byte block holding an identity 4x4
  matrix, while the sibling pass's s8 slot 0 is a V# over a **96-byte block whose content is flags, not a matrix**
  (`ffffffff 0000ffff b2392200`, 9 of 96 bytes non-black). Coverage still works from the NDC vertex buffer, so this is
  not the raster gap; it is the next data-path difference to close (the sibling pipeline's matrix A publication).

Next concrete step: trace the sibling PS's output chain offline (`fspx/lib_ps_00820`, `%820/%822/%826` built from packed
half stores fed by its private slots) to its exact source - a vertex colour attribute (all-zero in the sibling vertex
buffer) or a root/push constant the shim feeds as zero - and dump the sibling's full vertex attribute set. That names the
input the sibling pass is missing before any further substitution experiment.

### Full-target readback: the display target is uniformly black, and the passes cover a half-screen triangle

`PALMETTO_READBACK_FULL=1` copies the **whole** armed target (3840x2160, 8,294,400 pixels: one copy, no write control)
and reports mean RGB/alpha, non-black count, peak and an 8x8 tile occupancy map. Measured across three boots:

| boot | armed draws | target | full-target result |
|---|---|---|---|
| boot-full1 | 28000, 28001 | sibling 0x29928ea2fc0, compose 0x28b17047f70 | `mean 0 0 0, 0/8,294,400, peak 0`, alpha 0, **all 64 tiles 0** |
| boot-full2 | 24000, 24001 | sibling 0x1e11b241370, compose 0x1d30c1186a0 | same: `0/8,294,400`, all 64 tiles 0 |
| boot-full3 | 8000, 8001, 14000, 14001 | sibling 0x21a2190f990, compose 0x21a2196c100 | same: `0/8,294,400`, all 64 tiles 0 |

So the black is **not** a 128x128 origin-window artefact: the entire 4K display target (present plane resource) and the
sibling target carry no non-black pixel anywhere. The transient blue seen in an earlier boot's 128 window
(`mean 14 28 57, 4,608/16,384` in boot-twopass4) is not reproducible with full-target scanning.

The control axis at full resolution (boot-full4, PS-only substitution of the compose pipeline with fs_solid):
the same full target then reads `mean 16 80 120, non-black 4,147,200/8,294,400, peak 240, alpha mean 127` with a
**triangular tile map** (row 0: one 64,800 tile; row 1: 129,600 + 64,800; ... a half-screen lower-left triangle). So the
compose pass's geometry covers roughly half the target in a triangle and the target can hold content - but with the
title's own pixel shader the same target is uniformly zero, over that same coverage.

Conclusion: the display target is uniformly black because **the title PS writes zeros over its covered triangle**, not
because content lives outside the measured window or the target identity is wrong. The next falsifier is the already
scoped substitution control: bind a known solid debug image (through the same `write_image_descriptor` hook) as the
compose pass's sampled T#, then read the full target: output zero means the composition arithmetic has a zero weight,
output equal to the debug colour means the sampled content was zero at those pixels and the arithmetic copies it.

### Pre-draw capture and pass order: the passes share one list, sibling first, compose second

The readback-armed submission now prints its list/draw order and copies the compose pass's sampled texture at the list
it belongs to (`pre-draw sample readback`, next to the post-submission `frame dump sample readback`). Measured
(`boot-predraw1`, all switches, armed at draws 28000/28001):

```
readback armed at draw 28000: pipeline 0x2af9ef39480 target resource 0x2af9c4db820 ...
readback armed at draw 28001: pipeline 0x2afb03dfa10 target resource 0x2a1c6cd95f0 ...
order: submission 28005 list 0 of 2: 8 command(s), 2 draw(s), the readback-armed draw is here
order:   draw 0: pipeline 0x2afa74c9e58 target resource 0x2af9c4db820
order:   draw 1: pipeline 0x2afa445fa48 target resource 0x2a1c6cd95f0 (armed)
render-target readback [draw 28000 target resource 0x2af9c4db820]: mean 0 0 0, 0/16384, alpha 0/16384
render-target readback [draw 28001 target resource 0x2a1c6cd95f0]: mean 0 0 0, 0/16384, alpha 0/16384
colour path id match: sampled image 0x2b2d6365ad0 (resource 0x2af9c4db820) is draw 28000's target
pre-draw sample readback [1/1] image 0x2b2d6365ad0 (resource 0x2af9c4db820): mean 0 0 0, 0/16384, alpha 0
frame dump sample readback [1/1] image 0x2b2d6365ad0: mean 0 0 0, 0/16384, alpha 0
```

Read as a structural statement:

* both passes of the frame are **draw 0 and draw 1 of the same command list in the same submission** (list 0 of 2; list 1
  carries one command and no draw), and draw 0 executes before draw 1 - so the sibling pass runs first and the compose
  pass second, in the recorded/executed order our replay emits;
* the compose pass's sampled T# resolves to **draw 0's target** (same layer image, same resource `0x2af9c4db820`), i.e. it
  samples the sibling pass's output of the same frame and same list, not the other ping-pong buffer and not the previous
  frame; no render-target clear was counted in this submission's lists at this frame;
* the pre-draw copy and the post-submission copy are identical here because both are recorded after list 0, and list 0
  contains both draws: splitting the list emission at the armed draw is what a true pre-compose copy needs, and it is the
  one instrument change this measurement could not make without restructuring `emit` per draw. Because draw 0 precedes
  draw 1 in the same list, the content after the list IS the content the compose draw sampled, so the comparison still
  answers the question.
* at this frame the sibling target window was **black at both moments** (`0/16384`, alpha `0`): the blue element is
  transient frame to frame (the same window at the same draw indices read `mean 14 28 57, 4,608/16,384` in `boot-twopass4`),
  so the compose pass does sample black content on this frame. In frames where the window is blue, the sampled content is
  blue (the id match and the sample readback say so) and the compose target still reads black, so the remaining gap is
  the composition arithmetic/state, not the pass order, the buffer identity or the clear placement.

Next falsifier, smallest: (a) a true pre-compose copy by emitting the list up to the armed draw and copying there (one
`emit` split), and (b) the sample-coordinate trace - the pixel shader's sample coordinate (`%534`, built from the input
varyings) against the window that carries the blue element, which says whether the composition reads the region we
measure or a different one (mip/UV) and would close the arithmetic question without changing state blindly.

### Fourth channel and the compose pipeline's own coverage control (measured)

Readback lines now report channel 3 alongside 0..2 (mean/non-black/peak), and one boot (`boot-alpha1`, all switches plus
the PS-only control, armed at draws 28000/28001) measures both open questions:

- **The sampled image's alpha is not zero.** The compose pass's sampled T# readback (the sibling pass's 4K target, late
  frame) is `mean 23 115 172, non-black 11,776/16,384, peak 240, alpha mean 183, alpha non-black 11,776/16,384, alpha
  peak 255`: the blue element carries alpha 183..255 over the same coverage as its RGB, so an alpha-weighted composition
  is not fed a zero alpha. The alpha-zero hypothesis is refuted.
- **The compose pipeline's own vertex stage and raster state produce coverage.** Substituting the compose pipeline's
  pixel shader with fs_solid (title VS `fspx/lib_vs_00821`, cull-none) at the same draw leaves
  `mean 23 115 172, non-black 11,776/16,384, peak 240, alpha mean 183, alpha peak 255` in the compose target: the compose
  geometry, target binding, viewport/scissor, format and readback path all write, so the compose pass's raster/coverage is
  fine and only its pixel-shader arithmetic or the content it consumes at sampling time can produce the black output.
  (Caveat: the sibling constant pass substituted the same way read black in this boot although its real PS paints the
  blue element; that is a pass-specific coverage difference worth a separate look.)

Offline arithmetic trace of `fspx/lib_ps_00822`: the stored colour is two packed half2 words loaded from the PS's private
slots (`UnpackHalf2x16` of the two final words, then `OpCompositeConstruct` + `OpStore %3`), i.e. the PS writes the
**packed upscale result** of a long scalar FMA/mix chain. The chain's operands are the three typed views of the one
sampled texture (v4int/v4uint/v4float views, six sampling instructions) combined through `OpExtInst Fma` with unpacked
half constants and bitcast float constants; there is no path in which the stored colour is a constant alone. So the
disassembly names no single zero operand - the leading suspect is the **content at sampling time**: the compose draw
precedes the sibling pass in the submission, so it samples the sibling target before that pass writes this frame's blue
element (our post-submission sample readback sees it after the sibling drew). The decisive next measurement is a copy of
the sampled window **before** the sibling pass writes it (a pre-draw capture of the compose pass's sampled T#), which
distinguishes a clear/order gap from PS arithmetic.

### Offline trace of the compose pixel shader (`fspx/lib_ps_00822`): six samples, one texture, no discard

Disassembling the compose PS (structure only; instruction indices and ids, no shader bytes) answers what its six image
samples are:

| sample instruction (word offset, opcode) | sampled image | source of the sampled image |
|---|---|---|
| 2882, OpImageSampleImplicitLod | %557 | `OpSampledImage` over a `UniformConstant` array (`%9`) load at the index `%519` |
| 2887, OpImageSampleExplicitLod (+ Grad) | %557 | the same sampled image, so the same texture |
| 2997, OpImageSampleImplicitLod | %578 | `OpSampledImage` over a second typed array (`%8`) load at the same index `%519` |
| 3002, OpImageSampleExplicitLod (+ Grad) | %578 | the same, again |
| 3094, OpImageSampleImplicitLod | %594 | `OpSampledImage` over a third typed array (`%10`) load at the same index `%519` |
| 3099, OpImageSampleExplicitLod (+ Grad) | %594 | the same, again |

`%519 = OpGroupNonUniformBroadcastFirst` of a hash (`%213`) computed from four dwords **loaded through a physical-storage
pointer at `heap_base + (value & ~3) + 0x20`** - the table slot 1 T#, i.e. the descriptor the layer resolves (the sibling
pass's 4K target). All three sampled-image arrays are indexed with that one hash, and the sampler array (`%11`) is indexed
by a hash of a **literal sampler descriptor** the shim already registers (`register_literal_samplers`). So the six
sampling instructions are **three typed views of the one available texture, each sampled twice (implicit LOD and explicit
Grad)** - the compose pass is not missing sampled inputs at all, and the runtime's `ps_images = 6` is that count.

State: the module has exactly one `OpKill`, at the end; its predicate is a private bool initialised `true` and recomputed
from a subgroup-uniform `!= 0` test, so in the measured state the branch reaches `OpRetur`n and the colour is written
(`OpStore %3` before it). There is no alpha test, no depth/stencil, `RenderTargetWriteMask = ALL` and blend disabled in
the documented-default state the port uses for this pipeline, with colour format 37 on one 4K subresource - so the output
is neither discarded nor masked, and the sampled input the shader reads is the target it should be.

The conclusion for the colour path: the compose PS reads the sibling pass's target through its only available T#, samples
it six times, kills nothing and writes all channels. Its black output therefore comes from the **composition arithmetic or
the sibling pass content it consumes at that instant**, not from a missing sampled descriptor: the remaining measurements
are (a) the sibling target's alpha channel (the readbacks so far count only the first three channels; a constant pass
writing RGB with alpha 0 can make a composition that mixes by alpha output black), and (b) a PS-only control on the
compose pipeline's own vertex stage (`fspx/lib_vs_00821` + fs_solid) at a late frame, to prove the compose geometry
covers independently of both pixel shaders - the same acceptance shape that closed the title vertex path.

Page-binding attempt (PALMETTO_TEXTURE_BIND_FETCHED): the fetched T# at block + 0xa0 names title VA 0x100001600 (format 56, 3840x2160); with no created resource at that VA the layer asked the existing title-memory hook whether the pages carry bytes at all: **the pages are NOT readable** (neither an arena-mirrored console range nor committed title memory). So the compose pass's other sampled input is not a resource the layer can bind and not title page data it can import: the producer is missing, or the +0xa0 window is not a real T# at all (the published block is mostly small values, and a 32-byte window there can imitate a descriptor). The full-block scan (all 32-byte windows at 8-byte stride over the whole 5,768 bytes) finds exactly two candidates: +0x98 (address 0, rejected) and +0xa0 (the one above); no other sampled input is fetched from the block. Next step is offline and needs no boot: disassemble the compose pixel shader (fspx/lib_ps_00822) and trace its six image-sampling instructions to their descriptor sources, which says whether the shader re-samples the one table T# or expects descriptors the layer never provides.

Chain for a missing sampled input: PS fetch -> published matrix block (V# at table + 0x00, 5768 B, written by the slot-70 publication) -> T# at block + 0xa0 -> image title VA 0x100001600 (format 56, 3840x2160). Binding it (PALMETTO_TEXTURE_BIND_FETCHED: a registry of the slot-133 placed creations by title VA, then a layer image bound for every fetched T# whose address matches) found **no placed resource at 0x100001600**: the registry only knows title-placed creations, so nothing was bound and the readbacks did not change. The named next gap is that VA: a resource the layer never created a view or registry entry for. The likely producer map is the console creation ring (slot 70 pairing); if the VA is a created resource, register all console resources (not only placed ones) and bind from it; if it is a heap VA, the layer needs a layer image built from the title pages (texture-from-pages).

So the structural unknown is resolved: **the frame is a constant/background pass plus an upscale pass that samples the
previous pass's render target, and the display target is the upscale pass's**. The remaining black is the upscale pass's
composition, whose other sampled inputs are missing/unresolved; the next increment is the texture-shaped T# publication
(slot70) or the substitution control that binds a known image into the upscale pass's sampled window, then the same
two-draw readback shows whether the upscale output follows its input.

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
