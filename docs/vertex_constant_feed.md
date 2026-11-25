# The vertex/transform feed: the console driver block, and where the frame's transforms really come from

**Status: measured.** The frame's content is lost at the constant pass's vertex stage, and the loss is **not** a missing
slot-57 root-constant decode: the slot-57 record carries no root arguments at all (measured negative,
[root_arguments.md](root_arguments.md)), and the four frame modules do not read pushed transform scalars either. What a
translated vertex stage actually reads is the **console driver block** at `s0`/`s1` - the vertex-stream V# the console
driver synthesises from the draw's input-assembly vertex view - and, where a transform is needed (the compose pass), the
**matrices it fetches through the `s8` descriptor table**: matrix A at table `+0`, matrix B at table `+0x40`. This slice
implements both measured feeds in our structure behind their switches and measures them: the driver-block fetch now
decodes to the bound view's V# (address, stride 24, records 170, format 22) instead of the table's first descriptor, but
the constant pass's target stays uniformly zero, and two of the three with-feed arms stalled before their frame loop. Both
switches therefore stay **off by default**, and the next falsifiers are named in section 6. No title data is replaced:
the descriptor table and every descriptor the title wrote stay untouched, and the layer's own input-assembly vertex
binding is unchanged.

## 1. Why this measurement

At HEAD the frame is one command list with eight commands and two draws: draw 4000 (the constant pass,
`fspx/lib_vs_00821` + `fspx/lib_ps_00820`, zero image samples) draws into a sibling 4K target `B`; draw 4001 (the compose
pass, `fspx/lib_vs_00823` + `fspx/lib_ps_00822`, six image samples) draws into the presented target `A` and samples `B` by
exact resource identity. A full-target readback of both targets is uniformly zero in all four channels
([frame_first_light.md](frame_first_light.md) section 4), no copy/upload/resolve command exists in the boot, and the
compose pass's only T# resolves to `B`, so the presented black is the constant pass's empty target.

The vertex data is not the gap: `vb0` is bound with 4080 bytes, stride 24, 170 records, and its first three vertices are
the clip-space corners of a full-screen triangle ([frame_first_light.md](frame_first_light.md) section 6). The draw runs
but its vertex stage produces no rasterisable vertex, because the translated vertex shader reads its vertex-stream
descriptor from a *driver block address* in its user data while the port puts the descriptor table's pair there.

## 2. Measured shapes

### 2.1 What the graphics user data really carries

The translated graphics stages read their root arguments in the **console layout**, not the PC packing the graphics bind
point fed: the stage base is `s8` (vertex/pixel/geometry; `s16` mesh) and a descriptor table takes **one** dword, the
32-bit heap-relative VA of its first descriptor ([root_arguments.md](root_arguments.md) section 4). Before the root
arguments there are **driver-managed dwords** that no D3D12 API call writes:

| dword(s) | what it holds | evidence |
|---|---|---|
| `s0`/`s1` | the heap-relative address of the console **driver block** whose first 16 bytes are the vertex-stream V# | each frame vertex module builds `heap_base + ((s1 & 0xFF) << 32 \| (s0 & ~3))` and loads a 4-dword V# there |
| `s2` | the vertex-index addend (the shader's index is `s2 + (gl_VertexIndex - gl_BaseVertex)`) | same modules; the measured draws record `s2 = 0` and draw with `first = 0` |
| `s8`.. | the root arguments in console order: one dword per descriptor table | the measured table-only signatures place their table at `s8` |

The 32-dword block is therefore the recorded root arguments plus three driver dwords, not "root constants plus a table":
the title issues 31,458 `SetGraphicsRootDescriptorTable` calls and **0** calls to every root-constant/root-view setter in
the measured boot ([root_arguments.md](root_arguments.md) section 9); the slot-57 record's `0x108` bytes are the
root-signature pointer plus eight `0x20`-byte stage slots with no root-argument block (10,241 records, 0 refusals); and
the console stream subobject path the title uses for created pipelines carries only standard PC subobjects.

### 2.2 The four frame stages' published read sets

`user_sgpr_reads` for the boot's modules (`<shader dir>/pipeline_graphics/index.json`, the field
`tooling/user_sgpr_census.py` computes), cross-checked against the modules' disassembly (structure only):

