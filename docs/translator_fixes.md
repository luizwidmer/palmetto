# Translator fixes measured against the compute request union

**Status: measured, uncommitted translator slices.** Six translation-coverage fixes and the loop guard, implemented from
this tree's own analysis of the corpus, have measured effects on the title's later compute-pipeline request union (the
668 programs `corpus2/cov` asks for; [compute_slot67.md](compute_slot67.md)) and on the two corpora that already
translated completely. The gap analysis that ranked them is `translator_gap_plan.md` in the work folder; this page
holds the implemented state, its numbers, its tests and its limits. No program bytes, package paths, content hashes or
per-program identifiers are reproduced here: aggregate counts and refusal names only.

## 1. Why these fixes

The boot corpus has no translator gap: the 1,266 boot graphics programs and the 29 FidelityFX programs translate and
validate (measured again by the earlier slices, below). At the start of this fix series, the request union's stable tree
translated **444 (66.5%)** of 668 programs and refused 224; every refused program is a slot-67
`DeserializeComputePipelineStateX` refusal at run time. The later sections measure successive changes; the latest scan is
in section 15. The refusal classes are one problem reached from different sides: the listings come from `llvm-mc`, which
skips a word it cannot decode and keeps printing, so a program with a rejected word drifts out of alignment with its
blob; the translator re-anchors and refuses what it cannot place.

Two of the three fixes are small, self-contained refusals that were stopping 36 programs (25 + 11). The third is not
coverage at all: it is termination. A translated loop whose exit never becomes reachable spins forever on the GPU and
stalls the frame loop; the public fork's story-load work needed the same guard after exactly that hang. The published
corpus carries 4,881 `OpLoopMerge` loops in 1,423 modules, so the guard applies to almost every module that has a
loop, whether or not its exit is reachable for the inputs the title feeds it.

## 2. The loop guard (termination safety; no coverage)

New `tooling/spirv_loop_guard.py`, a post-pass over the SPIR-V assembly `translate()` returns, wired through
`finish_module()` in `tooling/rdna2spirv.py` and controlled by `--loop-limit N`:

| switch | meaning |
|---|---|
| `--loop-limit N` (default 65536) | a loop's header may be visited N times per invocation **per entry**; the visit after that leaves through the loop's own merge block with the current register values |
| `--loop-limit 0` | no guard: the unguarded module, byte for byte (measured: identical `.spv` to the tree before this slice on sample modules) |

