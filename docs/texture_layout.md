# Console texture layout (XG): the two refused calls decoded, and the linear layout as the default

**Status: measured increment, default flipped.** The frame's last open lead was the console texture-layout family
(`xg_xs.dll`: `XGSuggestSwizzleMode`, `XGCreateTextureComputer` and its methods), which refused with `E_NOTIMPL`.
The previous page recorded that answering it unlocks a real title-side upload path (65 subresource copies, 34,044 B
in 150 s) but that slots 2 and 10 still refused, so the linear answer stayed an opt-in measurement switch and the
default kept the layout cache closed. This page decodes the two calls from the executable's own code, corrects the
record size the previous page reported, fills the record the title reads, and measures the linear layout as the
default:

- the slot-2 record is **0x1B40 bytes** (a header and four 0x6C8-byte planes), not 0x6C8 - 0x6C8 is one plane;
- slot 2 is `GetResourceLayout`. The default now fills the whole record with the description's linear layout; the
  title's layout cache consumes it instead of an uninitialised stack record, and the 150 s boot stays alive;
- slot 10 is `GetResourceTiling` (D3D12's seven-argument shape); its structures are decoded, but a tile list only
  exists for a 64 KiB-tiled resource and the runtime creates none, so it still refuses by name and writes nothing;
- the upload path is unchanged: the same 65 copies / 34,044 B as the opt-in linear run, the same per-frame command
  shape, 0 exceptions and 0 device-lost in both arms, and the frame stays black (the content frontier is separate:
  the runtime's stand-in for a title-placed texture is an ordinary committed image that does not read the title's
  pages).

Counts, RVAs, format codes, structure offsets and the project's own wording only; no package bytes, paths or
extracted tables.

## 1. The measured surface

The retail executable imports two functions from `xg_xs.dll`. Both call-site sets were re-read statically and then
counted at run time through the shim (`PALMETTO_XG_TRACE=1`, `PALMETTO_XG_DUMP=1` for the record dumps):

| Entry / method | Call sites (game RVAs) | 150 s refusal arm | 150 s linear arm (default) |
|---|---|---:|---:|
| `XGSuggestSwizzleMode(desc, 0, uint32_t* mode)` | `0x4ECCCA2`, `0xC4B3CE` | **15** | **44** |
| `XGCreateTextureComputer(const desc*, computer** out)` | `0x16F7141`, `0x344235`, `0x979C75`, `0x5121A5E` | **8** | **24** |
| computer slot 1 (read as Release) | 3 sites | not counted (uninstrumented in the A/B) | not counted |
| computer slot 2 (`GetResourceLayout`, one out pointer) | `0x979C9F`, `0x5121A84` | **0** | **2, filled** |
| computer slot 8 (`CopyIntoSubresource`, six arguments) | `0x16F71FB` | **8, all `E_NOTIMPL`** | **65, all `S_OK` (34,044 B)** |
| computer slot 10 (`GetResourceTiling`, seven arguments) | `0x344283` | **0** | **0** |

The reached path is the title's placed-resource fill function (`game+0x193DE10`), through the per-subresource copy
loop at `game+0x16F7100`:

1. the description helper (`game+0x4ECC940`) builds a 0x38-byte XG resource description from the title's own texture
   record. Its mode field (offset `0x2C`) is the title's own value `0x100 | mode` for known formats, and for an
   unknown format byte (`0xFF`) the helper calls `XGSuggestSwizzleMode(desc, 0, &desc[0x2C])` and passes the result up;
2. `XGCreateTextureComputer` is called with that description (from `game+0x16F7100`, which does **not** check the
   HRESULT - the two callers that do check, `game+0x979C40` and `game+0x5121980`, are the slot-2/layout-cache
   paths);
3. for every subresource the title calls slot 8 once: measured arguments are `(destination resource address, 0,
   subresource index, linear source for that subresource, bytes per row, subresource bytes)`, e.g. a 256x128 8-bit
   description gives `arg5 = 0x100` (256 B/row) and `arg6 = 0x8000` (32,768 B), and the 1x1 4-byte descriptions give
   `arg5 = arg6 = 4`. The HRESULT of slot 8 is not read;
4. only when `XGCreateTextureComputer` answered `S_OK` do the two checking callers continue into the layout-cache
   path and reach slot 2 (`0x5121980` calls it and copies one plane out of the record).

The measured descriptions are one-level, one-slice textures: the 256x128 R8_UNORM image (format 61), 1x1
R8G8B8A8_UNORM (format 28) and 1x1 32-bit formats (28 / 30 / 41). The two slot-2 descriptions are 2048x2048 and
1024x1024 R8 (see section 2.1).

## 2. The two refused calls, decoded

### 2.1 Slot 2 is `GetResourceLayout`: a 0x1B40-byte record, not 0x6C8

The previous page reported a 0x6C8-byte stack record. Reading the two callers' stack frames settles it:

- `game+0x979C40` reserves `0x1B98` bytes (`mov eax, 0x1b98; call __chkstk; sub rsp, rax`) and passes `rsp+0x58`
  to slot 2: exactly 0x1B40 bytes remain to the top of the frame;
- `game+0x5121980` reserves `0x1BA0` bytes and passes `rsp+0x60`: again exactly 0x1B40 bytes;
- the same function then copies **one plane** out of that record:
  `memcpy(dst, rsp+0x60+0x18+arg*0x6C8, 0x6C8)` (`game+0x5121A9D..0x5121AA9`), and its cached map nodes store the
  same 0x6C8. So 0x6C8 is one plane of the record: `0x1B40 = 0x18 + 4 * 0x6C8`, and a plane is
  `0x38 + 15 * 0x70`.

`XG_RESOURCE_LAYOUT` (0x1B40 bytes), offsets the executable reads marked:

| Offset | Size | Field | Read by |
|---|---:|---|---|
| `0x00` | u64 | `SizeBytes` | (not read in the measured window) |
| `0x08` | u64 | `BaseAlignmentBytes` | (not read in the measured window) |
| `0x10` | u32 | `MipLevels` | (not read in the measured window) |
| `0x14` | u32 | `PlaneCount` | (the dump and tests use it; no measured title read) |
| `0x18` | 4 x `0x6C8` | `planes[]` | plane `k` at `0x18 + k*0x6C8` |
| `0x1B38` | u32 | `Dimension` (D3D12_RESOURCE_DIMENSION) | (not read in the measured window) |

Plane (0x6C8 bytes):

| Offset | Size | Field | Read by |
|---|---:|---|---|
| `0x00` | u32 | usage (1 colour, 2 CMASK, 4 HTILE, 5/6 depth/stencil, 7/8 depth/stencil of a depth buffer) | (not measured) |
| `0x08` | u64 | plane size | (not measured) |
| `0x10` | u64 | slice size | `game+0x5121690` (`0x5121854`), `game+0x555E9E0` |
| `0x20` | u64 | base alignment | (not measured) |
| `0x28` | u32 | bytes per element | (not measured) |
| `0x2C` | u32 | first mip in the tail | (not measured) |
| `0x30` | u32 | mips in the tail | (not measured) |
| `0x38` | 15 x `0x70` | mip levels | mip `i` at `0x38 + i*0x70` |

Mip level (0x70 bytes):

| Offset | Size | Field | Read by |
|---|---:|---|---|
| `0x00` | u64 | level size (one slice) | `game+0x979C40` (record+0x50), `game+0x5121690` (`0x512186F`), `game+0x555E9E0` |
| `0x08` | u64 | offset from the plane base | `game+0x5121690` (`0x512186A`), `game+0x555E9E0` |
| `0x10` | u64 | level slice size | `game+0x979C40` (record+0x60), `game+0x5121690` (`0x5121874`), `game+0x555E9E0` |
| `0x18` | u32 | pitch in pixels | (not measured) |
| `0x1C` | u32 | pitch in bytes | `game+0x979C40` (record+0x6C), `game+0x5121690` (`0x5121879`), `game+0x555E9E0` |
| `0x20` | u32 | level alignment | (not measured) |
| `0x24` / `0x28` / `0x2C` | u32 | padded width / height / slices | (not measured) |
| `0x30` / `0x34` | u32 | width / height in elements | (not measured) |
| `0x38` / `0x3C` | u32 | width / height in pixels | (not measured) |
| `0x40` | u32 | depth-or-slices | (not measured) |
| `0x44` | u32 | samples | (not measured) |
| `0x48` | u32 | swizzle mode (0 is linear) | (not measured) |
| `0x60` | u32 | block depth (1 for the 2D layouts) | (not measured) |
| `0x64` | u32 | in-mip-tail flag | (not measured) |

The record base the callers pass is what makes the arithmetic consistent: `game+0x979C40` reads
`record + 0x50 + i*0x70`, `record + 0x60 + i*0x70` and `record + 0x6C + i*0x70` for mip `i`, which is plane 0's mip
`i` (`0x18 + 0x38 = 0x50`) and its size @+0, slice size @+0x10 and pitch bytes @+0x1C. `game+0x5121690` and
`game+0x555E9E0` read the cached plane's slice size @+0x10 and, per level, size @+0, offset @+8, slice size @+0x10
and pitch bytes @+0x1C. No other code reads the record (the cache map is only referenced by the fill function and
its own map init/teardown).

Provenance of the table: the **offsets and the fields the title consumes** are pinned locally by those reads (and by
the record sizes the two stack frames reserve); the field **names** follow the GDK's public description of
`XG_RESOURCE_LAYOUT` (size, base alignment, mip count, planes, and a plane as mip levels) and the linear allocation
rule (256-byte rows) is the one the project's GFX10 tiling already documents for the neighbouring swizzled modes
(`port/src/gfx10_tiling.h`). No console tiling is inferred anywhere here.

The boot dumps the two records the title asks for (both from caller `game+0x5121A87`, i.e. the slot-2 call inside
`game+0x5121980`):

| Record | `SizeBytes` | `MipLevels` | `PlaneCount` | `Dimension` | plane 0 | mip 0 |
|---|---:|---:|---:|---:|---|---|
| 2048x2048, one level, R8 | `0x400000` | 1 | 1 | 3 (TEXTURE2D) | usage 1, size `0x400000`, slice `0x400000`, bpe 1 | offset 0, size `0x400000`, pitch 2048 px / 2048 B, padded 2048x2048, 1 slice, 1 sample, mode 0 |
| 1024x1024, one level, R8 | `0x100000` | 1 | 1 | 3 | usage 1, size `0x100000`, slice `0x100000`, bpe 1 | offset 0, size `0x100000`, pitch 1024 px / 1024 B, padded 1024x1024, 1 slice, 1 sample, mode 0 |

Both records are internally consistent (`pitch_bytes * height == size`) and the boot with them filled stays alive
(section 4). The values are the linear layout the description names (`0x100 | 0`, the mode the title's own
description helper writes), not a console tiling.

### 2.2 Slot 10 is `GetResourceTiling`: seven arguments, a 12-byte tiling list

The caller at `game+0x3441F0` reserves `0x168` bytes, builds a description and a computer, then calls slot 10 with
the D3D12 `GetResourceTiling` shape:

| Argument | Measured value | Meaning |
|---|---|---|
| `rdx` | `rsp+0x38` (u32, initialised to 0) | total tiles of the resource, out |
| `r8` | `rsp+0x58` (0x10 bytes zeroed) | packed-mip desc, out: `u8 NumStandardMips`, `u8 NumPackedMips`, `u16 NumTilesForPackedMips`, `u32 StartTileIndexInOverallResource` |
| `r9` | `rsp+0x48` (0x10 bytes zeroed) | standard tile shape, out: `u32 WidthInTexels`, `u32 HeightInTexels`, `u32 DepthInTexels` |
| `[rsp+0x20]` | `rsp+0x3c` (initialised to `0x10` = 16) | subresource-tiling count, in/out |
| `[rsp+0x28]` | 0 | first subresource tiling index |
| `[rsp+0x30]` | `rsp+0xa0` | subresource tiling list, out: 16 entries of **12 bytes** (16 * 12 = 0xC0 B, inside the frame) |

After the call the caller reads `byte[rsp+0x58]` and `byte[rsp+0x59]` (standard and packed mip counts), then walks
the list at `rsp+0xa0` in 12-byte steps and reads `dword @0`, `word @4` and `word @8` of each entry. Those are the
Xbox `XG_SUBRESOURCE_TILING` fields of the D3D12 shape: `WidthInTiles` u32 @0, `HeightInTiles` u16 @4,
`DepthInTiles` u16 @6, `StartTileIndexInOverallResource` u32 @8; the caller packs width/height/low tile index into
its own 32-bit word and adds `WidthInTiles * HeightInTiles` per standard mip.

A tile list exists only for a 64 KiB-tiled resource. The measured window creates none: slot 10 is called **0 times**
in both arms, the runtime creates no reserved/tiled resource, and a 64 KiB tile layout is not modelled
(`port/src/gfx10_tiling.h` covers two swizzled modes of one 2D level, without mips or tile indices). Filling a tile
list would therefore be fabricated, so it stays refused by name and writes nothing. What would pin it: a title path
that creates a tiled/reserved resource and asks `GetResourceTiling` for it (none in the measured window), or an
oracle comparison against the retail `xg_xs` library on 64 KiB-tiled descriptions.

## 3. What the shim does now, and what it refuses

**Default (`PALMETTO_XG_LAYOUT` unset or `linear`)** is the linear answer for a backend whose textures are ordinary
row-major committed images:

- `XGSuggestSwizzleMode` returns `S_OK` and writes **`0x100`**, the explicit linear layout the title's own
  description helper writes (`0x2C = 0x100 | mode`, mode 0 = linear). A console swizzle mode would make the title
  upload console-tiled bytes the GPU reads wrongly.
- `XGCreateTextureComputer` returns `S_OK` **only for a description whose whole layout can be written honestly**: by
  default a one-level, one-slice 1D/2D texture, `SampleDesc.Count == 1`, no `ALLOW_RENDER_TARGET` /
  `ALLOW_DEPTH_STENCIL` flag, a format with a whole-byte single-plane element (8/16/32/64/128-bit and the BCn
  formats), and the layout field `0x100`. The separately default-off `PALMETTO_XG_LINEAR_ARRAYS` switch is the narrow
  exception for the measured 1x1 format-28 arrays of 2, 6 or 12 slices (section 8). It always stores a computer in
  `*out`, because two measured callers use it without checking the HRESULT; the two checking callers (the layout cache)
  take their failure path on the named refusal, which is what keeps an unwritten record out of that cache.
- Slot 2 (`GetResourceLayout`) fills the whole **0x1B40** record from the description the computer was created with:
  header size / alignment / mip count / plane count / dimension, one plane (usage 1 colour, size, slice size,
  alignment, bytes per element, no mip tail) and its one mip level (size, offset 0, slice size, pitch in pixels and
  bytes, padded extent, element extent, pixel extent, depth-or-slices, samples, swizzle mode 0, block depth). Rows
  are padded to 256 bytes (the linear rule this runtime serves) and every byte of the record that is not one of
  those fields is zeroed, so no stack bytes survive. A description that cannot be laid out refuses by name and
  writes nothing.
- Slot 8 (`CopyIntoSubresource`) is unchanged from the measured linear answer by default: it copies the linear
  subresource at the offset its row-major source names, capped to what a resource of the recorded description can
  hold, both ranges checked committed and accessible. With `PALMETTO_XG_LINEAR_ARRAYS=1`, only the section-8 array
  shapes write their four-byte texel at the corresponding 256-byte slice offset; every other shape retains the prior
  flat path. The upload path measured below is unchanged by the record fill.
- At create and slot 2, everything else is refused by name: a buffer / 3D description, more than one mip level, any
  unmeasured array slice count/shape, MSAA, a render-target or depth-stencil resource (its CMASK / HTILE metadata plane
  is not modelled), a planar or unknown format, and any layout field other than the explicit linear `0x100`. Unmodelled
  slot-8 shapes still use the prior flat fallback. Slot 10 always refuses (section 2.2).

**`PALMETTO_XG_LAYOUT=refuse`** is the previous shim, bit for bit (the A/B baseline): both exports `E_NOTIMPL`, the
mode field untouched, the computer still stored, every slot returns `E_NOTIMPL` and writes nothing.

Implementation: `port/shims/xg_xs.cpp`. The record offsets, the exact fill (guard bytes past 0x1B40 untouched, all
unwritten fields zeroed), the two measured descriptions, the linear pitch/size arithmetic and the named refusals are
pinned by `port/test/xg_xs_test.cpp` (364 checks in the default mode, 33 in refuse mode; section 8 adds 242 array,
switch and census assertions).

## 4. The A/B: the linear layout as the default

Two 150 s boots, same tree, same package, harness only (`tooling/boot_report.py`), standard environment
(`PALMETTO_SHADERS=<shaders-dir>`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`), `PALMETTO_XG_TRACE=1` and `PALMETTO_READBACK_AT_DRAW=1` on both, the only
difference the layout switch. Boot 2 (the default) also has `PALMETTO_XG_DUMP=1`, which only logs the first 8
descriptions and records and does not change the title's calls; boot 1 is the opt-out.

| | boot 1: `PALMETTO_XG_LAYOUT=refuse` | boot 2: default (linear) |
|---|---:|---:|
| Run | ALIVE 150.2 s, 0 exceptions, 0 device-lost | ALIVE 150.2 s, 0 exceptions, 0 device-lost |
| D3D12 trace calls | 652,414 | 514,354 |
| `XGSuggestSwizzleMode` calls | 15 | **44** |
| `XGCreateTextureComputer` calls | 8 | **24** (the upload loop ignores the HRESULT; the layout-cache callers take the named refusal) |
| computer slot 2 calls | 0 | **2, filled** (2048x2048 and 1024x1024 R8, table above) |
| computer slot 8 calls | 8, all `E_NOTIMPL` (0 B) | **65, all `S_OK` (34,044 B copied)** |
| computer slot 10 calls | 0 | 0 |
| copies / dispatches / upload-memory allocations (trace) | 0 / 0 / 0 | 0 / 0 / 0 |
| placed creations / committed resources / SRVs / UAVs | 32 / 7 / 39 / 22 | 32 / 7 / 39 / 22 |
| `DrawInstanced` / `PresentX` / `ExecuteCommandLists` ok | 31,631 / 15,816 / 31,632 | 24,400 / 12,200 / 24,402 |
| draw's target readback | mean 0 0 0, non-black 0/16,384 | mean 0 0 0, non-black 0/16,384 |

The upload path is identical to the previous page's opt-in linear run: **65 copies, 34,044 B**, the 256x128 R8
image (32,768 B) and the 1x1 defaults (4 B each). The resource population and the per-frame command shape are
identical in the two arms: **2 draws and 2 submissions per presented frame** in both (24,400 draws / 12,200 presents
and 31,631 / 15,816), one `ClearRenderTargetView` and one `SetPipelineState` per draw. The absolute frame count of a
150 s boot varies with the machine load the boot runs under (other agents' builds run on the same host), so the
frame totals are not compared across arms; the per-frame shape and the creation counts are. No device-lost, no
exception, no GPU fault in either arm.

What answering the family changes is exactly what the previous page measured: the title's CPU-built textures reach
its own pages. What filling slot 2 changes on top of that is that the layout cache now consumes a written linear
record instead of an uninitialised stack record, which is why the default no longer has to keep the cache path
closed.

## 5. Why the frame is still black, and what this rules out

The 34,044 bytes the title copies land **in the title's own pages**, and the runtime does not read them for a
texture. Device slot 133 (`CreatePlacedResourceX`) for a placed **texture** says so itself:

```
[rt] d3d12: a title-placed texture (device slot 133) is an ordinary committed texture, not the title's pages; dimension 3
```

(`port/shims/d3d12_vulkan_device.cpp`, `create_at_host_address`: a texture gets an ordinary committed image on its
own `VkDeviceMemory`, and the title's writes at its page address do not reach it - only placed **buffers** get the
console-VA alias, and that alias rebases the VA for descriptors, it does not import the pages either). A layout
answer can only become pixels once the runtime makes a placed texture read the title's pages - the same page-import
problem `docs/page_content.md` recorded for buffers, one resource class further. `docs/texture_source.md` measured
that frontier separately: with the placement import opted in (`PALMETTO_TEXTURE_IMPORT=1`) every sampled image still
reads black, and the sampled 4K texture is a different population from the CPU-built textures this family serves.

So this increment rules XG out as the black frame's gate in the measured window: the family's uploads are small
CPU-built textures (one 256x128 atlas-like image and 1x1 defaults, 34 kB total), the resource population is
unchanged by the answers, no GPU copy or compute dispatch appears, and the readback is black in both arms (mean 0,
0/16,384). The layout records the title now caches describe its own pages, not the committed stand-in.

## 6. What remains unpinned, and what measurement would pin it

- **Slot 10's tile list.** The structures are decoded (section 2.2) but not served: no measured resource is tiled
  and the runtime creates no tiled resource, so tile width/height/depth and `StartTileIndexInOverallResource` would
  be fabricated. Pinned by a title path that creates a 64 KiB-tiled (reserved) resource and calls
  `GetResourceTiling`, or by an oracle comparison against the retail `xg_xs` library on 64 KiB-tiled descriptions.
  Both are outside the current window (slot 10 is called 0 times) and the runtime has no such resources.
- **Layout records for more complex resources.** The fill serves the measured case only: one mip level, one array
  slice, 1D/2D, colour, one plane. Multi-level and multi-slice records would need the linear mip ordering and the
  per-slice offsets, and render-target / depth-stencil resources their CMASK / HTILE metadata planes (a second
  plane with its own usage code, size and alignment); 3D and MSAA need further rules. The window's slot-2
  descriptions are one-level one-slice R8, so those cases are refused at `XGCreateTextureComputer` with the field
  named and the layout cache takes its failure path instead of consuming a record. Pinned by a measured title path
  that creates such a resource and asks for its layout, or by the console library's own answers for those
  descriptions.
- **The 256-byte row padding of the linear layout.** The record's pitch/size arithmetic pads rows to 256 bytes (the
  D3D12 linear layout this backend serves, `PALMETTO_XG_LAYOUT=linear`). Both records the title actually reads have
  widths that are multiples of 256 bytes (2048 B and 1024 B rows), so the observed records do not exercise the
  padding rule; the 1x1 upload textures that would are never asked for their layout in the window (their caller does
  not check the create and never reaches slot 2). Pinned for the read records by their own values; a padded-row case
  would be pinned by a measured layout-cache read of a texture narrower than 256 bytes per row, or by the console
  library's answer for that description.

`docs/texture_import.md` still lists slots 2 and 10 as "refused / unmodelled"; the slot-2 part is superseded by this
page (slot 10 is still refused, now by name with its structures decoded).

## 7. Reproduce

```
# default: the linear layout (slot 2 filled, slot 8 copies, slot 10 refused)
python -I tooling\\boot_report.py --exe port\\build\\palmetto.exe --image <title> --game-dir <package>
    --overlay <overlay-dir> --timeout 150 --out <out-dir>
    --env PALMETTO_SHADERS=<shaders-dir> --env PALMETTO_D3D12=vulkan
    --env PALMETTO_SKIP_INT3=1 --env PALMETTO_SEED_GLOBAL=75DF268:D8 --env PALMETTO_XG_TRACE=1
    --env PALMETTO_XG_DUMP=1 --env PALMETTO_READBACK_AT_DRAW=1

# the refusal arm of the A/B
... --env PALMETTO_XG_LAYOUT=refuse
```

`PALMETTO_XG_DUMP=1` adds, bounded to the first 8 calls of each entry, the description words and the filled layout
record (header, plane, mip) to the trace, so one boot shows the shapes above. The counters
(`[rt] xg_xs counters (...)`) and the slot-8 arguments are what the tables read; `PALMETTO_XG_TRACE=1` alone gives
the first calls without the record dump.

Tests: `cmake --build port\\build --target xg_xs_test` then `port\\build\\xg_xs_test.exe` (364 checks, 0 failures;
`PALMETTO_XG_LAYOUT=refuse` runs the same binary with 33 checks, 0 failures). The other test targets and the palmetto
build gate are unchanged; this slice does not build or relink `palmetto`.

## 8. Narrow opt-in: the measured explicit-linear array class

**Status: implemented behind a separate opt-in and covered by GPU-free tests; not booted or linked into `palmetto` in
this slice.** The shape census proves these arrays reached the title's copy path and the old shim refused their
`XGCreateTextureComputer` call at the array-size gate. It does not include a slot-2 layout read for these descriptors,
so the array slice arithmetic below is a deliberately narrow row-major model, not a measured retail `GetResourceLayout`
answer. Keep the switch off unless the lead clears integration.

### 8.1 Raw-log census and the previous accounting

The shape-only reaggregation of `D:\leonida-work\fsrstable\boot-xgplan3\stderr.log` uses the 128 distinct descriptor
prefixes (shape IDs 0–127), not only complete newline-delimited `-> verdict` strings. No package bytes or texture data
are used or copied into the repository.

| Census cut / first refusal class | Earlier page, 123 complete lines | Raw-log reaggregation, 128 shapes |
|---|---:|---:|
| Explicit-linear single-slice, one-level shapes | 7 | 8 |
| Non-linear layout field (not `0x100`) | 54 | 56 |
| More than one mip | 42 | 43 |
| More than one array slice | 19 | 20 |
| 3D dimension | 1 | 1 |
| **Total shapes** | **123** | **128** |
| **First-gate refusals** | **116** | **120** |

The raw census's 120 refusals are therefore 56 layout, 43 mip, 20 array and 1 3D; plus 8 explicit-linear singles
makes 128. The earlier [movie-port page](peer_port_movie.md) §12.1's 116 = 54 + 42 + 19 + 1 and 7 served counted
only 123 complete lines.

**Accounting discrepancy.** Five descriptor entries were absent or misjoined in that earlier newline-based aggregation.
Shape `#11` (`dim 3`, 128x72, array 1, mip count 7, format 41, layout `0x100`) had its `mip count` refusal suffix
joined after shape `#12` (`dim 2`, 128x1, array 1, mip 1, format 65, layout `0x100`). The earlier scrape therefore
counted the `#12` prefix as a mip refusal and missed the `#11` prefix; the raw fields separate them into one mip refusal
and one explicit-linear single. The remaining four prefixes are `#30` and `#45` (two layout refusals), `#61` (one mip
refusal) and `#93` (one array-first refusal; it also has multiple mips and a non-linear layout). Net change versus the
earlier counts: **+2 layout, +1 mip, +1 array, +1 explicit-linear single, 3D unchanged**. This is an accounting repair
from interleaved stderr writes, not five newly observed texture classes.

### 8.2 Shape and layout rule

The three newly served descriptions are exactly the measured one-pixel RGBA8 array class:

| Dimension | Width × height | Array slices | Mips | Format | Layout | Samples | Flags | Measured slot-8 row pitch / subresource bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `TEXTURE2D` (`3`) | 1 × 1 | 2 | 1 | `28` (`R8G8B8A8_UNORM`) | `0x100` | 1 | `0x300000` (no RT/DS bits) | 4 B / 4 B |
| `TEXTURE2D` (`3`) | 1 × 1 | 6 | 1 | `28` (`R8G8B8A8_UNORM`) | `0x100` | 1 | `0x300000` (no RT/DS bits) | 4 B / 4 B |
| `TEXTURE2D` (`3`) | 1 × 1 | 12 | 1 | `28` (`R8G8B8A8_UNORM`) | `0x100` | 1 | `0x300000` (no RT/DS bits) | 4 B / 4 B |

`PALMETTO_XG_LINEAR_ARRAYS` is a separate opt-in, read once. Unset, empty, `0`, `off`, `no` and any unknown value are
off; `on`, `1`, `yes` and `true` are on, case-insensitively. With it on, only the exact descriptor tuple above passes
create/layout validation; all other dimensions, extents, formats, sample counts, layouts, resource metadata flags,
mip counts and array counts still refuse at the named gate. The slot-2 record is `N * 0x100` bytes total, one colour
plane, one mip, `0x100` base alignment, `0x100` bytes per slice, a 1x1 texel extent and a 256-byte row pitch. Unused
record bytes remain zero, and writes past the 0x1B40-byte record are guarded as before.

Slot 8 handles plane 0 and subresources `[0, N)`: each measured source subresource contains one four-byte texel, which
lands at `subresource * 0x100`; the remainder of each padded 256-byte slice is left untouched. A nonzero plane,
out-of-range subresource, too-short pitch or byte count, inaccessible range, null pointer or duplicate subresource is
refused without writing. The shape whitelist is independent of the movie row-copy switch. With
`PALMETTO_XG_LINEAR_ARRAYS` unset/off, create and slot 2 retain their previous array refusal and slot 8 retains its prior
flat packed-copy result byte-for-byte. With the switch on, unmeasured arrays still fall through to that old flat copy;
they are not promoted just because the census called them arrays.

### 8.3 Exact falsifier and tests

The exact falsifier is a repeat trace of this same content window (or an authoritative XG layout answer) showing any
member of the claimed class with a descriptor other than `(dim 3, 1x1, array 2/6/12, mip 1, format 28, layout 0x100,
sample 1, flags 0x300000)`, a slot-8 call other than `(plane 0, subresource 0..N-1, row pitch 4 B, subresource bytes
4 B)`, or an array layout whose slice size is not `0x100` / whose slice offsets are not `i * 0x100`. Any one of those
observations falsifies the array layout model; keep the opt-in off and remove or revise this rule rather than expanding
the whitelist. A missing array call in a later boot alone is not a falsifier; it is an unobserved window.

The reused `xg_xs_test` target checks all three accepted counts and record shapes, exact format/layout/extent/mip/sample/
flag refusals, slot-8 argument/range/duplicate edges, switch parsing, census parsing and classification, pool reuse,
and the switch-off byte-parity result. Run under the x64 MSVC environment (PowerShell):

```powershell
cmake --build port\\build --target xg_xs_test --config RelWithDebInfo
$env:PALMETTO_XG_LAYOUT = 'linear'; .\\port\\build\\xg_xs_test.exe
$env:PALMETTO_XG_LAYOUT = 'refuse'; .\\port\\build\\xg_xs_test.exe
python -I tests\\test_docs_env_vars.py        # verifies PALMETTO_XG_LINEAR_ARRAYS is documented
```

Result for this slice: 364 checks / 0 failures in linear mode and 33 / 0 in refuse mode. The new switch is documented;
the full environment-documentation gate still reports one unrelated pre-existing missing entry in the current tree. No
`palmetto` build/relink or git write was done; only the shape-only trace metadata was read, and no package assets or
texture payload bytes were added to the repository.
