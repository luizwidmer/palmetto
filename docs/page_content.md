# Page content: copying the title's pages into the GPU's address space

**Status: measured increment.** `docs/page_address_space.md` put the title's page VAs into the one GPU address space,
so a draw resolves and executes. Its stand-ins still hold the layer's own (zeroed) bytes, not the title's pages: the
shader read correct addresses but stand-in data. This page records the fix that copies the bytes across, the measured
evidence that the copy happens with real content, and the measurement that now locates the frame's remaining blocker
outside this increment: the draw's own render target reads back uniformly black even though the draw is bound to the
resource the present blits.

Counts, VAs, ids and measured values only; no package bytes, paths or extracted tables.

## 1. Where the title's pages are, and that the CPU can read them

The title's 174 MiB placement (device slot 133) is an xmem range: the title maps physical pages, then places the
buffer on them. `rt::xmem::commit_physical_range` is registered as the slot's page hook (`port/src/main.cpp`,
`set_placed_at_va_pages_hook`), so the pages are committed **before** the slot-133 handler runs, and the CPU can read
what the title writes there. Measured in the pull boot: every copied page was `MEM_COMMIT` and readable, and the copied
bytes were not stand-in zeros - a vertex view of 0xFF0 bytes carried 24 non-zero bytes (three vertices of eight bytes,
the full-screen triangle the draw names) and an index view of 0x1E0 bytes carried 78 non-zero bytes.

The title VAs in that boot (ASLR per boot; the previous page's layouts have the same shape):

```
[rt] replay: title VA range 0x1C380B00000 + 0xAE00000 (buffer) is arena VA 0x10000
[rt] replay: title VA range 0x1C380000000 + 0x2600000 (descriptor heap) is arena VA 0xAE10000
```

The two ranges are in the same xmem region and overlap by 27 MiB, which is why `port/src/console_va_map.h` decides a
VA's meaning by the site (buffer vs descriptor heap) as `docs/page_address_space.md` section 2 records.

## 2. What is copied, and when

The copy is on demand, per draw, while the draw is prepared and before its command buffer is recorded
(`port/src/vulkan_replay.h`, `prepare_draw`; `port/src/vulkan_backend.h` and
`port/shims/d3d12_vulkan_device.cpp` for the host copy). Three sources, all the buffers a draw can read:

1. **Vertex and index views.** The view's title VA and byte count are exactly known, so that range is copied to the
   arena range that stands in for it (`arena VA = title range's arena + (va - title)`). The copy is page-wise: a page
   that is not committed is left as the stand-in has it.
2. **Constants and other buffer descriptors.** The draw's user data names its descriptor table; the table's slots are
   read from the console heap's arena mirror (already up to date from the per-submission heap refresh) and every
   decodable V# is resolved: its base is either the arena VA the page-address-space work rebased it to (reverse lookup
   gives the title span) or an unrebased title VA. The V#'s declared byte range (stride x records, or bytes) is copied,
   capped at 1 MiB. Measured: each draw's table has exactly one V#, and it is title-named and pulled.
3. **Root descriptor VAs.** Any other 64-bit user-data window that is a title VA gets a bounded 64 KiB window copy.

So the pull is complete for what these draws read: vertex, index and constants are title pages and are pulled; textures
and samplers are not page bytes (they are the layer's own images and sampler state, reached through T#/S# keys).

The measured cost is small: the first boot's pull lines show 0xFF0 + 0x1E0 bytes per draw; the frame loop slowed by a
few percent against the pull-off boot (submission 8,192 at 35.0 s off vs 38.6 s on; 24,576 at 100.4 s off vs 104.9 s on).

## 3. The A/B measurement, and what it rules out

Two clean 150 s boots, same tree, capture on (the presenter's `PALMETTO_CAPTURE` PPM readback of the presented swapchain
frame), the second with `PALMETTO_PAGE_PULL=0`:

| | pull off (`boot-content-off`) | pull on (`boot-content-on`) |
|---|---:|---:|
| Run | ALIVE 150.3 s, 0 exceptions, 0 device-lost | ALIVE 150.2 s, 0 exceptions, 0 device-lost |
| `ExecuteCommandLists` | ok 33,219, **failed 0, refused 0** | ok 30,944, **failed 0, refused 0** |
| Draws / presents | 16,609 / 16,609 ok | 15,471 / 15,472 ok |
| Captured presented frames | mean (0,0,0), non-black 0/921,600 in all 24 | mean (0,0,0), non-black 0/921,600 in all 24 |

So the page copy is healthy and drops no submission, and it does **not** change the presented picture - the frame was
already black for a reason further up the pipeline. The capture path itself is proven: the render-target readback in
section 4 copies and reads real bytes through the same layer, and reports zero content, so the black captures are black
content, not a broken capture.

## 4. Where the frame is now blocked: the draw's own target reads back black

A third boot added a one-shot diagnostic: for the first draw, the resolved colour attachment is copied to host memory
in the same submission and read back after it completes (`arm_attachment_capture` in `port/src/vulkan_backend.h`).
Measured:

```
[rt] replay: draw attachments: rt0 id 0x1D0B1D39980 image 1 format 37; area 3840x2160 layers 1;
            viewport 3840.0x2160.0 at 0.0,0.0 depth 0.000..1.000; topology 17; vertices counts 3/1 first 0
[rt] replay: render-target readback: 128x128 at the target's origin of 3840x2160, 4-byte texel:
            mean 0 0 0, non-black 0/16384, peak 0
```

and, from the same boot's present trace:

```
[rt]   plane 0 resource 0 (call 10000): 000001D0B1D38EE0
```

The draw's colour target is a real image (format 37, R8G8B8A8_UNORM, 3840x2160, one layer, 3-vertex list, viewport
covering it), and its resource id is one of the ids the present names as its plane - the draws render into the display
resource the present blits, not into an intermediate nothing resolves. The target is nevertheless uniformly black after
the draw submission.