For each `OpLoopMerge` header the pass inserts a counter `OpPhi` at the header (1 from every entry edge, counter + 1
along the back edge, so a fresh entry resets the budget), puts the increment and the comparison against the limit in
the latch block, and makes the latch's branch take the original back edge only while the budget lasts. A merge block
that gains the guard's exit edge has its phis fed an `OpUndef` for that edge, which is the honest value for a loop
that was cut short. A loop with more than one back edge, an `OpSwitch` back edge, or a conditional back edge that does
not also take the merge block is refused (`LoopGuardError`) rather than guarded: such a shape cannot be bounded while
leaving a module `spirv-val` accepts (the counted latch would leave the continue construct; the validator's exact
error is in the module's docstring). The reference implementation splits that edge instead; this tree does not,
because the split output is measured to fail `spirv-val`.

The default is **on**, because the guard is the measured-correct behaviour for a translated shader. It changes module
bytes only where a module has a loop: measured against the pre-slice translations, 909 of the 1,266 boot graphics
modules change and the 357 without a loop are byte-identical; 11 of the 29 FidelityFX modules change (the 11 with a
loop); 349 of the 444 already-translated union modules change (all of them have a loop, and none changed for another
reason). A `--loop-limit 0` run was compared module by module on sample modules and reproduces the pre-slice `.spv`
byte for byte. Because the guarded modules differ, the published corpus cannot be delta-published with the guard: it
belongs with the full retranslation the gap plan already schedules for `host_addr`. Within this slice the 33 newly
translated union programs are publishable on their own; they are new entries, not replacements. As a cross-check of
the port, the 29 FidelityFX modules are byte-identical to the gap plan's scratch run that ported the reference guard.

Measured loop census of the modules this slice translated (counted from the binary SPIR-V, not carried over):

| corpus | modules | loops | modules with a loop |
|---|---:|---:|---:|
| boot graphics (1,266 programs) | 1,266 | 1,113 | 909 |
| 668 request union, translated part | 477 | 2,842 | 382 |
| FidelityFX (repaired listings) | 29 | 30 | 11 |

All 1,772 modules translate and pass `spirv-val --target-env vulkan1.2` **with the guards applied**, with 0
refusals introduced by the guard itself.

## 3. TG_SIZE general value (COMPUTE_PGM_RSRC2 bit 10)

`translate()` now sets `g.cs["tg_size"] = True` whenever RSRC2 bit 10 is set and lets the existing compute prologue
write the whole value (the workgroup's wave count in bits [5:0], this wave's index in [11:6], at the SGPR after the
enabled workgroup ids), for every read of it and for wave32 and wave64 alike. The old `check_workgroup_info_use()`
accepted only one wave64 idiom and refused everything else; it is deleted.

What the 25 programs that class was refusing turned out to be, measured program by program on the union:

| first mention of the work-group-info SGPR | programs | the new translation |
|---|---:|---|
| read by the local-invocation-index idiom, `s_and_b32 sN, sTG, 0xfc0` then `v_mbcnt` | 8 | uses the Mesa value; all of them read bits [11:6] only |
| overwritten before any read (`s_mov_b32 sN, ...` or an in-place shift) | 17 | the program never consumes the hardware value; it translates because the register is not special to the translator |

The old check refused the 17 with "use" because it treated the first write as a bad use, and never accepted the 8
either: after finding the idiom it walked on and counted the later `v_mbcnt` mention of the same register as a second,
non-idiom use. So the class was a check bug that the general value replaces correctly.

One bug in the existing prologue came out with the gate gone: the wave count was ORed into bits [5:0] only when the
workgroup has more than one wave, so a one-wave workgroup wrote 0 there instead of 1. It is now written unconditionally
(pinned by `tests/semantic/test_cs_tg_size.py`).

Measured: **+25 of the 668** programs (all 25 of the class). One further union program (256 threads, four waves of 64)
sets the bit and already translated through the old idiom; its prologue value is unchanged (the old prologue wrote
the wave count only when there is more than one wave) and its module changes only by the loop guard.

**Limits.** The value's layout is Mesa's (`src/amd/common/nir/ac_nir_lower_intrinsics_to_args.c`, GFX6-10), not
measured on hardware here. The reference fork's own gfx1036 measurement also places the wave index in [24:20] and sets
bit 31 for the first wave; this tree does not model those bits, and no measured consumer here reads them (the 8
consumers above mask [11:6] only). A program reading any other field of the register would get 0 there. That is
strictly better than the old refusal for a program that overwrites the register, but a library program relying on an
unmodelled field would silently compute a different value while passing `spirv-val`; the guard against that is a GPU
fixture, which this slice does not have.

## 4. `s_bitreplicate_b64_b32` with a mask destination

The SOP1 handler now accepts a **mask destination** for `s_bitreplicate_b64_b32`: `exec` in either wave size and
`vcc` in wave64 code (the forms the corpus listings print). Lane L's mask bit is source bit L // 2 (the instruction
duplicates every source bit into an even and an odd destination bit), written through the existing lane-mask store:
lane L's own word becomes the source's bit L // 2 as its 0 / ~0. Every other lane-mask case is still refused: a
lane-mask **source** (the instruction would read other lanes), any mask destination for `s_quadmask_b32/_b64`, and a
wave32 `vcc` destination (there `vcc` is the data pair `s_load`/`*_saveexec` use, not a lane mask).

Measured: of the 11 programs in the class, **8 translate** (all `spirv-val` clean) and 3 reach the next named gap
(`s_flbit_i32_b64` of a lane mask, which stays refused by name).

## 5. Measured effect on the 668 request union

Same command the build uses (`rdna2spirv.py <disasm> --out <dir> --no-asm --wave64-emulate`), every module validated:

