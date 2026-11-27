# The BVH build route: measured gap and the builder-shaped answer

**Status (2026-11-27): opt-in public-D3D12 route; package-helper bridge remains blocked. The original 130.3 s boot
measured zero build/copy/postbuild/prebuild calls, and the measured `XboxBvhBuilder` runtime sequence still stops at
console list slot 148. This follow-on wires the GPU-free plan into public `ID3D12GraphicsCommandList4` Build/Copy/Emit
methods behind `PALMETTO_BVH_ROUTE=own` (default off), reusing Palmetto's own XDXR builder. It does not hook the
package's `GetProcAddress` exports, bypass slot 148, or implement rehydration; no title boot was run for this follow-on.
The prior readiness notes below are historical where §8 supersedes them. It follows [upstream_port.md](upstream_port.md);
peer material is the repository's own ledger, evidence only.**

## 1. Why this path is needed

The parity slice this page belongs to is rank 6 of [cleanroom_peers.md](cleanroom_peers.md), "the BVH build route and
inline-traversal wiring". Its matrix row records the two halves from the peers' public wording:

* XCL: "BLAS / TLAS built on the GPU by the game's own XboxBvhBuilder; RDNA2 BVH node format; BVH intersection
  instructions emulated in shaders" — the game's builder DLL runs on the PC and the traversal is inline.
* five-star: the port "must put structures in that layout at the console GPU addresses the game hands its shaders",
  building them from the DLL's own prebuild sizes on the CPU.

Our tree already has the inline half: the translator's `image_bvh_intersect_ray` / `image_bvh64_intersect_ray` follow
RDNA2 box and triangle rules measured on native hardware (716 sites, [design.md](design.md) "Ray tracing"), and the
layer has an XDXR BVH4 builder of its own ([bvh.md](bvh.md) "Building runtime BVHs", `port/src/xdxr_build.h`). What
the row still names missing is the route between the title's build descriptions and those structures. This page
measures that gap first, then closes the part that can be closed in our structure without touching another slice's
files.

## 2. Measured shapes

### 2.1 What the boot requests

One 130 s boot of the standard environment (`PALMETTO_D3D12=vulkan`, the translated shader corpus, the boot lock),
counted from the D3D12 trace and the startup trace. Counts only; no game data.

