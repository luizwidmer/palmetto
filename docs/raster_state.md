# The rasterizer cull axis: the slot-57 state record does not yield it, and the title's own draw is the falsifier

**Status: measured.** The 380..520-byte state record's rasterizer state is **not decodable** from this title's data
(re-derived here: the container decodes, and the only word whose value stays inside the D3D12 cull enum is the record's
stage-set word at `+0x2C`, not a per-pipeline value; section 3). The cull axis alone is decidable by *observation* of
the title's own draw, and the decisive PS-only-control measurement at an early draw **ran** once the tree's
reader-writer deadlock was fixed: with the substituted variant's cull mode off, the constant pass's own recorded
vertices paint exactly half of its 4K target (`mean 16 80 120, non-black 4,147,200/8,294,400, peak 240`, the diagonal
half-screen triangle; section 2). Under the documented stand-in (`CULL_BACK`, clockwise front) the same triangle is a
back face, which is the measured reason its target stayed zero. What is implemented is the narrow piece the policy
allows while the field's word stays refused: a measured cull axis that replaces **only** the documented stand-in's cull
pair, off by default, logged once (`PALMETTO_RASTER_CULL_MEASURED=1`), tested against the project's behavioural-probe
contract; the record's eight fields stay refused and the record decode stays negative. First light of the *presented*
frame is not claimed: the pass's own colour path and the compose pass's empty matrix A are still open (section 6).

## 1. Why this measurement

[vertex_constant_feed.md](vertex_constant_feed.md) measures the vertex/transform feed reaching the constant pass's
vertex stage: the driver-block V# now decodes to the bound view (`address 0x6aef30 stride 24 records 170 format 22`,
[frame_first_light.md](frame_first_light.md) section 6), so the fetched clip positions are the title's own vertex data
(-1,1,0 / -1,-1,0 / 1,-1,0 with `w = 1`) instead of the degenerate `(0,0,0,0)` the earlier boots measured. The pass's
target still reads zero. The named hypothesis is fixed-function state: the record's state blob is undecoded, the wiring
builds every pipeline with the documented stand-in (`CullMode = D3D12_CULL_MODE_BACK`, `FrontCounterClockwise = FALSE`,
[docs/graphics_pso.md](graphics_pso.md) section 4), and the pass's triangle is counter-clockwise in D3D screen space,
which under that stand-in is a back face and is culled before the pixel stage.

This page has three jobs: re-run the decisive measurement that tells a culled winding from a degenerate vertex path;
re-derive what the record's blob actually pins (it is nothing new, and the census is repeated here so the negative is
this slice's own); and, if the cull axis is measured, implement exactly that field without inventing the others.

## 2. The decisive test: coverage appears with culling off

The test is already implemented in the tree (`port/src/vulkan_backend.h`, the PS-only control): draw the armed draw's
own recorded command through the title's pipeline with only the pixel shader replaced by the project's constant-output
`fs_solid`, so coverage becomes a known colour whatever the title pixel shader would write; run it with
`PALMETTO_FRAME_DUMP_PS_CONTROL_CULL_NONE=1`, which changes only the substituted variant's cull mode. Non-black
coverage proves the title's vertex stage and rasterisation produce fragments; black moves the question to the fetched
vertices or the pixel stage.

| attempt | configuration | result |
|---|---|---|
| earlier slice, `boot-cullnone` | both feeds + PS control, cull none, early readback 100/101 | **stalled**: 14 submits / 6 presents, flat to +150 s |
| this slice, `boot-cullnone` | the same + full-target readbacks, 8 captures, frozen pre-slice binary | **stalled**: 106 submits / 52 presents from +30 s; the last lines are the frame dump of draw 100/101 and the backend's `order: submission 105 ... the readback-armed draw is here`; the next expected line, `frame dump PS control: drew ...`, never appears |
| this slice, `boot-measured` | the current tree's binary, the same feeds/readback but no PS control | **stalled** at the same submission; stderr's last line is `readback armed at draw 100`; the boot's pipeline-budget line reads `2 live, 2 built` at the first frame-pipeline bind |
| **this slice, `boot-cullnone3` - the decisive test** | the current tree's fixed binary, both feeds + PS control + cull none, early readback 100/101, `PALMETTO_FRAME_DUMP_SAMPLE=0` and `PALMETTO_VERTEX_PATH=0` (see below) | **ALIVE 150.2 s, 4,016 submits / 2,007 presents at +140 s; coverage measured** |