| run | translated | validated | refused | classes that moved |
|---|---:|---:|---:|---|
| tree before this slice | 444 (66.5%) | 444 | 224 | - |
| this slice | **477 (71.4%)** | **477** | 191 | CS work-group-info 25 -> 0; `s_bitreplicate` lane mask 11 -> 0; `s_flbit` 1 -> 4 (the next gap) |

Every refusal class that did not move is unchanged: fall-through into undecodable words 165, `ds_append` 10,
undecodable words 8, `v_cmp` destination 2, mask destination 1, normalized encoding 1.

## 6. Publishable programs

The **33** union programs that translate now and did not before are not in the published index (measured: 0 of the 33
already there, while all 444 programs the tree already translated are published, so the corpus is exactly the
translatable subset). They are additive entries: publishing them does not republish any existing module, and the loop
guard's presence in their modules is their own default, not a corpus-wide change. The keys are in the slice report;
this page keeps counts. The boot corpus is untouched: 1,266 + 29 translate and validate as before.

## 7. Tests

| test | covers |
|---|---|
| `tests/semantic/test_loop_iteration_guard.py` + `tests/semantic/fixtures/loop_iteration_guard.asm` | five scenarios on the CPU: a finite loop keeps its values; an unreachable exit stops exactly at the budget (16 and 3 visits); a budget of 0 disables the pass; a nested loop gets a fresh budget per outer entry (9 visits at limit 3, not 3); the dispatch-loop control-flow form is bounded too |
| `tests/test_rdna2spirv_loop_guard.py` | the pass itself: identity at 0, range checks, the counter/exit/phi shape on hand-written modules assembled and validated with SPIRV-Tools, the three refused shapes, the translator wiring (guarded by default, unguarded at `LOOP_LIMIT = 0`, both validate) |
| `tests/semantic/test_cs_tg_size.py` | the work-group-info value through `spirv_mini_interp`: wave64 and wave32 local invocation index, wave count with a partial wave, the whole value, a one-wave group writing 1, the register following the enabled workgroup ids, not written without the bit, and the general read forms the old check refused (copy, wave32, in-place idiom, mask 0x3f alone) |
| `tests/semantic/test_salu_quadmask_bitreplicate.py` (extended) | mask destinations: exec in wave32 and wave64, vcc in wave64 (consumed through `s_and_b64 exec, exec, vcc`), one bit per lane read back per lane; lane-mask sources and a wave32 vcc destination still refused |
| `tests/semantic/spirv_mini_interp.py` (extended) | `OpSwitch`, `OpUMod` and `OpUndef` support, needed to execute the dispatch-loop form and a guarded merge-block phi; the interpreter stays strict about everything else |
| `tests/semantic/test_control_flow_branches.py` (updated) | its own structured-control-flow interpreter follows the guard's counter phi (`OpPhi` support with predecessor tracking) |
| `tests/semantic/test_ds_swizzle_b32.py`, `tests/semantic/test_valu_permlane16.py` (updated) | the `--wave64-emulate` round loop can now run in the shared interpreter (it needs `OpSwitch`/`OpUMod`), so the two emulation tests check values against their own reference models instead of asserting the interpreter rejects the module |
| `tests/test_translate.py`, `tests/test_rdna2spirv_cli.py`, `tests/semantic/test_lds_basic_alloc.py` (updated) | TG_SIZE_EN is accepted and its value written (the old refusal test flipped); `--loop-limit` argument type and its effect on `LOOP_LIMIT`; the LDS doc note that TG_SIZE_EN was refused |

The semantic suite and the translator pytest files are the gates (see the slice report for the run results).

## 8. Cross-check against the reference

