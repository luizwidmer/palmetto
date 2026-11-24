# The console graphics pipeline record (device slot 57)

**Status: decoder, not a converter.** `port/src/graphics_record.h` / `.cpp` decode the record the title passes to
device slot 57 into the pieces a PC `D3D12_GRAPHICS_PIPELINE_STATE_DESC` needs, and refuse everything the
measurements do not pin. `port/test/graphics_record_test.cpp` builds every blob byte by byte and prints the
decoded/unknown table; `docs/boot_frontier.md` (*slot 57 is a deserialized graphics pipeline record*),
`docs/shader_library.md` sections 2 and 3 and `docs/input_layout.md` are the measurements it rests on. Nothing here
quotes game data: the tables carry counts, offsets and raw words only, and no shader bytes are stored.

## 1. The record

The slot-57 argument is the expansion of one pipeline-library graphics record (`docs/shader_library.md` section 2): a
root signature, the fixed-function state blob and the input-layout blob, then `(native program, kind-2 stage blob)`
pairs **in record-slot order**. The slot gives the stage; the stage blob itself carries no stage identity.

| offset | field | evidence |
|---:|---|---|
| `+0x00` | root signature pointer | `docs/boot_frontier.md` slot-57 table, first row |
| `+0x08` / `+0x10` | state blob pointer / size, 380..520 B | same table (388 B sample); `docs/pso_cache.md`, *stage record (sizes 380..520)* |
| `+0x18` / `+0x20` | input-layout blob pointer / size, 80..340 B | same table (188 B sample); an 80-byte layout with no elements is a mesh pipeline's (`docs/shader_library.md` section 2) |
| `+0x28` / `+0x30` | slot 0 (**PS**) program pointer / size | same table (`+0x28` 448 B program) |
| `+0x38` / `+0x40` | slot 0 (**PS**) kind-2 stage blob pointer / size | same table (`+0x38` 208 B blob) |
| `+0x48` / `+0x50` | slot 1 (**VS**) program pointer / size | same table (`+0x48` 384 B program) |
| `+0x58` / `+0x60` | slot 1 (**VS**) kind-2 stage blob pointer / size | same table (`+0x58` 752 B blob) |
| `+0x68` / `+0x70` | slot 2 (**MS**) program pointer / size | same pair pattern by slot number; no boot sample decoded with a mesh stage |
| `+0x78` / `+0x80` | slot 2 (**MS**) kind-2 stage blob pointer / size | same |
| `+0x88` .. `+0x108` | zero | measured zero in every sample |
| `+0x108` .. | unidentified tail | not decoded and not represented |

Slot to stage: `docs/shader_library.md` section 2 and `SLOTS` in `tooling/shader_library.py` (0 pixel, 1 vertex,
2 mesh). The library's measured stage sets are PS+VS, PS+MS, MS only and VS only; no record has a vertex and a mesh
stage together, and D3D12 forbids that pair, so the decoder refuses it.

## 2. The decoder's contract

```
bool decode(const void* descriptor, std::size_t available, Record* out);
bool decode_input_layout(Span blob, std::vector<Element>* out);
bool decode_state(Span blob, State* out);
bool decode_stage_blob(Span blob, StageBlob* out);
```

`decode()` parses only the record's own bytes. `available` must be at least `0x108`; the tail beyond that is ignored.
It **never dereferences** the pointers it copies out, so a record full of bad pointers cannot fault inside the decoder:
the pointed-to ranges are the caller's guarantee, and the content decoders take the spans it fills in.

`decode()` refuses, rather than passing on a shape it was not measured on:

* fewer than `0x108` readable bytes;
* a state size outside 380..520 B, a size that is not a whole number of 32-bit words, or a null state pair;
* an input-layout size outside 80..340 B or a null layout pair;
* a stage slot with only part of its pair set (a pointer without a size or a size without a pointer);
* a program size outside 52..239,464 B (`docs/shader_library.md` section 2, the measured program range);
* a stage-blob size that is not `0x10 + 32*k` (at least `0x30`), before reading any byte of it;
* a non-zero byte in `+0x88..+0x108` (measured zero everywhere);
* zero stages, or vertex and mesh stages together.

The two blob decoders then check the contents:

* `decode_state()` validates the measured envelope and copies the words out. No field is decoded (section 4).
* `decode_input_layout()` decodes the `0x50`-byte header, the element count at `+0x4C`, and one variable-length record
  per element (a `u32` name length including the NUL, the name bytes, then a 24-byte attribute block), with the blob
  filled exactly (section 5).