The stall's cause was named by the stall diagnostic slice with cdb all-thread stacks: the replay runs inside
`VulkanQueue::execute_now`, which holds `resource_mutex_` shared for the whole submission, and the frame-dump /
vertex-path device-memory callbacks re-acquire that same lock shared from the same thread; with a title compute-PSO
creation (and, earlier, the lazy-pipeline path) queued as an exclusive writer, the nested shared acquire blocks forever
- a reader-writer deadlock with no device lost and no exception. The backend's `heap_base()` now reads its arena address
without taking the lock (lock-free, the deadlock named in its comment), and the lean arm above also drops the two
diagnostics whose callbacks re-enter it; that combination is what let the decisive test run. A third failure mode was
also observed on the churning tree and is *not* this slice's: 4 of the 8 boots attempted here died at 33-40 s with an
identical title-side access violation (`game+0x2215C35`, `write 0x0`) before any graphics pipeline existed (also in a
boot whose configuration the successful one shares); each arm was retried.

What the decisive boot measured:

```text
[rt] replay: frame dump PS control: drew the armed draw's own vertices through the title pipeline
     (0x1fb029630d0) with fs_solid as the pixel shader, into 0x1fb9e120dc0; the readback below copies it
[rt] replay: frame dump PS control readback [draw 100 target resource 0x1fb9e120dc0]:
     3840x2160 at the target's offset of 0,0 of 3840x2160, 4-byte texel: mean 16 80 120,
     non-black 4147200/8294400, peak 240, alpha mean 127, alpha non-black 4147200/8294400, alpha peak 255
[rt] replay: full-target tile map [draw 100]: 8x8 non-black counts
     row 0: 64800 0 0 0 0 0 0 0
     row 1: 129600 64800 0 0 0 0 0 0
     ... one half tile then the full tiles, stepping one column per row: the diagonal half-screen triangle
[rt] replay: frame dump PS control readback [draw 101 target resource 0x1edb2435310]: mean 0 0 0,
     non-black 0/8294400, peak 0, alpha non-black 0/8294400, peak 0
```