The need and the shape were cross-checked against the public GPL-3.0 project
[ubiquitarianism/leonida](https://github.com/ubiquitarianism/leonida), file `tools/spirv_loop_guard.py` and the
translator hunks of commit `273b3bdf9` ("boot through the story load"), and its test `tests/semantic/test_cs_tg_size.py`
and bit-replicate hunks (`6ecd922d9`, story-load revision). This tree's implementation is its own, and three
differences are deliberate:

* **A conditional back edge that does not take the merge block is refused, not split.** The reference splits it
  through a second latch block; that module shape is rejected by `spirv-val` (a counted latch inside the continue
  construct cannot leave to the merge block). Our corpus never produces the shape: refusing it changed no measured
  translation.
* **The wave count is always written.** The prologue bug above is ours; the reference's test pins the whole value.
* **The extra gfx1036 fields are not modelled.** The reference's test pins the wave index in [24:20] and bit 31 of the
  first wave; this tree writes only Mesa's documented fields and records the difference here.

The VCC-tuple recovery bundle and the decoder-listing fallback (gaps 3 and 1 of the gap plan) are **not** part of this
slice; section 9 has the reason.

## 9. Deferred: the VCC-tuple recovery bundle

The third ranked fix is the `restore_skipped_instructions` recovery of llvm-mc-skipped words (VCC tuples, DPP
compares, the measured dot form). It is deferred as a bundle, not as a single fix: the gap plan measured that the
recovery alone frees 20 programs (444 -> 464) but that a recovered tuple can be used as a pointer, and without the
companions `host_addr`'s 48-bit sign extension, `s_load` with `vcc` as its base, the wave32 VCC pair marking and the
wave64 VALU mask widening, a module can compute a wrong address while passing `spirv-val`. `host_addr` alone changes
module bytes for every module that reads a buffer V#, i.e. most of the corpus, and needs its own full retranslation
and revalidation. Shipping the recovery without the companions would trade a named refusal for a silent wrong value,
which is exactly what the plan warns against. The bundle, its five reference tests and the full-library measurement are
the next slice.

The decoder-listing fallback (gap 1) is likewise out of scope: it is a different gate (its measured 11-of-24
module mismatch needs canonicalisation plus an equivalence gate) and nothing of it is enabled here.

## 10. Statements elsewhere that this slice makes stale

Out of this slice's write scope, recorded so the next documentation pass can fix them:

* `docs/design.md`, the "Compute shaders" contract row: it still says "Shaders with TG_SIZE_EN are rejected for now".
  They are not; this page is the replacement statement.
* `docs/remaining_mechanisms.md`'s translator row: it still says "no loop-bounds guard" and counts the work-group-info
  and lane-mask bit-replicate refusals as open. Both moved.
* `docs/pipeline_check.md` and `docs/shader_library.md`'s refusal table: after a full retranslation the numbers shift;
  only the two short notes in `shader_library.md` are updated here.

## 11. Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 project ubiquitarianism/leonida (<https://github.com/ubiquitarianism/leonida>), files
`tools/spirv_loop_guard.py`, `tools/rdna2spirv.py`, `tests/semantic/test_loop_iteration_guard.py` and
`tests/semantic/test_cs_tg_size.py`, commits `273b3bdf9`, `6ecd922d9` and `56200058e`. No code was copied from that
project; the upstream project remains the copyright of its contributors and its license continues to apply to its own
tree. The porting policy is [upstream_port.md](upstream_port.md).

## 12. Unsigned leading-bit count of a lane mask

### Measured class

A fresh run of `python -I tooling/rdna2spirv.py <listings> --out <scratch> --no-asm --wave64-emulate --refusals <scratch>/refusals.json` on the 668-program request union produced this baseline before the rule:

| refusal | programs |
|---|---:|
| fall-through into undecodable words | 165 |
| `ds_append` | 10 |
| undecodable words (cannot re-anchor) | 8 |
| `s_flbit_i32_b64` of a lane mask | 4 |
| `v_cmp` destination | 2 |
| mask destination | 1 |
| reachable instruction with normalized encoding | 1 |
| **total refused** | **191** |

The same scan after the change translated and validated **480 / 668** programs and refused 188: fall-through 165, `ds_append` 11, undecodable words 8, `v_cmp` destination 2, mask destination 1, normalized encoding 1. The measured `s_flbit_i32_b64` lane-mask class is now **0**. Three of its four programs translated; the fourth moved to the already-named `ds_append` refusal, so the net gain is three. All **477** modules that translated before this rule produced byte-identical `.spv` files afterward; the other three output modules are new.

### Rule and semantic fixture

For an operand proven by `lane_masks.py` to be a lane mask, `s_flbit_i32_b64` gathers the wave's 64 bits with the existing `wave_mask()` path and applies the existing unsigned count over the high and low dwords. Each lane receives the same count; zero maps to `0xFFFFFFFF`, and SCC is not written. The normal scalar-pair/immediate path is unchanged. `s_flbit_i32_i64` of a lane mask remains refused: its sign-relative rule is outside this measured class.

The synthetic fixture `tests/semantic/fixtures/disasm/cs_s_flbit64_lane_mask.s` covers single bits at 63, 40, 31 and 0, an empty and full VCC mask passed through an SGPR pair, and full EXEC. `tests/semantic/test_salu_flbit64_lane_mask.py` checks the expected result in all 64 lanes for each of the seven masks (448 lane-result checks). `tests/semantic/test_salu_flbit_64.py` keeps a negative test for the unsupported signed i64 lane-mask form. The standalone CPU semantic tests passed (1 new fixture test and 5 signed/data tests); no GPU arm was run or needed. The full request-union scan above is the corpus-closure evidence; it uses external listings and stores no game data or shader bytes in this repository.

### Full semantic suite and baseline limits

`python -I tooling/scripts/run_semantic_tests.py tests/semantic -j 4` ran 462 files / 6,064 tests; 460 files passed. The two failures were in `test_exec_computed.py` (2 failures) and `test_image_atomics_guard.py` (47 subtest failures). Both reproduce with the pre-slice HEAD `9edd62c44` translator loaded from an external scratch copy (which retains the original lane-mask refusal), so they are pre-existing and unrelated to this rule. Neither test was changed.

This rule was derived from the RDNA2 instruction semantics and this tree's own lane-mask analysis. The rank-5 note in `cleanroom_peers.md` is parity context only; no peer implementation, clone source, or fixture was consulted or copied, so no new `NOTICE.md` entry is needed.

## 13. LDS DS_APPEND in the compute request union

### Measured class

A fresh `python -I tooling/rdna2spirv.py <listings> --out <scratch> --no-asm --wave64-emulate --refusals <scratch>/refusals.json` scan of the 668-program request union had 480 translated and 188 refused. `ds_append` was the named refusal for 11 programs; an aggregate inspection counted 38 occurrences across those 11, all with an aligned `offset:` and no `gds` or other modifier. No program names or instruction bytes are recorded here.

| refusal before the rule | programs |
|---|---:|
| `ds_append` | 11 |
| fall-through into undecodable words | 165 |
| undecodable words (cannot re-anchor) | 8 |
| `v_cmp` destination | 2 |
| mask destination | 1 |
| reachable instruction with normalized encoding | 1 |
| **total refused** | **188** |

### Rule and semantic fixture

RDNA2 `ds_append` on LDS adds the count of EXEC-active lanes in the wave to the dword at the aligned instruction byte offset and returns the pre-operation counter to every active lane ([AMD RDNA2 machine-readable ISA, DS_APPEND](https://raw.githubusercontent.com/ROCm/rocm-systems/1b50d9a20e7dd31006c8252bd674e188c26a4880/shared/machine-readable-isa/isa/amdgpu_isa_rdna2.xml)). The translator elects the first active lane to issue one Workgroup `OpAtomicIAdd` by `popcount(EXEC)`, then broadcasts its old value; inactive lanes keep `vdst`. The LDS address uses `offset` directly (not `M0`); GDS, unknown modifiers, misaligned/out-of-field offsets and non-scalar VGPR destinations remain refused. The existing LDS allocation-bound path handles offsets outside the workgroup allocation.

The synthetic [wave32 fixture](../tests/semantic/fixtures/disasm/cs_w32_ds_append.s) and [wave64 fixture](../tests/semantic/fixtures/disasm/cs_w64_ds_append.s), run by the [DS_APPEND semantic test](../tests/semantic/test_ds_append.py), check partial EXEC (13/32 lanes with two wave32 waves sharing LDS; 40/64 lanes in native and emulated wave64), one returned value per active wave, unchanged inactive destinations, and the final counter. The negative cases keep GDS, misaligned offset and non-scalar destination forms refused. This is standard-library CPU interpretation only; no GPU was used.

The same union scan after the rule translated and validated **491 / 668** programs and refused 177: fall-through 165, undecodable words 8, `v_cmp` destination 2, mask destination 1, and normalized encoding 1; `ds_append` is now **0**. All **480** modules that translated before the rule are byte-identical afterward; 11 newly translating modules account for the net gain. The scan outputs were kept outside the repository and only these aggregate counts are recorded.

This clean-room rule uses the ISA and this tree's measured forms; rank-5 peer notes were evidence for which class to measure, not an implementation or fixture source. No new `NOTICE.md` entry is needed.

## 14. One plain SGPR move in an undecodable gap

### Measured sub-class

The current-tree baseline scan of the 668-program request union translated and validated 491 programs and refused 177. Within the 165 `fall-through into undecodable words` refusals, an aggregate-only pass decoded the first reachable gap with this tree's RDNA2 decoder and found exactly 12 gaps containing one valid 4-byte SOP1 `s_mov_b32` from an ordinary single-dword SGPR to another ordinary single-dword SGPR, without modifiers. No program names or native instruction bytes are recorded.

| refusal before this rule | programs |
|---|---:|
| fall-through into undecodable words | 165 |
| undecodable words (cannot re-anchor) | 8 |
| `v_cmp` destination | 2 |
| mask destination | 1 |
| reachable instruction with normalized encoding | 1 |
| **total refused** | **177** |

### Narrow rule and semantic fixture

At a reachable fall-through only, `reachable_code()` may ask the gap callback to inspect a 4-byte word when the next listed instruction starts exactly four bytes later. `scalar_smov_gap_word()` accepts only a complete one-instruction decode of `s_mov_b32`, with no modifiers and two ordinary one-dword SGPR operands; the callback repairs at most one such word per translation. This reuses the existing SALU move semantics. Branch targets into data, special-register and inline-immediate moves, literal/wider instructions, and all other gaps remain refused.

The synthetic [fixture](../tests/semantic/fixtures/disasm/ps_smov_sgpr_gap.s) and [CPU semantic test](../tests/semantic/test_smov_sgpr_gap.py) verify that a hidden `s_mov_b32 s4, s5` moves the input word to the exported pixel value. VCC_LO and inline-immediate source variants remain refused. No GPU, game program, or game bytes are used.

### Closure and byte-identity check

The final `python -I tooling/rdna2spirv.py <listings> --out <scratch> --no-asm --wave64-emulate --refusals <scratch>/refusals.json` scan again translated and validated **491 / 668** programs and refused 177: fall-through 165, cannot re-anchor 8, `v_cmp` destination 2, mask destination 1, and normalized encoding 1. The rule restored the 12 measured words, but each affected program still reaches a later unsupported gap, so the net full-module closure is **0 programs**. All **491** modules that translated before this rule are byte-identical afterward (491/491; 0 changed, none missing, none added). Scan outputs and the refusal manifest remain outside the repository; only aggregate counts are recorded.

The rule and fixture use this tree's RDNA2 decoder and semantics; peer notes were evidence-only, and no peer implementation or fixture was copied. No new `NOTICE.md` entry is needed.

## 15. One exact LGKM wait in a decode gap

### Measured sub-class

A fresh GPU-free Python scan of the current 668-program request union, before this rule, translated and `spirv-val` checked **491 / 668** programs and refused 177: fall-through 165, cannot re-anchor 8, `v_cmp` destination 2, mask destination 1, and normalized encoding 1. An aggregate-only pass over the 165 fall-through refusals inspected 138 reachable four-byte gap candidates; exactly **30** decode as one complete, unmodified SOPP `s_waitcnt` with the sole operand `lgkmcnt(0)`. No program identifiers or native words are recorded.

| refusal before this rule | programs |
|---|---:|
| fall-through into undecodable words | 165 |
| undecodable words (cannot re-anchor) | 8 |
| `v_cmp` destination | 2 |
| mask destination | 1 |
| reachable instruction with normalized encoding | 1 |
| **total refused** | **177** |

### Narrow rule and semantic fixture

At a reachable fall-through only, the callback examines a four-byte gap when the next listed instruction starts exactly four bytes later. `scalar_waitcnt_lgkm0_gap_word()` accepts only a complete one-instruction RDNA2 decode whose raw encoding matches and whose sole operand is exactly `lgkmcnt(0)`. The existing `NOOPS` path emits no SPIR-V operation for `s_waitcnt`; this recovery adds no new wait/barrier behavior. The one-word recovery budget is shared with the earlier plain-SGPR-move rule. Other wait counts (`vmcnt(0)`, nonzero `lgkmcnt`), other opcodes, wider gaps and branch targets remain refused.

The synthetic [fixture](../tests/semantic/fixtures/disasm/ps_s_waitcnt_lgkm0_gap.s) and [CPU semantic test](../tests/semantic/test_s_waitcnt_lgkm0_gap.py) verify the exported value after the hidden wait and keep `vmcnt(0)` / `lgkmcnt(1)` controls refused. This is CPU-only; no GPU or game shader is used.

### Closure and byte-identity check

The same union scan after the rule translated and validated **491 / 668** programs and refused the same 177 classes. The exact wait was recovered in 30 programs, but each still reaches a later unsupported gap, so full-module closure is **0 programs** and the refusal manifest has 0 status changes. All **491** previously translating modules are byte-identical afterward (491/491; 0 changed, missing or added), verified by SHA-256 comparison. Scan outputs were temporary and outside the repository; after the identity check only these aggregate counts remain.

This rule follows the current tree's decoder and existing no-op contract; peer materials were evidence-only and no peer implementation or fixture was used.

## 16. One exact EXEC-low SGPR move in a decode gap

### Measured sub-class

A fresh GPU-free Python scan of the current 668-program request union, before this rule, translated and `spirv-val` checked **491 / 668** programs and refused 177: fall-through 165, cannot re-anchor 8, `v_cmp` destination 2, mask destination 1, and normalized encoding 1. An aggregate-only pass over those 165 fall-through refusals inspected 138 reachable four-byte gap candidates; exactly **23** decode as one complete, unmodified SOP1 `s_mov_b32 exec_lo, sN` with a one-dword SGPR source. No program identifiers or native instruction words are recorded.

| refusal before this rule | programs |
|---|---:|
| fall-through into undecodable words | 165 |
| undecodable words (cannot re-anchor) | 8 |
| `v_cmp` destination | 2 |
| mask destination | 1 |
| reachable instruction with normalized encoding | 1 |
| **total refused** | **177** |

### Narrow rule and semantic fixture

At a reachable fall-through only, the callback may restore a four-byte gap word when the next listed instruction starts exactly four bytes later. `scalar_smov_exec_lo_gap_word()` accepts only one complete RDNA2 decode whose raw bytes match and whose exact shape is `s_mov_b32 exec_lo, sN`: no modifiers, one ordinary single-dword SGPR source. It hands the recovered instruction to the existing EXEC-low semantics; this adds no new register, wave-mask, or control-flow lowering. The one-word recovery budget is shared with the plain-SGPR and exact-LGKM-wait gap rules. `exec_hi`, special or immediate sources (including VCC), wider/invalid/normalized words, and branch targets into data remain refused.

The synthetic [fixture](../tests/semantic/fixtures/disasm/ps_smov_exec_lo_gap.s) and [CPU semantic test](../tests/semantic/test_smov_exec_lo_gap.py) verify the generated hidden SOP1 instruction through the existing interpreter: with the low EXEC source bit set, lane 0 exports; with it clear, no export occurs. `exec_hi`, VCC-source, and inline-immediate controls remain refused. This is wave32 synthetic coverage; no GPU, game program, or game bytes are used.

### Closure and byte-identity check

The same union scan after this rule translated and validated **491 / 668** programs and refused the same 177 classes. The 23 measured EXEC-low moves are restored, but every request-union status remains unchanged: these words do not close a full program, so net module closure is **0 programs**. All **491** previously translating modules are byte-identical afterward (491 common; 0 changed, none missing, none added), verified by SHA-256 comparison. The exact status transitions, scan outputs, and per-program refusal manifests stayed outside the repository and were discarded after comparison; only aggregate counts remain in this page.

This rule uses this tree's own decoder and established EXEC semantics. Peer notes were read-only evidence; no peer code or fixture was copied, so no `NOTICE.md` entry is needed.
