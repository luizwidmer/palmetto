# The slot-57 fixed-function state record: container decoded, fields refused

**Status: the container is decoded and measured on the title's whole library; no fixed-function field of it is
pinned, so the documented stand-in stays and this page records a documented negative.** The 380..520-byte blob the
slot-57 record carries at `+0x08` is the console driver's serialized graphics-pipeline state record. Its container -
two size families, a stage block per family, a shared block, and a variable tail whose size step is an item count -
decodes exactly on all **5,680 distinct state records** the library holds (referenced by **25,471** graphics records).
No word of it is pinned to a D3D12 rasterizer, blend, depth-stencil, topology, strip-cut or sample field by any
measurement of this title's data, so `rt::graphics_record::decode_state` keeps every word raw,
`rt::graphics_pso::decode_fixed_function_state` refuses all eight fields by name, and the slot-57 wiring keeps the
explicit, logged `documented_default_state()` stand-in (docs/graphics_pso.md section 4). What a decode would need is
listed in section 8.

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

## 6. The wiring patch (handed to the lead; the shim is not edited by this change)

The exact patch is in `d3d12_xs_device_stateblob.diff` next to this analysis in the work folder; it touches the
slot-57 branch only and does not add a refusal path:

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

The patched shim compiles (`cl /Zs` with the PALMETTO flags). After the lead applies it and boots, the log's stand-in
line becomes the measured container line plus `state.fixed_function`, and the boot numbers stay where this page's
baseline is.

## 7. Tests and gates

* `graphics_record_test` (**156 checks**): the fourteen measured container shapes decode (family, stage block, item
  count, variable offset/bytes/payload), the words are kept verbatim, and every rule in section 4 has a refusal case;
  `decode()`'s family/stage cross-check is covered both ways plus the 421..463 gap.
* `graphics_pso_test` (**170 checks**): `decode_fixed_function_state()` refuses all eight fields by name, keys the
  refusal `state.fixed_function`, reports every field false, leaves the output untouched, and refuses an undecoded
  container as `state`; the existing build/default tests are unchanged.
* `xs_proxy_mock_test` (**PASS**): its synthetic slot-57 record now carries the measured container (388 B, vertex
  family) and the mesh fixture the mesh family (464 B) - a deliberate update, because the decoder now proves the
  container where the mock previously sent a zeroed envelope-sized blob; the branch's E_NOTIMPL/E_INVALIDARG contract
  is unchanged. `unimplemented_policy_test` passes; `d3d12_fence_timing_test` remains the known pre-existing failure.
* Gates: `_setup\build_palmetto.cmd` -> `PALMETTO_OK`; `_setup\the test build script` -> `TESTS_BUILT`.

## 8. The next decode

The state values need a source this tree does not have yet. In rough order of cost:

1. **The serialized-state layout.** The blob is the `pState` component of the console's serialized graphics-pipeline
   state (the GDK's serialize/deserialize path). A layout table for that driver record (register/state order and bit
   fields) would turn the register hypothesis into a decode. None is public for the GDK path; a licensed GDK header or
   a driver trace is the shortest route, and it may be worth asking whether any console capture tool can emit the
   state mapping.
2. **A runtime measurement per field.** Each field can be pinned by a bounded behavioural probe of the title's own
   draws: depth test direction and write mask from a depth read-back, stencil ops from a stencil read-back, blend
   factors from a known colour written through the blend unit, cull/front face from a two-sided draw, topology from
   the vertices a draw emits, sample count from the resolved coverage. Each probe needs one draw with a known input
   through the replayed PSO, and the result is a measurement of this title's data, not an inference.
3. **Attachments.** The RTV/DSV formats are not in the runtime's bound views today (they are measured null,
   docs/display_path.md), so the state record stays the only candidate source; items 1 or 2 have to land first.

Until one of those lands, the stand-in stays and every pipeline carries it explicitly, logged once and counted per
call by field.