| module | stage | user-data reads | what the module does with them |
|---|---|---|---|
| `fspx/lib_vs_00821` | vertex | `s0` ptr64_lo, `s1` const32, `s2` const32 | driver-block address (`s0`/`s1`) and index addend (`s2`); the clip position is the fetched vertex's xyz with `w = 1.0` - no other user dword reaches it |
| `fspx/lib_ps_00820` | pixel | `s8` ptr32_lo, `s9` const32 | table address `(s9 << 32) \| (s8 & ~3)`, i.e. a one-dword table dword with the high half zero; reads the table's first 4 dwords, then a V# inside them at `+0x20` |
| `fspx/lib_vs_00823` | vertex | `s0` ptr64_lo, `s1` const32, `s2` const32, `s8` ptr32_lo | driver block + index addend, and the matrix table at `heap_base + (s8 & ~3)`: matrix A is 16 dwords at `+0`, matrix B 16 dwords at `+0x40`, each gated on the V#'s declared size; the clip position is the FMA chain over those 32 dwords and the fetched vertex tags |
| `fspx/lib_ps_00822` | pixel | `s8` ptr32_lo, `s9` const32 | table address as above; the sampled T# is fetched from the table |

So the earlier "four position scalars `s8`, `s9`, `s10`, `s28`" reading ([vertex_path.md](vertex_path.md) section 4) is
not the read set of the frame's modules: those dwords appear in the dump's hardcoded acceptance list, not in the
modules, and [root_arguments.md](root_arguments.md) section 3 already corrected the same summary for `lib_vs_00823`. The
transform is **fetched**, not pushed; what the vertex stage needs from the feed is the driver block and the table
pointer, not scalar constants.

### 2.3 Where the transforms really come from

| transform input | source | state |
|---|---|---|
| vertex stream | the title's input-assembly binding (`IASetVertexBuffers`, measured `vb0` 4080 B stride 24); the console **driver block** V# is the driver's synthesis of it and is what the shader fetches | this slice synthesises it from the bound view |
| matrix A (compose pass, table `+0`) | the console runtime's per-frame publication (device slot 70's 32-byte descriptor ring, whose record carries a 128-byte CPU block), not the title's own descriptor heap: the measured table slot 0 is empty | **open**: the layer refuses slot 70's publication ([root_arguments.md](root_arguments.md) section 7); outside this slice |
| matrix B (compose pass, table `+0x40`) | the title's own 480-byte constants buffer V# written into its table (measured V# base + 0x1E0, `w3 = 0x30014FAC`) | present; the console table feed points the shader at it |
| `s2` index addend | driver-managed; 0 for the measured draws | kept at the recorded value |