That locates the remaining blocker in the draw's pixel path, and the same boot names the most likely piece: the slot-57
record's fixed-function state is not decoded.

```
[rt] d3d12_xs: slot 57 stand-ins (logged once): attachments = 1x R8G8B8A8_UNORM, no depth-stencil, sample count 1
       (the slot-57 call carries no formats; the console's are in the state record and are not decoded);
       fixed-function state = rt::graphics_pso::documented_default_state() while every field of the state record is
       refused by name
[rt] d3d12_xs: slot 57 fixed-function state refused (first of "state.fixed_function"): ... refused: rasterizer, blend,
       depth_stencil, topology_type, strip_cut, sample_count, sample_quality, sample_mask
       (the state record is the console driver's serialized 380-byte pipeline state, docs/state_blob.md)
```

D3D12's documented defaults include back-face culling and depth test on; a pipeline built with them can discard a
full-screen triangle whose winding or depth state the title's record would have set differently. That was tested with a
bounded falsifier - one stand-in field overridden, target read back - and section 5 records the result: culling is not
the cause.

## 5. The fixed-function and geometry falsifiers: none of them moves it, and the readback instrument is proven

`documented_default_state()` was temporarily given an env-gated diagnostic override (`PALMETTO_POSE_DEBUG_STATE`,
uncommitted and reverted after these measurements). One field per boot:

| Boot | Draws / submits / presents | Draw's target readback | Captured frames |
|---|---|---|---|
| `cull_off` | 26,088 / 26,090 ok / 13,045 | mean 0 0 0, non-black 0/16,384 | black, 24/24 |
| `depthclip_off` | 626,652 d3d12 calls, frame loop steady | mean 0 0 0, non-black 0/16,384 | black, 24/24 |

All runs ALIVE 150.3 s, 0 exceptions, 0 device-lost, 0 refused, 0 failed `Close`. So back-face culling is **not** what keeps
the frame black, and neither is depth clipping. A "depth off" combo needs no boot: `documented_default_state()` already
sets `DepthEnable = FALSE`. With culling disabled, winding order cannot matter either.

The draw state around the target is equally clean: colour target a real 4K image (format 37, one layer), area
3840x2160, viewport 3840x2160 at 0,0, **scissor 0,0..3840,2160**, triangle-list topology, 3 vertices, one instance, no
depth attachment.

The readback was then given a **write control** in the same submission: after the frame's commands, the same target is
cleared by the layer to (0.2, 0.4, 0.8) and copied into a second window of the same host buffer. Measured:

```
[rt] replay: render-target readback: 128x128 at the target's origin of 3840x2160, 4-byte texel:
            mean 0 0 0, non-black 0/16384, peak 0
[rt] replay: write control: mean 51 102 204, non-black 16384/16384, peak 204
```

51/102/204 are exactly 0.2/0.4/0.8 on 8 bits. The image copy, the host mapping, the offsets and the target's
writability are therefore proven; the frame's own window is genuinely black. The diagnostic override is reverted (its
findings are negative).

## 6. What the drawn pixel stages actually sample

A bounded pipeline-stage diagnostic counts, per translated stage, the SPIR-V image sample/gather, image read/write and
load instructions, from the shader module the layer uploads. For the pipelines the frame's draws use it measured two
alternating pixel stages:

| Drawn pixel stage | SPIR-V image sample/gather | set-0 bindings | literal samplers | output locations |
|---|---:|---:|---:|---:|
| one | **0** | 0 | 0 | 1 |
| the other | **6** | 3 | 1 | 1 (some 0) |

The vertex stages have 0 images and 77/213 loads. So one of the two drawn passes samples nothing and reads its colour
from root arguments alone, and the other samples three bindings (images/samplers) and one literal sampler. Both
alternate on the same draws into the same presented resource and both leave it black.

That points at the texture-content path for the sampling pass, and the page pull has a measured gap that makes it
concrete: the pull covers the buffers a **draw** names (vertex, index, constants), but not the buffers a **copy**
names. A texture is filled by `CopyTextureRegion`/`CopyBufferRegion` from a source buffer - often the title's placed
pages - and the replay resolves that source VA through the title-VA alias but does not pull its bytes, so the copy
reads the stand-in's stale bytes and the texture stays empty. The sampling pass would then legitimately produce black
whatever the state and geometry are. Pulling copy sources (and other upload paths) is the next increment.

## 7. Copy sources: implemented, and measured to be unused in the observed window

The replay now pulls a copy's source bytes through the same `console_pull_pages` mechanism: when a processed command is
a `CopyBufferRegion`, a `CopyTextureRegion` whose source is a placed buffer footprint (the buffer -> image upload
path), or a `CopyResource`/tile copy whose source is a buffer, the source range's arena VA is translated back through
the title-VA map (`console_pull_arena` -> `ConsoleVaMap::title_span`) and the exact declared byte range is pulled from
the title's pages, capped at 8 MiB per copy. Uncommitted pages are left as the stand-in has them, as for every pull.
The first requests are logged as `page pull (copy source): arena VA 0x... + 0x..., a title range (pulled)` or
`not a title range (no pull needed)`, and the pulls are counted apart from the draw-read pulls.

Boot (`boot-copysrc3`, 150 s, capture on, readback armed): ALIVE 150.2 s, 0 exceptions, 0 device-lost,
`ExecuteCommandLists` ok 27,936 / failed 0 / refused 0, draws 27,935, presents 13,968, 0 failed `Close`. The measured
result is **negative but informative**: the window contains **no copy commands at all** - `CopyBufferRegion`,
`CopyTextureRegion`, `CopyResource`, `CopyTiles`, `ResolveSubresource`, `WriteToSubresource` and `ReadFromSubresource`
are all 0 in the trace, and so are the copy-source pull requests. The frame's commands are clears and draws only. The
target readback is unchanged (mean 0 0 0, non-black 0/16,384), the write control still reads 51/102/204, and all 24
captured frames are black.

So the sampling pass's textures are not filled by a copy of the title's pages during the measured window. Where the
sampled images get their data is the next question: the title may not have uploaded them yet (its 150 s boot reaches
about 28,000 draws), it may fill them through the texture-computer path (`XGCreateTextureComputer`/`XGSuggestSwizzleMode`
refuse with E_NOTIMPL), or through a resource-creation path that is still refused. That is measured next, not guessed.

## 8. The 300 s boot: still no uploads, and the late frame is still black

One clean 300 s harness boot (`boot-300`; capture every 2,000 presented frames, readback armed at draw
45,000 so it measures the frame around 170-190 s, not the first frame):

| Measure | 150 s boot | 300 s boot |
|---|---:|---:|
| Run | ALIVE 150.2 s, 0 exceptions, 0 device-lost | **ALIVE 300.3 s**, 0 exceptions, 0 device-lost, no `nvlddmkm` event after boot start |
| d3d12 calls recorded | 581,813 | 1,316,183 |
| `ExecuteCommandLists` | ok 27,936, failed 0, refused 0 | **ok 66,551, failed 0, refused 0** |
| Draws / presents | 27,935 / 13,968 ok | **66,550 / 33,275 ok**, 0 failed `Close` |
| CopyBufferRegion / CopyTextureRegion / CopyResource / CopyTiles / ResolveSubresource | 0 / 0 / 0 / 0 / 0 | **0 / 0 / 0 / 0 / 0** |
| Dispatch / DispatchMesh / ExecuteIndirect | 0 / 0 / 0 | **0 / 0 / 0** |
| WriteToSubresource / ReadFromSubresource / CreateReservedResource1 | 0 / 0 / 0 | **0 / 0 / 0** |
| Copy-source pull requests / pulls | 0 / 0 | **0 / 0** |
| Draw-read page content | - | 131,072 pull(s), 273.0 MiB |
| Draw's target readback (late, draw 45,000) | first draw: mean 0 0 0, non-black 0/16,384 | **mean 0 0 0, non-black 0/16,384**; write control 51/102/204 |
| Captured presented frames | black, 24/24 | **black, 17/17** (frame 1 through frame 32,001, spread over the whole run) |

