# The mesh and merged-geometry stage path: measured record shapes and the stream description

**Status: creation side landed, GPU-free; the runtime path is not wired and the programs are not published under the
key the record path resolves. Readiness, not a picture change.** The measured need is real: 12,062 of the library's
25,471 graphics records (47.4%) carry a mesh (MS) stage, and the translator has recompiled all 9,282 mesh programs and
all 195 merged-geometry (GS) programs. The measured gap is on two other ends: no MS/GS program is published under the
program key the slot-57 record path resolves with (0 of 7,498 library MS programs, 0 of 195 library-adjacent GS
programs), and the boot requests no mesh record at all (10,241 of 10,241 slot-57 calls are the vertex family). This
page records the measurement, the clean-room conversion this tree now owns, its refusals, the boot, and the next
falsifier. It follows [upstream_port.md](upstream_port.md); peer material is evidence only.

## 1. Why this path is needed

The peers reached the world with these stages and say so in public:

* victory-fork's README: world drawn and presented with **mesh pipelines**; of its 24,387 programs "all but under 300"
  recompile, and the named residual is **"Merged geometry stages (195 programs) and under 100 others"**
  (GPL-3.0-or-later, clone `9c184b3`).
* PercyScarlett's README: the world uses mesh pipelines, world shaders "mesh 8,191 of 8,506", residual "merged
  geometry stages" (GPL-2.0-only, clone `e44c465`).
* XCL's design spec: console program kinds include stage 7 = MS/NGG and stage 2/3 = ES/GS; its wave64-on-wave32 page
  implements "mesh (NGG), ES+GS" (GPL-2.0-only, clone `6d12f58`).
* The parity matrix row in [cleanroom_peers.md](cleanroom_peers.md) records the same claim and our state as "the
  mesh/merged-stage path is not carried".

Our own measurement says the need is a fifth of the graphics pipeline library and a tenth of the recompiled corpus
(section 2), while today's boot requests none of it: the world pass that would use mesh pipelines is stage-gated
([frame_capture.md](frame_capture.md) sections 6-9). The truthful classification of this slice is therefore **readiness
with a measured negative in the boot**, and the two remaining gaps are named in section 6.

## 2. Measured shapes

All counts below are from our own extracted pipeline library (the graphics records and programs `tooling/shader_library.py`
reads from the title's `shaders` container, kept in the work folder outside the repository) and from the translated
corpus index. No program bytes, paths, hashes or extracted tables are quoted here.

### 2.1 Record stage sets (25,471 graphics records)

The record names its stages by slot only (0 PS, 1 VS, 2 MS); slots 3 to 5 are never used and slot 6 appears in 2
records (section 2.4). The state record's size family follows the stage set on every record (13,409 vertex-family,
12,062 mesh-family, [state_blob.md](state_blob.md)):

| stage set | records | share | state family | measured on |
|---|---:|---:|---|---|
| PS + VS | 13,383 | 52.5% | vertex (380 + 8*k) | every record cross-checked |
| VS only | 24 | 0.1% | vertex | every record cross-checked |
| PS + MS | 10,726 | 42.1% | mesh (464 + 8*k) | every record cross-checked |
| MS only | 1,336 | 5.2% | mesh | every record cross-checked |
| PS + VS + slot 6 | 2 | 0.01% | vertex | 2 records, one program, no stage blob |

No record names a geometry (GS) slot: the console pipeline library has no such slot. The 195 translated geometry
programs are merged ES/GS shader containers, not library pipeline records (section 2.4).

### 2.2 Library programs by slot

| slot | programs | published under the resolver's program key |
|---|---:|---:|
| PS | 9,996 | 947 |
| VS | 1,343 | 319 |
| MS | **7,498** | **0** |
| slot 6 | 1 | 0 |
| CS (separate table) | 2,684 | 630 (the FidelityFX set) |
| **total** | **21,522** | 1,896 |

"Published under the resolver's program key" means an index entry with `fspx_code_sha256`, which is what
`rt::graphics_pso::resolve_native_stage()` looks a slot-57 record's native program up by
([graphics_pso.md](graphics_pso.md) section 2). The 1,266 library graphics modules are PS 947 + VS 319; **no mesh
library program and no geometry program has an entry of this kind**, so a mesh record would fail program resolution
before the host ever sees the stage.

### 2.3 Translated corpus by stage (26,501 entries)

