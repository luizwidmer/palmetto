# Slot 57 wired: the graphics record converted, the build histogram, and the draw path

**Status: wired and measured.** `port/shims/d3d12_xs_device.cpp`'s slot-57 branch decodes the deserialized
graphics pipeline record with `rt::graphics_record::decode`, builds the PC description with `rt::graphics_pso::build`,
and sends it to the PC device's own `CreateGraphicsPipelineState` exactly as slot 67 sends a compute description
(docs/graphics_pso.md sections 2-5). The wiring's own boot refused 8,926 of the first 10,000 records at the
input-layout blob; the element table was decoded next (docs/input_layout.md), and one post-decode 150 s boot now builds
**10,241 of 10,241 records with 0 refusals and 0 PC failures**, with the frame recording **42,721 `SetPipelineState` /
42,722 `DrawInstanced`** (docs/input_layout.md section 6). Every submit that carries one of those draws is then refused
by the replay layer on the title VA the draw binds, and registering that VA alone so the draw executes TDRs the GPU
(docs/replay_resolution.md).

## 1. The wiring

`proxy_create_graphics_pipeline_probe` keeps its record probe and, after it, calls `convert_slot57_record`:

1. **Read and decode.** The descriptor must be readable for the 0x108 bytes `decode()` validates; a descriptor that
   is not, or that `decode()` refuses, returns `E_INVALIDARG` and is counted as `record` / `decode`.
2. **Check what the decoder only points at.** The state blob, the input-layout blob and every stage's program bytes
   are range-checked for readability before the builder reads them; an unreadable one returns `E_INVALIDARG` with the
   field named (`record.state`, `record.input_layout`, `record.stages[].program`). Without this the layer would fault
   on a record pointer it cannot read.
3. **The inputs the call does not carry**, supplied explicitly and never silently:
   * **root signature** — the record's `+0x00`. The proxy does not wrap root signatures, so slot 16 now remembers every
     PC root signature the device hands out, and slot 57 passes `+0x00` on only when it is one of those objects
     (directly, or held in a title-side slot whose value is one). The mapping is proved against the table, not guessed
     from the pointer; anything else is left null and refused by the builder as `pRootSignature`. This is safe because
     `create_graphics` keys the cached PSO on `pRootSignature`, so it dereferences the pointer.
   * **attachments** — the call carries no RTV/DSV formats or sample count, and they live in the undecoded state blob.
     One documented, explicitly logged stand-in: **1 x `DXGI_FORMAT_R8G8B8A8_UNORM`, no depth-stencil, sample count
     1**. Logged once (with the state substitute below), never per record.
   * **fixed-function state** — `rt::graphics_pso::documented_default_state()`, the project's documented measurement
     stand-in (docs/graphics_pso.md section 4), logged once as a stand-in.
   * **stage bytecode** — each record stage's own native program span in its record slot (0 PS, 1 VS, 2 MS); the
     backend resolves it by SHA-256 in `create_graphics`'s native branch (docs/graphics_pso.md section 2).
4. **Build, then forward.** `build()` refuses by `why` field; the branch counts the field, logs the first occurrence
   with the record's fields, and answers a refusal that names a conversion path this tree has not built with
   `E_NOTIMPL` (a mesh stage needs the pipeline-state-stream description; an input-layout element value no measurement
   pins names the field and its value) and a malformed record with `E_INVALIDARG`. The records the measured boot carries
   build - the refusal histogram is empty (section 2). A built description goes to
   `self->real->CreateGraphicsPipelineState(&pc, *riid, out)` — the title's `riid` and out pointer, the real HRESULT
   back. It is not the label of a conversion: a PC failure is the title's HRESULT.
5. **The measurement.** Per refusal: count by `why` field, distinct record digest and record stage shape; first
   occurrence logged with state/layout sizes, element count and program sizes. Per built record: the PC HRESULT, counted;
   first success and first failure of each code logged. A compact histogram prints every 2,000 slot-57 calls (the last
   checkpoint is the table below: a boot makes 10,241 calls, so the 10,000-call checkpoint covers 97.6% of them).
   The input-layout blob is also described once per (size, count) shape: the element-count word at `+0x4C`, and the
   printable runs with their byte offsets. That description was the measurement that decided the element-table decode
   (section 3); on this boot it is a cross-check, not an open question.
   A `PALMETTO_XS_SLOT57_LAYOUT_DUMP=1` switch (off by default, measurement only) prints the first 176 bytes of one blob
   per shape; it was used for the measurement and its bytes are not quoted here.

