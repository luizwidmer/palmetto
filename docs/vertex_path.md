# The title's vertex path: the position scalars are zero and every vertex collapses to the origin

This page closes the geometry half of the black frame: the title's own vertex stage produces no fragments in the
presented target, even with culling forced off, while a project full-screen triangle into the same target reads back its
exact colour. The measured cause is not culling, not the vertex data and not an input-layout mismatch: the armed draw's
vertex shader is a translated FSPX blit shader whose clip position is the product of one fetched vertex register and four
push-constant scalars, and those four scalars are zero in the draw's recorded user data. Every vertex therefore lands at
`(0, 0, 0, 0)`: `w = 0`, a degenerate triangle at the origin, nothing rasterizes.

## 1. What was added (diagnostic only, off by default)

`PALMETTO_VERTEX_PATH=1` makes the replay dump the vertex path of the draw `PALMETTO_READBACK_AT_DRAW` arms. Bounded,
reads only, and off by default:

- the pipeline id and the shader-corpus identity the PSO resolved (module name and `<shader dir>/<name>.spv`), filled by
  the shim when it builds the PSO (`GraphicsPipeline::describe_vertex_path`, `VertexPathInfo`);
- the input layout actually bound to the Vulkan pipeline: every binding's stride/step rate/divisor and every attribute's
  location, `VkFormat`, byte size, offset and binding;
- the draw's vertex views: title VA, size, stride, and the resolved `VkBuffer` + offset the binding loop will use, plus
  the first 16 vertices' raw dwords read through the existing title-memory hook, and the first indices with the index
  view's format and size;
- the descriptor-table keys the draw names (the existing probe) and every non-image V#'s decoded address, declared size
  and byte statistics/dwords;
