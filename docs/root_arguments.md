# The slot-57 record carries no root arguments: the graphics user-SGPR layout is the gap

**Status: measured negative for the record hypothesis, a measured layout finding for the vertex path, and a safe
refusal for the A/B.** The slot-57 graphics-pipeline record was decoded end to end: it is a root-signature pointer and
eight 0x20-byte stage slots, exactly `0x108` bytes, and the measured zero region (`+0x88..+0x108`, required by the
decoder on all 10,241 records of a boot) rules out a root-argument/root-constant block inside it. The pipeline library's
40 "undecoded" graphics-record bytes are five 64-bit identity hashes, not argument values. The four user-data dwords the
armed draw's vertex shader actually reads are `s0`, `s1`, `s2` and `s8`; the draw carries `s0`/`s1` (the descriptor
table the title sets) and nothing else. The root argument that *is* there sits at the **PC packing's** dword `s0`/`s1`
while a translated vertex/pixel shader reads its parameters at the **console layout's** base `s8` - so the shader's
`s8` is zero. That is a layout (D3D-448) gap, not a missing record decode. A diagnostic A/B that feeds both layouts
loses the Vulkan device at 30.3 s, so the duplication is **not** a safe default; the number is recorded below as the
next falsifier's starting point.

This page rests on `docs/vertex_path.md` (the vertex-path dump and the degenerate verdict; that page belongs to the
geometry measurement and is not edited here), `docs/d3d12_vulkan.md` section 3.5 (the user-SGPR layout),
`docs/graphics_record.md` (the record decode) and `docs/boot_frontier.md` (how the record was found). Nothing here
quotes game data: offsets, dwords, counts, verdicts and readback numbers only.

## 1. What the slot-57 record carries, measured

The decoder validates `0x108` bytes (`kDescriptorValidatedBytes`), and the boot's 10,241 records all decode with **0
refusals** while it requires `+0x88..+0x108` to be zero. The measured shape is:

```
+0x00  root signature pointer
+0x08  state blob pair, +0x18 input-layout pair
+0x28  slot 0 (ps) program pair, +0x38 its stage-blob pair
+0x48  slot 1 (vs) program pair, +0x58 its stage-blob pair
+0x68  slot 2 (ms) program pair, +0x78 its stage-blob pair
+0x88  zero through +0x108                <- 8 x 0x20 stage slots after the 8-byte header
```

`8 + 8 x 0x20 = 0x108`: the record is the root-signature pointer followed by **eight** stage slots' worth of bytes,
and the decoder's measured zero requirement covers the five unused slots plus the 32-byte tail of the eighth. There is
no room left in the record for a root-argument block, and none of the 10,241 records carries a non-zero byte where one
would be.

The same question for the pipeline library's own graphics record (0x70 bytes: `u64` hash, state index, layout index, 40
bytes undecoded, seven program/blob index pairs) is answered by the library's own statistics (tooling/shader_library.py
layout, read from the reassembled library outside the repository): the 40 bytes are five 64-bit fields with 8,252 /
25,471 / 76 / 18 / 93 distinct values over the 25,471 graphics records - identity hashes, not per-draw argument values
(a constant field would have one value, and root arguments vary per draw).

The one number the record *does* carry that could have been a root argument - the record's `+0x00` root signature
pointer - is a PC object the proxy created, so the record names the signature rather than any of its argument values.

## 2. The root signatures the title creates, measured

`port/shims/d3d12_vulkan_device.cpp`'s `CreateRootSignature` decoded the title's signatures during a 25 s boot (a
temporary env-gated dump, removed after the measurement; the boot is `boot-rootargs6` outside the repository). All of
the measured graphics signatures are version 2 (1.1), every parameter is `D3D12_SHADER_VISIBILITY_ALL`, and they are
the shapes a bindless engine needs:

| shape measured | parameter layout |
|---|---|
| 2-3 params | one descriptor table (1..261 ranges) + 32-bit constants (1..4 values), or two tables |
| 4-6 params | constants 2..8, then 1..5 descriptor tables |
| 3 params | 19 constants + 3 descriptor tables |