| stage | entries | translator coverage |
|---|---:|---|
| PS | 12,100 | recompiled, `spirv-val`-clean at the corpus check |
| MS | 9,282 | recompiled (wave64 NGG, `design.md` "Mesh shaders") |
| VS | 2,235 | recompiled |
| CS | 2,689 | recompiled |
| GS | **195** | recompiled; all 195 are the merged ES/GS `$GS_VoxelizeGeneric` permutations, wave64 ([design.md](design.md), "Geometry shaders") |

All 26,501 entries are keyed by the carved source container's `fssm_sha256`, the FSSM/DXBC path's key, not by the
program key the slot-57 record path uses. The 195 geometry programs are exactly the peer READMEs' "merged geometry
stages (195 programs)" residual: our translator already covers that residual; what is missing is a record shape that
could ever request it and a publication under the program key.

### 2.4 The two slot-6 records

Two of the 25,471 graphics records name a third pair in slot 6: one program (`lib_slot6_21469`) and **no stage blob**,
beside PS + VS. The library's own program table gives that program no source container, and no measurement identifies
its stage. [shader_library.md](shader_library.md) section 2 calls slot 6 "used once and not identified" (two records
reference the same program). This slice accepts that exact measured shape in the record decoder - so the two records
are not mistaken for unknown ones - and refuses it by name in the converter rather than dropping a program the record
names (section 3).

### 2.5 What the host can already do

The host half is measured and in place, which is why this slice is a conversion, not a backend port:

* `CreatePipelineState` parses a `D3D12_PIPELINE_STATE_STREAM_DESC` in the d3dx12.h layout and builds an MS (+ PS)
  pipeline through `VK_EXT_mesh_shader`; without the extension it reports `D3D12_MESH_SHADER_TIER_NOT_SUPPORTED` and
  refuses a mesh PSO with `E_NOTIMPL` (`port/test/d3d12_mesh_test.cpp`, `docs/d3d12_vulkan.md`).
* The replay records a mesh draw as `Draw` with `mesh` set and issues `vkCmdDrawMeshTasksEXT`; a mesh pipeline and a
  vertex draw are mismatched at execution (`port/src/vulkan_replay.h`).
* A geometry shader is the translator's Vulkan geometry stage via `CreateGraphicsPipelineState`'s `GS` field and the
  `geometryShader` feature (`port/test/d3d12_geometry_test.cpp`).
* The host enforces the mesh rules: no VS/GS/HS/DS beside MS, the SPIR-V output arrays inside the device's mesh
  limits (`port/src/vulkan_mesh_limits.h`), and no input-assembly state (D3D12 ignores it for a mesh PSO).

### 2.6 Current runtime behaviour at the measured boot

| measurement | value |
|---|---|
| verdict | ALIVE 130.3 s (killed by the 130 s timeout), 0 exceptions |
| traced D3D12 calls | 239,617 (max call #241,702) |
| slot-57 calls / distinct records | 10,241 / 10,241 |
| records built at the 10,000-call checkpoint | decoded 10,000, decode-refused 0, built 10,000, PC `S_OK` 10,000, refused before the call 0 |
| stage shapes at that checkpoint | ps+vs 9,996, vs 4; **no ms and no gs shape** |
| state records by family at that checkpoint | vertex k=0 103, k=1 3,631, k=2 5,978, k=3 288; **no mesh-family row (0)** |
| mesh stage refused by the converter | 0 |
| frame records | `SetPipelineState` 9,906 / `DrawInstanced` 9,906 / `ExecuteCommandLists` 9,909 / `PresentX` 4,954 |
| `DispatchMesh` / mesh indirect execution | 0; the console `DISPATCH_MESH` command signature was created once as a named stand-in and never executed (`ExecuteIndirect` with it is refused by name, [command_signature.md](command_signature.md)) |
| captures | none taken by this boot (no capture switch); picture is the known stage-gated frame ([frame_capture.md](frame_capture.md) sections 6-9) |

So the whole measured gap in the boot is a zero: no slot-57 record carries a mesh stage yet. The conversion this
slice adds is exercised by the tests and synthetic records, not by the title (section 5). The frame counters above are
this boot's own; the tree has moved since the 150 s baseline on [slot57_wiring.md](slot57_wiring.md) section 2 (that
boot recorded 42,721 `SetPipelineState` / 42,722 `DrawInstanced`, and this tree's slot-55 derived-pipeline path is
active), so the frame numbers are not comparable with that page. The half this slice touches - the slot-57 conversion
and its histogram - is identical: 10,241 records built, 0 refusals, no mesh family either way.

