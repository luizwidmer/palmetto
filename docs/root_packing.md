# Directly indexed heaps and root-signature packing: the measured shapes, the spill model, and the heap-base dword

**Status: measured, implemented, latent.** The root-signature/descriptor path now carries the console's directly
indexed heap dwords (the bound heap's one-dword base after the explicit root arguments), marks a compute signature's
spill behind `s[0:1]` instead of refusing it, and keeps the measured RTS0 flag state in the model. The measured title
does **not** set either directly-indexed flag in this window: the boot's two signatures carry flags `0x00000000` and
`0x00000001`, every draw is fed from a descriptor table or a root view, no dispatch is recorded, and the one created
signature that spills in the console compute layout is never bound. The implementation is therefore readiness with named
refusals; the boot population is unchanged. The RTS0 flag-restore half of the reference mechanism lives in the
serializer wrapper (`port/shims/d3d12_xs*.cpp`), which this slice does not own; it is recorded as a limit below.

This page rests on [root_arguments.md](root_arguments.md) (the measured console layout and the table slot encoding),
[vertex_constant_feed.md](vertex_constant_feed.md) (the boot's two draws and their read sets),
[d3d12_vulkan.md](d3d12_vulkan.md) sections 3.4 and 3.5 (the descriptor model and the root-signature mapping) and
[remaining_mechanisms.md](remaining_mechanisms.md) section 3.6 (the parity rank). No game data, package path, shader
byte or extracted table appears here: counts, flag values, offsets and verdicts only.

## 1. Why this slice

The parity matrix ranks directly indexed heaps and root-signature packing sixth because every peer that reaches the
world carries it: the console driver puts the bound heap base into the user data, the compiled shaders index
`ResourceDescriptorHeap`/`SamplerDescriptorHeap` off it, and a signature past the 32-dword budget spills its trailing
parameters to memory. Our tree tracks the flags only to log "SGPR slot open", carries no heap-base dword, and refuses a
signature the console compute layout would spill. The reference story-load work is the evidence source for the
mechanism (files and commits in section 5, reference only, never copied); the D3D12 flag values and the heap-indexing
model are public (`D3D12_ROOT_SIGNATURE_FLAGS`, `ResourceDescriptorHeap`, [d3d12_vulkan.md](d3d12_vulkan.md) section
3.4).

## 2. Measured shapes (our own title)

The measurement is `PALMETTO_ROOT_PACK_MEASURE=1`, a bounded counter set in `port/shims/d3d12_com.cpp` that prints one
line per distinct root-signature shape a list binds, the draw/dispatch counts by flag state, and a periodic summary.
Boot `boot-measure1` (binary SHA-256 `41F31C38…4288`, the tree before this slice's implementation, 130.4 s, ALIVE, no
exception, 1 capture black) gives:

| measured shape | binds | flags | parameter (console / compute slot) | graphics / compute / console placement |
|---|---|---|---|---|
| root view (`D3D12_ROOT_PARAMETER_TYPE_CBV/SRV/UAV`) | 4,097 | `0x00000000` | 1 root view, PC s0, console s8, compute s4 | placed / placed / placed |
| descriptor table | 4,097 | `0x00000001` (ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) | 1 table, PC s0, console s8, compute s4 | placed / placed / placed |

* 8,194 binds, 8,192 draws, **0 dispatches**; **0 draws or dispatches carry either directly-indexed flag**.
* No signature bound in the window spills.
* One **created** graphics signature does spill in the console compute layout: the model's own creation line reads
  `root signature usable for graphics only: root parameter 8 does not fit in the user SGPRs from s4 (spill array not
  modelled)`. It is the boot's only such line and the signature is never bound at either bind point in the window
  (the boot writes 8,194 binds, all of the two shapes above). That sentence is the direct measurement behind the
  spill half of this slice.
* The two signatures are the frame's own blit draws ([vertex_constant_feed.md](vertex_constant_feed.md) section 2):
  they read descriptor tables, not heap indices, which agrees with the parity page's measured symptom ("no boot has yet
  exercised a directly indexed heap").

What the layer did with each shape before this slice: the two flags were read at bind and a note said the heap base was
"tracked on list (SGPR slot open)" - no user-data dword was written, so a shader that indexed the heap would read
whatever the last recorded argument left at that slot; the console spill was refused by `rt::rootsig::pack_stage`, which
made the one created signature graphics-only; the RTS0 flag dword was parsed into the model but the flags had no
consumer.

## 3. The console rule, and what it places

Two separate mechanisms share the user-data block, and both are properties of the signature, not of the draw:

1. **Directly indexed heaps.** A signature with `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` (`0x100`) and/or
   `SAMPLER_HEAP_DIRECTLY_INDEXED` (`0x200`) gets the base of each bound heap appended to the user data after the
   explicit root arguments, in order: the CBV/SRV/UAV heap's one 32-bit dword, then the sampler heap's. The descriptor
   stride is compiled into the shader; no dword carries it. The public D3D12 model (`ResourceDescriptorHeap[i]` is the
   descriptor at heap base + `i` × the heap increment) and the reference implementation agree on this placement; the
   measured console base in our tree's layout is s8 for vertex/pixel/geometry and s4 for compute
   ([root_arguments.md](root_arguments.md) section 4).
2. **The spill array.** A signature past the 32 user SGPRs moves the parameters that no longer fit, and every later
   parameter, into an array behind `s[0:1]` (the console compiler's `__XB_GetRootSignatureSpillPointer`). The reference
   work packs that array with no alignment padding: one dword per 32-bit constant, two per root CBV/SRV/UAV, one per
   descriptor table. Our tree models that packed shape for compute only (a graphics stage's spill-pointer SGPR is
   unmeasured, so a graphics spill is still refused by name).

The heap-base dword is only meaningful when the explicit arguments are inline: a signature that spills has no measured
"one dword after the arguments", so the model refuses the heap placement for it by name rather than guessing.

## 4. What we implemented

1. **The spill model instead of a refusal** (`port/src/root_signature.{h,cpp}`). `StagePackResult` gains `spill_dword`
   (per parameter, `kNoSpill` when inline), `spill_dwords` and `kNoSpill`; `pack_stage` packs a compute signature's
   trailing parameters contiguously after `end_sgpr` (the console's one-two-one widths, no padding), marks each as
   `first_sgpr == kNoSgpr`, and still refuses a graphics-stage spill by name and an unknown parameter type. The
   `d3d12_com.cpp` bind points fail closed on a spilled layout: the argument setters never write `kNoSgpr + offset`
   (which would wrap into `s0`), and a dispatch that needs the unwritten array is refused by name
   (`Dispatch(compute root signature whose arguments spill behind s[0:1])`) instead of launching with missing
   arguments.
2. **The direct-heap placement as a pure model** (`rt::rootsig::direct_heap_slots`). It takes the signature's flag
   dword and the argument end, and returns the resource and sampler heap dwords: with neither heap flag it is a
   successful no-op; with a flag it refuses by name an unmodelled PC flag bit, an unplaced or spilled layout, a heap
   dword that lands on an occupied user-data dword (the graphics bind point keeps the PC packing in the same block), and
   a slot past s31. The public D3D12 flag values are named constants; the Xbox type byte (`0xFF000000`, measured
   `0x03000000`) is recorded on the result and never selects a packing, and the low-flag state parsed from a blob
   survives the model unchanged (tested).
3. **The feed at the bind points** (`port/shims/d3d12_com.cpp`). On a signature bind the layer derives the argument
   end from the layout it already carries (graphics: the console layout's base and per-parameter slots, s8; compute:
   from s4, and a spilled parameter means no end) and stores the plan. A draw or dispatch then records its user data as
   the recorded arguments **plus** the bound heap bases at the planned slots. Each value is encoded exactly like the
   console descriptor-table slot the frame's modules already understand: the bound heap base is resolved through the
   device's title-VA map to its arena-relative value and must fit one dword (`console_table_value`), otherwise the
   whole feed is refused and named - a partial console ABI is never written. The heaps are sampled per command, so a
   heap rebind cannot change an already recorded draw. A bounded log prints the first successful feed (slots and heap
   bases) and the bounded reasons a feed does not happen. `PALMETTO_ROOT_PACK_MEASURE=1` keeps the shape counters used
   for the measurement in section 2 (off by default; the feed itself needs no switch because it only changes user data
   for a signature that carries the flags).
4. **The measured flag state, restored.** `RootArgumentLayout::flags` already comes from the parsed RTS0 blob rather
   than from any masked descriptor; this slice reads the flag state from there (`layout->flags`) for both the heap
   placement and the measurement, and its test proves an RTS0 payload carrying `0x03000000 | 0x100 | 0x200` keeps every
   bit through the parse and selects the measured packing. The serializer-side restore (a host `d3d12.dll` that refuses
   the heap-directly bits) is not in this slice; see the limits.

## 5. Cross-check against the reference

The reference read for this slice is the public GPL-3.0 project `ubiquitarianism/leonida`, checkout
`D:\leonida-work\leonida-fork` at `ed94fbef9` (the post-story-runtime merge):

* `runtime/src/root_signature.{h,cpp}` - the console stage bases and the compute spill array appended behind
  `s[0:1]` (`spill_dword`/`spill_dwords`, one dword per table, two per root CBV/SRV/UAV, graphics stages still
  refusing); mechanism commits `7c2252609` (D3D-448 console layout, measured for compute), `5fdd6abbc` and
  `b8563e949`/`00fc23b9a` (D3D-448-GRAPHICS: graphics in the console layout, the inline part kept when a signature
  overflows), and `273b3bdf9` (the story-load integration).
* `runtime/shims/d3d12_com.cpp` - the heap dwords appended after the explicit arguments in the order resource then
  sampler, snapshotted per command, with named notes when a heap is unbound or the arguments spill; direct-heap commits
  `5115f0887` (RT-DIRECT-HEAP-COMPUTE) and `3ee0037fd` (RT-DIRECT-HEAP-GRAPHICS).
* `runtime/shims/d3d12_xs_rootsig.{h,cpp}` - the RTS0 flag rewrite that keeps the heap-directly bits on a host that
  refuses to serialize them (commit `a330747f2`).

**Agrees.** The heap dwords follow the explicit arguments, resource heap first, sampled from the bound heaps, and the
spill array's packed shape; both trees refuse a graphics-stage spill. **Differs.** This tree's graphics bind point still
sends the PC packing from s0 (two dwords per table) and carries the console layout beside it, because per-stage user-data
windows for mesh pipelines are not carried yet ([d3d12_vulkan.md](d3d12_vulkan.md) section 3.5, D3D-448); the reference
switched the graphics bind point itself to the console layout (`5fdd6abbc`). So our graphics heap dwords are placed from
the console placement the layout already carries, not from the bind point's packing, and the console feed still needs
`PALMETTO_GRAPHICS_CONSOLE_ROOT` for a translated stage to read the explicit arguments at s8. The RTS0 flag-restore
wrapper is absent here (limit 1 below). No code, comment text, identifier or table was copied from the reference.

## 6. Boot measurement

Both arms: the standard environment (`PALMETTO_SHADERS`, `PALMETTO_D3D12=vulkan`, `PALMETTO_SKIP_INT3=1`,
`PALMETTO_SEED_GLOBAL=75DF268:D8`) plus `PALMETTO_ROOT_PACK_MEASURE=1` and the presenter capture (every 6,000 presented
frames); the boot lock was held; one boot per arm. The boot binaries also carried other slices' work that was
uncommitted during the runs; the lead has since committed it, and this slice's own files are the uncommitted ones now.

| arm | binary | verdict | d3d12 calls | draws | executes | presents | dispatches | captures | shapes / direct-indexed |
|---|---|---|---|---|---|---|---|---|---|
| before (measurement) | `boot-measure1`, SHA-256 `41F31C38…4288` | ALIVE 130.4 s, 0 exceptions | 246,841 | 8,192 | 10,290 | 5,145 | 0 | 1/1 black (2,564x1,421, mean 0 0 0, 0 of 3,643,444 non-black) | 2 shapes, 0 flagged |
| after (implementation) | `boot-final2`, SHA-256 `AFBE78D9…FB26` | ALIVE 130.4 s, 0 exceptions | 221,576 | 8,192 | 8,997 | 4,498 | 0 | 1/1 black (same frame) | 2 shapes, 0 flagged |

The signature and draw population is unchanged: the same two signature shapes, the same 8,194 binds and 8,192 draws per
130 s. The present/execute counts vary run to run on the shared boot slot (an earlier run of this implementation
measured 4,978 presents and 9,957 executes) and none of the variation is attributable to this slice: the feed is not
entered without the flags, and the spill model's only visible effect is the create-time line below. That create-time
line - `root signature usable for graphics only: root parameter 8 does not fit in the user SGPRs from s4 (spill array
not modelled)` in the before arm - is gone in the after arm, because the model now places that signature's inline part
and marks the spill; the signature is still never bound. The presented capture is black in both arms, as expected at
this stage (the frame's black is the content path, [colour_path.md](colour_path.md)); this slice is not a picture fix
and does not claim one.

## 7. Tests and gates

* `port/test/root_signature_test.cpp`: **81 checks pass, 0 failures**. Nine are the spill model's checks: the
  measured created-signature shape (`27 constants + root view`, inline part to s31, two spilled dwords), the packed
  spill widths for a mixed signature, the graphics-stage refusal by name, the 64-dword cost bound, and the unknown
  parameter type. Fifteen cover the direct heaps: the two measured frame shapes (table at s8 -> heap base at s9; root
  view at s8 -> resource at s10 and sampler at s11), the FidelityFX compute shape (arguments end s14 -> heaps at
  s14/s15), the no-flag no-op with the Xbox type bits recorded, the flag state through an RTS0 parse (heap bits and
  Xbox high byte), and the named refusals (unmodelled PC bit, unplaced layout, spilled layout, no room at s31, no room
  past s31, a computed layout with a spill, a heap dword on a PC-packed dword, and the clear case above it).
* `d3d12_root_args_validation_test` (the command-list root-argument parity test, which links the changed
  `palmetto_d3d12_com`): 29 parity cases against live WARP and an NVIDIA RTX 4090, 0 validation errors, 0 failures - the
  bind-point changes above do not move the measured D3D12 parity behaviour.
* `graphics_pso_test` (the graphics-PSO/pipeline-stream target the mesh slice owns): all 312 checks pass on the current
  tree, so the `pack_stage` change does not disturb the console placement or stream builder it covers.
* Gates: `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`; `_setup\build_tests.cmd` -> `TESTS_BUILT`;
  `root_signature_test` passes; the docs checks pass (33 passed, 192 subtests; this page names
  `PALMETTO_ROOT_PACK_MEASURE`, the only new variable).
* Boots: `D:\leonida-work\rootpack\boot-measure1`, `boot-final2` (an intermediate `boot-final1` measured the same
  shapes), each with its stderr/boot trace/capture outside the repository; the boot lock was acquired and released per
  boot.

## 8. Limits and next falsifiers

1. **The RTS0 flag-restore half is not carried.** The serializer wrapper (`port/shims/d3d12_xs.cpp`,
   `port/shims/d3d12_xs_rootsig.*`) forwards a description with the Xbox high byte cleared and does not retry without
   the heap-directly bits, so a host `d3d12.dll` that refuses `0x100`/`0x200` (measured on Windows 10) would fail the
   serialization before the signature reaches this layer, and a description's flag state would be lost. Those files are
   outside this slice's scope; the measured boot never exercises the case (no signature carries the flags, and the
   serializer log shows no refusal). Named next slice: add the retry-and-restore there, with synthetic serialize tests.
2. **The heap feed is latent.** No measured title signature sets either heap flag, so no boot exercises the dword; the
   placement is a model with unit tests, not a measured title behaviour. The falsifier that would close it: a content-pass
   boot whose signature carries `0x100`/`0x200` and whose shader indexes the heap. What would confirm it: the bounded
   "direct heaps" line naming the slot, plus the drawing pass reading a descriptor through the heap index.
3. **The heap base's encoding is our table encoding.** A console descriptor table slot is a one-dword arena-relative VA
   pinned from the frame's modules ([root_arguments.md](root_arguments.md) section 5b); the heap base is fed the same
   way, because the shader forms `heap_base + the slot`. The console driver's own heap-base value was not measured
   against a translated shader, so a heap whose GPU base the shader treats differently (an offset form) would be a fresh
   measurement.
4. **Mesh heap offset not fed.** The s16 mesh base is not placed by the graphics feed path ([vertex_constant_feed.md](vertex_constant_feed.md)
   section 6), and the reference leaves the mesh heap offset open too. A mesh pipeline with a direct-heap signature
   refuses the feed by name.
5. **A spilled dispatch is refused, not served.** The `s[0:1]` array is not written (its pointer is not modelled), so a
   compute dispatch whose signature spills is refused by name rather than launched with missing arguments. Serving it
   needs the spill pointer's SGPR measured.
6. **Graphics still uses the PC packing by default.** The feed places heap dwords from the console placement carried
   beside the PC packing; a translated stage reads its explicit arguments at the console slots only with
   `PALMETTO_GRAPHICS_CONSOLE_ROOT=1`, whose measured A/B is in [vertex_constant_feed.md](vertex_constant_feed.md)
   section 5.

## 9. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project ubiquitarianism/leonida (<https://github.com/ubiquitarianism/leonida>), files
`runtime/src/root_signature.cpp`, `runtime/src/root_signature.h`, `runtime/shims/d3d12_com.cpp`,
`runtime/shims/d3d12_xs_rootsig.cpp` and `runtime/shims/d3d12_xs_rootsig.h`, checkout `ed94fbef9`, with mechanism
commits `7c2252609`, `5fdd6abbc`, `b8563e949`, `00fc23b9a`, `273b3bdf9`, `5115f0887`, `3ee0037fd` and `a330747f2`.
No code from that project was copied. The upstream project remains the copyright of its contributors and its license
continues to apply to its own tree. `NOTICE.md` is the lead's to update; this page carries the same attribution.