`PALMETTO_STANDIN_PIPELINES` still works: a refused record can be handed the project's own diagnostic stand-in, so a
boot with the switch on answers the old question ("is the missing PSO the blocker?") without changing what the default
boot does.

## 2. The histogram

One boot: retail image, shader corpus, Vulkan backend, `PALMETTO_SKIP_INT3=1`, `PALMETTO_SEED_GLOBAL=75DF268:D8`; one
boot at a time, 150 s timeout, a boot runs to the timeout. Harness `tooling/boot_report.py`, profile
`_setup/profile_boot.py`. The "before" column is this page's wiring measurement, the same tree before the element-table
decode (docs/input_layout.md section 6 has the full before/after boot table and the layout-shape histogram).

| | wiring boot (before the decode) | post-decode boot |
|---|---:|---:|
| verdict | ALIVE 150.2 s, no exception | **ALIVE 150.2 s, no exception** |
| traced D3D12 calls | 1,410,454 | **1,503,369** |
| slot-57 calls / distinct records | **10,241 / 10,241** | **10,241 / 10,241** |
| record decode refusals | 0 | 0 |
| built (`CreateGraphicsPipelineState` reached) | **1,074** | **10,241 of 10,241** |
| PC pipelines created (`S_OK`) | **1,074** | **10,241** |
| refused before the call | **8,926** | **0** |
| PC `CreateGraphicsPipelineState` failures | 0 | 0 |
| `SetPipelineState` | 0 | **42,721** |
| `DrawInstanced` / `DrawIndexedInstanced` | 0 / 0 | **42,722 / 0** |

The wiring boot's refusal histogram, all 10,000 checkpoint calls, had exactly one row: `record.input_layout` **8,926**
(ps+vs 8,923, vs 3) against **1,074** built (ps+vs 1,073, vs 1). The only layout shape that could build was the 80-byte
empty one (count 0, 1,074 records); every layout with elements refused, because the committed decoder's contiguous-names
element encoding did not match the blobs the boot carries (section 3). The post-decode histogram is **empty**: every
format word in all 10,241 records is one of the five measured DXGI values, 1,021 layouts are empty and 8,979 carry
elements (docs/input_layout.md section 6).

The gates at the same 10,000-call checkpoint, wiring boot, say the same thing: record `+0x00` is a PC root signature
object **10,000** (through a title-side slot 0, untracked 0, null 0); stage program bytes unreadable 0; input layouts
that decode with elements 0, empty 1,074, undecodable 8,926; fixed-function state stand-in used 10,000 and attachment
stand-in used 10,000 - so neither stand-in refused anything. Post-decode: input layouts empty 1,021, with elements 8,979,
undecodable 0; root signature a PC object 10,000; unreadable stage bytes 0; PC results `S_OK` 10,000.

### Boot outcome

The conversion is real and cheap: the frame loop keeps its submit rate, every record builds, and the title records its
draws for the first time - `Close` succeeds 86,059 times with 0 failures, the run is ALIVE for the whole timeout, and
`ExecuteCommandLists` is attempted 85,444 times (34,134 per 60 s). But no draw executes: 42,724 submits succeed and
**42,720 fail, one per recorded draw**, all with `ExecuteCommandLists failed: unknown resource; nothing was submitted`
from the replay layer. That refusal - not the layout, which is decoded - is the next functional frontier, and it is the
draw's resource binding: docs/replay_resolution.md sections 1-3 name the check (`buffer_at` cannot cover the title VA
inside the title-placed 174 MiB buffer) and the measured wall behind it (the title-VA address space, docs/issues.md
O16/O17).

## 3. The layout blobs are an element table

The boot carries six layout shapes: 80 B/count 0, 120/1, 152/2, 156/2, 188/3 and 224/4. Their printable semantic names
are not a contiguous table at `+0x50`: each sits inside a per-element record, the first at `+0x54`, one element after
another, and the word at `+0x50` before the first name is that element's length word including its NUL. The committed
contiguous-names decoder accepted a shape whose total is 4 bytes larger than every measured blob, so it could not decode
any non-empty layout - the 8,926 refusals above.

