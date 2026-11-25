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
  tested in `graphics_pso_test` (base, table dword width, constant count, root-view two dwords, mesh base, spill
  refusal) and its one-dword table encoding (`console_table_value()`: an arena-relative value below 4 GiB passes unchanged, a
  larger one is refused, never truncated).
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

That first A/B (**title-VA form**) is refuted: writing the title VA's low dword into a one-dword console slot left the
replay's 64-bit rebase scan unable to recognise the pair, so the shader added `heap_base` to an un-rebased title VA and
faulted the GPU.

### 5b. The pinned console encoding, and the feed that works

The exact encoding comes from the translated `lib_vs_00823` module, not from assumption. It builds its first two
addresses as `heap_base + (s8 & 0xFFFFFFFC)` and `heap_base + (s8 & 0xFFFFFFFC) + 0x40`; the local slot that would be
the high half of that address (`s9`) is **stored zero** before use, and `heap_base` is the 64-bit value the shader
itself builds from push-constant dwords 32/33 (`0x300010000` in these boots). So a console table slot is ONE dword: the
arena-relative VA of the table's first descriptor (the value the replay's rebase would place in the PC pair's low dword),
with its low two bits masked off by the shader. A value that does not fit 32 bits cannot be expressed and is refused.

The feed now resolves the title VA to that same arena-relative form **at record time** (through the device's title-VA
map, `rebase_console_va()`), writes the one dword, and leaves the neighbouring dword alone - so no second title VA
window exists for the replay to mis-read, and a descriptor slot is never clobbered by a neighbour's high half.

Measured with the fixed feed (`PALMETTO_GRAPHICS_CONSOLE_ROOT=1`, 150 s):

| | value |
|---|---|
| verdict | **ALIVE 150.1 s**, no exception, no `VkResult -4`, no device lost |
| traced D3D12 calls / presents | 663,911 / 16,126 |
| console feed | `table title VA -> arena-relative 0xC16EF00 at s8` (one line, first occurrence) |
| armed draw user data | `s0/s1` = the table's title VA pair (rebased by the replay), `s8` = `0x0C16EF00`, `s2..s31` otherwise 0 |
| provenance | the value equals `arena_base + (title VA - heap range base)`: `0xae10000 + 0x135EF00` |
| clip-position diagnostic | `x 1.16e-31 y 0 z 0 w 0 -> non-degenerate` (the slot is now non-zero; the diagnostic reads it as a float) |
| display readback (128x128 at 0,0) | mean 0 0 0, non-black 0/16384, peak 0 - **unchanged** |
| PS-control readback | the control draws (`frame dump PS control: drew the armed draw's own vertices ...`), but no control readback line was produced in this boot - the acceptance axis is not measured yet |

So the layout half is now **safe and pinned**: the device survives with the console slot carrying exactly the value the
disassembly says the shader adds to `heap_base`, and the armed draw still reads back black. **Acceptance is not met**:
this increment removes the layout error, not the remaining cause of no coverage.


## 6. The next falsifier

The layout error is closed; the armed draw still produces no colour. The next measurements, in order:

1. **What each fetch slot resolves to.** The armed draw's user data now has the console table at `s8`, but the table's
   first 16 bytes (the descriptor the shader reads through `s0`/`s1`) are zeros, while its 64 slots also hold a V#
   over a 480-byte constant buffer of identity-shaped floats at `0x86dc40` (the vertex-path dump's constants probe).
   Dump the four dwords at each address the shader actually reads - `heap_base + (s0&~3)`, `heap_base + (s8&~3)`,
   `heap_base + (s8&~3) + 0x40` - at the armed draw, and say which of the shader's three V# fetches gets a real
   descriptor. The likely remaining gap is that the vertex-stream V# the shader expects is the **driver-block** value
   (`s0`/`s1` on the console), which this layer has never synthesised from the draw's bound vertex buffer; the table VA
   was filling it by accident.
2. **A named faulting submission (if the device dies again).** Run the same boot with `PALMETTO_VK_VALIDATION=1` (and
   synchronization validation) or arm the frame dump on the last draw before a loss, to name the pipeline whose shader
   read a console dword as an address.
3. **The PS-only control as the coverage axis.** `PALMETTO_FRAME_DUMP_PS_CONTROL=1` +
   `PALMETTO_FRAME_DUMP_CONTROL_PS=<shader dir>/fs_solid.spv` draws the armed draw's own vertices through the title's
   pipeline with a constant-output pixel shader: a coloured readback means the vertex stage and rasterisation produce
   coverage and the black is the title PS's own work; staying black means the geometry still produces no fragment. The
   control-draw line printed in this increment's boot but its readback line did not, so the control's own readback path
   needs checking (framecap's harness) before that axis can be read.

What would confirm the layout reading end to end: the console slot filled for the armed pipeline only and a PS-control
readback that changes from black to the control colour. What would refute it: `s8` non-zero at the draw while the
PS-control stays black after the fetch slots above are shown to hold the descriptors the shader wants - then the vertex
path is closed and the remaining gap is the colour/sample path (`docs/texture_source.md`).


## 7. The fetch-site spec (handoff to the frame-capture encoder)

The translated `lib_vs_00823` module loads **three descriptors** itself, all through `heap_base + (push-constant dword
& ~3)`, and they are not three copies of the same view. The exact reads and their consumers (disassembled with
`spirv-dis` from the published module; offsets/opcodes only):

| site | address the module forms | dwords loaded | what the module does with them |
|---|---|---|---|
| `s0`/`s1` | `heap_base + (((s1 & 0xFF) << 32) \| (s0 & 0xFFFFFFFC))` | 4 at +0 | **vertex-stream V#** (4-dword gfx10 V#): `base = ((w1 & 0xFFFF) << 32) \| (w0 & ~3)`, `stride = w1[29:16]`, `index = s2 + (gl_VertexIndex - BaseVertex)`, `count = w2`, `OOB = w3[29:28]`, `format = w3[18:12]`, `dst_sel = w3[11:0]`; it then loads the vertex dwords at element `index`, byte offsets 0/4/8 (POSITION), 12 (COLOR, typed) and 16/20 (TEXCOORD) |
| `s8+0x00` | `heap_base + (s8 & 0xFFFFFFFC)` | 4 at +0 | **matrix-A V#**: `base = ((w1 & 0xFFFF) << 32) \| (w0 & ~3)`, `declared = (w1[29:16] == 0) ? w2 : w2 * w1[29:16]`; the module reads **16 dwords** (a 4x4, 64 bytes) at base+0..60, each word loaded only when `declared >= 4*(k+1)`; **w3 is never read** |
| `s8+0x40` | `heap_base + (s8 & 0xFFFFFFFC) + 0x40` | 4 at +0 | **matrix-B V#**, same rule and same 16-dword fetch |

The 16 dwords from the two matrix V#s are the operands of the FMA chain that builds `gl_Position` (matrix-A words are
consumed at `%992..%1062`), and the vertex tags are the other operand, so all three fetches must hold real data for a
fragment to exist. Two field gates the module applies, both measured from the code:

* the **COLOR** attribute's typed path is gated on `bounds && (w3[18:12] != 0)` of the *vertex-stream* V# - a raw
  V# (format 0) silently returns zero COLOR (the conversion itself is inlined, so any non-zero format enables it; the
  exact format value is not pinned by this module);
