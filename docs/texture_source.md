# Texture data source: the sampled texture is a placed 4K image the XG fill never targets

**Status: measured increment, negative on content with the mapping chain named.** `docs/frame_capture.md` measured that
the six-image pixel shader's sampled images read black. This page follows one sampled T# through the layer to its D3D12
resource and its title-placed address, and measures where the title's texture-computer fill actually writes: the import
names the right image (not a wrong-image attachment), the fill does not target the sampled texture, and the sampled 4K
texture is a **different population** from the CPU-built defaults/atlas the fill serves. An A/B with both texture paths
on (`PALMETTO_XG_LAYOUT=linear` + `PALMETTO_TEXTURE_IMPORT=1`) leaves every sampled image black.

Ids, counts, formats, addresses and the project's own wording only; no package bytes, paths or extracted tables.

## 1. What was measured, and with what

The frame dump's sample mode (`PALMETTO_FRAME_DUMP_SAMPLE=1`, armed at draw 20001) was extended so that it reports
**every** resolved sampled T# of the draw's descriptor table, not only the first:

- for each descriptor-table slot key it prints the T# words, and when set 0 holds a layer image for that key it also
  prints the mapping chain: `key N T# image 0x… format F WxH, resource 0x…, title VA 0x…` - the layer image, the D3D12
  resource behind it, and the title VA the resource was placed at (`none (committed, not placed)` for an ordinary
  committed resource). The probe resolves every T# through the shim's descriptor state (it has the layer image, format,
  extent, resource id and placed address of every T# it registered);
- each resolved image is copied to host memory after the submission (GENERAL layout both ways, no write control - they
  are the title's textures) and reported as `sample readback [i/n] image 0x…: mean/non-black/peak`;
- with `PALMETTO_TEXTURE_IMPORT=1`, every placed-texture import decision is logged with the resource id and the title VA,
  so a sampled resource can be matched to the decision made for it: import, size skip, all-zero-head wait, or the
  bounded give-up.

Two 150 s harness boots, standard environment plus the switches below. All values are from the run the text names.

## 2. Experiment A: both texture paths on, sampled images still black

Boot A (`PALMETTO_XG_LAYOUT=linear` + `PALMETTO_TEXTURE_IMPORT=1`, sample mode, readback armed at draw 20001): ALIVE
150.3 s, 0 exceptions, 0 device-lost, 0 failed/refused submissions, 28,418 draws / 14,209 presents.

- The armed draw's table resolves one sampled T# (key 1): `T#[01000016 c3800000 021bc3bf 90000fac …]` ->
  `image 0x16558e85ad0 format 37 3840x2160, resource 0x16234fd4940, title VA 0x160812d0000`. The second T#-flagged slot
  (key 5, `45700000 45070000 00000000 ffffffff …`) has no set-0 entry, so it is not resolved and not read back.
- The sampled image's readback: `mean 0 0 0, non-black 0/16384, peak 0`.
- The display target in the same boot: `mean 0 0 0, non-black 0/16384`; write control `51 102 204`, 16,384/16,384.
- The import decided on that exact resource: `resource 0x16234fd4940 at title VA 0x160812d0000 is larger than the
  measured texture-computer fill (33177600 B); left as the committed stand-in`. 33,177,600 B is 3840 x 2160 x 4 B: the
  sampled texture is a 4K R8G8B8A8 placed texture, and the import's 16 MiB cap (calibrated to the measured 34,044 B
  fill) refuses it before the content gate is even consulted.
- No placed texture was imported: the small ones waited for the fill and gave up after their 512-attempt budget
  (`pages never held texture-computer bytes`), the large ones were size-skipped. So with both paths on nothing entered
  any image and no sampled image becomes non-black.

## 3. Experiment B: where the fill writes, against the sampled texture's address

Boot B (same as A plus `PALMETTO_XG_TRACE=1`): ALIVE 153.7 s, 0 exceptions, 0 failed/refused, 20,270 draws / 10,135
presents. The same chain was measured (boot-specific ids): sampled T# key 1 -> image `0x186547c5ad0` (format 37,
3840x2160) -> resource `0x183203e9500` -> **title VA `0x181a4ed0000`**; the import decision for exactly that resource is
`larger than the measured texture-computer fill (33177600 B); left as the committed stand-in`.