So it is not a boot-duration or early-loading-window artefact: after five minutes the frame loop still issues only
clears and draws, never a copy, a dispatch, an indirect command or a reserved-resource creation, and the late frame's
own target still reads black. The title never uploads content in this window.

The one new pointer from the same boot: the title probes the console texture computer **once** at startup and never
again - `XGSuggestSwizzleMode`, `XGCreateTextureComputer` and texture computer slot 8 each return E_NOTIMPL once
(callers `game+0x4eccca7`, `game+0x16f7146`, `game+0x16f71ff`) - and it never calls `AllocateUploadMemory` (console list
slot 148) at all. The per-present `XboxID3D12CommandAllocator::Slot9 unsupported` (33,277 times, one per presented frame)
is the pre-existing per-frame recording-memory probe the title ignores. That pattern - a render loop that repeats with
no uploads - reads as a title still waiting for its streaming/texture path rather than a render-path fault; the
texture-layout/texture-computer area and the DirectStorage/zlib streaming path are the measured next places to look,
not the GPU composite.

## 9. What was added, and what still refuses

Added: the page pull (`console_pull_pages`/`console_pull_table` in `port/shims/d3d12_vulkan_device.cpp`, the
`pull_title_pages`/`pull_title_table` replay hooks, `ConsoleVaMap::rebase_span`/`title_span`), the copy-source pull
(`console_pull_arena`, the `pull_title_arena` replay hook, with request/pull counters), the bounded diagnostics (page
pull content counts, page table scan, draw attachments with scissor and pixel-stage sampling counts, pipeline-stage
SPIR-V instruction counts, one-shot render-target readback with a write control), and the `PALMETTO_PAGE_PULL=0` A/B
switch. `docs/page_address_space.md`'s refusal policy is unchanged: anything the VA map cannot place still refuses at
`draw.console_va`, never executes.

Still open, in order:

1. **The title never uploads.** Measured over 300 s (section 8): clears and draws only, no copy, dispatch, indirect
   command or reserved-resource creation, and the late frame's target still reads black. The title rewrites a load/stream
   loop without ever moving content to the GPU, so the render path is not the thing to fix next. The measured places to
   look are the texture-layout/texture-computer area (the title probes it once and never calls the upload-memory path)
   and the DirectStorage/zlib streaming path - a title stuck waiting for content looks exactly like this.
2. **The no-image pass.** One of the two drawn pixel stages samples nothing and reads only root arguments; it is black
   too. That is consistent with the title being in a pre-content loop (it may clear/draw a black cover), but if content
   does appear and this pass stays black, its recorded root constants and output state are next.
3. **The pixel path itself.** The target is real, sized, write-controlled and bound to the presented resource, and reads
   back black; culling, depth and depth clipping are ruled out by measurement, as are address and page-content faults.
4. **The fixed-function state stand-in.** Every field of the slot-57 state record is refused by name, so draws use
   `documented_default_state()`; blend and the other stand-in fields remain unmeasured.
5. **Compute-layout descriptor tables** (32-bit dwords) and any unrebased title VA stay refused, as in
   `docs/page_address_space.md` section 5.
6. The readback is one-shot and drains its queue, its write control clears the captured target at frame end, and the
   readback can be armed at a later draw with `PALMETTO_READBACK_AT_DRAW`; the copy-source pull is implemented but
   unexercised by every observed window. These are diagnostics/correctness paths, not behaviour changes.

> **Correction, measured later (docs/frame_capture.md).** The one-shot readback armed at the first (even) draw reads the
> frame's **intermediate** 4K buffer, not the presented display buffer. Each frame is two draws - the no-image pass writes
> an intermediate 4K target, the six-image pass writes the display buffer the present names - and the display buffers are
> double-buffered, so measuring the presented resource needs the readback armed at an odd draw. With the present plane's
> exact resource id, the **presented** resource is genuinely black immediately after its own last writer (clear first, draw
> last, only a layout barrier after it), and a known-good full-screen control triangle drawn into that same resource reads
> back its exact colour (mean 32 160 240, 16,384/16,384 non-black). That proves the raster, target binding, viewport,
> subresource, layout transition and readback path for the presented target, and leaves the title draw's own pixel output
> as the frontier.