## 3. What we implemented

Two files, additive, no wiring:

### 3.1 The record decoder: shapes and the measured slot-6 exception

`port/src/graphics_record.{h,cpp}`:

* `RecordShape` and `record_shape()` / `record_shape_name()` name the measured stage sets (`ps+vs`, `vs`, `ps+ms`,
  `ms`, `ps+vs+slot6`), and `has_mesh_stage()` exposes the slot-2 test the converter needs.
* `decode()` now accepts the one measured non-zero region after the three slot pairs: the slot-6 program pair
  (0xE8 pointer / 0xF0 size, in the measured program size range, no stage blob at 0xF8..0x108, beside a VS slot and
  no MS stage). It keeps the program as `Record::unidentified_program`. Every other non-zero byte in the region is
  still refused exactly as before, so a record shape no measurement shows is not silently ignored.
* The vertex and mesh paths are unchanged: `decode()` still refuses a VS+MS pair, a partial pair, an unknown slot, and
  a state family that disagrees with the slots. The default path is byte-identical for every record the earlier boots
  carried.

### 3.2 The converter: the pipeline-state-stream description

`port/src/graphics_pso.{h,cpp}` adds `rt::graphics_pso::build_mesh_stream()`. A `D3D12_GRAPHICS_PIPELINE_STATE_DESC`
has no mesh-shader field, so a record whose slot 2 is used is described through `D3D12_PIPELINE_STATE_STREAM_DESC`:

```
ROOT_SIGNATURE, MS, PS (when the record has one), RENDER_TARGET_FORMATS, DEPTH_STENCIL_FORMAT,
RASTERIZER, BLEND, DEPTH_STENCIL, SAMPLE_MASK, SAMPLE_DESC, NODE_MASK, FLAGS
```

in the d3dx12.h layout (a type word, the payload at its alignment, each subobject pointer-aligned) - the exact rule
the PC device's parser applies. The stream is owned by `MeshPipelineStream`, whose `desc()` is what the device's
`CreatePipelineState` takes. No VS/GS/HS/DS subobject is emitted (D3D12 forbids them beside MS and the record has no
such slot), and no input-layout, topology or strip-cut subobject is emitted: D3D12 ignores input-assembly state for a
mesh pipeline, and the measured mesh records carry the 80-byte empty layout. A mesh record with an element layout is
therefore refused as an unmeasured shape rather than having its layout silently dropped.

The fixed-function state is `GraphicsPsoInputs::defaults`, written verbatim exactly as `build()` writes it; the state
record's own words stay undecoded ([state_blob.md](state_blob.md)). The converter is GPU-free: it creates no device
object, calls no PC method, and the tests walk its bytes themselves.

Named refusals (each names its field; `out` untouched on failure):

| refusal starts with | meaning |
|---|---|
| `record.stages: the record names no mesh stage` | the record is a vertex-family shape; `build()` owns it |
| `record.stages: a vertex and a mesh stage together` | not a stage combination D3D12 accepts |
| `record.stages: the record names the unidentified slot-6 program` | the 2 measured slot-6 records: accepted by `decode()`, refused by name here, never dropped |
| `record.state: ...` | the state blob does not decode, or is not the measured mesh family for a record with an MS stage |
| `record.input_layout: ...` | the layout does not decode, or a mesh record carries elements (unmeasured) |
| `record.root_signature` / `pRootSignature` / `stages[i] (stage)` | missing console/PC root signature, a stage without resolved bytes, bytes for a stage the record does not name, vertex bytes for a mesh pipeline |
| `render_target_count` / `rtv_formats[i]` / `defaults.*` | the same input contract `build()` enforces |

`MeshPipelineStream::describe()` and `MeshStageCounters::summary()` are the bounded one-line summaries a slot-57
wiring logs (160 and 120 characters at most, tested): records with a mesh stage, streams built, refusals.

### 3.3 What is not wired, and why

The proxy's slot-57 branch (`port/shims/d3d12_xs_device.cpp`) still answers a mesh record with the converter's refusal
as `E_NOTIMPL` ([slot57_wiring.md](slot57_wiring.md) section 1.4). Wiring it means: on a refusal whose text names the
mesh stage, call `build_mesh_stream()`, then the PC device's `CreatePipelineState` with the stream and the title's
`riid`/`out`, and count with `MeshStageCounters`. That file is another slice's while this was written; the split is
recorded here rather than smuggled through a shared file.

