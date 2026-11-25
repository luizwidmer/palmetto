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
- the 144-byte push-constant block the shaders receive: `s0..s31` and `heap_base`/`code_base`; and
- the clip-position transform the measured FSPX vertex shader applies (see section 4) with an explicit `degenerate`
  verdict.

Pure helpers added to `port/src/vulkan_replay.h` and pinned by `port/test/frame_dump_test.cpp`:
`vertex_format_bytes`, `vertex_layout_problems`, `blit_transform`, `blit_transform_degenerate` (format sizes, layout
overruns, and the four-scalar transform with its degenerate check).

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
- **Push constants**: `s0`/`s1` are the descriptor-table VA (readable through the layer); `s2..s31` are **all zero**.
  `heap_base` is a valid arena base; `code_base` 0.

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

## 6. The exact gap and the smallest next fix

**Gap**: the slot-57 PC-object root signature's root-argument values are not decoded into the draw's user data; the FSPX
lib_vs_00823 blit shader needs push-constant dwords `s8`, `s9`, `s10`, `s28` as its clip-position x/y/z/w scales, and
receives zero.

**Smallest next fix**: decode the record's root-argument/root-constant block (or synthesize the root constants for the
PC-object root signature) and write those dwords into the draw's user data before the replay emits it — in the slot-57
record path (`port/shims/d3d12_xs_device.cpp` and the root-signature decode it uses), which is outside this increment's
files. Acceptance test: the `PALMETTO_VERTEX_PATH=1` boot prints `clip-position scalars x ... y ... z ... w ... ->
non-degenerate` with a nonzero `w`, and the PS-only control / display-target readback changes from black. The readback
must not be claimed changed until it is measured.

**Next falsifier**: with the record's root arguments decoded, re-run the same boot; if the scalars become non-zero and
the display readback still stays black, the vertex path is then closed and the remaining gap is the sample/colour path
(`docs/texture_source.md`); if the scalars still decode to zero, the record's root-argument block is not where the title
puts them and the next place is the record layout itself (`docs/state_blob.md`).

## 7. Limits

- The dump is bounded and opt-in; it changes nothing on the default path (the shim's describe hook is only read when
  `PALMETTO_VERTEX_PATH` is set, and every helper is pure).
- The position formula was read off the translated module and checked against the measured push constants; the fetched
  vertex register's own value does not matter for this conclusion because all four multipliers are zero.
- No game bytes, package addresses, shader bytes or extracted tables are recorded here: module identities, ids, offsets,
  formats, counts and positions only.
- Boots for this increment: the vertex-path boots on disk outside the repository (`boot-vertex1`, `boot-vertex2`,
  `boot-vertex3`); `boot-vertex3` is the final binary's run quoted above. The boot lock was released after each run.