This answers the three candidates in the slice scope: **not the slot-57 record** (no root-argument block, measured
negative), **not the PSO stream blob** (the public reference's stream path is a PC pipeline-state carrier; section 4),
and **not a per-frame constant buffer the title fills for the vertex stage** - the only per-draw buffer the vertex stages
fetch is the title's descriptor table itself, and its transform slots are the two matrices above.

### 2.4 The driver-block V# form, and the measured failure before the feed

The V# encoding is pinned from the translated module ([root_arguments.md](root_arguments.md) section 7) and unchanged:
`w0 = uint32(address)`, `w1 = ((address >> 32) & 0xFFFF) | (stride << 16)` with bit 31 clear, `w2 = records`,
`w3 = 0x00000FAC | format << 12 | oob_select << 28`; format 22 (`GFX10_FORMAT_32_FLOAT`, non-zero because the module
gates its inlined colour conversion on it). The measured draw's bound view gives address `0x76ef30`, stride 24, records
170.

The vertex-path dump of draw 4000 in the shipping-path boot ([frame_first_light.md](frame_first_light.md) section 6) reads
`fetch bases: s0/s1 pair 0xc06ef00, s8/s9 pair 0x0`, and the fetch at the `s0`/`s1` pair decodes to
`V# address 0x76ff20 stride 0 records 96 format 20` - the 96-dword V# the title's table holds at its first slot - while
the driver-block-shaped V# for the bound view is `address 0x76ef30 stride 24 records 170`. The shader therefore reads a
dword buffer as a vertex stream; the `s8`/`s9` pair is zero because the console table feed was off by default.

## 3. What we implemented

Both feeds are behind their measured diagnostic switches - **off by default**, because the A/B below does not meet the
slice's acceptance. With both off the path is exactly the shipping path the Control arm measured.

1. **A safe console root-argument feed** (`port/shims/d3d12_com.cpp`, the graphics root-argument writer;
   `PALMETTO_GRAPHICS_CONSOLE_ROOT=1`): every graphics parameter is written at its console slot as well as at its PC
   dword, so a translated stage finds the same table at `s8` the PC path feeds at `s0`. A descriptor table's console slot
   is one dword, the arena-relative VA the shader adds `heap_base` to (`rt::graphics_pso::console_table_value`).
2. **The dual feed is planned, not hoped for** (`port/src/graphics_pso.{h,cpp}`, `console_feed_plan`): the console layout
   and the PC packing share the one 32-dword block, so the console feed runs only when every console placement fits the
   32 dwords **and** no console dword lands on a PC-packed parameter's dword. A refused signature feeds nothing, keeps
   every dword at its PC/previous value, and is logged once with the parameter named
   (`graphics console root: not fed for this signature: ...`).
3. **The console driver block is synthesised and emitted**
   (`port/src/vulkan_replay.h`, the vertex/user-data emit region; `PALMETTO_VERTEX_STREAM_DESCRIPTOR=1`): one 64-byte
   arena scratch range per pipeline holds the vertex-stream V# for the draw's bound slot-0 view, rewritten whenever the
   view's address or shape changes, and its heap-relative address is emitted in `s0`/`s1` after the recorded title VAs
   are rebased. `s2` and every other dword keep the recorded value (0 for the measured draws). The layer's
   input-assembly vertex binding, the title's descriptor table and the table's own V#/T# slots are not touched.
4. **The feed decides per draw from measured facts** (`frame_dump::driver_block_decision`): a non-mesh pipeline whose
   input layout uses vertex slot 0, a slot-0 view with a stride and at least one record, and a descriptor table pair at
   `s0`/`s1` in the root signature (the dwords the feed replaces). A draw that does not qualify keeps its recorded user
   data and the reason is logged once by name. The decision no longer depends on the console feed: the console ABI puts
   the driver block at `s0`/`s1` whether or not the table at `s8` is also fed.
5. **The vertex-path dump gained a `driver block:` line**, so the acceptance is read from the same boot as the
   readbacks: the fetch at the emitted `s0`/`s1` pair must decode to the bound view's V# (stride 24, records 170), not to
   the table's first descriptor.

## 4. Cross-check against the public reference

The reference read for this slice is the public GPL-3.0 project `ubiquitarianism/leonida` (checkout
`D:\leonida-work\leonida-fork`), files `runtime/shims/d3d12_xs_pso_stream.{h,cpp}` and `runtime/src/root_signature.h`, at
that checkout's HEAD `ed94fbef9` (the post-story-runtime merge).

- **What agrees**: the reference's pipeline-state-stream module captures and re-derives a **PC pipeline state stream**
  (slot 86's `D3D12_PIPELINE_STATE_STREAM_DESC`, slot 55's derived records) so a PSO can be rebuilt from PC subobjects.
  It carries no user-data array, no root-argument block and no transform, which agrees with our measurement that the
  created pipeline state is not a transform carrier. Its `root_signature.h` also documents the console layout as "from
  the stage's first root-argument SGPR, one dword per descriptor table" with the graphics base `s8`.
- **What differs / what is still missing there**: the reference does not synthesise a console driver block from the
  draw's input-assembly binding; its graphics-layout note derives the base from a corpus census rather than a live feed.
  Our measurement shows what the driver dwords before the base are (`s0`/`s1` driver block, `s2` index addend) and this
  slice feeds them. Where the reference and this measurement disagree, the measurement wins
  ([upstream_port.md](upstream_port.md) sections 2-3), and the disagreement is recorded here.
- No code, comment text, identifier or table was copied from the reference. The reference remains the copyright of its
  contributors under GPL-3.0; its license continues to apply to its own tree.

## 5. A/B measurement

One frozen binary per arm (`palmetto-feed.exe`, SHA-256
`E5396B47CE5F8CD8D3B5E6EF4AF1E64053E162E4B39BE292D0F54601A726BF2E`, built from `D:\leonida` at 14:37:58 with this
slice's files and other agents' uncommitted work in the tree), one boot per arm, the boot lock held, 150 s each, the
shipping environment plus the read-only frame diagnostics (`PALMETTO_FRAME_DUMP=1`, `PALMETTO_FRAME_DUMP_SAMPLE=1`,
`PALMETTO_VERTEX_PATH=1`, `PALMETTO_READBACK_FULL=1`, `PALMETTO_CAPTURE` every 300 presented frames). The two switches are
set explicitly, so each arm is the same configuration the committed code produces with those switches:

| arm | switches | readback draws | verdict | target `B` (full 4K) | target `A` (full 4K) | captures |
|---|---|---|---|---|---|---|
| Control | both off | 4000/4001 | ALIVE 150 s, ~8,912 submits / 4,455 presents at +140 s | mean 0 0 0, **0/8,294,400** (alpha 0) | mean 0 0 0, **0/8,294,400** | 8/8 black |
| Console only | `GRAPHICS_CONSOLE_ROOT=1` | 4000/4001 | ALIVE 150 s, ~9,892 / 4,945 at +140 s | mean 0 0 0, **0/8,294,400** | mean 0 0 0, **0/8,294,400** | 8/8 black |
| Both feeds | both on | 4000/4001 | **stalled**: 154 submits / 76 presents at +50 s, flat to +150 s | not reached | not reached | 1/1 black |
| Both feeds, early readback | both on | 100/101 | ALIVE 150 s, ~9,237 / 4,618 at +140 s; driver-block fetch = V# address `0x6aef30` stride 24 records 170 format 22 | mean 0 0 0, **0/8,294,400** | mean 0 0 0, **0/8,294,400** | 8/8 black |
| Both feeds + PS control (cull none) | both on + `PALMETTO_FRAME_DUMP_PS_CONTROL=1`, `_CULL_NONE=1` | 100/101 | **stalled**: 14 submits / 6 presents, flat | not reached | not reached | 1/1 black |

### 5.1 What the numbers say

- **The feed's structural acceptance is met.** In the early-readback arm the vertex-path dump's
  `fetch slot driver(s0/s1)+0x00` decodes to `006aef30 00180000 000000aa 00016fac -> V# buffer address 0x6aef30 stride
  24 records 170 format 22` - exactly the V# shape the bound view implies - instead of the shipping path's
  `address 0x76ff20 stride 0 records 96 format 20`. The emitted `s0`/`s1` pair is the driver block
  (`driver block: s0/s1 <- 0xd6a7800` on that boot), and the recorded table pair is not emitted.
- **The constant pass's target is still uniformly zero** (`0/8,294,400` in all four channels, the 8x8 tile map all
  zero), so the slice's acceptance ("the constant pass writes non-black") is **not** met, and the presented target stays
  black. The feed is necessary but not sufficient.
