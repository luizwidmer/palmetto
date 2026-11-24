# Console texture layout (XG): what the title does with the answers, and whether the refusal gates its uploads

**Status: measured increment.** The frame's last open lead was the console texture-layout family (`xg_xs.dll`:
`XGSuggestSwizzleMode`, `XGCreateTextureComputer` and its methods), which refuses with `E_NOTIMPL`. This page records
what the title's call sites do, what it does when the family answers, what was implemented and what is still refused,
and the measured result: **answering the family unlocks a real title-side upload path (65 subresource copies, 34,044 B
in 150 s, where the refusal boot made 8 no-op calls), but the frame stays black** because the runtime's stand-in for a
title-placed texture is an ordinary committed image that does not read the title's pages.

Counts, RVAs, format codes and the project's own wording only; no package bytes, paths or extracted tables.

## 1. The measured surface

The retail executable imports two functions from `xg_xs.dll`. Both call-site sets were re-read statically and then
counted at run time through the shim (`PALMETTO_XG_TRACE=1`):

| Entry / method | Call sites (game RVAs) | Measured calls, 150 s refusal boot |
|---|---|---:|
| `XGSuggestSwizzleMode(desc, 0, uint32_t* mode)` | `0x4ECCCA2`, `0xC4B3CE` | **15** |
| `XGCreateTextureComputer(const desc*, computer** out)` | `0x16F7141`, `0x344235`, `0x979C75`, `0x5121A5E` | **8** |
| computer slot 1 (read as Release) | 3 sites | not counted (uninstrumented in the A/B) |
| computer slot 2 (one out pointer, large structure) | `0x979C9F`, `0x5121A84` | **0** |
| computer slot 8 (six arguments, loop over subresources) | `0x16F71FB` | **8** |
| computer slot 10 (seven arguments) | `0x344283` | **0** |

The three `E_NOTIMPL` lines a boot prints are `warn_once` lines, one per entry point by design - they are **not** a
count of calls. The same boot reaches the family 15 / 8 / 8 times, and the linear boot below reaches it 44 / 24 / 65
times. The probe is therefore not a one-time startup question: the title asks again for every CPU-built texture it
makes over the whole window.

The reached path is the title's placed-resource fill function (`game+0x193DE10`), through the per-subresource copy
loop at `game+0x16F7100`:

1. the description helper (`game+0x4ECC940`) builds a 0x38-byte XG resource description from the title's own texture
   record. Its mode field (offset `0x2C`) is the title's own value `0x100 | mode` for known formats, and for an
   unknown format byte (`0xFF`) the helper calls `XGSuggestSwizzleMode(desc, 0, &desc[0x2C])` and passes the result up;
2. `XGCreateTextureComputer` is called with that description (from `game+0x16F7100`, which does **not** check the
   HRESULT - the two callers that do check, `game+0x979C40` and `game+0x5121980`, are the slot-2/layout-cache
   paths and are not reached while the family refuses);
3. for every subresource the title calls slot 8 once: measured arguments are `(destination resource address, 0,
   subresource index, linear source for that subresource, bytes per row, subresource bytes)`, e.g. a 256x128 8-bit
   description gives `arg5 = 0x100` (256 B/row) and `arg6 = 0x8000` (32,768 B), and the 1x1 4-byte descriptions give
   `arg5 = arg6 = 4`. The HRESULT of slot 8 is not read.

So the copy the refusal suppresses is a **CPU copy into the title's own page memory** (the destination addresses are
title page addresses; the source is a linear buffer, in the 1x1 cases a stack buffer). It is not a D3D12 copy: the
measured window has zero `CopyBufferRegion`/`CopyTextureRegion`/`CopyResource`/`CopyTiles`/`ResolveSubresource`
commands and zero compute dispatches in both boots. `docs/page_content.md` section 7 measured the same absence.

## 2. The A/B: what answering the family changes