When the device cannot run the stage (no `VK_EXT_mesh_shader`) the device refuses the stream with `E_NOTIMPL` and its
own named log. There is deliberately **no VS/PS stand-in for a mesh record**: a stand-in would silently drop the
geometry the record asked for, and a silent drop is exactly what this tree refuses. The diagnostic stand-in switch
(`PALMETTO_STANDIN_PIPELINES`) keeps its existing meaning for records the converter refuses.

## 4. Cross-check against the reference and the peers

The policy's cross-check section, with our measurement first and the reference second.

* **The record shape is ours.** The console pipeline library's 25,471 records and the 2 slot-6 records are measured
  from our own extraction; the peer READMEs never state a record stage-set census.
* **The host mapping agrees.** The public reference's D3D-29 section ("pipeline state streams, mesh PSOs and
  `DispatchMesh`", `docs/d3d12_vulkan.md`, clone commit `154e4ec03`) describes the same contract this tree already
  carries: the d3dx12.h stream layout, MS + PS on the mesh stage with no input assembly, `VK_EXT_mesh_shader`, mesh
  output limits checked at creation, `DispatchMesh` recorded as a mesh draw and replayed with
  `vkCmdDrawMeshTasksEXT`, and an MS stream refused when it also carries VS/GS. Agreement, not a difference: the host
  half needed no new code for this slice.
* **The reference's stream path is a different caller.** The reference's `runtime/shims/d3d12_xs_pso_stream.{h,cpp}`
  (read at clone commit `154e4ec03`, introduced by `db19bbd34`/`273b3bdf9`) carries slot 86 `CreatePipelineState`
  and slot 55 derived pipeline states over streams, including GS and MS subobjects. That is the console's own stream
  call, not the pipeline-library record: our title uses the pipeline library record (slot 57), so this tree's
  equivalent source of a mesh pipeline is the record conversion added here. The mechanism is the same; the entry
  point differs.
* **Difference 1, program identity.** The reference's mesh PSO resolves the `ms` index entry by FSSM hash; our
  library graphics programs are native RDNA2 kind-6 programs and resolve by FSPX program SHA-256 (or the raw program
  key), which is what the measured slot-57 records carry ([graphics_pso.md](graphics_pso.md) section 2). Our
  measurement wins; a port of the reference's lookup would miss every library graphics program.
* **Difference 2, a citation to correct.** The parity row names the reference's `runtime/shims/d3d12_geometry_views.h`
  as a source for this stage path. Read at clone commit `ba626820e`, that file is the title's **geometry buffer
  views** (vertex/index data copies behind the draw), not a shader-stage path; it is the vertex-data half of the
  reference's page. The merged-geometry residual is the 195 GS programs' translation and a record shape, and our
  translator already covers it (section 2.3). Recorded as a disagreement, per [upstream_port.md](upstream_port.md)
  section 2.
* **The peers' residual is our coverage.** The peer READMEs count 195 merged-geometry programs as *not recompiled*;
  our corpus index recompiles all 195. The remaining distance is a runtime shape and a publication key, both named in
  section 6.

No peer or reference code, comment text, identifier or table was copied into this tree; the counts above are our own
measurements, the peer claims are quoted from their public READMEs, and the prose in this page is ours.

## 5. Tests and the boot

GPU-free tests, no device and no game files:

| target | before | after | what the new checks cover |
|---|---:|---:|---|
| `graphics_record_test` | 158 | **180** | the five measured shapes classify and are named; PS+MS and MS-only decode; the slot-6 record is accepted with its opaque program span and every other reserved-byte shape is still refused (a slot-6 blob, a half-set pair, a size outside the program range, no VS slot, beside an MS stage) |
| `graphics_pso_test` | 272 | **312** | `build_mesh_stream()`: the stream is walked with the PC device's own layout rule; twelve subobjects and their payloads (root signature, MS and PS bytecode, render-target formats, DSV format, the caller's fixed-function state verbatim, node 0, no flags); no VS/GS/HS/DS and no input-assembly subobject; `describe()` bounded; the counters and their summary; both measured mesh shapes; and every refusal by name |