Read as a statement: **the constant pass's own recorded vertices, input layout and rasterisation produce fragments** -
half of the 4K target, the exact area of a triangle whose corners are (-1,1,0), (-1,-1,0), (1,-1,0) with `w = 1` - and
the constant-output pixel shader paints them (`fs_solid`'s colour, peak 240, alpha 255). The compose pass's own draw
(draw 101) stays empty in the same arm, as expected: its vertex stage multiplies fetched vertices by matrix A, and
matrix A's source is still empty ([root_arguments.md](root_arguments.md) section 7). Since the substituted variant
differs from the pipeline the stand-in built **only** in its cull mode, and the documented stand-in's
`CULL_BACK`/clockwise front is exactly the state under which this counter-clockwise triangle is a back face, the cull
hypothesis is measured: the stand-in's cull mode is what discarded the pass. The direct cull-back control arm is the
same substitution without the override (section 5; the tree's 33 s title-side flake forced retries).

## 3. Re-deriving the record state: the container decodes, the cull field does not

The record is a container, not a D3D12 description ([state_blob.md](state_blob.md), [graphics_record.md](graphics_record.md)):
40-byte header, 76/160-byte stage block, 168-byte shared block, variable region. `rt::graphics_record::decode_state`
proves the envelope and keeps every word raw; `rt::graphics_pso::decode_fixed_function_state` refuses all eight
fixed-function fields by name, so the wiring substitutes `documented_default_state()`.

The census behind that refusal is repeated here on the same measured corpus - the pipeline library's **5,680 distinct
state blobs** referenced by its 25,471 graphics records (read with `tooling/shader_library.py` outside the repository):

| question | measured answer |
|---|---|
| blobs whose container decodes | **5,680 of 5,680** |
| aligned words whose value stays inside the D3D12 cull enum (`0..3` incl. the invalid `0`) on **every** blob | **one**: `+0x2C`, values `1..2` |
| what `+0x2C` actually is | it takes one value for every record of every stage set the library holds (`2` on all 13,383 PS+VS, 10,726 PS+MS, 1,336 MS-only and 24 VS-only records) and a different one (`1`) on exactly the 2 records that carry a slot-6 stage - it tracks the record's stage set, the only structural difference in the corpus, so it cannot be the per-pipeline cull mode |
| aligned words per D3D12 rasterizer enum (`FillMode` 2..3, `CullMode` 1..3, the six 0/1 flags) | `FillMode`: **0**; `CullMode`: **1** (the container word above); each 0/1 flag: 49 (the false-positive background, [state_blob.md](state_blob.md) section 9) |
| byte/nibble extraction over the 380-byte minimum container | `CullMode` 3, `FillMode` **0** ([state_blob.md](state_blob.md) section 9) |

Read as a statement: the blob does not store the D3D12 enum where this decoder can pin it. The `FillMode` enum having no
place at all is the strongest single argument - a record that stored the PC enum would have to store its fill mode
somewhere - and the one word that accepts the cull domain is explained by the record's stage set. A decode that picked
one of the remaining places would be a guess about the driver's state order, not a measurement of this title, and the
project's policy is explicit that a guess is refused ([graphics_record.md](graphics_record.md) section 9,
[state_blob.md](state_blob.md) section 3). The eight-field refusal therefore stays exactly as it is. What would lift it
is the serialized `pState` layout (section 6), not another value pattern.

## 4. What we implemented: the measured cull axis, and nothing else

The one field a measurement can decide without the blob's words is the cull axis, because a pass's own draw is an
observation: the title's own triangle is drawn under cull NONE and is a back face under the stand-in's
`CULL_BACK`/clockwise-front, so the record's rasterizer state is not the stand-in's. The PS-only control above is the
measurement that made this a decided fact (section 2), and it is applied as an explicit, opt-in substitution rather
than as a blob decode because the observation cannot name the record's word. The implementation is:

* `port/src/graphics_pso.{h,cpp}`:
  * `measured_cull_axis_enabled()` reads `PALMETTO_RASTER_CULL_MEASURED` once (off unless set to a truthy value);
  * `apply_measured_cull_axis(FixedFunctionState*, bool)` is the pure decision: it sets **exactly**
    `rasterizer.CullMode = D3D12_CULL_MODE_NONE` and `rasterizer.FrontCounterClockwise = FALSE` and returns whether it
    changed anything. Every other field - solid fill, depth bias/clamp/slope bias, depth clipping, multisample,
    antialiased lines, forced samples, conservative raster, blend, depth-stencil, topology type, strip cut, sample
    description - is untouched;
  * the zero-argument overload applies the env decision; `documented_default_state()` calls it last and logs the
    substitution once (`[rt] graphics_pso: measured cull axis: ...`). With the switch off the returned set is
    byte-identical to the documented stand-in, which is what every default boot carries.
* The value applied is cull **NONE**: the observation can only tell the stand-in apart from the three states that keep
  the triangle (`NONE`, `BACK` with counter-clockwise front, `FRONT` with clockwise front), and NONE is the only one of
  the three that adds no winding rule the record has not yielded. Picking a front-face flip would be a second guess.
* The slot-57 wiring is unchanged: it already passes `documented_default_state()` into `build()`, so the measured axis
  reaches pipeline creation through the same path, and the record's eight fields are still refused by name.

Switch policy ([upstream_port.md](upstream_port.md) section 4): the switch is **off by default**. The decisive boot
proves the stand-in's cull mode is what discarded the pass, but it does not pin the record's value among the three
states that keep the triangle (section 6, item 2), so the measured axis stays an opt-in experiment; what would make it
the default is a per-record value - the serialized layout - or a boot whose A/B over the whole frame range shows the
NONE substitution moves nothing else.

## 5. Boots

One boot per arm under the shared boot lock, 150 s each, the standard environment plus the read-only frame
diagnostics (`PALMETTO_FRAME_DUMP=1`, `PALMETTO_FRAME_DUMP_AT_DRAW=100`, `PALMETTO_READBACK_AT_DRAW=100,101`,
`PALMETTO_READBACK_FULL=1`, `PALMETTO_CAPTURE` every 300 presented frames, at most 8), and the two measured feeds
(`PALMETTO_VERTEX_STREAM_DESCRIPTOR=1`, `PALMETTO_GRAPHICS_CONSOLE_ROOT=1`). The decisive arm uses the current tree's
binary (SHA-256 `63D7A8CBEC01E5D8ED188FE86254ABD6C649C3B69C00B08A6ACD2163FB00A2E4`) and replaces the two device-memory
diagnostics with `PALMETTO_FRAME_DUMP_SAMPLE=0` and `PALMETTO_VERTEX_PATH=0` (the lock workaround, section 2).

| arm | binary / switches | verdict | target `B` (draw 100, full 4K) | target `A` (draw 101, full 4K) | captures |
|---|---|---|---|---|---|
| `boot-cullnone` (earlier slice) | pre-slice frozen binary, PS control, cull none | **stalled** 14 submits / 6 presents | not reached | not reached | 1/1 black |
| `boot-cullnone` (this slice) | pre-slice frozen binary, PS control, cull none, all diagnostics | **stalled** 106 / 52 from +30 s | not reached (the frame dump and the order line are the last output) | not reached | 1/1 black |
| `boot-measured` (this slice) | current binary (pre-lock-fix), no PS control, measured axis, all diagnostics | **stalled** at the armed submission, last line `readback armed at draw 100` | not reached | not reached | 1/1 black |
| `boot-cullnone3` - **the decisive test** | current fixed binary, both feeds, PS control, cull none, `SAMPLE=0`/`VERTEX_PATH=0` | **ALIVE 150.2 s, 4,016 submits / 2,007 presents at +140 s** | **mean 16 80 120, non-black 4,147,200/8,294,400, peak 240, alpha mean 127, alpha non-black 4,147,200, alpha peak 255** (the control's `fs_solid` coverage) | mean 0 0 0, non-black 0/8,294,400 (the control draw is empty: the compose pass's vertex stage is degenerate) | 8/8 black (frames 1..2,101; the control paints only the armed frame's `B`, which the compose pass still cannot show) |
| stand-in with the real pixel shader (earlier slice, [vertex_constant_feed.md](vertex_constant_feed.md) section 5) | both feeds, no control | alive, but the pass writes nothing | mean 0 0 0, **0/8,294,400** | mean 0 0 0, **0/8,294,400** | 8/8 black |
| `boot-cullback-lean` / `boot-measured3` / retries | the cull-back control and the measured axis with the real pixel shader | **did not run to the readback**: the tree's 33 s title-side access violation (section 2) killed four of the eight boots attempted, and the re-queued arms did not get the single boot slot again before this page was written (other slices held it) | - | - | - |

Read as a statement: with the cull mode off, the armed draw's own vertices and rasterisation paint **exactly half** of
the pass's 4K target in the substituted pixel shader's colour (the area of the (-1,1,0)/(-1,-1,0)/(1,-1,0) triangle),
and the sibling draw (whose vertex stage multiplies by the still-empty matrix A) paints nothing in the same arm - so
the control path, its readback and the pass's raster path all work, and culling is the difference between the stand-in
and coverage. Under the stand-in with the real pixel shader the same target reads zero (the earlier slice's arm). What
the real pixel shader writes once its geometry is drawn is the still-open question (section 6, item 3); the cull-back
and measured-axis retries were killed by the title-side flake and the boot-slot queue, and their outcome does not change
the cull conclusion above, only the colour frontier.

Evidence outside the repository: `D:\leonida-work\raster\boot-cullnone3\` (the decisive boot: `boot\stderr.log`,
`boot\arm_meta.txt` with the frozen binary's SHA-256 and the exact switch set, `capture\` with the 8 PPMs),
`boot-cullnone\`, `boot-measured\`, `boot-cullback-lean\`, `boot-measured3\` (the stalled/crashed arms),
`bin\palmetto-raster.exe` (the measured binary), and the analysis script `analyze_boot.py`.

## 6. Limits and the next falsifiers

1. **The record's cull field still has no place.** The serialized driver `pState` layout (or a console driver trace)
   is the only source that would pin it; a behavioural observation cannot name the word it came from
   ([state_blob.md](state_blob.md) section 9). This slice adds no mapping and keeps the container's words raw.
2. **The measured axis is one of three consistent states.** The observation separates the stand-in (`CULL_BACK`,
   clockwise front) from the three states that keep the triangle; it cannot tell `NONE`, (`BACK`, counter-clockwise
   front) and (`FRONT`, clockwise front) apart. NONE is the minimal choice; the layout is the falsifier.
3. **The constant pass's own colour is the next gate.** Its pixel shader has no image reads; its output is built from
   data it fetches through the `s8` descriptor table, and the table's V# slot is the title's 96-byte block, which reads
   back all zero in the measured boots; the dword the pixel stage reads beyond its table-only signature is
   driver-defined and has no measured source ([root_arguments.md](root_arguments.md) section 9). The decisive boot
   shows the geometry rasterises when the pixel stage is replaced by a constant output, so a real-shader arm that stays
   black after the cull axis is enabled names the colour path, not the raster.
4. **The compose pass's matrix A is still empty** ([root_arguments.md](root_arguments.md) section 7): slot 70's
   per-frame publication is refused, so the presented target is expected to stay black even when the constant pass's
   target gains content. First light for the presented frame needs both this slice's raster gate and matrix A.
5. **The measured axis is a pass-level observation.** It is applied to every record the slot-57 wiring builds when the
   switch is on, including content pipelines whose rasterizer state may genuinely cull back faces; that is why it is
   opt-in and why a per-record decode is the default criterion.
6. **Measurement limits this slice hit.** Any arm that feeds and arms a device-memory read stalled on the pre-fix tree
   (reader-writer deadlock, section 2; fixed in the tree mid-slice); the lean workaround is what made the decisive
   boot possible. A title-side access violation (`game+0x2215C35`, write `0x0`, 33-40 s, before any graphics pipeline,
   identical in four boots including configurations that also ran) made roughly half of the attempts unusable; the
   failing arms are recorded above rather than averaged away.

## 7. Tests and gates

* `port/test/graphics_pso_test.cpp` gains `test_measured_cull_axis()`: the explicit decision off is byte-identical to
  the documented stand-in; on changes exactly the two cull fields and leaves every other field (the same roster the
  documented-defaults test checks) alone; and the project's own winding-pair probe tells the two states apart - the
  stand-in culls one half (1 culled, 4 fragments), the measured axis draws both (0 culled, 8 fragments). The
  eight-field refusal test is unchanged and still covers the record side.
* Results: `graphics_pso_test` **272 checks passed** (was 263), `graphics_record_test` **158 checks passed**,
  `frame_dump_test`, `xs_proxy_mock_test`, `unimplemented_policy_test` (98 checks), `xbox_present_test`,
  `buffer_va_index_test`, `dstorage_test` (224 checks) and `xapu_chad_test` (65 checks) pass; the quarantined
  `d3d12_fence_timing_test` is untouched.
* Gates on the tree the measurements ran on (HEAD `61f544a1b` plus this slice's files and other slices' uncommitted
  work): `_setup\build_gta6rt.cmd` -> `PALMETTO_OK`, `_setup\build_tests.cmd` -> `TESTS_BUILT`; ten of the twelve
  documentation checks pass. The two failures are other slices' work, not this page: `test_docs_cli_flags.py` fails
  pre-existing on `interval option` in [frame_first_light.md](frame_first_light.md) line 318, and `test_docs_env_vars.py`
  fails on `PALMETTO_STAGE2_TRACKER` read by the stage-2 slice's uncommitted `port/src/main.cpp:2504`. This page's own
  checks (links, tables, lists, encoding, repeated lines, repo paths, table ids) pass, and the new switch is named here.
* Uncommitted files this slice: `port/src/graphics_pso.h`, `port/src/graphics_pso.cpp`,
  `port/test/graphics_pso_test.cpp`, `docs/raster_state.md`. The tree also carries other slices' uncommitted work
  (bindless, memory growth, the lazy-pipeline slice); the measured binaries are frozen copies in
  `D:\leonida-work\raster\bin` with their SHA-256 in each boot's `arm_meta.txt`.

## 8. Cross-check and attribution

The public reference for the surrounding slot work is the GPL-3.0 project
[ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida), at the checkout `D:\leonida-work\leonida-fork`
and the commit [vertex_constant_feed.md](vertex_constant_feed.md) section 4 records (`ed94fbef9`, the post-story-runtime
merge); the files read for this cross-check are `runtime/shims/d3d12_xs_pso_stream.{h,cpp}` and
`runtime/shims/d3d12_xs_native_pso.h`.

* **What agrees**: the reference carries fixed-function state as **PC subobjects** - it captures the slot-86
  `D3D12_PIPELINE_STATE_STREAM_DESC` (a parser for `RASTERIZER`, `RASTERIZER1/2`, `BLEND`, `DEPTH_STENCIL*`,
  `PRIMITIVE_TOPOLOGY`, ... with the console stream's own subobject numbering) and patches a copy of that stream with the
  slot-55 derived records (tag 1 = `D3D12_RASTERIZER_DESC`). Its measured agreement with this tree's slot-55 record
  layout (tags 0/1/2/3, payload at +4, 0x14c-byte records) is independent support for the same console ABI.
* **What differs, and why this tree cannot use it for the frame's draws**: in this tree slot 86 is a beyond-table stub
  (answered `S_OK` without writing the out pointer) and the frame's pipelines are created from slot-57 records
  ([slot57_wiring.md](slot57_wiring.md), [frame_first_light.md](frame_first_light.md) section 3), whose state blob is
  the serialized driver state, not a PC stream. Slot 55 derivations on this title carry parts 0 and 2 only
  ([device_slot55.md](device_slot55.md)), so no rasterizer patch exists for the frame's pipelines. The reference
  therefore has no rasterizer carrier for these draws either, which matches this page's negative.
* No code, comment text, identifier or table was copied from the reference; the reference remains the copyright of its
  contributors under GPL-3.0, and its licence continues to apply to its own tree. The `NOTICE.md` entry for this slice
  is the lead's to add.
