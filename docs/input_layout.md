# The slot-57 input-layout element table, decoded

**Status: decoded, mapped and wired.** The input-layout blob of the slot-57 graphics pipeline record is an element
table: a 0x50-byte header with the element count at `+0x4C`, then one variable-length record per element (a length-
prefixed semantic name followed by a 24-byte attribute block), the records' bytes zero-padded to a 4-byte boundary.
The attribute block is the `D3D12_INPUT_ELEMENT_DESC` fields in their D3D12 order with the name pointer replaced by the
stored name, and its format word is the documented `DXGI_FORMAT` enum: the measured words `2/6/16/28/30` are
`R32G32B32A32_FLOAT`, `R32G32B32_FLOAT`, `R32G32_FLOAT`, `R8G8B8A8_UNORM` and `R8G8B8A8_UINT`, and the formats' byte
sizes are exactly the measured packed byte offsets. `graphics_pso::build` now emits one `D3D12_INPUT_ELEMENT_DESC` per
element; an unmeasured format word, input slot, input-slot class, step rate or aligned offset still refuses with the
field and its numeric value named, so the slot-57 boot histogram reports exactly which values remain unmapped.

Decoded field table (all six measured shapes, `docs/graphics_record.md` section 3 has the record-level table):

| field | status | evidence |
|---|---|---|
| element count | decoded | `u32` at `+0x4C`; 0/1/2/2/3/4 on the six measured shapes |
| name length | decoded | `u32` before every name, including the NUL; 9 for `POSITION`/`TEXCOORD`, 6 for `COLOR` |
| semantic name | decoded | the `name length` bytes after the length word, printable ASCII NUL-terminated |
| attribute block | decoded, 6 words | 24 bytes after each name; word order below |
| attribute word 0 | decoded: `SemanticIndex` | 0 on every sample except the 224-byte shape's last element, which is 2 (`COLOR` index 2) |
| attribute word 1 | decoded: `Format` (DXGI) | 2/6/16/28/30; sizes explain the packed offsets (section 3) |
| attribute word 2 | decoded: `InputSlot` | 0 in every measured element |
| attribute word 3 | decoded: `AlignedByteOffset` | emitted verbatim; packed in the measured dumps, and a measured gapped instance puts `COLOR` at 24 after a 12-byte `POSITION` (section 2) |
| attribute word 4 | decoded: `InputSlotClass` | 0 = `PER_VERTEX_DATA` in every measured element |
| attribute word 5 | decoded: `InstanceDataStepRate` | 0 in every measured element |
| header `+0x00..+0x4B` | **unknown, kept raw** | varies per blob instance (e.g. `+0x10`, `+0x24`, `+0x38` differ for the same shape across boots); no meaning measured |
| padding after the records | decoded, must be zero | 0..3 bytes to the next 32-bit boundary |

## 1. The element table, measured

The blob is `0x50` header bytes, then `count` element records back to back from `+0x50`, then zero padding to a 4-byte
boundary:

```
+0x4C  u32 element count
+0x50  element 0: u32 name length including the NUL  |  name bytes  |  6 x u32 attribute
...    element 1, element 2, ...
pad    zero bytes until size % 4 == 0

size == 0x50 + sum(4 + name_len + 24), rounded up to a multiple of 4, and the blob must be filled exactly
```

The six boot shapes fix the relation:

| blob | count | elements (payload order) | records end | pad |
|---|---:|---|---:|---:|
| 80 B | 0 | — | 0x50 | 0 |
| 120 B | 1 | `POSITION` | 0x75 | 3 |
| 152 B | 2 | `POSITION`, `COLOR` | 0x97 | 1 |
| 156 B | 2 | `TEXCOORD`, `POSITION` | 0x9A | 2 |
| 188 B | 3 | `POSITION`, `COLOR`, `TEXCOORD` | 0xBC | 0 |
| 224 B | 4 | `COLOR`, `TEXCOORD`, `POSITION`, `COLOR` | 0xDE | 2 |

The measured first-name offsets are `+0x54` in every shape, and the later names follow the preceding element's length
at `+0x54`, `+0x79`/`+0x76`, `+0x9B`, `+0xC0` exactly - e.g. the 188-byte shape's `POSITION` at `+0x54`, `COLOR` at
`+0x79` (9-byte first name + 24 attribute bytes + its length word) and `TEXCOORD` at `+0x9B`. That fixes the record
boundary and the 24-byte attribute block: the four bytes immediately before each later name are its length word, not
part of the previous attribute block.