* `decode_stage_blob()` decodes the kind-2 blob's envelope (section 6).

## 3. Field table: decoded vs unknown

"Kept raw" means the decoder returns the original words/bytes and no meaning is attached to them.

| field | status | evidence / raw source |
|---|---|---|
| `Record.root_signature` | decoded, opaque pointer | `+0x00`; not validated and not dereferenced, a zero is not interpreted |
| `Record.state` pointer/size | decoded, range-checked | `+0x08`/`+0x10`; 380..520 B |
| `Record.input_layout` pointer/size | decoded, range-checked | `+0x18`/`+0x20`; 80..340 B |
| `Record.stages[i].slot` | decoded | record slot 0 = PS, 1 = VS, 2 = MS |
| `Record.stages[i].program` | decoded span only | native program bytes; the shader index keys it by SHA-256 of the bytes as passed (`docs/boot_frontier.md`) |
| `Record.stages[i].stage_blob` | decoded span only | contents via `decode_stage_blob()` |
| record `+0x88..+0x108` | decoded, must be zero | measured zero in every sample; anything else is refused |
| record tail `+0x108..` | **unknown** | unidentified tail; not represented |
| `State.size` | decoded, range check | 380..520 B, whole number of words |
| `State.raw` | **unknown**, kept raw | all words of the 388-byte sample, including the 0x40 word at `+0x150`; measured sample words `0x4005d808, 4, 0x00100060, 0x00020042, 0xb50516a0` are preserved verbatim, not interpreted |
| fixed-function state (rasterizer, blend, depth-stencil, sample desc, topology) | **unknown** | no word's meaning is measured; see section 4 |
| `Element.semantic` | decoded | the `name length` bytes after the length word, from `+0x54` for the first element, in element order |
| `Element.attribute` | decoded, 6 words (24 bytes), kept verbatim | the `D3D12_INPUT_ELEMENT_DESC` fields in D3D12 order with the name pointer replaced by the stored name; `graphics_pso::build` maps them (section 5) |
| `Element.semantic_index` | decoded | attribute word 0; measured 0, and 2 on the 224-byte shape's last `COLOR` (`COLOR2`) |
| `Element.format` | decoded | attribute word 1, a documented `DXGI_FORMAT`: measured 2/6/16/28/30 = `R32G32B32A32_FLOAT`/`R32G32B32_FLOAT`/`R32G32_FLOAT`/`R8G8B8A8_UNORM`/`R8G8B8A8_UINT`; sizes match the measured offsets (section 5) |
| `Element.input_slot` | decoded | attribute word 2; 0 in every measured element |
| `Element.aligned_byte_offset` | decoded | attribute word 3; emitted verbatim, packed in the measured samples, with one measured gapped instance after a 12-byte `POSITION` (section 5) |
| `Element.per_vertex_data` | decoded | attribute word 4 `InputSlotClass`; 0 = `PER_VERTEX_DATA` in every measured element, and word 5 `InstanceDataStepRate` is 0 |
| `StageBlob.kind` | decoded | word at `+0x00` is 2 (`docs/shader_library.md` section 3) |
| `StageBlob.declared_size_word` | decoded relation | `+0x04 + 12 == blob size`, measured; why the constant is 12 is not known, so it is a consistency check only |
| `StageBlob.head_word_08` | **unknown**, kept raw | word at `+0x08` |
| `StageBlob.head_tag` | decoded | word at `+0x0C` is 0x40 on all 20,318 distinct blobs; `tooling/shader_library.py:stage_digest` checks the same word |
| `StageBlob.records[i].tag` | decoded | 0x41 then 0x42, 32-byte records from `+0x10` |
| `StageBlob.records[i].body` | **unknown**, kept raw | the seven body words; the chunk field readings in `docs/pso_cache.md` are marked unproven there |
| `StageBlob.source_digest` | decoded, opaque | 16 bytes after the final 0x43 tag: the source container's `HASH` digest, `docs/shader_library.md` section 3 / `docs/fspx.md` kind 7 |
| `StageBlob` trailing 12 bytes | decoded, must be zero | measured zero in all 20,318 blobs (`docs/pso_cache.md` 0x43 chunk: a 16-byte value then 12 zero bytes) |

## 4. The fixed-function state blob