| measurement | value |
|---|---|
| verdict | ALIVE 130.3 s (killed by the 130 s timeout), 0 exceptions |
| traced D3D12 calls | 250,021 (max call #252,634) |
| `BuildRaytracingAccelerationStructure` / `CopyRaytracingAccelerationStructure` / postbuild calls | **0** |
| `GetRaytracingAccelerationStructurePrebuildInfo` calls | **0** |
| `XboxBvhBuilder.dll` mapped by the loader | **0** |
| `CreateComputePipelineState` calls (the builder's constructor makes 53 on its own) | 2, neither a builder program |
| slot-57 pipeline records converted | 10,241 of 10,241, 0 refused |
| compute calls (`SetPipelineState`/`Dispatch` family) | 479, 406 created |
| submits / presents | 10,317 / 5,158 |

The title is still stage-gated before the pass that would build or trace against BVH data, so the boot's contribution
to this slice is a measured zero: the shape the route must serve is not exercised by the title at this tree.

### 2.2 What the tree already implements

| piece | state | evidence |
|---|---|---|
| XDXR build descriptions as the DLL reads them | decoded and checked against `d3d12.h` | [xdxr.md](xdxr.md), `port/src/xdxr_descs.h`, `port/test/xdxr_descs_test.cpp` |
| GPU BVH4 builder (root, nodes, leaves, instances, refit, BOX16, TRI1 pairing) | implemented and validated against brute force | [bvh.md](bvh.md), `port/src/xdxr_build.h`, `port/src/xdxr_build.comp` |
| `BuildRaytracingAccelerationStructure` on the Vulkan device (D3D12 API form) | implemented, records XDXR structures into the title's arena VAs | [d3d12_vulkan.md](d3d12_vulkan.md) section 4.8 |
| `GetRaytracingAccelerationStructurePrebuildInfo` | answers from `XdxrLayout`, exactly the build's size | same page; `port/shims/d3d12_vulkan_device.cpp` |
| builder DLL loaded and constructed on our stack | 53 of 53 slot-62 compute pipelines, 24 object bytes written, no fault | `port/test/bvh_builder_test.cpp`, measured again below |
| builder's translated programs in the shader library | 51 of 52 translate and pass `spirv-val` | [embedded_shaders.md](embedded_shaders.md) |
| Xbox batched/streaming build entry points in the layer | **absent** | [d3d12_vulkan.md](d3d12_vulkan.md) section 4.8 |

### 2.3 The package helper's build route, measured on our stack

The title's own image carries a dynamic-import table naming the package exports it resolves (mangled names beside
`XboxBvhBuilder.dll`): the constructor, `BuildRaytracingAccelerationStructureBatched2X`,
`GetRaytracingAccelerationStructurePrebuildInfo`, `CopyRaytracingAccelerationStructure`,
`RehydrateRaytracingAccelerationStructureBatched2X` and `Set64BitMortonCodes`. `bvh_builder_test` gained a
`PALMETTO_BVH_BUILD=1` arm that plays that run-time sequence with synthetic geometry. Measured on the Vulkan device
(RTX 4090, driver 610.88, 2026-11-25):

| step | result |
|---|---|
| constructor | 53 of 53 compute pipelines created from the translated `<shaders>/bvh` index; object body 24 bytes; 33.7 s; no fault |
| prebuild (4 synthetic triangles, 12 vertices, no index buffer) | result 1,024 bytes, scratch 17,152 bytes, update scratch 0; the DLL computes its own console layout, not our `XdxrLayout` |
| build, first attempt (upload-heap scratch and destination) | refused **inside the helper**: "D3D12 XDXR Error: DestAccelerationStructureData can not have more than 40b of address set", then `E_INVALIDARG` as a C++ exception |
| build, second attempt (default-heap arena destination and scratch, 256-byte aligned, VA below 2^40) | the helper asks console list slot 148 for 40 bytes at 16-byte alignment: `AllocateUploadMemory stub called`; its own message is "Failed to allocate upload buffer"; `E_FAIL` (`0x80004005`) |
| dispatches recorded / destination written in that attempt | 0 of the build's list slots (14, 25, 33, 35, 45, 146); 0 of 1,024 destination bytes |
| `CpuValidateResultX` on a failed build | not usable as an oracle here: its measured shape needs the pipeline state a successful build leaves behind |

Two measured facts come out of this that the route has to respect:

1. **The 40-bit destination rule is live, not just static.** The docs already record the static check
   ([d3d12_vulkan.md](d3d12_vulkan.md), "the BVH builder only takes destination addresses below 2^40"); the arm
   reproduced the throw with an upload-heap buffer, whose console VA is its device address minus the arena base and
   can be at or above 2^40, and passed the same rule with a default-heap arena range. Every buffer a console client
   hands the builder must be an arena range.
2. **The build path needs a console-only list method the proxy does not serve.** Slot 148 is not in the public GCL8
   chain; the builder allocates a small upload block through it before its first barrier. The list proxy answers it
   with a named stub, so the helper throws before recording a single dispatch. That is the measured blocker on the
   package's own route, and it sits in another slice's file.

### 2.4 The route's shapes, from our own measurement

The route that this slice implements answers the same two exports from the layer's own numbers:

* **prebuild** takes the console `Inputs` (public layout: type, flags, count, layout, descriptor pointer) and an
  optional `D3D12XBOX_XDXR_STREAM_DESC`; its sizes are `XdxrLayout::compute(elements, top_level, box16)`, the same
  numbers the device's `GetRaytracingAccelerationStructurePrebuildInfo` already returns. `box16` is the layer's
  MINIMIZE_MEMORY rule for bottom-level builds; a top-level build keeps BOX32.
* **build** takes one `BATCHED_DESC2` where the first 0x30 bytes are the public build description and the rest is the
  postbuild array and the stream desc ([xdxr.md](xdxr.md) section 1). Each element becomes one of the layer's build
  records: destination, scratch, source, top/bottom level, update, build flags, the triangle geometries (console VAs),
  or the instance list, and the postbuild descriptions the layer writes.

## 3. What we implemented

Two additive headers, no wiring:

* `port/src/xdxr_layout.h`: `XdxrLayout` moved out of `port/src/xdxr_build.h` unchanged. It is the layout
  arithmetic, and it was only unreachable from a GPU-free test because it lived beside the Vulkan builder. The
  builder, this route and the device's prebuild now share one definition. `port/src/xdxr_build.h` includes it; no
  behaviour changes.
* `port/src/xdxr_route.h`: the builder-shaped route. Plain types only (no Vulkan, no D3D12), so the shapes are
  testable offline.

The route's API and what each name means:

| name | meaning |
|---|---|
| `route::prebuild(inputs, stream, armed)` | the prebuild answer: result, scratch and update-scratch sizes from `XdxrLayout` |
| `route::plan_batched(descs, count, armed)` | one batched build's records: one `BuildRecord` per element, plus the per-element stream notes |
| `route::plan_single(desc, postbuild_count, postbuilds, armed)` | the non-batched export, which the shipped one wraps into a `BATCHED_DESC2` on the stack |
| `route::route_armed()` | the switch: `PALMETTO_BVH_ROUTE=own` arms the route; unset or any other value leaves it off |
| `route::refusal_name(refusal)` | the named refusal a caller logs; nothing is guessed |

Every answer is the layer's own measured shape, and every unsupported shape is refused by name (`Refusal`, one enum
value per reason). The refusals that matter here:

| refusal | why |
|---|---|
| `disabled` | the switch is off; the caller keeps the package helper's answer |
| `input_type`, `descs_layout`, `unknown_flag` | the measured valid values (types 0/1, layouts 0/1, public flag bits `0x3f`) |
| `geometry_type`, `vertex_format`, `index_format`, `triangle_count`, `geometry_pointer`, `pointers_unreadable` | the GPU builder reads f32 positions and none/R16/R32 indices; a pointer list is resolved on the CPU at record time, and a null entry is refused rather than dereferenced |
| `alignment`, `address` | the measured 256-byte alignment and the measured below-2^40 destination rule |
| `update_flags` | `PERFORM_UPDATE` needs `ALLOW_UPDATE`; an in-place update is the destination itself |
| `offline_statistics` | postbuild type `0x10`: the shipped builder throws `E_INVALIDARG` for every build ([xdxr.md](xdxr.md) section 4) |
| `serialization` | postbuild type 2: the layer refuses serialization at replay instead of faking it ([bvh.md](bvh.md)) |
| `counts`, `too_many_elements` | a count the layout cannot hold, and a bounded batch |

Types 0 and 3 postbuild descriptions are passed through (the layer writes compacted and current size); type 1 and any
other value are counted in `ignored_postbuilds`, which is what the shipped builder does with them. The stream
description changes no size in this layout: the three measured records are read (first record of each type, the walk
stops at an unknown type, exactly as the DLL reads them) and reported as `StreamNotes`, including whether type 0's
`+0x08` value selects the shipped builder's alternate bottom-level path — which this layout does not reproduce and
does not need to, because the route answers both the prebuild and the build from one layout.

### 3.1 What remains unwired: the package-helper export path

The public D3D12 caller is wired in §8, but no caller reaches `xdxr_route.h` through the title's package-helper exports.
The title's builder exports are resolved from the mapped image in `port/src/pe_loader.cpp`; substituting the route there
would require an export-resolution hook when the switch is on. The other blocker — console list slot 148 — is in
`port/shims/d3d12_xs_device.cpp`. Neither package-helper blocker is bypassed here. The public switch still defaults off,
so the original command-list branches remain the fallback.

## 4. Cross-check against the reference and the peers

This slice did not open a peer clone. The cross-check uses this repository's own peer ledger
([cleanroom_peers.md](cleanroom_peers.md), the ray-tracing rows); no peer file, table or code was copied.

