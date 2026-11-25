# The slot-57 fixed-function state record: container decoded, fields refused

**Status: the container is decoded and measured on the title's whole library; the behavioural probe contract is in
place; no fixed-function field of the blob is pinned, so the documented stand-in stays and this page records a
documented negative.** The 380..520-byte blob the slot-57 record carries at `+0x08` is the console driver's
serialized graphics-pipeline state record. Its container - two size families, a stage block per family, a shared
block, and a variable tail whose size step is an item count - decodes exactly on all **5,680 distinct state records**
the library holds (referenced by **25,471** graphics records). No word of it is pinned to a D3D12 rasterizer, blend,
depth-stencil, topology, strip-cut or sample field by any measurement of this title's data, so
`rt::graphics_record::decode_state` keeps every word raw, `rt::graphics_pso::decode_fixed_function_state` refuses all
eight fields by name, and the slot-57 wiring keeps the explicit, logged `documented_default_state()` stand-in
(docs/graphics_pso.md section 4). What a decode would need is listed in section 8; section 9 adds the behavioural
probe a pin has to survive and measures what it still cannot decide.

The blobs were read from the reassembled pipeline library the shader toolchain already produces
(`tooling/shader_library.py --save`), outside the repository: 25,471 graphics records each name a state blob from the
library's blob table, and those 25,471 references hold 5,680 distinct blobs. Nothing here is game data; the tables
carry sizes, offsets, counts and decoded structural values only.

## 1. The container, measured

All fourteen size shapes the 5,680 distinct blobs take, and how many blobs each holds:

| size (B) | family | variable item count k | blobs |
|---:|---|---:|---:|
| 380 | vertex | 0 | 211 |
| 388 | vertex | 1 | 1,366 |
| 396 | vertex | 2 | 725 |
| 404 | vertex | 3 | 155 |
| 412 | vertex | 4 | 49 |
| 420 | vertex | 5 | 21 |
| 464 | mesh | 0 | 293 |
| 472 | mesh | 1 | 313 |
| 480 | mesh | 2 | 599 |
| 488 | mesh | 3 | 727 |
| 496 | mesh | 4 | 618 |
| 504 | mesh | 5 | 497 |
| 512 | mesh | 6 | 57 |
| 520 | mesh | 7 | 49 |

The layout every blob fills, exactly:

```
+0x00  40-byte header                      (words below)
+0x28  stage block:  76 B (vertex family) or 160 B (mesh family)
       shared block: 168 B, byte-identical in both families after the +84-byte stage-block difference
       variable region: 28-byte header, then a (68 + 8*k)-byte payload
size = 380 + 8*k   (vertex family, k = 0..5)
size = 464 + 8*k   (mesh family,   k = 0..7)
```

The decomposition is not fitted: the mesh family is the vertex family with 84 extra bytes between `+0x28` and the
shared block (re-aligning the two dumps at `+0x74` vs `+0xC8` makes the 168-byte shared block agree word for word),
and the variable region's own offset is what the pair at `+0x28` encodes.

The header words (every one checked on all 5,680 blobs; a deviation refuses the record):

| offset | measured value | meaning |
|---:|---|---|
| `+0x00` | `0x4005D800` with only bits 3 and 4 free | a measured container word; the flags are **not decoded** |
| `+0x04` | `4` (vertex family) / `6` (mesh family) | the stage family's word; the same value `pso_cache.md` reports as the stage's shared chunk-record reference |
| `+0x08` | `0x00100060` | a measured constant; not decoded |
| `+0x0C` | `(k << 1)` with low nibble `2*k`, plus one of three flag sets `0`, `0x20040`, `0x400C0` | the variable item count and its flags; the flags are **not decoded** |
| `+0x10` | `0xB50516A0` | a measured constant; not decoded |
| `+0x28` | `(payload bytes << 16) \| variable offset`, payload = `size - offset - 28`, offset `0x11C` (vertex) / `0x170` (mesh) | fixes the variable region and fills the blob exactly |

The size family is a cross-check on the record itself, and the library satisfies it on every record: a record with a
VS slot carries a vertex-family state record (13,383 PS+VS + 24 VS-only + 2 PS+VS+slot-6), a record with an MS slot
carries a mesh-family one (10,726 PS+MS + 1,336 MS-only), and no record names the other family. `decode()` refuses a
state size that is in the envelope but in neither family (the measured gap is 421..463 bytes) or that disagrees with
the record's stages.

What the variable items and the three flag sets mean is **not measured**: the item count is decoded (it decides the
size), the items themselves are kept raw.

## 2. Decoded vs refused