Two clean 150 s boots, same tree, same package, harness only (`tools/boot_report.py`), standard environment
(`PALMETTO_SHADERS=F:\palmetto-work\shaders\spirv`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`), `PALMETTO_XG_TRACE=1` on both. Boot 2 also has capture on
(`PALMETTO_CAPTURE`) and the one-shot target readback (`PALMETTO_READBACK_AT_DRAW=1`).

| | boot 1: family refuses (default) | boot 2: `PALMETTO_XG_LAYOUT=linear` |
|---|---:|---:|
| Run | ALIVE 150.2 s, 0 exceptions, 0 device-lost | ALIVE 150.2 s, 0 exceptions, 0 device-lost |
| `XGSuggestSwizzleMode` calls | 15 | **44** |
| `XGCreateTextureComputer` calls | 8 | **24** |
| computer slot 2 calls | 0 | **2** (caller `game+0x5121A87`) |
| computer slot 8 calls | 8, all `E_NOTIMPL` (0 B written) | **65, all `S_OK` (34,044 B copied)** |
| copies / dispatches / upload-memory allocations (trace) | 0 / 0 / 0 | 0 / 0 / 0 |
| committed resources / placed creations / SRVs | 7 / 32 / 38 | 7 / 30 / 37 |
| `DrawInstanced` / presents | 32,848 / 16,424 | 29,116 / 14,559 |
| `ExecuteCommandLists` failed / refused | 0 / 0 | 0 / 0 |
| draw's target readback | mean 0 0 0, non-black 0/16,384 | mean 0 0 0, non-black 0/16,384 |
| readback write control | mean 51 102 204, 16,384/16,384 | mean 51 102 204, 16,384/16,384 |
| captured presented frames | (capture off; the baseline is black 24/24) | **mean 0.0000/255, non-black 0/24** |

The frame counts differ only because boot 2 has capture on (every 30th presented frame is copied to host memory and
written to disk), which slows the loop; the per-frame shape is unchanged.

What the answers unlocked, read from the copy contents (counts only, bytes not quoted):

- one **256x128, 1-mip** description with a real 32,768-byte 8-bit image as its source (non-uniform content - a font
  or UI atlas, not a solid colour);
- seven (and in boot 2 more) **1x1, 4-byte** descriptions whose source is a single 4-byte word, measured
  `0xFFFFFFFF` / `0x00FFFFFF` - the title's white default texture, built on the stack and copied into its page.

So the refusal does gate the upload of these CPU-built textures: with the family refusing, slot 8 is a no-op and the
title does not fall back to any other upload (no GPU copy, no upload-memory allocation, no second probe). What the
title does with each answer:

| Answer | What the title does with it |
|---|---|
| `XGSuggestSwizzleMode` = `E_NOTIMPL`, mode field untouched (stays 0) | The helper returns the HRESULT up; the reached caller does not branch on it. The description keeps mode 0 and is passed to `XGCreateTextureComputer`. |
| `XGSuggestSwizzleMode` = `S_OK`, `0x100` | Same call site, no branch; mode `0x100` reaches the computer description. 29 more suggest calls happen over the window because more description helpers now run. |
| `XGCreateTextureComputer` = `E_NOTIMPL`, `*out` set | The upload loop ignores the HRESULT and calls slot 8 through the object; the two checking callers take their clean failure path (so their layout-cache paths stay closed). |
| `XGCreateTextureComputer` = `S_OK` | The upload loop is unchanged; the two checking callers now continue into the layout-cache path and reach slot 2 (2 calls measured). |
| slot 8 `= E_NOTIMPL` | No bytes are written, no fallback, no error surfaced. |
| slot 8 `= S_OK` (linear copy) | The title continues to its next subresource/texture; the destination pages receive the row-major source bytes (34,044 B measured). |
| slot 2 `= E_NOTIMPL` | The caller (`game+0x5121980`) does not read the HRESULT and copies the uninitialised record it passed in as the layout - see section 4. |

## 3. What is implemented, and what still refuses

Default (`PALMETTO_XG_LAYOUT` unset or `refuse`) is bit-identical to the previous shim: both exports refuse with
`E_NOTIMPL`, `XGSuggestSwizzleMode` writes nothing, `XGCreateTextureComputer` still stores a computer (two measured
callers use it without checking), and every computer slot writes nothing.

`PALMETTO_XG_LAYOUT=linear` (measured here) serves what the backend can actually represent - ordinary row-major
images:

- `XGSuggestSwizzleMode` returns `S_OK` and writes **`0x100`**, the value the title's own description helper writes for
  a known texture whose mode byte is 0. The mode is only an input to the computer description; a console swizzle mode
  would make the title lay out console-tiled bytes the GPU reads wrongly, which this runtime cannot serve.
- `XGCreateTextureComputer` returns `S_OK` and a computer that keeps a copy of the description.
- Slot 8 (`CopyIntoSubresource`) copies the linear subresource: the destination subresource sits at the offset the
  title's own row-major source names (`source - first source address of that computer`), and the copy length is the
  measured `arg6`, capped by the largest a subresource of that description can be (16 bytes per texel x dimensions),
  with both ranges checked committed and accessible first. Long copies are not run outside a committed range.
- **Slot 2 (`GetResourceLayout`) and slot 10 (the tile list) still refuse by name.** Their structures are not
  modelled, and fabricating a console layout is forbidden by the project's "refuse rather than guess" rule. The
  measured 150 s window does not need slot 2 to answer (it is only reached when `XGCreateTextureComputer` answers
  `S_OK`, and then only through the layout cache), so the linear answer is complete for the upload measurement.

Implementation: `runtime/shims/xg_xs.cpp`; the call shapes, both modes and the copy bounds are pinned by
`runtime/test/xg_xs_test.cpp` (17 checks in the default mode, 21 with `PALMETTO_XG_LAYOUT=linear`).

## 4. Why the frame is still black, and what this rules out

The 34,044 bytes the title now copies with the linear answer land **in the title's own pages**, and the runtime does
not read them for a texture. Device slot 133 (`CreatePlacedResourceX`) for a placed **texture** says so itself:

```
[rt] d3d12: a title-placed texture (device slot 133) is an ordinary committed texture, not the title's pages; dimension 3
```

(`runtime/shims/d3d12_vulkan_device.cpp`, `create_at_host_address`: a texture gets an ordinary committed image on its
own `VkDeviceMemory`, and the title's writes at its page address do not reach it - only placed **buffers** get the
console-VA alias, and that alias rebases the VA for descriptors, it does not import the pages either). So a layout
answer can only become pixels once the runtime makes a placed texture read the title's pages - the same page-import
problem `docs/page_content.md` recorded for buffers, one resource class further.

That also rules XG out as the black frame's gate in the measured window:

- the family's uploads are small CPU-built textures (one 256x128 atlas-like image and 1x1 defaults, 34 kB total), not
  the streamed world textures. The 30-32 placed-resource creations and 7 committed resources per boot are unchanged by
  the answers, and no GPU copy or compute dispatch appears, so the sampled images still get no content from here;
- the frame's black is unchanged in both boots (target readback mean 0, capture 0/24), with the readback instrument
  proven by a write control in the same boot (51/102/204);
- the streaming side (DirectStorage: one zlib request then nothing, no upload-memory allocation) is a different lead
  and is being measured separately; answering XG is measurably not what unblocks it.

One caveat on the `S_OK` answer: with `XGCreateTextureComputer` answering `S_OK`, the title's layout-cache caller
(`game+0x5121980`) does not check slot 2's HRESULT and treats the (unwritten) stack record as a layout. That is why the
linear mode is an opt-in measurement switch, not the default, and why filling slot 2 is part of the next increment.
The default boot keeps the layout-cache path closed, exactly as measured before this page.

## 5. Reproduce

```
# refusal baseline (default) - writes nothing, frame stays black
python -I tools\\boot_report.py --exe runtime\\build\\palmetto.exe --image <title> --game-dir <package>
    --overlay <overlay-dir> --timeout 150 --out <out-dir>
    --env PALMETTO_SHADERS=F:\\palmetto-work\\shaders\\spirv --env PALMETTO_D3D12=vulkan
    --env PALMETTO_SKIP_INT3=1 --env PALMETTO_SEED_GLOBAL=75DF268:D8 --env PALMETTO_XG_TRACE=1

# linear answers (the A/B above)
... --env PALMETTO_XG_LAYOUT=linear --env PALMETTO_CAPTURE=<capture-dir> --env PALMETTO_READBACK_AT_DRAW=1
```

The counters (`[rt] xg_xs counters (...)`) and the first calls with their description words and slot-8 arguments are
what the table above reads; the capture frames are measured with `ppm_brightness.py`.

## 6. Next increment

1. **Make a title-placed texture read the title's pages.** The linear answer already produces correct row-major bytes
   in those pages; the backend needs to import them (or copy them into the stand-in image before the texture is
   sampled), for the placed-texture class, the way `alias_console_va` handles buffers. This is the increment that
   would turn the 34,044 bytes the title writes into visible content - and it is in
   `runtime/shims/d3d12_vulkan_device.cpp`, outside this page's write scope.
2. **Slot 2 (`GetResourceLayout`) and slot 10.** Only then can `XGCreateTextureComputer` answer `S_OK` without the
   layout cache consuming an uninitialised record. The structure shapes are known from the call sites (slot 2: one out
   pointer to a 0x6C8-byte record; slot 10: seven arguments, a count and a 12-byte-entry list) but their fields are not
   decoded, so both refuse by name today.
3. **Re-run the streaming measurement with the linear answers in**: if the streamer's wait state is what gates the
   world textures, XG answering will not change it, and the two leads can be closed independently.