The texture-computer's slot 8 (CopyIntoSubresource) first eight destinations in the same boot:

```
slot8 #1  dst 0x18190C02000  src linear, arg5 0x100 (256 B/row)  arg6 0x8000 (32,768 B)   -> copied 32768 B
slot8 #2  dst 0x1819F1C7000  arg5 4  arg6 4  -> copied 4 B
slot8 #3  dst 0x1819F1C8000  ...             -> copied 4 B
slot8 #4..8  dst 0x1819F1C9000, 0x1819F1CA000, 0x1819F1CB000, 0x1819F1CC000, 0x1819F1CD000 -> 4 B each
```

None of them lies in the sampled texture's range at `0x181a4ed0000`; the fill writes a 32,768 B atlas and 4-byte "white
default" pages in the title's allocation area, while the sampled texture's placed span is 33,177,600 B. This agrees with
`docs/texture_layout.md`, which counted the whole fill as 65 copies / 34,044 B: there is no 33 MB fill for a 4K texture,
so the fill could not fill it even if the import's cap were lifted.

## 4. The chain, and which hypothesis it supports

```
sampled T# key (descriptor table slot 1)
  -> layer image (format 37, 3840x2160)                      [set 0's entry for those T# bytes]
  -> D3D12 resource 0x…                                      [the resource the layer image belongs to]
  -> placed title VA 0x… (33,177,600 B)                      [device slot 133 placement]
  -> import decision: skipped by the 16 MiB cap              [names the same resource and title VA]
  -> XG texture-computer fill destinations: 0x18190C02000 + 4-byte pages
     at 0x1819F1C7xxx, none inside the sampled texture's span [fill trace, same boot]
```

Verdict on the three candidates the increment was designed to separate:

| Candidate | Measurement | Verdict |
|---|---|---|
| the import is attaching to the wrong image | the import's decision line names exactly the sampled resource `0x…` and title VA `0x…` | **not supported** - the right image is seen |
| the fill goes elsewhere | the fill's destinations are a 32,768 B atlas and 4-byte defaults in a different address range; the whole fill is 34,044 B against a 33,177,600 B texture | **supported** - the fill serves other textures |
| the sampled image is a different population | a 4K RGBA8 title-placed texture the CPU-built defaults/atlas path never writes; nothing in the observed window fills it | **supported** - this is the content gap |

So the texture-data source for the six-image pass is not the XG texture-computer path at all in this window. The import
machinery is correct but has no source for this resource, and the import's size cap (calibrated to the measured fill)
would refuse a 4K texture regardless.

## 5. What was added, and what is next

Added (all diagnostic-only, off unless the named switch is set; default path unchanged):

- multi-key sampled readback in `port/src/vulkan_replay.h` / `port/src/vulkan_backend.h`: every resolved T# of the armed
  draw is read back (`PALMETTO_FRAME_DUMP_SAMPLE=1`), up to `max_sampled_keys` images per submission, each with its own
  window copy and `[i/n]` line; the mapping chain (image, resource, placed title VA) is printed per key;
- the sample probe in `port/shims/d3d12_vulkan_device.cpp` resolves every T# (not only the first) and carries the
  resource id and the placed title VA from the descriptor registration (`ImageEntry`), with
  `Resource::placed_pages()` naming the placement;
- per-placed-texture import decisions name the resource id and title VA for the bounded first eight of each kind
  (import, size skip, all-zero-head wait, give-up), so a sampled resource can be matched to its fate;
- `port/test/frame_dump_test.cpp` is unchanged; the mapping/readback paths are exercised by the boots above.

Next falsifier: **does anything ever write the sampled 4K texture's pages?** Run a bounded watch with the import's size
cap lifted for this texture only (`PALMETTO_TEXTURE_IMPORT_MAX` style, or a watch mode that samples the pages' non-zero
byte count every N submissions and imports only once content appears), over 150-300 s; if its pages never become
non-zero, the title is not writing this texture through its pages at all in the observed window, and the next place to
look is which path is supposed to produce this population (the title's streaming/DirectStorage upload, or a GPU-side
producer not yet reached). The current measurement already closes the XG-fill and wrong-image explanations.