The 380..520-byte blob is the main unknown; `docs/boot_frontier.md` calls it undecoded and this decoder does not change
that. `decode_state()` proves only its envelope (measured size range, word count) and keeps the whole blob as
`State.raw`. **No rasterizer, blend, depth-stencil, sample-description, topology or strip-cut field is decoded**, and
the converter must not infer one from a word position or value. The blob is kept because it is needed twice later: to
decode the state, and as part of the record's identity/cache key.

## 5. The input-layout blob

Decoded on the six measured boot shapes (80 B/count 0, 120/1, 152/2, 156/2, 188/3, 224/4), docs/input_layout.md
section 1. The blob is a `0x50`-byte header, the element count at `+0x4C`, then one variable-length record per element
from `+0x50`, then zero padding to a 4-byte boundary:

```
+0x4C  u32 element count
+0x50  element 0: u32 name length including the NUL  |  name bytes  |  6 x u32 attribute
...    element 1, element 2, ...
pad    zero bytes until size % 4 == 0

size == 0x50 + sum(4 + name_len + 24), rounded up to a multiple of 4, and the blob must be filled exactly
```

The first name is at `+0x54`, and each later name follows the preceding element's 24-byte attribute block, so the four
bytes immediately before every later name are its length word. The measured boundaries fix the record shape: the 188-byte
shape's `POSITION` at `+0x54`, `COLOR` at `+0x79` (9-byte first name + 24 attribute bytes + its length word) and
`TEXCOORD` at `+0x9B`. The decoder requires printable name bytes, a length word that fits, and the exact total with zero
padding, and otherwise returns false; this is what rejects a count that disagrees with the bytes and a name that runs
past the blob.

**The contiguous-names reading is refuted.** "All names first, then `count * 28` attribute bytes" has the same total
size expression (`4 + name_len + 24 == name_len + 28`) and this decoder accepted it earlier, but it needs the header to
end at `+0x54` while every measured name starts at `+0x54` only because of the `+0x50` length word: its totals are
4 bytes larger than every measured shape, and it cannot produce the measured length word before each later name. The
decoder now accepts only the interleaved form, `graphics_record_test` covers the refuted one as a refusal, and
docs/input_layout.md section 1 is the measurement.

**The attribute block is the `D3D12_INPUT_ELEMENT_DESC` fields in D3D12 order** with the name pointer replaced by the
stored name (docs/input_layout.md section 2). This decoder keeps the six words verbatim in `Element.attribute`;
`graphics_pso::build` decides their meaning from the measured values and refuses what no measurement pins:

| word | field | measured |
|---:|---|---|
| 0 | `SemanticIndex` | 0 on every element except the 224-byte shape's last `COLOR`, which is 2 (`COLOR2`) |
| 1 | `Format` (DXGI) | 2/6/16/28/30 = `R32G32B32A32_FLOAT`/`R32G32B32_FLOAT`/`R32G32_FLOAT`/`R8G8B8A8_UNORM`/`R8G8B8A8_UINT`; sizes are exactly the measured offset advances |
| 2 | `InputSlot` | 0 in every measured element |
| 3 | `AlignedByteOffset` | stored offset, emitted verbatim; packed in the measured samples, with one measured gapped instance (`COLOR` at 24 after a 12-byte `POSITION`) |
| 4 | `InputSlotClass` | 0 = `PER_VERTEX_DATA` in every measured element |
| 5 | `InstanceDataStepRate` | 0 in every measured element |

The format words are the documented DXGI values, not the console's GFX10 codes: the console-code reading of the same
numbers gives byte sizes that cannot produce the measured advances, so it is refuted by the data (docs/input_layout.md
section 3). An unmeasured format word, input slot, class, step rate or an overlapping stored offset refuses by field with
the value in the name (`input_layout.elements[i].format[<word>]`, ...), which is the post-decode boot's empty histogram
(docs/input_layout.md section 4).

## 6. The kind-2 stage blob

From `docs/shader_library.md` section 3 and `docs/fspx.md` (kind 7 shares the record layout), measured on all 20,318
distinct stage/layout blobs:

```
+0x00  u32 kind = 2
+0x04  u32 declared size word: + 12 == blob size
+0x08  u32 unknown
+0x0C  u32 head tag = 0x40
+0x10  N x 32-byte records: first word 0x41, then 0x42, in that order
       last 32-byte record: first word 0x43, 16-byte source digest, 12 zero bytes
```

