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

Next falsifier: **does anything ever write the sampled 4K texture's pages?** Section 6 records the instruments and the
measured answer (nothing does: the pages stay all-zero over the window, and the lifted import cap finds nothing to copy).
The current measurement also closes the XG-fill and wrong-image explanations.

## 6. The page-writer watch and the import cap override: measured, nothing writes the pages

Two opt-in instruments were added for this measurement; both are diagnostic-only, and with them unset the default path is
unchanged.

**`PALMETTO_TEXTURE_PAGE_WATCH=1` - the page-writer watch.** It watches up to four title-VA spans: every placed texture
whose subresource 0 is at least 16 MiB is watched from its creation, so the watch starts before the frame's draws; the
armed draw's own sampled texture is watched when the descriptor probe resolves its T#, so the exact population the
six-image pass reads is always one of them. Every 64th submission it counts the non-zero bytes of each span's page heads
and prints the first sample, every change of (committed bytes, non-zero bytes), and every 64th sample as a running
"latest" line.

**`PALMETTO_TEXTURE_IMPORT_MAX=<bytes>` - the import size cap override.** The placed-texture import's cap (default
16 MiB, calibrated to the measured 34,044 B XG fill) is replaced by the given byte count, logged loudly:

```
placed texture import: size cap overridden to 67108864 B by PALMETTO_TEXTURE_IMPORT_MAX (default 16777216 B)
```

**Measured boot** (150 s, harness, boot lock): standard environment plus `PALMETTO_XG_LAYOUT=linear`,
`PALMETTO_TEXTURE_IMPORT=1`, `PALMETTO_TEXTURE_IMPORT_MAX=67108864`, `PALMETTO_TEXTURE_PAGE_WATCH=1`,
`PALMETTO_FRAME_DUMP_SAMPLE=1`, `PALMETTO_READBACK_AT_DRAW=20001`, `PALMETTO_FRAME_DUMP=1`. ALIVE 150.2 s, 0
exceptions, 0 device-lost, 0 failed/refused submissions, 20,452 draws / 10,226 presents.

- The sampled T# resolves to the same population as before: `key 1 T# image 0x… format 37 3840x2160, resource 0x…,
  title VA 0x…` - the placed span is `0x1FA4000` = 33,177,600 B (3840 x 2160 x 4), one of the four watched spans.
- The watch registered that span and three other >=16 MiB placed spans (two 16,777,216 B, one 29,491,200 B). At samples
  1, 64, 128, 192, 256 and 320 its line for the sampled span reads **`sampled non-zero bytes 0 -> 0 of 129792 scanned`**:
  129,792 bytes = every one of the span's 507 64 KiB pages, 256 bytes of each page head, all zero. The other three spans
  likewise read `0 of 65536`, `0 of 65536` and `0 of 115200` scanned at every sample. **The pages are committed and
  readable and never held a non-zero byte in the window; no change line follows the first sample.**
- The import with its cap lifted to 64 MiB (enough for the 33,177,600 B texture) decided on exactly that resource: `the
  pages' head is all zero, waiting for the texture-computer fill`, and after its 512-attempt budget `pages never held
  texture-computer bytes; left as the committed stand-in`. Every other placed texture ended the same way. So the import
  would have copied the sampled texture and had nothing to copy.
- The sampled image readback is `mean 0 0 0, non-black 0/16384`; the display target is `mean 0 0 0, 0/16384`; the write
  control is `51 102 204`, 16,384/16,384.

Instrument corrections found on the way, recorded so the numbers are attributable: the first watch version used the
console-page pull's protection filter, which also skips non-cached and write-combined pages; the title's pages are console
memory and are mapped non-cached, so a readable span read as "0 of 0 scanned". The corrected watch accepts any page that
is not `PAGE_NOACCESS`/`PAGE_GUARD` (the import's own rule), walks every 64 KiB page of a large committed region rather
than only its first page, and samples 256 bytes per page head so a whole-span scan stays cheap (the watched boot made
20,452 draws, against 30,796 in an unwatched boot under the same switches).

**The CPU page-read question.** Plain CPU loads on the title side cannot be trapped diagnostic-only (guard pages or a
debugger watchpoint over a guest allocation would change the title's own memory protection), so the substitutes are: the
write watch (0 over the whole window), the device-level read entry points (`ReadFromSubresource`, `Map` - 0 in every
boot), and the fact the title's own draw samples the T#. The measurement says the CPU never produced this population's
bytes.

**Conclusion and next place.** Nothing produces the sampled 4K placed texture's content in the window: not a page writer
(the watch), not the XG texture-computer fill (`docs/texture_layout.md`; it writes only the 32 KB atlas and 1x1
defaults), and not a CPU copy (none was ever possible - the import's head reads saw zero at every attempt). What the title
does sample is a placed 4K RGBA8 texture whose pages are committed, readable, and never written, while the T# is consumed
by the draw. The next place named is therefore **the title-side producer that is supposed to write this resource**: the
only CPU-side content path measured alive in this build is the streamed-block consumer, and `docs/streamed_consumer.md`
shows the stream is consumed only through request N with the consumer stopping early (its next number is where between
requests 16 and 256 it stops), so the falsifier is either to bound that stop or to find the device-side producer a
console-addressed texture of this population was meant to come from. It is not the streamed block addresses themselves
(`docs/streamed_consumer.md` section 5) and not an import or fill address mix-up (section 4 above).