* **Agreement on the mechanism.** XCL runs the package's builder (its constructor builds the compute pipelines,
  builds go through the batched export, sizes from its own prebuild). Our measurement agrees with every piece of that
  sentence on our stack: the constructor creates 53 compute pipelines, the batched export is the one the title's
  image names, and the prebuild answer comes from the helper. The builder's programs resolve from the shader library
  (51 of 52 translated, [embedded_shaders.md](embedded_shaders.md)).
* **Agreement on the destination rule.** The static 40-bit check was already recorded in
  [d3d12_vulkan.md](d3d12_vulkan.md); the arm reproduced it live and confirms the practical consequence the docs
  state: a buffer a console client hands the builder must be an arena range.
* **Difference on the missing link.** five-star builds and rehydrates on the CPU from the game's own vertex data and
  names the console layout; our route uses this tree's own GPU builder and its own `XdxrLayout`, because that is the
  layout the layer's `BuildRaytracingAccelerationStructure` already writes and the layout
  [bvh.md](bvh.md) validates against brute force. The two are compatible at the traversal level (both produce node
  records the game's shaders read), not at the byte level: the console builder's internal layout is not public, and
  this slice does not claim to reproduce it.
* **Difference on rehydration.** The peers' rehydrate path is out of this slice. This route has no rehydrate export:
  the `Rehydrate*` family is on the package DLL and would need the offline stream's decoded form first; the slice
  records it as a limit rather than a stand-in.

## 5. Historical A/B measurement (readiness-only revision)

This A/B (2026-11-25) predates §8's public COM wiring: two boots of the same standard environment and switch set, one
before the readiness planner and one after (the tree contained other slices' in-flight work in both; the boot lock was
held and one `palmetto` ran at a time). At that time the route was only a header and `XdxrLayout` moved without changing
a number. This remains the pre-integration baseline; it does not measure route-on title behavior.

| measurement | before readiness planner | after planner, before §8 wiring |
|---|---|---|
| verdict | ALIVE 130.3 s, 0 exceptions | ALIVE 130.4 s, 0 exceptions |
| traced D3D12 calls | 250,021 | 162,307 |
| pipeline timeline at +120 s: slot-57 records / graphics created / compute calls | 10,241 converted 0 refused / 51,786 / 474 (403 created) | 10,241 converted 0 refused / 51,786 / 474 (403 created) |
| AS build / prebuild / copy calls | 0 / 0 / 0 | 0 / 0 / 0 |
| `XboxBvhBuilder.dll` mapped | no | no |
| submits / presents | 10,317 / 5,158 | 5,801 / 2,900 |

**Historical verdict for the readiness-only revision: unchanged in every counter it touched, latent negative in the boot.**
The recording counters that identify the tree's stage are identical at the same timeline mark. The frame counters are
about half in the second boot; in that revision the route was an unused header and added no runtime call. The only
shared-hardware variable was other slices' GPU tests, so the difference is recorded as run-to-run load, not claimed as
an effect of this follow-on. The title still asked for no ray-tracing work at that stage, which is the measured negative
[cleanroom_peers.md](cleanroom_peers.md) predicted. The package helper's build path was measured to a concrete blocker;
§8 records the later public-COM integration and its synthetic tests.

## 6. Limits and the next falsifier

1. **Package-helper export handoff remains not wired.** The public D3D12 COM caller is now wired under §8, but no caller reaches that adapter through the package helper's export. Next falsifier: a boot on which the
   title's builder export is actually called. Wiring it means one of two measured steps — hook the loader's export
   resolution for `XboxBvhBuilder.dll` when `PALMETTO_BVH_ROUTE=own` (`port/src/pe_loader.cpp`), or serve console list
   slot 148 so the package helper can proceed (`port/shims/d3d12_xs_device.cpp`). Either one is another slice.
2. **The package helper's own route is blocked before a dispatch.** Slot 148's shape is measured (40 bytes, 16-byte
   alignment, then "Failed to allocate upload buffer"); what the console returns and what the helper writes into it is
   not. Whoever serves it must read the helper's use of the block rather than return a fixed buffer.
3. **The package helper's copy and rehydrate exports are not in this route.** `CopyRaytracingAccelerationStructure` maps onto the layer's
   existing copy record under §8; the `Rehydrate*` family needs the offline stream's decoded form.
4. **The stream description's semantics are read, not applied.** The measured alternate bottom-level path (type 0,
   `+0x08` above 10.0, with `MINIMIZE_MEMORY`) and the type 1 level record are reported in `StreamNotes` and change no
   size, because the route answers both prebuild and build from `XdxrLayout`. If a future measurement shows the title
   passes a non-empty stream, the notes are where the difference is visible.
