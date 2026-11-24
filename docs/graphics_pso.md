# The slot-57 graphics PSO backend: native stages and the record builder

**Status: wired and measured.** The proxy calls this at device slot 57 (docs/slot57_wiring.md); after the input-layout
element table was decoded (docs/input_layout.md), one 150 s boot builds **10,241 of 10,241** records into real PC
graphics pipelines with **0 refusals and 0 PC failures**, and the frame records **42,721 `SetPipelineState` / 42,722
`DrawInstanced`** (docs/input_layout.md section 6). This is the conversion layer for the console's deserialized graphics
pipeline record
(device slot 57): `port/src/graphics_record.{h,cpp}` decodes the record (docs/graphics_record.md), and this layer turns
it into the two things the PC side needs - a library entry for each native stage (`PipelineState::create_graphics`'s
native branch) and a `D3D12_GRAPHICS_PIPELINE_STATE_DESC` for the record (`rt::graphics_pso::build`). Nothing here calls
a device: `port/test/graphics_pso_test.cpp` builds every byte and a synthetic shader index, 159 checks, no GPU and no
game files. The proxy wiring is described in section 5 and measured in docs/slot57_wiring.md.

The record's remaining gap from docs/graphics_record.md is handled as the project requires - an explicit input, never a
silent guess:

* the **fixed-function state** (380..520-byte blob, no decoded word) is an input the caller fills; section 4 is the one
  documented default set, and the caller logs that it substituted it;
* the **input layout's element table is decoded** (docs/input_layout.md): every measured format word is mapped to its
  DXGI value, the six attribute words are the `D3D12_INPUT_ELEMENT_DESC` fields in D3D12 order, and a value the tree has
  not measured still **refuses by field with the value in the name**
  (`input_layout.elements[i].format[<word>]`) - no `D3D12_INPUT_ELEMENT_DESC` is built from a guess.

## 1. What is here

| file | what it is |
|---|---|
| `port/src/graphics_pso.h` / `.cpp` | `resolve_native_stage()`, `pick_entry()`, `documented_default_state()`, `GraphicsPsoInputs`, `build()`; the input contract and the refusal surface are in the header comments, the tables below. |
| `port/shims/d3d12_vulkan_device.cpp` | `create_graphics`'s stage loop: the native branch (section 2). The FSSM/DXBC path and the diagnostic stand-in path are unchanged; `pick_entry` moved to the header so compute and graphics share it. |
| `port/test/graphics_pso_test.cpp` | 159 checks over synthetic records, synthetic native programs and a synthetic `fspx/index.json`; the element-table cases are in docs/input_layout.md section 5. |
| `port/CMakeLists.txt` | the `graphics_pso_test` target (`ctest -R graphics_pso`), and `src/graphics_record.cpp` + `src/graphics_pso.cpp` in `palmetto_d3d12_com` because `d3d12_vulkan_device.cpp` now calls into the layer. |

## 2. The native branch in `create_graphics`

The title's precompiled pipeline-library graphics programs are native RDNA2 programs, not FSSM/DXBC
(docs/fspx.md, docs/pipeline_program_source.md). For each stage in the existing stage table (`ms` or `vs`, `gs`, `ps`)
whose `D3D12_SHADER_BYTECODE` holds no FSSM blob, `create_graphics` now calls
`rt::graphics_pso::resolve_native_stage()` where it used to refuse with "shader bytecode holds no FSSM program". Hull and
domain stages never reach the loop (refused earlier, as before).

**Identity.** The key is the SHA-256 of the program bytes as passed - the index's `fspx_code_sha256` - the same key
`create_compute` resolves compute programs with:

1. `palmetto::compute_program()` classifies the bytes: an FSPX payload or a bare kind-6 program gives its kind-6 bytes and,
   for a payload/container, its kind-0 dispatch state.
2. `library->program_candidates(stage, program, &ambiguous)` is asked under the record slot's stage name (`ps`, `vs`,
   `ms`; the slot gives the stage, nothing in the stage blob does).