That measurement fixed the structure the decode needed: a `0x50`-byte header, the element count at `+0x4C`, then a
length-prefixed name and a 24-byte `D3D12_INPUT_ELEMENT_DESC` attribute block per element, the records zero-padded to a
4-byte boundary and the blob filled exactly. The decode, the refutation of the contiguous-names reading, the DXGI
format words (2/6/16/28/30) and the per-field refusal names are docs/input_layout.md; the record-level view is
docs/graphics_record.md section 5. The blob bytes themselves are not stored in the repository.

## 4. The next decode, and the smallest next change

**Next decode: the fixed-function state blob** (the ledger's O15), not the layout. The element table is decoded and every
record builds, so the fields still on stand-ins are the rasterizer, blend, depth-stencil, sample description, topology,
strip cut and the attachment formats/sample count that only the state blob carries (docs/graphics_pso.md sections 4-6,
docs/graphics_record.md section 4). The smallest next change there is to decode one field at a time and histogram it over
the same 10,241 records, as the layout decode did - a field measurement, not a boot refusal.

The functional frontier the draws expose is separate and higher priority: every draw-carrying submit is refused on the
title VA the draw binds (docs/replay_resolution.md). The single-candidate VA registration removes **all 43,750
refusals** in the later replay baseline boot, but the first executed draws TDR the GPU at 22.95 s because the un-rebased
title VA is not an arena address; until the title-placed pages are imported and their VAs rebased (docs/issues.md
O16/O17), refusing the submit is the honest behaviour, and executing it is a device reset (docs/replay_resolution.md
sections 4-5).

## 5. Tests and gates

`port/test/xs_proxy_mock_test.cpp` compiles the real `src/graphics_record.cpp` and `src/graphics_pso.cpp` into the
GPU-free test (its record builder and refusal contract stay unit-tested by `graphics_record_test` and
`graphics_pso_test`), and covers the new branch: a valid PS+VS record building and forwarding the description (record
programs in VS/PS, `pRootSignature`, 1 x R8G8B8A8_UNORM, no depth, sample 1, empty input layout, documented default
state, title `riid` and out pointer), a root signature named through a title-side slot, a measured element layout whose
format word is unmeasured refused as `E_NOTIMPL` (`input_layout.elements[0].format[7]`), a mesh record refused as
`E_NOTIMPL`, a missing and an untracked root signature refused as `E_INVALIDARG`, a non-decoding record refused as
`E_INVALIDARG`, and a failed PC call returned unchanged with no object. The mock's device slot 10 is
`CreateGraphicsPipelineState` and slot 16 `CreateRootSignature`; the other mock kinds keep the generic forwarded call on
those slots (the console queue forwards onto its real object's 10). The decode's own test additions are
docs/input_layout.md section 5.

Gates run for this change:

```
_setup\build_palmetto.cmd  -> PALMETTO_OK
_setup\the test build script   -> TESTS_BUILT
xs_proxy_mock_test       -> PASS (9 allocations refused on purpose)
unimplemented_policy_test -> 98 checks, 0 failed
graphics_record_test     -> 117 checks passed
graphics_pso_test        -> 159 checks passed
d3d12_fence_timing_test  -> still crashes (known pre-existing failure, untouched)
```

No git writes. Scope: `port/shims/d3d12_xs_device.cpp`, `port/test/xs_proxy_mock_test.cpp`, this file (the decode
is a separate change, `d0095ca70`).

## 6. Follow-ups this measurement leaves open

* **The fixed-function state blob** (O15): the only record field still on a documented stand-in; decoding it decides
  fidelity, not whether a record builds.
* **The recorded draws do not execute.** They are refused on the title VA (`unknown resource`, one per draw); the
  candidate VA registration removes every refusal but TDRs the GPU. The next work is the title-placed page import and
  the VA rebase (O16/O17): docs/replay_resolution.md sections 4-5.
* **Presentation is a separate frontier**: the title's own display surface carries a null resource, so the frame has
  nothing to present (docs/display_path.md).
* When this page was written, docs/graphics_pso.md said "backend, not wired" and the root README's tracking block said
  slot 57 is not wired. docs/graphics_pso.md is repaired in the same documentation pass; the root README's block is
  generated from docs/progress.json and stays stale until that is refreshed (outside this change's scope).