5. **The public COM records now reach Palmetto's builder under §8; the package helper still does not.** The GPU half
   already has its own tests (`vk_xdxr_build_test`, `d3d12_xdxr_rt_test`); the route test in §8 now joins them for the public D3D12 API form.

## 7. Tests

* `xdxr_descs_test` (`port/test/xdxr_descs_test.cpp`, target `xdxr_descs`, CPU-only): the new route section covers the
  prebuild sizes against `XdxrLayout` for a BLAS with and without an index buffer, the BOX16 difference, a top-level
  build, the measured refusals, the two-element batch plan (bottom level with a postbuild array, top level with an
  ARRAY_OF_POINTERS instance list), the update rules, the alignment/2^40 rules, the offline-statistics and
  serialization refusals, a geometry pointer list with and without a null entry, the vertex-format refusal, the
  non-batched form, the stream notes (including the alternate bottom-level path), and the environment switch. Synthetic
  descriptions only. Those 43 checks are the original readiness-plan coverage; §8 records the added command-route bridge checks.
* `bvh_builder_test` (`port/test/bvh_builder_test.cpp`, target `bvh_builder_test`, package-gated, exits 77 without
  `PALMETTO_GAME_DIR`): the `PALMETTO_BVH_BUILD=1` arm above. It checks the prebuild answer and the destination rules,
  reports the build's measured blocker, and passes when the helper refuses before writing the destination. Nothing
  from the package is written to disk. Its arm adds 14 checks.
* The layout move is checked by every target that already used `XdxrLayout` (`vk_xdxr_build_test`,
  `d3d12_xdxr_rt_test`, the device prebuild) and by the full `palmetto` build.

## 8. Public D3D12 COM route follow-on (2026-11-27)

### Peer comparison and measured delta

The peer claims below are the public descriptions summarized in rank 6 of [cleanroom_peers.md](cleanroom_peers.md), not
source-level parity claims. The local baseline is the 130.3 s trace in §2: zero standard Build/Copy/postbuild calls,
zero prebuild calls, and no title request that reaches traversal. The helper arm in §3 independently stops at slot 148.

| Peer mechanism | Local comparator and this delta | Remaining limit |
|---|---|---|
| XCL: the game's `XboxBvhBuilder` GPU build plus inline shader traversal | Palmetto already has its own GPU XDXR BVH4 builder and `image_bvh_intersect_ray` / `image_bvh64_intersect_ray` lowering. This follow-on routes public D3D12 command-list records into that builder when explicitly armed. | It does not execute XCL's builder or hook the game's helper exports; no title boot reaches the RT pass in the measured trace. |
| Five Star: CPU-built XDXR structures placed at the console GPU addresses calculated by the helper's prebuild | Palmetto's own `XdxrLayout` and GPU builder write the same XDXR record family at supplied arena addresses. The adapter is not a CPU port of Five Star's builder and does not resolve its prebuild exports. | Helper prebuild/build/rehydrate exports remain out of this route; no package stream is consumed. |

### Wiring, switch, and refusals