- **Two of the three with-feed arms stalled** before reaching the readback they armed. The stall is a title freeze: no
  further submissions or presents, process memory flat, threads parked, no exception and no device-lost line; the
  pipeline counters stop at the last submission. The Control and Console-only arms never stalled. The stall is not
  deterministic across arms (the early-readback arm ran 150 s), so it is timing/phase dependent and must be attributed
  before any of these feeds can default on.
- **The console feed alone is safe and moves nothing**, which is consistent with the vertex stage still being degenerate
  without the driver block: it is a layout repair, not the content fix.

### 5.2 The named hypothesis for the remaining black

With the vertex stream now fetched, the constant pass's clip positions are the bound vertices' xyz with `w = 1.0`
(-1,1,0 / -1,-1,0 / 1,-1,0), i.e. a full-screen triangle. Its winding in D3D12 screen space (framebuffer coordinates,
with the negative-height viewport the replay uses) is counter-clockwise, while the pipeline's rasterizer state comes from
the still-undecoded slot-57 record and is the documented stand-in `documented_default_state()`: `CullMode = BACK`,
`FrontCounterClockwise = FALSE`. Under that state the triangle is a back face and is culled before the pixel stage, which
explains a uniformly zero target with a correct vertex fetch. That is a fixed-function-state question
(`port/src/graphics_pso.h`, `docs/state_blob.md`), not a vertex-feed one. The decisive measurement is already
implemented but did not run: the PS-control substitution of the constant pass's pixel shader (a constant-output
`fs_solid`) with `PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE=1` at an early draw - non-black coverage would prove the feed
and lay the black at the stand-in's cull mode; black would move the question to the fetched vertices or the PS.

## 6. Limits and the next falsifiers