- the 144-byte push-constant block the shaders receive: `s0..s31` and `heap_base`/`code_base`;
- the pipeline's root-signature parameter table (each parameter's type, first user-data dword, dword count and constant
  count) and **per-push-constant-dword provenance**: `table` (a descriptor-table window), `root` (a root constant or root
  descriptor the draw's root arguments fill), `loose` (nonzero but no parameter covers it) or `unset`, with the owning
  parameter index;
- an `acceptance inputs` line naming the provenance of the four scalars the FSPX blit vertex shader uses (`s8`, `s9`,
  `s10`, `s28`), so the boot after the root-argument decode states their source directly;
- **the descriptor fetch slots** the shader reads itself: the dwords at `heap_base + (s0 & ~3)`, `heap_base + (s8 & ~3)`
  and `+ 0x40`, decoded with the existing gfx10 classifiers (T# image with address/format/extent, V# buffer with
  address/stride/records/format, S#, or `empty`), read through the shim's device-memory reader (the arena's host mapping);
  and the **driver-block-shaped V#** each bound vertex view would have (arena-relative address, stride, records), so the
  missing vertex-stream descriptor can be looked for in both places;
- the clip-position transform the measured FSPX vertex shader applies (see section 4) with an explicit `degenerate`
  verdict - degenerate when `w` is not `> 0` (zero, negative or NaN) or when x, y and z are all zero, so a console-feed
  value read as a float (a subnormal `s8`) cannot pass as extent while `w` stays zero.

Pure helpers added to `port/src/vulkan_replay.h` and pinned by `port/test/frame_dump_test.cpp`:
`vertex_format_bytes`, `vertex_layout_problems`, `blit_transform`, `blit_transform_degenerate`,
`push_constant_provenance` and `push_constant_provenance_name` (format sizes, layout overruns, the four-scalar
transform with its clipping-rule degenerate check, and the dword provenance from the root-signature table plus the
replay's table mask).

## 2. The armed draw, measured

Boot: standard environment (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`) plus `PALMETTO_VERTEX_PATH=1`, `PALMETTO_READBACK_AT_DRAW=20001`,
`PALMETTO_FRAME_DUMP=1`, 150 s through the harness, boot lock held. ALIVE, no exceptions, no failed submissions.

- **Draw and targets**: draw 20001, 3 vertices, 1 instance, first vertex 0, topology 17 (triangle list), full
  3840x2160 viewport and scissor, one colour target (format 37, 3840x2160), no depth. It is the six-image pass.
- **Pipeline**: `ps present 6 image(s) vs 213 load(s)`; the resolved modules are `fspx/lib_vs_00823` and
  `fspx/lib_ps_00822` (corpus files `spirv/fspx/lib_vs_00823.spv`, `spirv/fspx/lib_ps_00822.spv`).
- **Input layout bound**: binding 0, stride 0 at creation (dynamic, set at draw time), step rate vertex, divisor 0;
  attributes location 0 = `R32G32B32_SFLOAT` (12 B) at offset 0, location 1 = a 4-byte format at offset 12, location 2 =
  `R32G32_SFLOAT` (8 B) at offset 16. Total 24 bytes = the draw's stride. `vertex_layout_problems` = 0.
- **Vertex view**: title VA carries the view, size 4080, stride 24; resolved to a valid arena buffer + offset; the draw
  binds it with stride 24.
- **Vertex bytes** (through the title-memory hook): the view's first 384 bytes hold 24 non-zero bytes — exactly three
  vertices 24 bytes apart, positions `(0,0,0)`, `(0,2160,0)`, `(3840,2160,0)` (pixel coordinates of a full-screen
  triangle), plus a `0xFFFFFFFF` component and uv pairs `(0,0)`, `(0,1)`, `(1,1)`. The rest of the window is zero.
- **Push constants**: `s0`/`s1` are the descriptor-table title-VA pair, the rest zero in the pre-console-feed boot. In the
  console-feed boot (`PALMETTO_GRAPHICS_CONSOLE_ROOT=1`) `s8` also carries the table's arena-relative VA (`0xc16dea0`), so
  the shader can form `heap_base + s8`; `s9`, `s10`, `s28` stay zero. `heap_base` is a valid arena base; `code_base` 0.

So the vertex data the title put in the vertex buffer is present and sane, and the layer binds it correctly. The earlier
hypothesis "the vertex bytes are zero or the strides are wrong" is excluded.

## 3. What the vertex shader reads

`spirv-dis` on the corpus module shows the interface (no game bytes quoted here, only the structure):

- `OpEntryPoint Vertex` lists outputs `gl_Position` and two `Location 0/1` varyings, inputs `gl_VertexIndex`,
  `gl_InstanceIndex`, `BaseVertex`, `BaseInstance`, and the push-constant block. **There is no Input variable at
  Location 0, 1 or 2** — the bound input layout and the 24-byte vertex view are never read as vertex attributes.
- The module uses `PhysicalStorageBufferAddresses` (raw 64-bit addresses): it loads descriptors and data through
  `heap_base + <64-bit value>` where the 64-bit values come from the push constants (the translated-shader address model,
  `docs/d3d12_vulkan.md` section 3.5). It performs 213 loads; it samples no images.

This is why the layer's input-layout attributes are inert for this VS: the shader is a self-fetching translated FSPX
blit shader, not an attribute-driven one.

## 4. The position it computes, and why nothing rasterizes

The store to `gl_Position` in the module is a `select(true, ...)` of a composite built from one fetched vertex register
and four push-constant scalars:

```
gl_Position.x = s8  * vx
gl_Position.y = s10 * vx
gl_Position.z = s9  * vx
gl_Position.w = s28 * vw
```

with `vx`/`vw` the shader's own fetched vertex register dwords (its self-fetch, not the bound attributes). The four
scalars are the whole transform. The measured boot prints them:

```
vertex path: clip-position scalars x 0 y 0 z 0 w 0 -> degenerate: every vertex at (0,0,0,0), nothing rasterizes
```

The draw's recorded user data holds only the descriptor-table VA in `s0`/`s1`; `s8`, `s9`, `s10` and `s28` are zero.
Multiplying the fetched register by zero gives `(0, 0, 0, 0)` for every vertex (`w = 0`; if the fetched register were a
NaN the product is NaN, still not rasterizable): a degenerate triangle at the origin, clipped away, **no fragments**.
The verdict is written for that clipping rule: degenerate when `w` is not `> 0` (a NaN or a zero/negative `w`) **or**
when x, y and z are all zero, so a console-feed value read as a float (for example the arena-relative table VA
`0x0C16EF00`, which is a subnormal `~1.2e-31`) does not read as a real extent while `w` stays zero.

The dump also prints where every push-constant dword comes from, and names the four acceptance inputs. In the measured
boot the provenance line reads (parameter table: one descriptor table at `s0..s1`, nothing else):

```
vertex path: root parameters: 1
  p0 type 2 at s0 dwords 2 constants 0
vertex path: push-constant provenance
  s0 table(p0)  s1 table(p0)  s2 unset  s3 unset  ...  s8 unset ... s28 unset ...
vertex path: acceptance inputs s8 unset s9 unset s10 unset s28 unset
```

That is the acceptance gate for the root-argument decode: after the fix, the same boot must print the four scalars with
their provenance (`root` or, once the record path supplies them, the record block) and the transform line must say
`non-degenerate`. A dword the record decode fills without a root-signature parameter covering it prints `loose`, which
tells the decode's parameters and the recorded arguments disagree.

This explains every earlier measurement consistently: the PS-only control (title VS recipe + a project constant-output
PS) is black because the title VS emits nothing to rasterize; forcing culling off cannot help a degenerate triangle; the
same target read back with a project full-screen triangle draws fine (raster and readback are proven); and the sampled
textures being empty is a separate gap that would only matter once fragments exist.

## 5. Why the four scalars are zero: the root arguments never reach the draw

The push-constant block is the draw's 32 root-argument dwords plus heap/code bases. The trace shows the title issues
`SetGraphicsRootDescriptorTable` 31,458 times in the boot and **`SetGraphicsRoot32BitConstant`/`SetGraphicsRoot32BitConstants`
zero times** — the scalars are never sent as root-constant calls. They can only come from the console pipeline record
that creates these PSOs (slot 57's "PC object" root signature, a deserialized state record, `docs/state_blob.md`): the
record's root-argument block carries the values, and the shim's record decode currently reconstructs the descriptor table
(`s0`/`s1`) but not the record's root constants, so the draw's `s2..s31` stay zero.

So the gap is a record/root-signature decode gap, not a raster, cull or vertex-data gap: the FSPX blit vertex shader's
transform coefficients (measured exactly: `s8`, `s9`, `s10`, `s28`) are never populated for the draw.

The console-feed increment (`PALMETTO_GRAPHICS_CONSOLE_ROOT=1`, `docs/root_arguments.md`) then made the feed safe and
filled `s8` with the table's arena-relative VA - but that changed the semantics, not the coverage: `s8` as a float is a
subnormal (`1.16e-31`), and `s9`/`s10`/`s28` are still zero, so the clip position is still degenerate. The shader does not
multiply the fetched vertex by a real transform; it fetches a *descriptor* through the address it builds, and that
descriptor is what is now missing. Section 5b measures the three fetch slots.

## 5b. The descriptor fetch slots, measured

The dump reads what each of the shader's three fetches actually lands on, through the arena's host mapping, and decodes
the 4/8 dwords with the existing gfx10 classifiers (T#, V#, S#, or empty). Console-feed boot (150 s, ALIVE 150.2 s, no
device loss, 16,244 presents):

```
vertex path: heap_base 0x300010000 code_base 0x0; user data
  d6f5dea0 000001a9 00000000 ... (s0/s1) ... 0c16dea0 ...  (s8)
vertex path: fetch slot s0: heap_base + 0x4135dea0 (device 0x34136dea0) is not readable
vertex path: fetch slot s8: heap_base + 0xc16dea0 (device 0x30c17dea0): 00000000 00000000 00000000 00000000
                           -> empty (all four dwords zero)
vertex path: fetch slot s8+0x40: heap_base + 0xc16dee0 (device 0x30c17dee0): 0086dc40 00000000 000001e0 30014fac
                           -> V# buffer address 0x86dc40 stride 0 records 480 format 20
vertex path: driver-block-shaped V# for vb0: arena-relative address 0x86df10 stride 24 records 170 (view 4080 B)
```

and the provenance line says `s8 loose`: the console feed writes dword 8, but the pipeline's decoded root-signature
parameter table covers only the table at `s0..s1`, so no parameter owns `s8`.

Read together: the descriptor table the shader is pointed at (arena-relative `0xc16dea0`, 64 slots) has an **all-zero
first slot** - that is the fetch at table + 0, the console driver-block value the shader reads its vertex stream
through - while its only real V# (table + `0x40`, key 2) is the 480-byte constant buffer at `0x86dc40`. The draw's
actual vertex stream is bound at arena-relative `0x86df10` (stride 24, 170 records, 4080 B, the three measured
vertices), immediately after that constant buffer, and **no descriptor in the table names it**. So the vertex-stream
descriptor exists in the draw's binding, not in any slot the shader fetches; the s0 fetch is additionally unreadable
because `s0`/`s1` hold a title VA rather than an arena-relative value.

## 6. The exact gap and the smallest next fix

**Gap**: the vertex-stream descriptor the FSPX lib_vs_00823 blit shader fetches (its table's first slot, the console
driver-block value for the IA vertex stream) is all zeros; the layer binds the vertex buffer to the pipeline but never
synthesises that console descriptor from it. The table's only V# is the constant buffer, and the draw's vertex stream
(`0x86df10`, stride 24, 170 records) is not named by any descriptor the shader can reach. `s9`, `s10`, `s28` remain zero
as well, so the transform still collapses every vertex.

**Smallest next fix**: in the console-feed path (`PALMETTO_GRAPHICS_CONSOLE_ROOT`, `docs/root_arguments.md`), synthesise
the driver-block vertex-stream descriptor from the draw's bound vertex views - address = the view's arena-relative VA
(`0x86df10` for the measured draw), stride = the view's stride (24), num_records = size / stride (170), the layout's
format words - and write it at the table slot the shader fetches (table base + 0). That path is in the slot-57
record/console-feed files, outside this increment's files. The record path can keep marking
`VertexPathInfo::record_root_arguments` so the dump attributes the dwords to the record block. Acceptance test: the
`PALMETTO_VERTEX_PATH=1` boot's `fetch slot s8` decodes to a V# with address `0x86df10`/stride 24/records 170 instead of
`empty`, the PS-control readback changes from `mean 0 0 0, non-black 0/16384` to the control colour (`mean 32 160 240,
non-black 16384/16384`), and the display target readback changes from black. The readback must not be claimed changed
until it is measured.

**Next falsifier**: after the descriptor is synthesised, re-run the two-instrument boot. If the fetch slot is a real V#
and the PS-control readback still stays black, the descriptor is being read but the vertex fetch's format/layout words
are wrong, and the next place is the driver-block descriptor's own format/stride words against the input layout. If the
PS-control reads the control colour but the display target stays black, the vertex path is closed and the remaining gap
is the colour/sample path (`docs/texture_source.md`).

## 7. Limits

- The dump is bounded and opt-in; it changes nothing on the default path (the shim's describe hook is only read when
  `PALMETTO_VERTEX_PATH` is set, and every helper is pure).
- The position formula was read off the translated module and checked against the measured push constants; the fetched
  vertex register's own value does not matter for this conclusion because all four multipliers are zero or the descriptor
  fetch is empty.
- No game bytes, package addresses, shader bytes or extracted tables are recorded here: module identities, ids, offsets,
  formats, counts and positions only.
- Boots for this increment: the vertex-path boots on disk outside the repository (`boot-vertex1`, `boot-vertex2`,
  `boot-vertex3`), and the console-feed verification boot `boot-fetchslots1` (150 s, ALIVE 150.2 s, 0 exceptions, 0
  device loss, 16,244 presents) whose fetch slots, provenance and PS-control readback are quoted in sections 4-6. The
  boot lock was acquired for each run and released after it; the control readback's own line is documented in
  `docs/frame_capture.md` section 6.

## 8. The geometry fix, measured: the vertex-stream descriptor is filled, coverage is still absent

The fix is opt-in (`PALMETTO_VERTEX_STREAM_DESCRIPTOR=1`, on top of `PALMETTO_GRAPHICS_CONSOLE_ROOT=1`): one scratch
range per pipeline in the GPU address arena, the bound vertex stream's V# written there (`address` = the view's
arena-relative VA, `stride` 24, `records` 170, format 0, identity swizzle), the block's heap-relative address emitted in
the s0/s1 pair (which the module reads as the driver block), and - in the later variant - the same V# written into the
s8 descriptor table's slot 0 (which the module reads as one of its two table V#s). Default path unchanged; every boot
below is ALIVE 150 s with 0 exceptions, 0 failed/refused submissions and no device loss.

| boot | what was written | fetch slots after the write | PS-control readback |
|---|---|---|---|
| `boot-vsdesc1` | s8 table slot 0 only | s8+0 = the V# (address 0x86df10, stride 24, records 170); s8+0x40 = constants V# | mean 0 0 0, 0/16384 |
| `boot-vsdesc3` (no PS control) | s8 table slot 0 only | same | display readback: mean 0 0 0, 0/16384 |
| `boot-vsdesc4` | driver block in s0/s1 (table left as the title wrote it) | s0/s1 = 0xd46e500 with the V#; s8+0 = zeros; s8+0x40 = constants V# | mean 0 0 0, 0/16384 |
| `boot-vsdesc5` | driver block + s8 table slot 0 | both s0/s1 and s8+0 decode to the V# | mean 0 0 0, 0/16384 |
| `boot-vsdesc6` | as 5 but with the V#'s OOB mode raw (word 3 = 0x30000fac, the constants V#'s shape) | as 5 | mean 0 0 0, 0/16384 |

So every descriptor location the module's disassembly names now holds a descriptor that decodes the way the layer
encodes one: the driver-block V# at the s0/s1 address, the same V# in the s8 table's first slot, and the title's
constants V# at s8+0x40. Coverage is still absent, so the remaining wrong thing is **not a missing slot**: it is the
content/encoding of the synthesized driver-block V# (which fields the shader's fetch reads as address/stride/records and
whether the driver-block layout carries format/offset words beyond a gfx10 V#), or a data source the three fetches do not
cover. That is the console driver-block layout question, which `docs/root_arguments.md`'s owner measured - the smallest
next step is a joint review of this encoder against the module's fetch site (rootargs offered it in the same increment
that defined the user-SGPR layout).

The fix's instrumentation is kept for that review: it logs `vertex stream descriptor: pipeline 0x… driver block 0x…
(emitted at s0/s1), table slot 0 filled: wrote V# address 0x… stride … records … format 0 (dwords …)` once per pipeline
(bounded to 8 lines), writes nothing unless the switch is set, and the fetch-slot dump in sections 5b/6 shows both
addresses and the table. No pixel is claimed: the PS-control readback is the acceptance, and it has not changed.