3. A program that `compute_program()` cannot classify (no 36-byte FSPX prologue - the measurements find
   `21,463` of the library's `21,522` programs with it) is looked up by the **raw key**: the SHA-256 of the bytes
   themselves, which is the hash the graphics publication keyed its entries by. Only bytes the index actually holds are
   accepted as a program; a miss returns "not a native program", so the FSSM/stand-in path and its refusal apply.

**Entry selection.** `pick_entry()` (the rule compute already used) takes the first entry that fits and declares no set-0
bindings, else the first that fits, else the first without bindings, else the first. The per-entry `fits` test is
`create_graphics`'s existing one: a vertex stage's entry must have its input signature fed by the PSO's input layout
(`layout_feeds`); the other stages pass everything.

**Ambiguity is a refusal.** `program_candidates` returns no entry (with `ambiguous` set) when several translations of the
same kind-6 bytes differ in module or runtime metadata and the bytecode came without its kind-0 state, which is exactly
the slot-57 case: the record carries the program but not its kind-0 entry. The branch refuses with an `ambiguous:` line
naming the SHA-256, and a program published only for another stage gets a `stage mismatch:` line naming that stage. Both
are `E_INVALIDARG` with the reason in the log, never a picked module.

**What is built.** The entry is stored in the stage's existing `fssm` field - the same field the code base is uploaded
from, and for a native stage the bytes the pipeline copies are the kind-6 program, exactly what `create_compute` copies
(`program.bytes`). The rest of `create_graphics` then handles the stage as it handles a translated one: bindless set 0
when the entry declares set-0 bindings (the device's bindless set is required, as before), mesh output limits, at most one
code-base stage (`question 14`), `spirv()` module creation, `--f16-interp-fallback` linking, the conservative-raster and
writable-storage checks, and subgroup size requests. The cached-PSO key still hashes the stage bytecode, so a native
stage keys exactly like the bytes the title passed.

**The limits that now apply to native stages.** They are the existing `create_graphics` rules, unchanged: a VS and a PS
that both read their code base are refused (one push-constant slot holds one code base), a stage whose module writes a
storage image needs the device feature, a mesh stage needs `VK_EXT_mesh_shader` and its output arrays inside the device
limits. Whether the published graphics programs hit the code-base limit is unmeasured (section 6).

**Log lines** (once each, as the rest of the file does): `shader bytecode holds no FSSM or native FSPX kind-6 program`
(replaces the FSSM-only wording); `<stage> program has no FSPX prologue; it was found by the SHA-256 of its program
bytes (fspx_code_sha256): <sha>`; the resolver's `ambiguous:` / `stage mismatch:` / `no translation:` refusal lines.

The FSSM/DXBC path, the `PALMETTO_STANDIN_PIPELINES` diagnostic path and `create_compute` are unchanged in behaviour.

## 3. The builder

```cpp
bool rt::graphics_pso::build(const rt::graphics_record::Record& record, const GraphicsPsoInputs& inputs,
                             D3D12_GRAPHICS_PIPELINE_STATE_DESC* out, std::string* why);
```

`out` is untouched on failure; `why` names the field that stopped it (the table below). The record is what
`rt::graphics_record::decode()` returned; `inputs` is what the caller knows.

### The input contract

| input | who supplies it, and why |
|---|---|
| `root_signature` | The caller, mapped from `record.root_signature` (+0x00). The decoder copies only the console pointer, so the caller validates/maps it and passes the PC object; null is refused as `pRootSignature`. `create_graphics` uses it for the cached-PSO key only (the pipeline layout is built from the translated entries). |
| `rtv_formats[]`, `render_target_count` | The caller, from the render targets the PSO is created against. A used slot needs a format; slots at and above the count must be `DXGI_FORMAT_UNKNOWN`, as D3D12 requires. |
| `dsv_format` | The caller; `DXGI_FORMAT_UNKNOWN` for no depth-stencil attachment. The PC layer still validates that the format maps to Vulkan. |
| `defaults` (`FixedFunctionState`) | The caller: `rasterizer`, `blend`, `depth_stencil`, `topology_type`, `strip_cut`, `sample_count`, `sample_quality`, `sample_mask`. The console state blob yields none of these (section 4). |
| `stages[0..2]` | The caller: the resolved native program bytes per record slot (`record.stages[i].program`, or an FSPX payload carrying its kind-0 state when the caller has one). The caller resolves them (by SHA-256); `build()` cannot look a program up and does not pretend to: `create_graphics` resolves the bytes again. A record stage must have bytes, a slot the record does not use must have none. |

### What the record proves

* the record's shape: 1..3 stage slots, each slot named once, the PS/VS/MS set `decode()` accepts (a vertex and a mesh
  stage together is refused here too);
* the fixed-function **state blob's envelope only** (`decode_state`: 380..520 bytes, whole words). No word of it is
  used, by construction;
* the input layout (`decode_input_layout`): the `0x50`-byte header, the element count at `+0x4C`, and one record per
  element - a length-prefixed semantic name and a 24-byte attribute block - with the blob filled exactly
  (docs/input_layout.md section 1). All six measured shapes decode, from the `80`-byte empty layout to the `224`-byte
  four-element one, and every element becomes a `D3D12_INPUT_ELEMENT_DESC` in the description below.

### The refusal table (`why` names the field)

| `why` starts with | meaning |
|---|---|
| `record.stage_count` / `record.stages[i].slot` | not a graphics record's shape (no stages, an unknown slot, a slot twice, vertex+mesh) |
| `record.root_signature` | the record carries no root signature at +0x00 |
| `record.state` | the fixed-function blob is missing or outside its measured envelope |
| `record.input_layout` | the layout blob does not decode (count/name/attribute shape) |
| `input_layout.elements[i]...` | a decoded element carries a value the tree has not measured: `format[<word>]` (not one of the five measured DXGI values), `input_slot[n]` (only slot 0 measured), `input_slot_class[n]` (only per-vertex data), `instance_step_rate[n]` (only 0), `aligned_byte_offset[n]` (overlapping the preceding elements' measured sizes, or the append sentinel), `semantic_index[n]` (a duplicate semantic name and index). The exact table is docs/input_layout.md section 4 |
| `pRootSignature` | the caller mapped no PC root signature |
| `stages[i] (stage)` | a record stage has no resolved bytes, or gets bytes for a slot it does not use |
| `render_target_count` / `rtv_formats[i]` | an attachment list D3D12 would refuse (too many, a used slot without a format, an unused slot with one) |
| `defaults.*` | the caller's fixed-function state is not a valid value (`topology_type`, `sample_count`, `sample_quality`, `strip_cut`, `rasterizer.FillMode`, `rasterizer.CullMode`, `rasterizer.ConservativeRaster`) |
| `record.stages[].slot: a mesh stage` | a mesh stage cannot be expressed: a `D3D12_GRAPHICS_PIPELINE_STATE_DESC` has no mesh-shader field, so a record whose slot 2 is used needs the `D3D12_PIPELINE_STATE_STREAM_DESC` path (the `MS` subobject) instead |

The input-layout refusals decided the reach **before** the decode: in the wiring boot, 8,926 of 10,000 checkpoint
records refused at `record.input_layout` (docs/slot57_wiring.md section 2). After the decode the same histogram is
**empty** - every format word in all 10,241 records is one of the five measured DXGI values, so no element value
refuses, 1,021 layouts are empty and 8,979 carry elements (docs/input_layout.md section 6). The rows above are the shape
of the measurement if a future boot carries an unmeasured value: a field measurement, not a silent guess.

### The description it produces

Every field of `D3D12_GRAPHICS_PIPELINE_STATE_DESC` is set: `pRootSignature` from the caller; `VS`/`PS` from
`inputs.stages`; `DS`/`HS`/`GS` empty (a record has no such slot); `StreamOutput` empty; `BlendState`, `SampleMask`,
`RasterizerState`, `DepthStencilState`, `IBStripCutValue`, `PrimitiveTopologyType`, `SampleDesc` from
`inputs.defaults`; `InputLayout` from the decoded element table - `{nullptr, 0}` for the `80`-byte empty layout,
otherwise one `D3D12_INPUT_ELEMENT_DESC` per element in a per-thread buffer that is valid until the next `build()` on
that thread (docs/input_layout.md section 4); `RTVFormats`/`DSVFormat`/`NumRenderTargets` from the caller. `NodeMask = 0`, `Flags = D3D12_PIPELINE_STATE_FLAG_NONE`, `CachedPSO` empty: the
console record carries none of them, and each is the neutral value the PC runtime takes rather than a console value
guessed from the blob.

A pixel-only record builds a description (D3D12 expresses it); the PC layer's `create_graphics` currently requires a
vertex or mesh stage, so such a record is refused there - a separate, existing limitation.

## 4. The documented default set

`rt::graphics_pso::documented_default_state()` (checked field by field in the test, so this table and the code cannot
drift):

| field | value |
|---|---|
| rasterizer | solid fill, back-face culling, clockwise front, no depth bias/clamp/slope bias, depth clipping on, multisample rasterization off, antialiased lines off, no forced samples, conservative rasterization off |
| blend | off, alpha-to-coverage off, one blend state for all targets, neutral factors and ops (ONE/ZERO/ADD), every target writes every colour channel, logic op off |
| depth-stencil | off; valid API values kept: write mask all, `LESS`, full stencil masks, KEEP/ALWAYS neutral ops |
| topology type | triangle |
| index-buffer strip cut | disabled |
| sample description | count 1, quality 0, mask `0xffffffff` |

**It is a measurement stand-in, not the console's state.** It mirrors the values the graphics stand-in path
(`create_standin_graphics_pipeline`) already uses, and passing it is a decision the caller makes: `build()` writes exactly
the `FixedFunctionState` it is given (the test builds twice with different states and checks every field), and the wiring
logs the substitution once per record shape. A zeroed state is refused (`defaults.topology_type`, ...), not silently
turned into a default.

## 5. The proxy wiring

`port/shims/d3d12_xs_device.cpp`'s slot-57 handler now does this through `convert_slot57_record` (the measured
wiring is docs/slot57_wiring.md section 1); the sketch below is the shape it fills:

```cpp
rt::graphics_record::Record record;
if (!rt::graphics_record::decode(desc, rt::graphics_record::kDescriptorValidatedBytes, &record)) {
    std::fprintf(stderr, "[rt] d3d12_xs: slot 57: the record does not decode; refused\n");
    return E_INVALIDARG;
}

rt::graphics_pso::GraphicsPsoInputs inputs{};
inputs.root_signature = /* the PC root signature for record.root_signature, or null to be refused */;
inputs.render_target_count = /* the attachment stand-in: the slot-57 call carries no formats */;
inputs.rtv_formats[...] = /* as above */;
inputs.dsv_format = ...;
inputs.defaults = rt::graphics_pso::documented_default_state();
for (std::size_t i = 0; i < record.stage_count; ++i)
    inputs.stages[record.stages[i].slot] = {record.stages[i].program.data, record.stages[i].program.size};

D3D12_GRAPHICS_PIPELINE_STATE_DESC pc{};
std::string why;
if (!rt::graphics_pso::build(record, inputs, &pc, &why)) {
    std::fprintf(stderr, "[rt] d3d12_xs: slot 57: refused: %s\n", why.c_str());
    return E_INVALIDARG;
}
return self->real->CreateGraphicsPipelineState(&pc, *riid, out);
```

That last call is the PC device's own `CreateGraphicsPipelineState` (the proxy already forwards slot 67 the same way), so
the description lands in `PipelineState::create_graphics`, whose native branch (section 2) resolves the stage bytes this
call passes.

Two inputs belong to the proxy; neither comes from the call, and both are filled explicitly and logged once
(docs/slot57_wiring.md section 1):

* **the root signature**: `record.root_signature` is the title's live root-signature pointer; the proxy maps it against
  the PC root signatures the device handed out (directly, or through a title-side slot whose value is one), or passes
  null and takes the refusal. Measured: every one of the 10,000 checkpoint records names a tracked PC object
  (docs/slot57_wiring.md section 2). `create_graphics` only keys on it.
* **the attachments and sample count**: the slot-57 call does not carry them - RTV/DSV formats and the sample description
  live in the undecoded state blob. Until that is decoded, the only honest call is an explicit, logged documented
  attachment stand-in (one `R8G8B8A8_UNORM` render target, no depth-stencil, sample count 1 - the same family as the
  fixed-function default set), never a silent one. A wrong format here is a wrong pipeline: the replay creates the draw's
  image views with the PSO's format (`prepare_draw`, `viewed.format`), so the console's actual formats decide whether the
  channels are right.

## 6. What remains unmeasured

The post-decode build is measured in docs/input_layout.md section 6 and the wiring in docs/slot57_wiring.md section 2. The
next decode is the fixed-function state blob (section 4, the ledger's O15): it decides fidelity, not whether a record
builds - all 10,241 build with the documented stand-in - while the functional frontier after the draws appear is the
draw's resource binding (docs/replay_resolution.md).

* **Every fixed-function field** of the 380..520-byte state blob: rasterizer, blend, depth-stencil, sample description,
  topology, strip cut. That is what `GraphicsPsoInputs::defaults` substitutes, explicitly and logged.
* **Formats beyond the measured five.** The element table and its format words are decoded (docs/input_layout.md
  sections 2-3): words 2/6/16/28/30 are mapped in `kMeasuredInputFormats`, and any other word refuses as
  `input_layout.elements[i].format[<word>]`, so the next boot names it by element position and value. Words 2 and 30 are
  the DXGI reading of a value whose size is not independently offset-confirmed; a following element at an offset below
  that size refuses as the falsifier.
* **RTV/DSV formats and the sample count at slot-57 time** (section 5): no field of the record nor of the call carries
  them.
* **Whether the record's VS+PS pairs both read their code base.** The single-code-base limit (`question 14`) is unchanged;
  if both published entries set `uses_code_base`, the pair is refused. The 947 PS / 319 VS published entries' metadata
  would answer it without a boot.
* **Whether the published graphics entries declare set-0 bindings**, which needs the device's bindless set; and the
  translator's `uses_code_base` / mesh metadata for these programs was not read by this change.
* **No mesh program is published**: the measured records name 947 PS and 319 VS programs and no MS one, and the builder
  refuses a mesh record for the description's shape anyway (it needs the pipeline-state-stream path).
* **Whether every boot's records decode the same way.** The decode boot measured 1,021 empty layouts and 8,979 layouts
  with elements, and every record built (docs/input_layout.md section 6); a boot whose records differ reports the
  unmeasured values as the named per-value refusals above - a measurement, not a blocker on this tree.

## 7. Tests and gates

```
cmake --build port/build --target graphics_pso_test
port/build/graphics_pso_test.exe      # prints the input/refusal tables, exits 0
ctest --test-dir port/build -R graphics_pso
```

`port/test/graphics_pso_test.cpp` builds, byte by byte:

* the record (the same measured shape `graphics_record_test.cpp` uses), the six measured input-layout shapes (80 B/count
  0 through 224 B/count 4) and an unmapped format word;
* native programs: the 36-byte FSPX prologue plus filler, the same without the prologue, and an FSPX payload with kind 0;
* a shader directory (`index.json` + `fspx/index.json` + one module per entry) with entries for each program, two
  translations of one program with different modules (ambiguous) and two with the same module (not ambiguous).

and checks the resolver (accepts a classified program, the raw-key program, a payload matched on its kind-0 state;
refuses no-translation, stage mismatch and ambiguity; returns false for FSSM blobs and unknown bytes), `pick_entry`'s
rule, the documented default set field by field, the measured format table against the documented DXGI enum values, the
mapped input-element descriptions (formats, semantic indexes, stored offsets, slot, per-vertex class, step rate), the
format-2 size falsifier, and the builder: the full description from the inputs, the caller's state used verbatim (so no
silent default), a PS-only record, an FSPX payload passed through, and every refusal in section 3 with its field name.
159 checks, all passing (docs/input_layout.md section 5).

Gates run for this change: `_setup\build_palmetto.cmd` -> `PALMETTO_OK`, `_setup\build_tests.cmd` -> `TESTS_BUILT`,
`unimplemented_policy_test` (98 checks), `xs_proxy_mock_test`, `graphics_record_test` (117 checks) and
`graphics_pso_test` (159 checks) passing; `d3d12_fence_timing_test` still crashes as it did before this change (a known
pre-existing failure, not touched here).