The armed draw's pipeline (`fspx/lib_vs_00823` + `fspx/lib_ps_00822`) needs the shape with **a descriptor table first**
(the title's draw-time `SetGraphicsRootDescriptorTable` sets that table and no other root argument; the boot records
31,458 table calls and 0 root-constant and 0 root-view calls).

## 3. What the vertex shader actually reads

The published shader metadata for `fspx/lib_vs_00823` (`<shader dir>/pipeline_graphics/index.json`, the same
`user_sgpr_reads` field `tooling/user_sgpr_census.py` computes over the corpus) is:

```
s0  ptr64_lo     the low dword of a 64-bit pointer
s1  const32
s2  const32
s8  ptr32_lo
```

That is four user-data dwords, not the four "position scalars" `docs/vertex_path.md` section 4 summarised from the
module: the SPIR-V the geometry measurement describes is a long fetch-and-FMA chain, and these four are the only
*user-data* registers in it. The translated module's use of them (disassembled from the published module, structure
only) is:

* `s0` (with `s1`'s low byte, whose only use is a stored-but-never-loaded flag) is a **32-bit heap-relative address** of
  a 4-dword V# descriptor, which the shader then uses to fetch the vertex stream;
* `s2` is an integer added to `gl_VertexIndex` (a base-vertex style offset; zero is the natural value for the draw);
* `s8` is a **32-bit heap-relative address** of a descriptor table, from which the shader fetches two more V#
  descriptors at `+0` and `+0x40` (checked against `heap_base` and a per-dword bounds test).

The armed draw's recorded user data is `s0`/`s1` = the descriptor-table VA (rebased to the arena) and `s2..s31` = 0.
So `s8` is the only *accepted* root argument the shader reads that the draw does not carry; the table VA it needs is
present in the same 144-byte block, one layout away.

## 4. The mapping rule, and why the value is at the wrong dword

`docs/d3d12_vulkan.md` section 3.5 carries two models:

* the **PC packing** (`rt::rootsig::pack`, what the graphics feed path uses today): root parameters in order from `s0`,
  **two dwords per descriptor table** (the 64-bit table VA), two per root CBV/SRV/UAV, one per 32-bit constant;
* the **console layout** (`rt::rootsig::pack_stage`, measured for compute): from the **stage base** - `s8` for
  vertex/pixel/geometry, `s16` for mesh - with **one dword per descriptor table** (its 32-bit heap VA), two per root
  view, one per constant.

For the measured graphics signatures the two layouts agree in *order* but not in dword: the table the title sets per
draw is parameter 0, so the PC packing puts its VA at `s0`/`s1` and the console layout puts it at `s8`. The translated
vertex shader reads `s0`/`s1` as an address too, and there it is a valid V# address - which is why the vertex stream is
fetched - but the second table address it reads at `s8` is zero, so its two other V# fetches resolve against the arena
base instead of the descriptor table. That is the layout half of the black frame, and it is the same D3D-448 follow-up
the graphics packing documents ("keeps `pack()` until per-stage user-data windows exist").

## 5. What is fed, what is added, and the A/B

Added in this increment:

* `rt::graphics_pso::console_graphics_user_data()` (port/src/graphics_pso.{h,cpp}): places a parsed root signature in
  the console user-SGPR layout for a graphics pipeline - base `s8` for a vertex/pixel/geometry pipeline, `s16` for a
  mesh one, one dword per table - and refuses a signature that would spill, naming the parameter. Pure, GPU-free,
  tested in `graphics_pso_test` (10 new checks: base, table dword width, constant count, root-view two dwords, mesh
  base, spill refusal).
* `RootArgumentLayout` carries each parameter's console slot (`console_offset`, `console_ok`, `console_base`), filled
  from that helper in `CreateRootSignature`.
* `PALMETTO_GRAPHICS_CONSOLE_ROOT=1` (diagnostic, **default off**): the command-list root-argument writer fills the
  console slot of every parameter in addition to the PC packing, and the graphics pipeline's `table_windows` marks the
  console table dword so the replay rebases it like the PC one.

Measured, one boot each, same environment and overlay, 150 s:

| | console-root off (control) | console-root on (A/B) |
|---|---|---|
| verdict | **ALIVE 150.1 s**, no exception | **device lost at 30.3 s** |
| traced D3D12 calls | 661,869 | 51,353 |
| `s2..s31` in the armed draw's user data | 0 | (armed draw never reached) |
| clip-position verdict | `x 0 y 0 z 0 w 0 -> degenerate` | - |
| display readback (128x128 at 0,0) | mean 0 0 0, non-black 0/16384, peak 0 | - |
| write control | mean 51 102 204, non-black 16384/16384, peak 204 | - |
| first exception | none | `0xC0000005` at `game+0x4da41c0` after the device loss |

So the duplication is **refused as a default**: writing the console slot unconditionally changes what every draw's
shaders read, and some pipeline in the frame path faults the GPU. The console layout itself is now measured and exposed;
the safe way to use it is not "write both layouts for every draw", which is what this A/B rules out.

## 6. The next falsifier

The A/B says the layout value must reach **only the shader that reads its stage's layout**, per pipeline, not every
draw. In rough order of cost:

1. **A per-pipeline console window mask.** The replay already carries a per-pipeline `table_windows` mask; give the
   pipeline a per-stage console layout record (the console base its stages read: `s8` vs/ps, `s16` ms) and feed only
   that layout, from the pipeline's own creation (`root_signature_layout` is available there), not from the command
   list. That is the D3D-448 "per-stage user-data windows" item and would make the two-layout collision behind the A/B
   impossible by construction.
   *A likely mechanism for the A/B fault, not yet measured:* the console table slot is one dword, and the feed writes
   only its low half; a console table VA is a 41-bit title VA at record time, so the replay's 64-bit window scan sees
   `(low, stale high)` and does not recognise (and therefore does not rebase) it, while the shader adds `heap_base` to
   the un-rebased title VA. Writing both dwords of a console table slot - the shader reads only the low one - would let
   the existing rebase recognise the pair; that is the first thing the per-pipeline attempt should test.
2. **A named faulting submission.** The A/B lost the device with nothing naming the draw; run the same boot with
   `PALMETTO_VK_VALIDATION=1` (and synchronization validation), or arm the frame dump on the last draw before the loss,
   to name the pipeline whose shader read a console dword as an address. The pipeline's root signature and its shader
   read set then say whether the value was a table VA, a constant or a collision between the two layouts.
3. **Narrow the diagnostic to the armed draw only.** With the layout known, feed the console slots for a single armed
   pipeline (the frame dump's `PALMETTO_READBACK_AT_DRAW` draw) so the vertex-path dump can show `s8` non-zero and the
   draw's fragments appear (or not) without risking the rest of the frame. This is the smallest measurement that can
   confirm the layout finding end to end.

What would refute the layout reading: a boot with the console slot filled for the armed pipeline only that still shows
`s8` zero at the draw (the write did not land where the shader reads), or a non-zero `s8` whose shader still produces no
fragments (then the vertex path is closed and the remaining gap is the colour/sample path, `docs/texture_source.md`).

## 7. Limits and tests

* No game data, package addresses, shader bytes or extracted tables: the tables above hold counts, offsets and dword
  positions, and the boot logs quoted are on disk outside the repository (`D:\palmetto-work\boot-rootargs*`).
* The record negative is measured on one boot's 10,241 distinct records plus the library's 25,471 records; a record
  shape the decoder refuses (0 today) would be a different shape and its tail would need a fresh measurement.
* `PALMETTO_GRAPHICS_CONSOLE_ROOT` is diagnostic and off by default; the default path was booted ALIVE 150.1 s with the
  same numbers as before this increment.
* Gates: `build_gta6rt.cmd` -> `PALMETTO_OK`; `build_tests.cmd` -> `TESTS_BUILT`; `graphics_pso_test` -> 225 checks
  passed (10 new); `graphics_record_test` unchanged.
* The temporary root-signature dump used for section 2 was removed from the shim after the measurement; the boot is
  `boot-rootargs6` (25 s, ALIVE) and `boot-rootargs4/5` (25 s, ALIVE).