**The contiguous-names reading is refuted.** "All names first, then `count * 28` attribute bytes" has the same total
size expression (`4 + name_len + 24 == name_len + 28`) and the earlier decoder accepted it, but it needs the header to
end at `+0x54` while every measured name starts at `+0x54` only because of the `+0x50` length word: its totals are
therefore 4 bytes larger than every measured shape (`192` for the 188-byte shape, `226` for the 224-byte one), and it
cannot produce the measured length words before each later name. The decoder refuses it (its tests cover that blob).

## 2. The attribute block: the D3D12 element fields, in order

The six words are the `D3D12_INPUT_ELEMENT_DESC` fields in their D3D12 order with `SemanticName` replaced by the
inline name:

```
word 0  SemanticIndex          measured: 0, and 2 on the 224-byte shape's last COLOR (i.e. COLOR2)
word 1  Format                 measured: 2, 6, 16, 28, 30 (DXGI_FORMAT, section 3)
word 2  InputSlot              measured: 0
word 3  AlignedByteOffset      emitted verbatim; the measured layouts are non-overlapping and mostly packed
word 4  InputSlotClass         measured: 0 = PER_VERTEX_DATA
word 5  InstanceDataStepRate   measured: 0
```

Word 1 and word 3 are pinned by the measured values themselves: the format sizes are exactly the byte offsets. Word 0
is pinned by the 224-byte shape's last element, which stores 2 in word 0 next to the `COLOR` name - the measured
`COLOR2` semantic index, where every other sample has 0. With words 1 and 3 at the D3D12 field positions 1 and 3, and
word 0 at position 0 carrying an index value, the order is the D3D12 order; words 2, 4 and 5 are 0 in every measured
element, so `build()` emits slot 0, per-vertex data and step rate 0 and refuses any other value by field with the
value in the field name.

`AlignedByteOffset` is not required to be the packed running total: `D3D12_INPUT_ELEMENT_DESC` takes the offset
verbatim and the description carries no vertex stride, so `build()` emits the stored offset and only refuses one that
overlaps the preceding elements' measured sizes (or the unmeasured `D3D12_APPEND_ALIGNED_ELEMENT` sentinel). The
measured samples are packed, but the post-decode boot also has an 188-byte instance whose `COLOR` sits at offset 24
after a 12-byte `POSITION` - a measured gap - and that shape builds with its stored offsets.

## 3. The format word: DXGI_FORMAT, not a console enum

Hypotheses tested against the measured blob bytes and the documented enums:

* **DXGI_FORMAT (supported).** The measured words are the documented DXGI values and their byte sizes are exactly the
  measured offset advances:

  | word | DXGI format | bytes | whose size the measured offsets confirm |
  |---:|---|---:|---|
  | 2 | `DXGI_FORMAT_R32G32B32A32_FLOAT` | 16 | only ever the last element of its blob: not offset-confirmed |
  | 6 | `DXGI_FORMAT_R32G32B32_FLOAT` | 12 | 152 B: `COLOR` at offset 12 after it; 188 B: same |
  | 16 | `DXGI_FORMAT_R32G32_FLOAT` | 8 | 156 B: `POSITION` at 8 after it; 224 B: `POSITION` at 12 after 4 + 8 |
  | 28 | `DXGI_FORMAT_R8G8B8A8_UNORM` | 4 | 152 B: `COLOR` at 12 = 12 + 0; 224 B: `TEXCOORD` at 4 after it |
  | 30 | `DXGI_FORMAT_R8G8B8A8_UINT` | 4 | only ever the last element of its blob: not offset-confirmed |

  The semantics agree as well: `POSITION` is 3- or 4-component float, `TEXCOORD` is float2, `COLOR` is a packed
  8-bit-per-channel colour (UNORM or UINT).
* **Console GFX10 format code (refuted).** `port/src/gfx10_descriptors.h` maps the same numbers to
  `GFX10_FORMAT_8_SNORM` (word 2, 1 byte), `GFX10_FORMAT_8_SINT` (6, 1 byte), `GFX10_FORMAT_8_8_USCALED` (16, 2 bytes)
  and `GFX10_FORMAT_16_16_SINT` (28, 4 bytes), and has no row for 30 at all. Those sizes cannot produce the measured
  advances of 12, 8 and 4 bytes, so the console-code hypothesis is refuted by the data.