[the route adapter](../port/src/bvh_route.h) adapts a validated `route::BuildDesc` plan into this project's existing
`BuildAccelerationStructure` and postbuild command-list records; it is a translation layer, not a second builder. The
public D3D12 geometry unions are copied field-by-field into the measured planner shape, including pointer-layout arrays;
no public geometry byte-layout is assumed. `port/shims/d3d12_com.cpp` consults this adapter in the existing Build,
Copy and Emit command-list methods only when `PALMETTO_BVH_ROUTE=own`. The environment switch is absent/off by
default: the original branches remain the fallback and the synthetic GPU test starts with them before arming the route
for update/postbuild/copy. No same-input byte-for-byte A/B dump was run; the off path itself is retained rather than
rewritten. The new branch also verifies that the attached device has XDXR acceleration structures enabled and returns
a named backend refusal on the KHR-AS configuration.

The planner bounds geometry and postbuild walks at `0x10000` entries, rejects strides wider than the command IR can
represent, and keeps the measured destination alignment/address guard (`256` bytes and below `2^40`). The adapter
supports triangle geometry in the two measured vertex formats (`R32G32` and `R32G32B32` float), no/R16/R32 indices,
TLAS instance arrays and pointer arrays, and the existing build-flag mapping. The Copy branch accepts Clone/Compact
only with aligned, guarded source/destination addresses; Emit accepts compacted/current-size postbuild types. Bundles,
unsupported geometry/formats/flags, invalid counts, serialization/offline-statistics requests and KHR-AS are named
refusals, not a speculative fallback. Existing device prebuild answers still come from `XdxrLayout`; this follow-on
does not add a second prebuild implementation.

The descriptor/heap and shader translator paths were not modified. The adapter carries the requested console VAs into
the existing builder; it does not allocate/rewrite descriptor heaps, root arguments, T#/S# descriptors or translated
shader pointers. Consequently, the synthetic build/readback test below is not an end-to-end descriptor-driven shader
trace. The original boot still measured zero RT requests, and no route-on title boot was run; this work does not claim
the game now reaches BVH traversal.

### Tests and single synthetic GPU arm

* `_setup\\build_gta6rt.cmd`: passed `PALMETTO_OK` after the shared xmem link fix. `d3d12_com.cpp` and the adapter
  compiled in the Palmetto target.
* `xdxr_descs_test.exe`: passed (`xdxr_descs: ok`, checked against `d3d12.h`). The added bridge covers BLAS flag/
  format/instance mapping, postbuild records and ignored types, opt-out, backend/copy/emit refusals, count bounds and
  over-wide stride refusal.
* CTest `d3d12_xdxr_rt` and `d3d12_xdxr_rt_check`: passed 2/2 on an NVIDIA GeForce RTX 4090. The log confirms
  `PALMETTO_BVH_ROUTE=own` was armed for the update/postbuild/copy segment. The checker reported zero validation errors
  and 0/1000 differing rays for BLAS, TLAS, update, compact, BOX16, BOX16 update and paired BLAS; this validates the
  written CPU-readable XDXR structures against the independent ray checker, not a game shader dispatch. No device loss.
* `python -I tooling/xdxr_bvh.py selftest`: four synthetic random/grid BOX32/BOX16 cases, 0 validation errors and
  0/400 ray differences for each.
* Python shader/decode units: `test_image_bvh*_hw.py` 5/5; `test_image_bvh_census.py` 15/15;
  `test_rdna2_decode.py` 16 passed, 1 skipped because `llvm-mc` is unavailable.
* Focused docs checks passed: links 7/7, repo paths 3/3, table rows 2/2, encoding 1/1, repeated lines 2/2,
  repeated list items 3/3 and table IDs 2/2. The full docs suite was not green in the same run: its global CLI-flag and
  environment-variable checks reported unrelated entries in `_setup/xcl/HANDOFF.md` and other in-flight slices; two
  synthetic Git tests initially inherited malformed `GIT_CONFIG_*` and passed on the focused rerun after those
  process-local variables were removed.
* The descriptor, console-heap and `SetDescriptorHeaps` C++ targets were built with the isolated target build but were
  not run in this pass. No descriptor-specific integration claim is made.
* `_setup\\build_tests.cmd` was attempted but stopped in `port/test/graphics_pso_test.cpp` with syntax diagnostics
  around lines 409–428 during another in-flight slice. Its owner later repaired that test; the shared integration rerun
  is pending, so this run does not claim `TESTS_BUILT`.