`decode_stage_blob()` decodes the kind, the size relation, the 0x40 head tag, the record tags in order, the 0x43
position, the 16-byte digest and the 12 zero bytes. It refuses kind 3 (the compute layout blob), any other tag, a 0x41
after a 0x42, a missing or non-final 0x43, a non-zero trailing byte, and a size that disagrees with the size word. The
0x41/0x42 bodies (and the `+0x08` word) are kept raw: the field readings in `docs/pso_cache.md` are unconfirmed there.
The digest is kept as opaque identity bytes and is not verified against any container.

## 7. How a converter will use it

1. `decode(desc, available, &record)`; on false, refuse the slot-57 call (`E_INVALIDARG` and a log line) instead of
   creating a PSO from a shape the decoder does not know.
2. Programs: for each stage in `record.stages[0..stage_count)`, look the program up by `SHA-256(program bytes)` in the
   shader index (`fspx_code_sha256`, the key compute already resolves with) and use the translated module as
   `VS`/`PS`/`MS` bytecode. The stage is `stage.slot`, not anything in the blob. (Both of the gaps this note used to
   name are now closed elsewhere: the library's graphics programs are published - 1,266/1,266, `docs/boot_frontier.md` -
   and the native-stage resolver is `docs/graphics_pso.md` section 2. This decoder is not where either lives.)
3. Input layout: `decode_input_layout(record.input_layout, &elements)` gives the element table (section 5). The
   converter maps the six words to `D3D12_INPUT_ELEMENT_DESC` (`semantic_index`, `format`, `input_slot`,
   `aligned_byte_offset`, class and step rate) and refuses an unmeasured value by field (docs/input_layout.md section 4).
4. State: `decode_state(record.state, &state)` gives raw words only, so every fixed-function field of the PC
   description is still missing (section 4).
5. Root signature: pass `record.root_signature` to `pRootSignature` after validating it in the caller (the decoder only
   copies the pointer value).
6. Stage blobs: `decode_stage_blob()` gives the stage's source digest and record tags; today they are for logging and
   identity, not for fields of the PC description.

Mapping to `D3D12_GRAPHICS_PIPELINE_STATE_DESC` today: `pRootSignature`, the per-stage bytecode pointers, the
translated modules and `InputLayout` (one `D3D12_INPUT_ELEMENT_DESC` per decoded element, docs/input_layout.md) can be
supplied; `RasterizerState`, `BlendState`, `DepthStencilState`, `SampleDesc`, `PrimitiveTopologyType`, `IBStripCutValue`,
`DSVFormat` and `RTVFormats` cannot, because their console sources are the unknown fields above.
`port/shims/d3d12_vulkan_device.cpp` (create_graphics) shows what the PC side then consumes, and
`docs/graphics_pso.md` is the builder that does the mapping.

## 8. Tests

`port/test/graphics_record_test.cpp` — synthetic blobs only, no device and no game files. It prints the
decoded/unknown table and covers: a PS+VS record with every span at its measured offset; a record of fake pointers
that must decode without being dereferenced; the 80-byte mesh layout; both sides of every measured bound; the decoded
element table on all six measured shapes (count, name lengths, name offsets, the six words, padding), the 4-byte fill
rule and the refuted contiguous-names form; and the malformed cases (short read, zero stages, half-set pairs, non-zero
reserved byte, vertex+mesh stages, unterminated or unprintable semantic, count contradicting the bytes, stage-blob kind,
size word, head tag, tag order, final 0x43 and trailing zero bytes). 117 checks in all (docs/input_layout.md section 5).

```
cmake --build port/build --target graphics_record_test
port/build/graphics_record_test.exe     # prints the table, exits 0
ctest --test-dir port/build -R graphics_record
```

The target is registered in `port/CMakeLists.txt` next to the other pure-stdlib tests (`graphics_record_test` /
`graphics_record`).

## 9. The next decision

**The converter author must decide what a graphics PSO's fixed-function state is while the 380..520-byte state blob is
undecoded.** The decoder deliberately returns no rasterizer/blend/depth-stencil/sample/topology values, so the choice
is explicit and belongs to the conversion: either refuse graphics PSOs until the state blob is decoded, or adopt a
documented default for every state field and record that a default was substituted. A silent guess here is as hard to
debug as a wrong format code. The input-layout formats (section 5) were that decision one measurement later: they are
decoded and mapped (docs/input_layout.md), so the fixed-function state blob is now the remaining decision of this kind.