* **Something else (not needed).** No third encoding explains the sizes and the offsets.

The mapping is `rt::graphics_pso::kMeasuredInputFormats`. Words 2 and 30 are the DXGI reading of a measured value whose
size is not independently offset-confirmed (each only appears as the last element of a layout). The falsifier is the
builder's overlap check for a following element: if a layout puts an element after word 2 or word 30 at an offset below
the DXGI size, the builder refuses it as `input_layout.elements[i].aligned_byte_offset[<offset>]`, which says the size
reading is too large; a size reading that is too small cannot produce a wrong description, because the description
carries the stored offset and no stride. An unmeasured word refuses with
`input_layout.elements[i].format[<word>]`.

## 4. What builds now and what refuses

`rt::graphics_pso::build` turns the decoded elements into `D3D12_INPUT_ELEMENT_DESC` entries:
`SemanticName` points at the record's own validated NUL-terminated name bytes; `SemanticIndex`, `Format`, `InputSlot`,
`AlignedByteOffset`, `InputSlotClass` and `InstanceDataStepRate` come from the measured words as in section 2. The
element descriptions live in a per-thread buffer owned by the backend, valid until the next `build()` on the same
thread; the slot-57 wiring calls `CreateGraphicsPipelineState` with the description immediately, which is the
documented lifetime. An empty 80-byte layout stays `NumElements 0` with a null array.

| refusal (exact `why` field prefix) | meaning |
|---|---|
| `record.input_layout` | the blob is not the measured element table (size, count, name, padding or fill) |
| `input_layout.elements[i].format[word]` | the format word is not a measured DXGI value; the histogram lists the value |
| `input_layout.elements[i].input_slot[n]` | an input slot other than 0 (the only measured slot) |
| `input_layout.elements[i].input_slot_class[n]` | an input-slot class other than 0 (per-vertex data) |
| `input_layout.elements[i].instance_step_rate[n]` | an instance step rate other than 0 |
| `input_layout.elements[i].aligned_byte_offset[n]` | an offset that overlaps the preceding elements' measured sizes, or the unmeasured append sentinel |
| `input_layout.elements[i].semantic_index[n]` | two elements with the same semantic name and index (D3D12 needs unique) |
| `record.stages` | a layout with elements but no vertex stage to consume it |

The refusal field names carry the numeric value because the slot-57 histogram keys a refusal by the text before its
first `:`, so `input_layout.elements[0].format[7]` and `input_layout.elements[2].format[7]` are separate rows: the next
boot says exactly which words (and which element positions) remain unmapped.

## 5. Tests and gates

* `port/test/graphics_record_test.cpp` (117 checks): the decoded table on all six measured shapes (count, name
  lengths, name offsets, the six words, padding), the 4-byte fill rule, and the malformed cases - size outside
  80..340 or not a word multiple, count too high/low, unterminated or unprintable name, zero/absurd name length,
  non-zero padding, slack after the padding, a zero count with trailing bytes, and the refuted contiguous-names form.
* `port/test/graphics_pso_test.cpp` (159 checks): the measured format table against the documented DXGI enum values,
  the mapped descriptions for the 120/152/156/188/224-byte shapes (formats, semantic indexes, packed offsets, slot,
  per-vertex class, step rate, `SemanticName` pointing into the record blob), the format-2 size falsifier (a following
  element at 16 bytes builds, one at 12 refuses at `aligned_byte_offset`), and every refusal above.
* `port/test/xs_proxy_mock_test.cpp`: the slot-57 element case now builds the measured element form with an
  unmeasured format word 7 and expects the same `E_NOTIMPL` refusal, so the branch's contract is unchanged.
* Gates: `_setup\build_palmetto.cmd` -> `PALMETTO_OK`; `_setup\the test build script` -> `TESTS_BUILT`;
  `graphics_record_test`, `graphics_pso_test`, `xs_proxy_mock_test` and `unimplemented_policy_test` pass.

## 6. Boot measurement, before and after

