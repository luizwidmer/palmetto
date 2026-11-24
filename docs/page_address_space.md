# The title's page VAs in the one GPU address space

**Status: measured increment** - the translated shaders compute `host_address = heap_base + console_va`
(`docs/d3d12_vulkan.md` section 3.3), and the title names some of its memory by VAs the layer did not choose: the
two-terabyte host VAs of the pages behind Xbox device slot 133 (the placement behind `docs/replay_resolution.md`), and
the 64 KiB-aligned console address of a shader-visible descriptor heap it places through device slot 14. This page
records the design the evidence picks, what was implemented, and the measured result: the draws execute, no GPU fault,
no refused submission and no lost device in a clean 150 s boot.

Counts, VAs and error codes only; no package bytes, paths, shader data or extracted tables.

## 1. The two designs, and which one the evidence picks

**A. Page import.** Reserve the title VA ranges, commit the pages and import them as external host memory, so
`heap_base + console_va` is literally the imported memory's address. This does not satisfy the equation. An imported
range is a host allocation whose *device* address is chosen by the driver, outside the arena; the shader adds one
`heap_base` (the arena's device address) to whatever VA the descriptor holds. For an out-of-arena buffer the only VA
that satisfies the equation is `device_address - heap_base`, and that bias **is measured to fail for a V#**: the base
field is 48 bits, and a negative or above-2^48 bias truncates into a wild address (`docs/d3d12_vulkan.md` section 3.3,
D3D-53: "the bias works for 64-bit root arguments but not for a V#"; the same page measures the BVH builder refusing
destination addresses at or above 2^40). Import also refuses the one placed buffer we actually have: the whole
182,452,224-byte range is not importable (`E_NOTIMPL`; xmem commits the pages lazily), which is why the stand-in exists
at all (`docs/replay_resolution.md` section 3). Import can therefore fix *what the pages contain* - the separate
limitation that the title's CPU writes do not reach the GPU - but not *where the shader looks*.

**B. VA rebasing, chosen.** Keep the title VA as an alias for command arguments and rewrite every shader-visible copy of
it to an arena VA. One `heap_base` then holds for every resource in the draw, which is the property the whole arena
design exists for (`docs/d3d12_vulkan.md` section 3.3, "Why one arena rather than several"). This is also the design the
diagnosis implies: the one-registration candidate made all 43,750 refusals disappear and the first executed draw then
faulted the GPU, and a control that redirected every vertex-buffer binding to the zero page faulted at the same point
(`docs/replay_resolution.md` section 4) - the fault is addressing, not fetched data. The measured mechanism is a
shader-visible title VA; the response is to put an arena VA in the shader's hands instead.

## 2. What the title's address space measurably looks like

One diagnostic boot (`F:\palmetto-work\boot-pageva2`, standard environment plus the existing
`PALMETTO_XS_CLEAR_BARRIER_PROBE=1`; behaviour-neutral) measured the values the two designs have to work with:

```
[rt] d3d12_xs: slot 133 CreatePlacedResourceX: va=000001CD10B00000 dimension=1 width=182452224 ...
[rt] d3d12_xs: probe device slot 14 heap type=0 num=1245184 flags=0x1 address=0x1cd10000000 size=0x2600000
              cpu=0x1cd10000000 gpu=0x1cd10000000
[rt] replay: draw user data 64-bit windows: s0=0x1cd10b5dea0
[rt] replay: draw user data 64-bit windows: s0=0x1cd10b7dea0
[rt] replay: draw user data 64-bit windows: s0=0x1cd10b9dea0
[rt] replay: draw user data 64-bit windows: s0=0x1cd10bbdea0
```

1. **Two title ranges, and they overlap.** The placed buffer's title range is
   `[0x1CD10B00000, +0xAE00000)`; the descriptor heap's is `[0x1CD10000000, +0x2600000)`. Both are inside the
   title's own xmem VA region, and the buffer's range overlaps the heap's first 27 MiB. A VA therefore does not say by
   itself which of the two it means; the site does (a descriptor table names the heap, a root descriptor names a
   buffer), which is why the map keeps one range per kind and rebases by the root signature's parameter type.
2. **The first draws' shader-visible VA is a descriptor-table VA, not a misplaced root descriptor.** The draw's user
   data carries one 64-bit value, `s0`, advancing 0x20000 per draw; it is inside both ranges. As a root-descriptor VA it
   would have to be 256-byte-aligned for a CBV or 64-byte-aligned for a raw/structured SRV or UAV - it is 32-byte
   aligned only (`0x5DEA0` inside the buffer). As a descriptor table's VA it is exactly `heap_address + 0xB5DEA0` =
   descriptor 372,469 of 1,245,184 in the console heap. So the first draws read a table of descriptors the title placed
   in its console heap, and the V#s inside that table are what name its pages.
3. **The title's own V#s name the placed pages.** The heap's mirrored bytes contain 6-11 48-bit V# bases inside the
   buffer's title range on each refresh; those are descriptors the title wrote itself (the layer's own `Create*View`
   writes name arena VAs already).

## 3. What was implemented