| field | status | evidence |
|---|---|---|
| state-record size, family, stage block, shared block | decoded | all 5,680 blobs; the stage family agrees with the record's stage slots on all 25,471 records |
| variable region offset, byte count and payload bytes | decoded | the `+0x28` pair holds on all 5,680 blobs |
| variable item count `k` | decoded | the `+0x0C` low nibble is `2*k` on all 5,680 blobs; k = 0..5 / 0..7 |
| container header constants and flag bits | decoded as words; flags **not decoded** | `+0x00/+0x04/+0x08/+0x0C/+0x10` hold on all 5,680 blobs |
| variable-tail items | **unknown, kept raw** | no measurement pins an item's meaning |
| rasterizer (fill, cull, front face, bias, clip, multisample, line AA, forced samples, conservative) | **refused by name** | no word is pinned |
| blend (alpha-to-coverage, independent blend, 8 targets) | **refused by name** | no word is pinned |
| depth-stencil (depth test/write/func, stencil masks/ops/faces) | **refused by name** | no word is pinned |
| topology type, index-buffer strip cut | **refused by name** | no word is pinned |
| sample count, quality, mask | **refused by name** | no word is pinned |
| attachment count, RTV formats, DSV format | **refused by name** | no word is pinned; the runtime's bound views are measured null (docs/display_path.md), so there is no second source yet |
| behavioural probe (section 9) | **built; the falsifier for a pin** | 7 of the 8 field groups are distinguishable by a scene (the test measures each change); sample quality is not observable in principle. The record supplies no observation, so the probe cannot lift any refusal on its own |

`rt::graphics_pso::decode_fixed_function_state()` therefore returns false for every measured state record, with a
refusal that names all eight fields and the histogram key `state.fixed_function`; a record whose container did not
decode refuses as `state` instead, because nothing was decoded. `out` is untouched on refusal, so a caller must pass
an explicit state (the wiring passes the documented default set).

## 3. The register hypothesis: tested, not adopted

The words are consistent with AMD GFX10 hardware state rather than D3D12 desc fields in struct order: `0x00CC0010`
(the GFX10 `CB_COLOR_CONTROL` reset value), `0xFFFFFFFF` and eight-word runs whose values are shaped like
`CB_BLEND0..7_CONTROL` all appear at fixed offsets, and several words decode into valid GFX10 enum fields. Two things
stop that from being a decode:

1. **No offset-to-register map.** A check of every word's values against the public GFX10 register/field definitions
   (Mesa's `gfx103.json`, 402 register types) shows thousands of register types accept the values of almost every
   offset, and no word is valid for exactly one register. A clean-looking match is therefore a hypothesis about the
   driver's state order, not a measurement of this title's data, and the lead confirmed no external layout note for
   this serialized blob exists (unlike the `0x41` chunk record, which `pso_cache.md` maps from one).
2. **The D3D12 block is upstream of the driver.** Even a correct GFX10 register reading would be a hardware state,
   not the D3D12 rasterizer blend/depth values the PC description takes; the mapping D3D12 enum -> GFX10 field would
   itself be an inference, and a wrong PSO is exactly what this project refuses to emit.

So the words stay raw. This is the honest negative: the container is proven, the field values are not.

## 4. Exact-fill rule (what `decode_state` refuses)

`decode_state()` returns false, and the caller refuses the record, for: a size outside the 380..520-byte envelope; a
size that is not a whole number of 32-bit words; a size in the 421..463 gap between the two families; a size off its
family's 8-byte step, or with an item count above 5 (vertex) / 7 (mesh); a `+0x28` pair that does not encode
`(payload << 16) | offset` for the blob's own size and family; `+0x00` outside `0x4005D800`'s measured flag bits;
`+0x04` not the family's stage word; `+0x08` not `0x00100060`; `+0x0C` whose low nibble is not `2*k` or whose flags
are not one of the three measured sets; `+0x10` not `0xB50516A0`. `decode()` additionally refuses a state size family
that disagrees with the record's slot set (a VS record with a mesh-family state or the reverse), which is the
record-level exact-fill check.

## 5. Boot histogram over the whole library

Counting with the decoder's rules, over the 5,680 distinct state records and the 25,471 records that reference them:

| | count |
|---|---:|
| distinct state records | 5,680 |
| state records whose container decodes | **5,680 (100%)** |
| state records the container rule refuses | **0** |
| vertex-family / mesh-family records | 2,527 / 3,153 |
| graphics records referencing a state | 25,471 |
| ...whose state size family matches their stage slots | **25,471 (100%)** |
| ...whose state size is in neither family | 0 |
| fixed-function fields pinned | **0 of 8** |
| state records with a fixed-function field yielded | **0 (0%)** |

That is the wiring decision in one line: the container decodes on every blob the boot can carry, and **every** call
will count one `state.fixed_function` row (all eight fields refused), so the attachments and fixed-function stand-ins
stay and the boot numbers do not move. A boot whose records carry a container outside the rule reports that record as
`record.state` (`decode_state` refuses it; `build()` already did) instead of guessing.

**Measured boot after the probe pass** (one 150 s boot with the probe layer compiled into the runtime; 0 exceptions
in the startup trace, the run killed by the timeout as usual). The slot-57 histogram measured, at its 10,000-call
checkpoint:

```
[rt] d3d12_xs: slot 57 histogram #5 at 10000 call(s), 10000 distinct record(s): decoded 10000, decode-refused 0,
     built 10000, PSOs created 10000, refused before the call 0
[rt] d3d12_xs: slot 57   state records by family/item count: vertex k=0 103, vertex k=1 3631, vertex k=2 5978,
     vertex k=3 288; containers decoded 10000, refused 0; fixed-function fields pinned 0
     (state.fixed_function rows 10000)
```

and the same run recorded 568,548 D3D12 calls: 27,220 `DrawInstanced`, 27,222 `ExecuteCommandLists` (0 failed),
27,848 `Close` (0 failed) and 13,611 `PresentX` (all ok), with no device-removed/device-lost line. The probe layer
changes no runtime behaviour - no field is decoded, so the stand-in is still exactly what every pipeline carries -
and the presented frame is still black for the frame-content reasons (docs/frame_capture.md); this boot makes no claim
about pixels.

## 6. The wiring (applied; measured by the boot in section 5)

The patch that produced this wiring is kept in the work folder outside the repository
(`d3d12_xs_device_stateblob.diff`); it touches the slot-57 branch only and does not add a refusal path:

1. after the readability checks in `convert_slot57_record`, decode the state record's container, remember its
   `"<family> k=<n>"` shape, and call `decode_fixed_function_state()`;
2. pass the measured state to `GraphicsPsoInputs::defaults` **when it is yielded** (`call.state_fields_decoded`,
   today never) and `documented_default_state()` otherwise - the fallback is the explicit stand-in, logged once with
   the attachment stand-in;
3. count per call in `note_slot57`: containers decoded/refused, the shape histogram, `state_fields_pinned`, and the
   refusal field rows (`state.fixed_function`), with a first-occurrence log line naming every refused field;
4. print the state histogram in the periodic summary (shapes, decoded/refused, fields pinned, `state.fixed_function`
   rows) and replace the `fixed-function state stand-in documented_default_state()` gate text with the measured
   sentence.

The shim is wired this way today (`port/shims/d3d12_xs_device.cpp`, `convert_slot57_record`), which is what the
section 5 boot measured: the first-occurrence log line names every refused field under `state.fixed_function`, the
periodic summary prints the container/refusal histogram, and the stand-in gate text is the measured sentence above.

## 7. Tests and gates

* `graphics_record_test` (**158 checks**): the fourteen measured container shapes decode (family, stage block, item
  count, variable offset/bytes/payload), the words are kept verbatim, and every rule in section 4 has a refusal case;
  `decode()`'s family/stage cross-check is covered both ways plus the 421..463 gap. The probe's input contract is
  checked on all fourteen shapes: every word is copied byte for byte and never masked, zeroed or reordered, so a probe
  scan reads the driver's state exactly, and the stage-block + shared-block window it scans is word-aligned and ends at
  the variable region.
* `graphics_pso_test` (**215 checks**): `decode_fixed_function_state()` refuses all eight fields by name, keys the
  refusal `state.fixed_function`, reports every field false, leaves the output untouched, and refuses an undecoded
  container as `state`; the existing build/default tests are unchanged. The behavioural probe is checked for every
  contract row: the winding pair tells cull mode and front-face flag apart, the depth scene tells depth enable/function/
  write mask apart, the overlap scene tells blend enable/factors/op/write mask apart, the triangle layer tells a
  topology type outside the draw's list class apart, the strip tells the 0xFFFF cut apart, the partial pixel tells the
  sample count and mask apart, and sample quality changes no observation at all. The probe is deterministic, and a
  decodable container still yields no field.
* `xs_proxy_mock_test` (**PASS**): its synthetic slot-57 record now carries the measured container (388 B, vertex
  family) and the mesh fixture the mesh family (464 B) - a deliberate update, because the decoder now proves the
  container where the mock previously sent a zeroed envelope-sized blob; the branch's E_NOTIMPL/E_INVALIDARG contract
  is unchanged. `unimplemented_policy_test` passes; `d3d12_fence_timing_test` remains the known pre-existing failure.
* Gates: the project's runtime build script -> `PALMETTO_OK`, its test build script -> `TESTS_BUILT`;
  `graphics_record_test` (158 checks) and `graphics_pso_test` (215 checks) pass, and the section 5 boot is the runtime
  measurement.

## 8. The next decode

The state values need a source this tree does not have yet. In rough order of cost:

1. **The serialized-state layout.** The blob is the `pState` component of the console's serialized graphics-pipeline
   state (the GDK's serialize/deserialize path). A layout table for that driver record (register/state order and bit
   fields) would turn the register hypothesis into a decode. None is public for the GDK path; a licensed GDK header or
   a driver trace is the shortest route, and it may be worth asking whether any console capture tool can emit the
   state mapping.
2. **A runtime measurement per field.** Each field can be pinned by a bounded behavioural probe of the title's own
   draws: cull/front face from a two-sided draw, depth enable/function/write from a depth read-back, blend
   enable/factors/op from a known colour written through the blend unit, topology from the draw's vertices and its
   recorded dynamic topology, strip cut from an indexed strip, sample count and mask from the resolved coverage. The
   probe itself is now built and tested (section 9): the seven observable fields each have a contract scene, and the
   test measures that a value change moves the observation. What is missing is the *observation*: a title draw that
   produces fragments, and the join from a draw to the state record that built its pipeline (section 9, "What a pin
   would need").
3. **Attachments.** The RTV/DSV formats are not in the runtime's bound views today (they are measured null,
   docs/display_path.md), so the state record stays the only candidate source; items 1 or 2 have to land first.

Until one of those lands, the stand-in stays and every pipeline carries it explicitly, logged once and counted per
call by field.

## 9. The behavioural probes: the falsifier a pin has to survive

This pass adds the behavioural probe the ledger's next step asks for: for every fixed-function field a defined
rendering result depends on, a minimal scene whose observation changes with that field, so a decode can be *tested*
instead of asserted. A field is pinned only when both of these are true:

1. **The probe tells the field's candidate values apart.** A scene rendered under the candidate state produces a
   different observation for each value the field could take. A "decode" that cannot be told apart from the documented
   stand-in is not a measurement, and the probe is the falsifier that says so.
2. **The title's own draw supplies the observation.** The scene has to be that draw through the record - its target,
   its recorded dynamic topology and, for the state question, its fragment coverage - so the observed result can be
   compared with the values the probe predicts.

The probe is `rt::graphics_pso::run_probe()` with the contract table `kProbeContract` (`port/src/graphics_pso.{h,cpp}`):
a GPU-free, deterministic model of a 4x4 RGBA8 target with a per-sample depth buffer and D3D12's standard sample
positions, so its answers are exact and reviewable. `port/test/graphics_pso_test.cpp` renders each contract scene under
the documented stand-in and under a state that changes exactly one field, and checks that the observation changes.
What it observes, under `documented_default_state()` (one sample unless named), is below; the measured colours are
RGBA8.

| field | probe scene | default observation | contrasting value | the change |
|---|---|---|---|---|
| rasterizer | winding pair (two triangles, opposite winding) | cull back: 1 triangle culled, 4 fragments (the clockwise half) | cull none | 0 culled, 8 fragments (both halves) |
| rasterizer | winding pair, front-face flag | cull back, clockwise front: 4 fragments | counter-clockwise front | cull back: the other half (same count, different pixels); cull front equals the flipped face |
| rasterizer | depth-clip layer | clipping on: 4 triangles clipped, 0 fragments | clipping off | 16 fragments, depth-clamped |
| blend | overlap (two layers) | blend off: colour 191 64 64 64 (the second layer's own colour) | src-alpha / one-minus-src-alpha | colour 72 64 112 159 |
| blend | overlap | as above | factor, blend op or write mask changed | a different resolved colour each |
| depth-stencil | two depth layers (near red, far green) | depth off: green (the far layer overwrites) | depth on, less, write all | red, 16 fragments rejected by the test |
| depth-stencil | two depth layers | depth on, write all: red | write mask zero | green (the far layer still passes) |
| depth-stencil | two depth layers | depth on, less: red | comparison greater | nothing written (both layers fail against the cleared depth) |
| topology type | triangle layer | triangle type: 4 primitives, 16 fragments | point or line type | 0 primitives: a type outside the draw's list class is the invalid pair D3D12 rejects |
| strip cut | indexed strip with a 0xFFFF sentinel and no such vertex | cut disabled: 3 out-of-range indices | cut value 0xFFFF | 0 out-of-range indices (the strip restarts) |
| sample count | partial pixel at one sample position | 1 sample: 0 fragments (the centre is outside) | 4 samples | 1 fragment, the pixel's covered-sample count is 1 |
| sample mask | partial pixel at four samples | mask all: 1 fragment | mask 0xFFFFFFFE | 0 fragments (the covered sample is dropped) |
| sample quality | all seven scenes | quality 0: the observation | quality 1 | **no change**: no defined rendering result depends on it |

The probe contract is also a measured statement about what a pin could ever be. It names the seven fields a draw can
decide and the one it cannot: D3D12 defines sample quality 0 as "the default pattern" and leaves other values
device-dependent, so only the serialized layout (or a driver trace) could pin that field, never a scene.

### What the container still leaves undetermined

Two decodes in the same field group would have to be told apart by an observation, so the first question is how many
places of the serialized record could carry each field at all. The rule measured over the library's 5,680 distinct
state records: a *place* is an aligned word, a byte or a nibble at any byte offset of the smallest measured container
(380 B, so all 5,680 records carry it), and it is a candidate for a field when the value extracted there stays inside
that field's D3D12 value domain on **every** record. For the closed-domain fields, places with one observed value are
not counted (a constant place can be a per-PSO field only if the field is constant, and two constants cannot be told
apart); for an open-domain field (a mask, a quality value), a constant is admissible, so every aligned word is a
candidate. The counts below are therefore a lower bound on the ambiguity: every field keeps several candidate places.

| field group | D3D12 values used | candidate places (of 95 aligned words / 380 bytes / 760 nibbles) |
|---|---|---:|
| rasterizer: cull mode | `1..3` (`NONE`/`FRONT`/`BACK`) | 3 |
| rasterizer: fill mode | `2..3` (`WIREFRAME`/`SOLID`) | **0** - no place takes the enum as a unit, so the record does not store the D3D12 enum |
| rasterizer: front face, depth clip, multisample, line AA, forced samples, conservative | `0`/`1` each | 49 each |
| blend: enable, alpha-to-coverage, independent blend | `0`/`1` each | 49 each |
| blend: source/destination factor | `1..19` | 16 |
| blend: blend op | `1..5` | 4 |
| depth-stencil: enable, write mask, stencil enable | `0`/`1` each | 49 each |
| depth-stencil: comparison function, stencil op | `1..8` | 12 each |
| topology type | `1..4` | 4 |
| index-buffer strip cut | enum `0`,`1`,`2` (`disabled`, `0xFFFF`, `0xFFFFFFFF`) | 72 |
| sample count | `1,2,4,8,16` | 6 |
| sample mask | any `UINT` | every aligned word (95): an arbitrary mask is admissible anywhere |
| sample quality | any `UINT` (device-dependent) | every aligned word (95): same reason |
| attachments (render-target/DSV formats) | - | not a field of the decoded layout at all |

So even the fields the probe can observe keep several candidate places in the record, and the container adds a second
unknown on top: a place is not a value. A pin needs both unknowns closed at once - the field's value from an
observation, and the place that value came from from the record.

### What a pin would need, ordered by cost

1. **An observation of a title draw that produces fragments.** The contract scenes are the measurement's model; the
   measurement itself is blocked by the frame-content frontier. The title's own vertex/geometry path currently produces
   no coverage (docs/frame_capture.md sections 6-8: the PS-only control with culling forced off still reads back
   black), so no state word changes a result the title produces. Once a draw covers pixels, the seven observable
   fields become measurable per pipeline through the same contract.
2. **A per-pipeline association between a draw and the state record that built its pipeline.** The slot-57 histogram
   records the state record and its shape (section 5), and the replay records each draw's target and dynamic topology,
   but the wiring does not join a draw to the record that built its pipeline. Without that join the observation cannot
   be attributed to a record. The one draw fact measured today - the frame's draws use a triangle list (docs/
   frame_capture.md section 3) - constrains the topology type *value* of those pipelines (a point or line type would be
   the invalid pair the probe's `mismatch` observable detects); it does not name a word and covers two pipelines of
   25,471 records.
3. **The serialized-state layout, or a driver trace, for the fields no scene can decide.** Sample quality is not a
   rendering result at all, and the attachments have no place in the decoded container yet; the GDK's `pState` layout
   (section 8 item 1) is the only source that could pin them.

Until (1) and (2) land, the probe pass pins **0 of 8 fields** - the same count as before - and the value added is that
the refusal is now testable: every field has a named scene and observable, and every candidate place is counted, so a
future observation can be checked against the contract instead of being guessed at. The stand-in therefore stays,
explicitly and logged once per shape.