1. **The stall is the blocking falsifier for defaulting the feeds on.** It needs a named cause: a run with
   `PALMETTO_VK_VALIDATION=1` (and synchronization validation) or a device-lost counter, plus the frame dump armed at the
   first with-feed submission, to say whether the first real physical-storage fetch the pixel stage performs with the
   console table fed (a bad descriptor address, a GPU fault) or a host-side wait is responsible. Until then both switches
   stay off, which is why the default path is byte-identical to the measured shipping path.
2. **The cull/stand-in hypothesis** (section 5.2) needs the PS-control + cull-none measurement above; if coverage appears,
   the frozen black is the slot-57 fixed-function decode (task-44's field), not this feed.
3. **Matrix A of the compose pass is still empty** (section 2.3): slot 70's per-frame publication is refused by the layer
   and is outside this slice, so the presented target is expected to stay black even when the constant pass writes
   content.
4. **The dual feed handles only signatures whose two layouts do not intersect** (section 3.2). A signature whose PC
   packing reaches `s8` is logged by name and keeps the PC values; what a translated stage reads at `s8` there is
   unmeasured. A signature with more than one descriptor table places the second at console `s9`, while the pixel
   modules build their table address from the `s8`/`s9` pair - such a signature needs a fresh measurement (the measured
   frame's signatures are table-only).
5. **Mesh pipelines** (`s16` base) are not fed by this path, and the vertex-path dump's fixed acceptance list
   `s8 s9 s10 s28` is left as it was: it is the dump's list, not the frame modules' read set (section 2.2).
6. The console driver block is a replay-side synthesis in the GPU address arena; it is not written back to the title's
   memory and disappears with the arena. The measured window is the stationary background/upscale loop
   ([frame_first_light.md](frame_first_light.md) section 10); a later content pass with different vertex formats is not
   claimed.

## 7. Tests, gates and boot evidence

- `port/test/graphics_pso_test.cpp` (`test_console_feed_plan`): the dual-feed plan - the measured table-only signature
  (`PC s0/s1` + console `s8`) is safe, two tables are safe, eight constants before a table is refused by name, a
  placement past `s31` is refused as a spill, and an empty/unplaced signature is a safe no-op. The target printed **263
  checks passed** (it also carries other slices' checks).
- `port/test/frame_dump_test.cpp`: the driver-block decision truth table (mesh, no slot 0, no view, stride 0, a view
  shorter than one record, no table pair at `s0`/`s1`, and the taken case with no reason text) beside the existing V#
  encode/decode round trip.
- Gates: `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`, `_setup\build_tests.cmd` -> `TESTS_BUILT`, both test targets pass.
  The documentation checks: links, encoding, table rows, list items and repeated lines pass; two checks fail on **other
  slices' uncommitted work** (`port/src/alloc_trace.h` referenced by `docs/frame_first_light.md` is untracked, and
  `PALMETTO_MEM_TRACE` / `PALMETTO_PIPELINE_LAZY_BUILD` / `PALMETTO_PIPELINE_LIVE_CAP` / `PALMETTO_STATE_TRACE` are read
  by uncommitted code and not yet named in a document). They are not this slice's files.
- Boots: `D:\leonida-work\vfeed\boot-control`, `boot-consoleonly`, `boot-fix`, `boot-fix-early`, `boot-cullnone`
  (each with its stderr/boot trace/captures outside the repository); the boot lock was acquired and released per boot.

## 8. Uncommitted files and attribution

This slice's uncommitted files: `port/shims/d3d12_com.cpp`, `port/src/graphics_pso.h`, `port/src/graphics_pso.cpp`,
`port/src/vulkan_replay.h`, `port/test/frame_dump_test.cpp`, `port/test/graphics_pso_test.cpp`, and this page. The tree
also carries other agents' uncommitted work (measured binaries include it; see the arm metadata); the lead commits
through the normal flow.

The repository's `NOTICE.md` entry for this slice is the lead's to add (the policy's attribution step, and this slice's
scope excludes editing it):

```text
This slice was implemented from this project's own measurements. The behaviour was cross-checked against the
public GPL-3.0 project ubiquitarianism/leonida (https://github.com/ubiquitarianism/leonida), files
runtime/shims/d3d12_xs_pso_stream.cpp, runtime/shims/d3d12_xs_pso_stream.h and runtime/src/root_signature.h,
commit ed94fbef9 (the post-story-runtime merge). No code from that project was copied. The upstream project
remains the copyright of its contributors and its license continues to apply to its own tree.
```
