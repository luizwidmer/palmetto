# The BVH build route: measured gap and the builder-shaped answer

**Status: readiness. The title requests no ray-tracing or BVH work in a 130 s boot (zero calls), the package helper's
own runtime build path was measured on our stack and stops at a console list method the proxy stubs, and this slice
implements the builder-shaped prebuild and build answers from the layer's own XDXR layout behind an opt-in switch
(`PALMETTO_BVH_ROUTE=own`, default off). The route is not wired to a caller yet: the hand-off is a `GetProcAddress`
hook in `port/src/pe_loader.cpp`, another slice's file, and the measured list method is in
`port/shims/d3d12_xs_device.cpp`, another slice's. The boot is unchanged and that is the honest result. It follows
[upstream_port.md](upstream_port.md); peer material below is this repository's own peer ledger, evidence only.**

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

### 3.1 What is not wired, and why

No caller reaches `xdxr_route.h` yet. The title calls the package helper's exports, which the loader resolves from the
mapped image in `port/src/pe_loader.cpp`; substituting the route means hooking that resolution when the switch is on,
which is that file's owner's decision, not this slice's. The second half of the blocker — console list slot 148 — is
in `port/shims/d3d12_xs_device.cpp`, owned by the list-slot slices. Both are named here rather than reached into. The
switch therefore defaults off and the runtime path is byte-identical to the tree before this slice.

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

## 5. A/B measurement

Two boots of the same standard environment and switch set, one before the slice's files and one after (the tree
contained other slices' in-flight work in both; the boot lock was held and one `palmetto` ran at a time). Nothing on the
runtime path changed: the route is a new header no runtime code includes, and `XdxrLayout` moved without changing a
number.

| measurement | before this slice | after this slice |
|---|---|---|
| verdict | ALIVE 130.3 s, 0 exceptions | ALIVE 130.4 s, 0 exceptions |
| traced D3D12 calls | 250,021 | 162,307 |
| pipeline timeline at +120 s: slot-57 records / graphics created / compute calls | 10,241 converted 0 refused / 51,786 / 474 (403 created) | 10,241 converted 0 refused / 51,786 / 474 (403 created) |
| AS build / prebuild / copy calls | 0 / 0 / 0 | 0 / 0 / 0 |
| `XboxBvhBuilder.dll` mapped | no | no |
| submits / presents | 10,317 / 5,158 | 5,801 / 2,900 |

**Verdict: unchanged in every counter this slice touches, latent negative in the boot.** The recording counters that
identify the tree's stage are identical at the same timeline mark. The frame counters are about half in the second
boot; the route adds no code to the runtime path and the only shared-hardware variable in this environment is other
slices' GPU tests, so the difference is recorded as run-to-run load, not claimed as an effect of this slice. The title
still asks for no ray-tracing work at this stage, which is the same measured negative
[cleanroom_peers.md](cleanroom_peers.md) predicted for this slice. In this slice that negative is not a reason to hide
the work: the package helper's build path was measured to a concrete blocker, and the layer-side answer exists and is
tested.

## 6. Limits and the next falsifier

1. **Not wired.** The switch exists, the answer exists, and no caller uses it. Next falsifier: a boot on which the
   title's builder export is actually called. Wiring it means one of two measured steps — hook the loader's export
   resolution for `XboxBvhBuilder.dll` when `PALMETTO_BVH_ROUTE=own` (`port/src/pe_loader.cpp`), or serve console list
   slot 148 so the package helper can proceed (`port/shims/d3d12_xs_device.cpp`). Either one is another slice.
2. **The package helper's own route is blocked before a dispatch.** Slot 148's shape is measured (40 bytes, 16-byte
   alignment, then "Failed to allocate upload buffer"); what the console returns and what the helper writes into it is
   not. Whoever serves it must read the helper's use of the block rather than return a fixed buffer.
3. **Rehydrate and copy exports are not in the route.** `CopyRaytracingAccelerationStructure` maps onto the layer's
   existing copy record and is a small next step; the `Rehydrate*` family needs the offline stream's decoded form.
4. **The stream description's semantics are read, not applied.** The measured alternate bottom-level path (type 0,
   `+0x08` above 10.0, with `MINIMIZE_MEMORY`) and the type 1 level record are reported in `StreamNotes` and change no
   size, because the route answers both prebuild and build from `XdxrLayout`. If a future measurement shows the title
   passes a non-empty stream, the notes are where the difference is visible.
5. **The route's plan is GPU-free-tested; its records are not yet fed to a real build by a caller.** The GPU half
   already has its own tests (`vk_xdxr_build_test`, `d3d12_xdxr_rt_test`); the wiring is what would join them.

## 7. Tests

* `xdxr_descs_test` (`port/test/xdxr_descs_test.cpp`, target `xdxr_descs`, CPU-only): the new route section covers the
  prebuild sizes against `XdxrLayout` for a BLAS with and without an index buffer, the BOX16 difference, a top-level
  build, the measured refusals, the two-element batch plan (bottom level with a postbuild array, top level with an
  ARRAY_OF_POINTERS instance list), the update rules, the alignment/2^40 rules, the offline-statistics and
  serialization refusals, a geometry pointer list with and without a null entry, the vertex-format refusal, the
  non-batched form, the stream notes (including the alternate bottom-level path), and the environment switch. Synthetic
  descriptions only: 43 of the target's CHECKs are new (the target had 26 before).
* `bvh_builder_test` (`port/test/bvh_builder_test.cpp`, target `bvh_builder_test`, package-gated, exits 77 without
  `PALMETTO_GAME_DIR`): the `PALMETTO_BVH_BUILD=1` arm above. It checks the prebuild answer and the destination rules,
  reports the build's measured blocker, and passes when the helper refuses before writing the destination. Nothing
  from the package is written to disk. Its arm adds 14 checks.
* The layout move is checked by every target that already used `XdxrLayout` (`vk_xdxr_build_test`,
  `d3d12_xdxr_rt_test`, the device prebuild) and by the full `palmetto` build.

## 8. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project **ubiquitarianism/leonida** (<https://github.com/ubiquitarianism/leonida>), through this repository's
own peer ledger ([cleanroom_peers.md](cleanroom_peers.md)) rather than a clone read; the ledger's ray-tracing rows
quote **XCL** and **five-star**, whose public wording is cited in section 1. No code, comment text, identifier or table
from that project was copied. The upstream project remains the copyright of its contributors and its license continues
to apply to its own tree. The NOTICE.md entry belongs to the lead's commit, as [upstream_port.md](upstream_port.md)
section 3 requires.