One boot, retail image, shader corpus, Vulkan backend, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`, 150 s
timeout, a boot runs to the timeout; harness `tooling/boot_report.py`, profile `_setup/profile_boot.py`. The "before"
column is the slot-57 wiring measurement (`docs/slot57_wiring.md` section 2, the same tree before this decode).

| | before | after |
|---|---|---|
| verdict | ALIVE 150.2 s, no exception | **ALIVE 150.2 s, no exception** |
| traced D3D12 calls | 1,410,454 | **1,503,369** |
| slot-57 calls / distinct records | 10,241 / 10,241 | **10,241 / 10,241** |
| record decode refusals | 0 | 0 |
| built (`CreateGraphicsPipelineState` reached) | 1,074 | **10,241 of 10,241** (10,000 at the 10,000-call checkpoint, then no refusal logged) |
| PC pipelines created (`S_OK`) | 1,074 | **10,241** (0 PC failures logged) |
| refused before the call | 8,926, all `record.input_layout` | **0** |
| PC `CreateGraphicsPipelineState` failures | 0 | 0 |
| `SetPipelineState` | 0 | **42,721** |
| `DrawInstanced` / `DrawIndexedInstanced` | 0 / 0 | **42,722 / 0** |
| `ExecuteCommandLists` ok / failed | 87,395 / 0 | **42,724 / 42,720** |
| `Close` ok / failed | 88,018 / 0 | 86,059 / 0 |

The input-layout shape histogram at the 10,000-call checkpoint (the same comparison as `docs/slot57_wiring.md`):

| blob shape (`size`/count) | before | after |
|---|---:|---:|
| 80 B / count 0 | 1,074 | 1,021 |
| 120 B / count 1 | 687 | 687 |
| 152 B / count 2 | 5,457 | 5,510 |
| 156 B / count 2 | 144 | 144 |
| 188 B / count 3 | 2,566 | 2,566 |
| 224 B / count 4 | 72 | 72 |

The only difference is 53 records between the 80-byte (empty) and 152-byte shapes: boot-to-boot record ordering, not the
decoder - in both boots every blob decodes. Post-decode gates: input layouts empty 1,021, with elements 8,979,
undecodable 0; root signatures PC object 10,000; unreadable stage bytes 0; PC results `S_OK` 10,000. The boot makes
10,241 slot-57 calls in all and logs no refusal and no PC failure after the checkpoint, so every one of them builds.

The refusal histogram is **empty**: the 10,000-call checkpoint has zero `why` rows, which is also the format-word
answer - every format word in all 10,241 records is one of the five measured DXGI values, so no value remains
unmapped on this boot. If a boot carries another word, its row appears as `input_layout.elements[i].format[<word>]`
with the element position and the value, and the first-occurrence log prints the whole sentence.

**The draw path is reached.** The trace records **42,721 `SetPipelineState`** and **42,722 `DrawInstanced`** calls,
both new: the frame now binds the pipelines the slot-57 branch builds and records one draw per frame (the layout was the
last field between the draw path and real pipelines). The two counters are different calls and are kept separate in the
table above: 42,721 binds, 42,722 draws. The title keeps looping: `Close` succeeds 86,059 times with 0 failures, no
exception, and `ExecuteCommandLists` attempts 85,444 submits in 150 s (34,134 per 60 s; the baseline's 87,395 is
34,957 per 60 s). But only 42,724 of those submits succeed: from the first recorded draw whose resource the replay layer cannot match, the
runtime logs `ExecuteCommandLists failed: unknown resource; nothing was submitted` for the rest. That is the next
frontier (the draw's resource binding, not the layout), and it is why the ok-submit count is half the baseline.

## 7. The next decode

* **The header words.** `+0x00`, `+0x10`, `+0x24`, `+0x38` vary per blob instance for the same shape, so they are a
  hash or a per-layout string, not layout state; nothing needs them for the element table.
* **Formats beyond the measured five.** The post-decode histogram's `input_layout.elements[i].format[<word>]` rows are
  the exact list to measure next; each needs a measured offset advance (or the PC's own validation) before it is added
  to `kMeasuredInputFormats`, exactly as words 2 and 30 are flagged here.
* **The fixed-function state blob** is still undecoded (`docs/graphics_record.md` section 4) and supplies the
  attachments, topology, blend/depth state and strip cut the PC description takes from the documented stand-ins today.
* **The draw's resource binding** is the frontier the decode exposes: the first frames' draws replay and submit, then
  `ExecuteCommandLists` starts failing with `unknown resource; nothing was submitted` (the runtime's replay layer cannot
  match a resource the recorded draw names). The layout and the pipelines are not what refuses there.
* **The fixed-function state** is the other record field still on the stand-ins: once decoded it replaces
  `documented_default_state()` and the 1 x `R8G8B8A8_UNORM` attachment list, which are the values every built pipeline
  above carries.