* the vertex fetch's bounds test depends on `OOB_SELECT` (`w3[29:28]`): `0 structured_with_offset` needs
  `index < count` and `offset + 4 <= stride`; `3 raw` compares against `count`; both pass for the measured
  `index < 170, stride 24` and byte offsets up to 20.

### What the frame-capture encoder must write

* **`s0`/`s1` pair**: the heap-relative address of the 64-byte driver block (only its first 16 bytes are read), holding
  the vertex-stream V# at block+0:
  `w0 = uint32(vertex_arena_relative)`, `w1 = ((vertex_arena_relative >> 32) & 0xFFFF) | (stride << 16)` with
  **bit 31 = 0**, `w2 = view.size / stride` (170 measured), `w3 = identity DST_SEL (0x00000FAC) | format << 12`
  (format must be non-zero for COLOR) `| OOB_SELECT << 28` (0 or 3 measured passing).
* **table slot `s8+0x00`**: a **matrix** V# (raw constant buffer), *not* the vertex stream:
  `w0 = uint32(matrix_arena_relative)`, `w1 = ((matrix_arena_relative >> 32) & 0xFFFF) | (stride << 16)` (stride 0 for a
  raw buffer, then `w2` is the byte count), `w2 >= 64`, `w3` unused (write 0 or `0x30000FAC`).
* **table slot `s8+0x40`**: the title's own constants V# (`0x86DC40`, stride 0, `w2 = 0x1E0` = 480 bytes,
  `w3 = 0x30014FAC`) - leave it untouched.

**Why the earlier vertex-V#-in-slot-0 variant stays black**: the first 64 bytes of the vertex stream are the measured
vertices (`0,0,0,0xFFFFFFFF, ...`), and the module reads that as matrix A, so the 4th dword is `0xFFFFFFFF` = **NaN** as a
float; the FMA chain then produces non-finite clip coordinates and nothing rasterizes. Putting the vertex V# in slot 0
therefore does not fill matrix A, it poisons it.

**Where matrix A really comes from**: the draw's table slot 0 is empty in the title's own console heap (measured: all four
dwords zero, every boot), while its slot 1 is a T# and its slot 2 the constants V# above. The only producer of 32-byte
descriptor slots measured in this title is **console device slot 70** (`docs/device_slot70.md`): 14,262 calls in a window,
destinations zeroed before the call, a 1024-slot 32-byte ring, each record carrying a 128-byte CPU block, 32 elements and
a format word - i.e. the runtime, not the title, writes those descriptors, and this layer's measured answer there is a
refusal. So matrix A's source is the slot-70 publish (its 128-byte block is the per-frame transform; the shader reads its
first 64 bytes), and synthesising slot 0 from the vertex stream cannot be correct. A coverage-only *experiment* (not a
fix) would point matrix A at the existing 480-byte constants buffer: its first 4x4 is identity-shaped, so if matrix B is
the screen transform the pair may produce coverage; that is a measurement, not the title's data.

## 8. Limits and tests

* No game data, package addresses, shader bytes or extracted tables: the tables above hold counts, offsets and dword
  positions, and the boot logs quoted are on disk outside the repository (`D:\palmetto-work\boot-rootargs*`).
* The record negative is measured on one boot's 10,241 distinct records plus the library's 25,471 records; a record
  shape the decoder refuses (0 today) would be a different shape and its tail would need a fresh measurement.
* `PALMETTO_GRAPHICS_CONSOLE_ROOT` is diagnostic and off by default; the default path was booted ALIVE 150.1 s with the
  same numbers as before this increment.
* Gates: `build_gta6rt.cmd` -> `PALMETTO_OK`; `build_tests.cmd` -> `TESTS_BUILT`; `graphics_pso_test` -> 227 checks
  passed (12 new this task: 10 placement + 2 encoding); `graphics_record_test` unchanged.
* Boots: control `boot-rootargs-control` (150 s, ALIVE), console feed `boot-console3`, `boot-console5/6` (PS control)
  and the final binary's `boot-console7-final` (150 s, ALIVE), all outside the repository; the 25 s root-signature
  measurement boots are `boot-rootargs6` and `boot-rootargs4/5`.