Commands: `cmake --build port/build --target graphics_record_test graphics_pso_test` then run both; `ctest --test-dir
port/build -R "graphics_(record|pso)"`. The gates were `PALMETTO_OK`, `TESTS_BUILT`, the two tests above,
`xs_proxy_mock_test` and `unimplemented_policy_test` (98 checks).

**Boot: the measured negative.** One 130 s boot with the standard environment: the shader corpus on the work folder,
`PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, no other switch, the retail image,
one boot at a time under `_setup/boot_lock.ps1`, harness `tooling/boot_report.py`. The result is in section 2.6: **10,241
slot-57 records, all built, 0 refusals, no mesh-family state record and no ms/gs stage shape, 0 `DispatchMesh`**, the
`DISPATCH_MESH` signature created once as a stand-in and never executed, 4,954 presents and no capture. The population
and captures are the existing stage-gated frame and this slice adds no pixels; the slot-57 conversion and its histogram
are unchanged from the baseline (10,241 built / 0 refused / no mesh family), which is the A/B result the planning page
asked for: **unchanged**. That is the falsifier [cleanroom_peers.md](cleanroom_peers.md) slice 7 named - no boot record
carries a mesh stage, so the slice is readiness only and the runtime path stays unwired. The boot's slot-67
`E_INVALIDARG` refusals (61 calls, not on the slot-57 path) and its slot-55 derived-pipeline activity are the shared
tree's live state from other slices; this slice changes neither.

**Verdict: unchanged (readiness).**

## 6. Limits and the next falsifier

* **0 of 7,498 library MS programs and 0 of the 195 GS programs are published under the resolver's program key.** A
  mesh record would build its stream description and then fail its stage resolution in the device
  (`no translated ms for FSPX ...`), because the program-keyed publication holds only PS 947 + VS 319 (+ the 630
  FidelityFX compute entries). The next slice must extract the library's MS programs (`tooling/shader_library.py`
  already takes a `--stages` list with `ms`) and translate/publish them keyed by `fspx_code_sha256`, exactly as the
  PS/VS library programs are. Only then can a boot's mesh record reach a created pipeline.
* **No slot-57 wiring yet.** The proxy answers `E_NOTIMPL` for a mesh record; the stream builder is GPU-free and
  tested but not called by the title. The next slice wires it and adopts `MeshStageCounters` in the boot histogram.
* **The mesh draw path is a second, separate gap.** The console `DISPATCH_MESH` command signature is a named
  stand-in and `ExecuteIndirect` with it is refused by name; there is no `IndirectKind::dispatch_mesh`, no
  `vkCmdDrawMeshTasksIndirectEXT`, and no recorded mesh indirect work ([command_signature.md](command_signature.md)).
  That half lives in the command-list, replay and proxy files, not here.
* **Merged geometry has no record shape.** The 195 GS programs are translated, but the console pipeline library never
  names a geometry slot (section 2.1), so no runtime path can request them. If a future measured record ever resolves
  a program that is published only for stage `gs`, `resolve_native_stage()` already names it (`stage mismatch: ...
  published for stage gs`) instead of choosing a module. The remaining "merged geometry" work is a record-shape
  measurement, not a mapping to invent.
* **GS on a host without `geometryShader`** would be refused by the device with its own log; no compute-emulation
  stand-in is attempted here.
* **No picture claim.** The world pass that would use these stages is stage-gated; every frame number above is the
  existing frame loop, and this slice adds no pixels.

## 7. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0-or-later project ubiquitarianism/leonida (<https://github.com/ubiquitarianism/leonida>), files
`runtime/shims/d3d12_xs_pso_stream.h`, `runtime/shims/d3d12_xs_pso_stream.cpp`, `runtime/shims/d3d12_geometry_views.h`
and `docs/d3d12_vulkan.md`, commits `db19bbd34` (Rain156), `273b3bdf9`, `ba626820e` (Moriz82) and `154e4ec03` (Jan
Trencansky). The peer claims quoted in section 1 are from the public READMEs of victory-fork (GPL-3.0-or-later, clone
`9c184b3`), PercyScarlett (GPL-2.0-only, clone `e44c465`) and XCL (GPL-2.0-only, clone `6d12f58`), read as evidence at
the commits [cleanroom_peers.md](cleanroom_peers.md) records. No code from any of those projects was copied. The
upstream projects remain the copyright of their contributors and their licences continue to apply to their own trees.
The NOTICE.md row for this slice is the lead's to add when the slice is committed, per [upstream_port.md](upstream_port.md)
section 3.