One synthetic GPU route test was the only light arm in this follow-on. No game data or package bytes were read, written,
or used; no title boot was run.

### Next falsifier

Resolve the actual Xbox helper import/export path and measure the bytes/semantics of its slot-148 upload allocation
before serving it; then exercise `BuildRaytracingAccelerationStructureBatched2X`, helper Copy and Rehydrate with synthetic
geometry/stream data. Do not claim helper-export parity until those calls reach the layer and the output is checked in the
title's shader-visible address/descriptor path.

## 9. Attribution

This follow-on compared its own measured route with the public claims summarized in rank 6 of
[cleanroom_peers.md](cleanroom_peers.md): **XCL** (<https://github.com/xcl-dev/xcl>, GPL-2.0-only) and **Five Star
Recomp** (<https://github.com/ThatRecompDev/five-star-recomp>, GPL-3.0-or-later). For this campaign the reference
snapshots are XCL `623d02e` (README, design/wave64/shader-container documentation) and Five Star `ce2b333b` (README,
D3D12.X/memory/storage/ray-tracing/audio documentation and loader-shim headers); the ledger's older audit snapshots
are not the read target for this follow-on. No peer code, comment text, identifier or table was copied. The
implementation comes from this project's own measured D3D12/XDXR descriptions, command-list IR and GPU builder. The
NOTICE attribution row belongs to the lead's commit, as [upstream_port.md](upstream_port.md) section 3 requires.

## 10. Helper-export interception addendum (2026-11-27)

This addendum supersedes the earlier statement in §3.1 that no helper-export interception exists. It does **not** claim
package-output parity or title-route readiness. This turn used only repository source and the recorded measurements
cited below; no game data or package bytes were opened, read or executed, and no title boot was run.

### 10.1 Measured export and resolver contract

The title's bundled `XboxBvhBuilder.dll` exports are reached in two ways: static module-export resolution in
`Loader::resolve` and dynamic `GetProcAddress` through `Loader::resolve_module_export` (the latter is called by the
library-loader shim). Before this change both returned the mapped `Module::exports` address unchanged. The new bridge
matches the exact helper module basename and measured export method prefix/ordinal only; it does not rewrite the PE
export table or mapped bytes.

The existing evidence in [xdxr.md](xdxr.md) and
[d3d12_command_objects.md](d3d12_command_objects.md) establishes these signatures and shapes:

| export | measured contract | route result with `PALMETTO_BVH_ROUTE=own` |
|---|---|---|
| prebuild (ordinal 2) | public `INPUTS` plus 16-byte `XDXR_STREAM_DESC`; exports return `void`; the helper builds a `BATCHED_DESC2` | answer from this tree's `XdxrLayout` |
| standard build (ordinal 3) | GCL4, public build desc, postbuild count and pointer; wrapper builds one batched record | delegate to public COM build after route preflight |
| batched build (ordinal 4) | GCL6, count and `0x50`-byte `BATCHED_DESC2` array; 256-byte `Dest` alignment and postbuild loop | preflight the whole batch, unwrap the Xbox list proxy, then record each build through GCL4 |
| copy (ordinal 5) | GCL4, destination/source VAs and copy mode; the route supports clone/compact only | delegate to the public COM copy path after named validation |
| rehydrate batched2 (ordinal 8) | GCL8, count, public desc array and a trailing `UINT64*` | named refusal: trailing array contract is unmeasured |
| rehydrate batched (ordinal 9) | GCL8, count and public desc array | named refusal: the helper's CPU-side dehydrated-stream reconstruction is not wired to the GPU XDXR builder |
| rehydrate device (ordinal 10) | Device11 and one public desc; the recorded CPU form reads source/geometry/destination/scratch as host pointers | named refusal: no measured destination extent or hidden GPU-submission contract for this route |

`bvh.md` §4a records byte-exact CPU reconstruction of the offline BVH format for eight measured BLAS, plus checks on
the archive stream packing. That data-format evidence is distinct from a safe runtime adapter for all three exported
rehydrate signatures: the batched2 trailing array and the device-only call's destination/submission ownership are not
implemented here. Refusal thunks return `E_NOTIMPL` and never fall through to the package implementation while the
switch is armed.

The package build's measured console-only slot 148 request (40 bytes, 16-byte alignment) remains the reason not to
call the package build routine: its returned CPU pointer is passed unchanged as a GPU root address. The new build
adapter avoids that call by unwrapping the console command-list proxy with the existing proxy helper and invoking the
public GCL4 route. It uses this tree's XDXR layout/builder, not the helper's private compute pipelines or its internal
BVH byte order. Constructor, `CpuValidateResultX`, `Set64BitMortonCodes`, other unrecognized exports and the package's
own helper state are not intercepted; this slice makes no claims about them.

### 10.2 Switch-off contract and focused coverage

`PALMETTO_BVH_ROUTE` remains opt-in and defaults off. Both static and dynamic export resolution pass the original
mapped function pointer through the selector. With the switch off, or for a different module/unknown symbol, the
selector returns that exact pointer; no PE bytes or export entries are patched. With the switch on, measured
prebuild/build/batched-build/copy symbols resolve to route thunks and the three rehydrate symbols resolve to named
refusal thunks. The route classifier and pointer selector have CPU tests in `xdxr_descs_test`.

`d3d12_xdxr_rt_test` now adds synthetic standard build, public-GCL6 batched-build and helper Copy calls through the
export bridge, compares helper prebuild sizes against the direct public answer, and writes each structure for
`xdxr_bvh.py check-build` ray checks. It also calls the three rehydrate refusal thunks and expects `E_NOTIMPL`. It uses
only synthetic geometry. The test calls the C bridge directly with public lists; it does not yet
exercise actual PE-loader resolution or a console command-list proxy. These new tests were **not built or run in this
slice**: the lead requested one serialized full gate after all slices report. Schedule the existing `d3d12_xdxr_rt` and
`d3d12_xdxr_rt_check` tests in that gate window, including the synthetic GPU cases; `TESTS_BUILT` and `PALMETTO_OK`
remain pending. No title boot was run.

### 10.3 Limits and next falsifier

This change adds a source-level hook at the dynamic/static resolver callsites, but tests the C bridge directly—not the
`Loader` or library-loader execution path, and not a console command-list proxy. It is not a live title import or a
shader-visible title frame. The route's public prebuild uses `XdxrLayout`; it does not reproduce helper-specific stream
alternate layouts, exact decompressed offline BVH topology, or package CPU write timing. The original helper constructor
and non-routed exports remain in place. No claim of helper-export parity, title-ready BVH writes or rehydrate support is
made until the deferred checks pass.

**Next falsifier:** in the lead's serialized gate, build/run the synthetic GPU route and verify that the helper-resolved
batched build and helper Copy pass the existing brute-force ray checker, while the off-mode pointer identity and
rehydrate named-refusal tests pass. Add or schedule a bounded synthetic check that enters through `Loader::resolve_module_export`
and supplies the Xbox command-list proxy, proving that `command_list_proxy_real_list` unwraps to the public GCL4 before
Build/Copy. A later, separately authorized test should supply a synthetic dehydrated stream to each rehydrate entrypoint
and check output bytes/addresses and command-list completion timing; any output mismatch, non-null batched2 side data
that cannot be decoded, or device-only destination-size ambiguity keeps that export refused. Only after those checks
should a boot test capture the title's actual dynamic import resolution and the shader-visible destination path.

### 10.4 Attribution pointer for the lead's NOTICE row

The code is this project's own implementation, derived from its local measured XDXR/COM contracts; no peer source,
comments, identifiers or data tables were used. The slice evidence was cross-checked against rank 6 of
[cleanroom_peers.md](cleanroom_peers.md): XCL (`623d02e`), Five Star Recomp (`34b4dfed`), VIctory (`63bcca4`),
PercyScarlett (`e44c465`) and the victory-fork snapshot (`9a9091d`). The NOTICE row belongs to the lead's commit, as
[upstream_port.md](upstream_port.md) section 3 requires.