`runtime/src/console_va_map.h` is the unit: title-VA ranges (`title`, `size`, arena VA, kind = placed buffer or
descriptor heap), a kind-preferring rebase (a VA in both ranges is resolved by what the site means), and
`patch_descriptor_bases`, which rewrites a 48-bit V# base that names a placed buffer to the arena VA of its stand-in.
`runtime/test/buffer_va_index_test.cpp` covers it (ranges, overlap preference, V# rewrite, removal, incomplete flag).

The pieces that use it:

1. **Arena range at creation.** The slot-133 stand-in is a default-heap buffer, so it is already a range of the arena
   whose console VA is an arena offset; `create_at_host_address`'s fallback now registers the VA the title placed the
   pages at as an alias for it (`Resource::alias_console_va`).
2. **Title VA as an alias for command arguments.** `buffer_at`/`host_at` translate a title VA through the map before
   the lookup, so vertex, index and indirect views and copy regions that name the pages by the title's VA resolve to
   the stand-in.
3. **Console descriptor heaps get an arena mirror.** A shader-visible heap created at a console address keeps its CPU
   bytes where the title's handle expects them and gains an arena mirror; before every submission the slots the title
   changed are copied into the mirror and their V# bases are rebased in place. The heap's GPU handle stays the console
   VA, so the title's own address arithmetic is unchanged, and every copy of that VA is rebased at replay.
4. **Every shader-visible copy of a title VA is rebased at replay.** The 32 user-data dwords are scanned as 64-bit
   windows; a window that the pipeline's root signature says is a descriptor table is rebased as a descriptor-heap VA,
   every other one as a placed-buffer VA (`GraphicsPipeline::table_windows`, filled from `RootArgumentLayout` when the
   PSO is created). The recorded command is never modified: the rebase writes the push-constant copy.
5. **Anything not rebased stays refused.** `prepare_draw` and the dispatch path refuse with `unresolved_resource` at
   `draw.console_va` while a title range the title can name as a shader-visible VA has no arena stand-in (the
   `incomplete` flag is set when a same-kind range collides, an arena mirror cannot be allocated, or a console heap
   cannot be backed). A refusal is always available as the fallback; executing an unrebased title VA is not.

The mirror refresh costs one pass over the title's (CPU) descriptor bytes per submission and copies only the 32-byte
slots that changed: a frame rewrites a few descriptors, and the mirror is GPU-readable memory where a full rewrite is
the expensive side.

## 4. What executes now

One clean 150 s boot (`F:\palmetto-work\boot-pageva4`; standard environment, the harness, no other boot running, tree
`523d94759` plus this increment):

| Measure | Result |
|---|---|
| Run | **ALIVE 150.3 s**, 0 exceptions, 0 `device was lost` |
| TDR | **none**: no `nvlddmkm` event 153 at or after the boot's start; the last 153 records on the machine are from 15:11-15:16, before this work |
| D3D12 calls recorded | 687,093 (max #689,801) |
| `DrawInstanced` / `SetPipelineState` recorded | 17,674 / 17,674 |
| `ExecuteCommandLists` | **ok 35,349, failed 0, refused 0** - the one-refusal-per-draw class is gone |
| `Close` | ok 35,976, failed 0 |
| `PresentX` | ok 17,674, failed 0 |
| Window | created hidden, then "showing the PresentX window (1280x720 client): the presented frames are in it now" after the first presented frame |
| Pacing | two submissions per presented frame; submission 1 at 0.0 s, 1,024 at 3.8 s, 32,768 at 124.0 s (about 256 submissions/s and 118 presented frames/s); steady, no stall |
| Asserts | the same four `int3_skipped` addresses as every earlier boot; no new one |

The title-VA work in that boot: the two title ranges were aliased (the placed buffer's 0xAE00000 bytes at arena VA
0x10000, the descriptor heap's 0x2600000 bytes mirrored at arena VA 0xAE10000, both inside one xmem region and
overlapping by 27 MiB); the first draws' table VA (`heap + 0xB5DEA0`, descriptor 372,469) was rebased to its mirror VA
`0xAE10000 + 0xB5DEA0`; the console heap's V# bases that named the placed pages were rewritten in the mirror (the first
refresh after creation copied 327,669 changed descriptor slots, then a frame rewrites tens of slots, and some refreshes
rewrite V# bases).

The cost of the mirror is measurable and is this increment's main open performance item: the first refresh (a full
39.8 MB copy plus a whole-heap V# scan) took 728,676 us; after that, a refresh costs 3.4-4.4 ms, one pass over the
title's 39.8 MB of CPU descriptor bytes, and that is most of the ~3.8 ms between submissions. It does not drop or refuse
a submission; it makes each frame slower than the refused-draw baseline, which is why the old 43,750 refused draws per
150 s are now 17,674 executed draws. A dirty-page scan (compare 4 KiB pages, walk only the changed pages' slots) is the
next measured step if the frame rate matters before the page-content work.

The window is up and the presented frames are in it; **whether the drawn pixels reach it cannot be established from the
logs** - the present consumes the title's display resource (real since the console slot-127 fix) and no draw's
render-target binding is refused, but that does not prove the draw wrote that resource. An automated capture (a
window grab or a readback of the display resource's bytes) would answer it.

## 5. What still refuses, and the next increment

* **Page contents.** The stand-in is still the layer's own zeroed arena range, not the title's pages: the draw's
  addressing is now valid and the draw executes, but its vertex/constant reads see stand-in bytes. The honest
  substitute for the refused import is a copy of the title's pages into the stand-in (the title's writes are on CPU
  pages the layer can read; the copy has to be ordered like the console heap refresh). That is the next increment, and
  it is a data fix, independent of this page's addressing fix.
* **Compute-layout descriptor tables.** The console compute layout gives a table one 32-bit dword; a title VA above
  4 GiB is truncated there, so it cannot be reconstructed the way the graphics layout's two dwords are. Dispatches are
  measured not to fault today; if one ever names a title VA it must be refused, not guessed.
* **Unrebased title VAs.** Any title VA in a same-kind overlap, or in a heap whose mirror cannot exist, keeps its
  submission refused (`draw.console_va`), never executed.
* **Mirror lifetime.** The mirror is refreshed in place; a title that rewrites descriptors for work already in flight
  keeps the race it would have on the console, and a heap released while work is in flight releases its arena range
  immediately, as the layer's arena descriptor heaps already do.
