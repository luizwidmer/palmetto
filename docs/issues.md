# Issue ledger

Ledger of **what people are sending** to the project's GitHub home, plus our own tracked findings for the boot port. Each row carries area, state and one-line evidence. Counts, error classes and commit ids only — no game data, package paths, shader bytes or hashes.
Maintained by the issue-tracker agent as a standing, read-only-plus-this-file role.

**Repo:** `F:\palmetto`, branch `main`, HEAD `309f30380` · **GitHub:** [luizwidmer/palmetto](https://github.com/luizwidmer/palmetto)
**Updated:** 2026-11-24 (refreshed after the slot-57 wiring, the input-layout decode, the display-path diagnosis and the replay-resolution measurement) · **Incoming:** 2 open (#2, #3 + 1 third-party comment) · **Ours open:** 11 (O3 partial, O5, O8, O9, O11–O17) · **Done:** 14 · **Superseded:** 3 (O1, O6, O10) · **#5 closed:** 1 · **Open PRs:** 0.

**Policy: ledger only.** Do not open, comment on, close, delete or edit GitHub issues/PRs, or commit/push, without an
explicit ask. Draft replies below are **HOLD** — the repo owner has not approved public replies; nothing has been posted.

**Where the project stands (measured on one machine, one 150 s boot unless named).** The retail target console title boots
and runs through the runtime: **ALIVE for the whole timeout, no exception**, **all 10,241 slot-57 records build into
real PC graphics pipelines**, the title records **43,750 `SetPipelineState` and 43,750 `DrawInstanced`**, and **no
`Close` fails** (86,059 successful in the decode boot; later boots also report 0 failed). Recording is converted end to
end with no mutes (list lifecycle reopen, null-view clear, resource-less barriers). Of the frame's submits, 43,755
succeed and 43,750 — one per recorded draw — are refused `unknown resource` (O11); the measured wall behind them is the
title's console VA space (O17), and the single-candidate fix that removes every refusal is reverted because the first
executed draw TDRs the GPU. Presentation is still empty because the title's own display surface carries a null resource
(O12) and `PresentX` is refused (O14). Evidence: `docs/input_layout.md` §6, `docs/handles_barriers.md` §6,
`docs/display_path.md`, `docs/replay_resolution.md`.

## Incoming reports (GitHub)

| # | sender | state | what they sent | tracking |
|---|---|---|---|---|
| #2 | bittertea0323 | open · ours to answer | Independent reproduction, Windows 10 + RTX 4070 SUPER: two host build blockers (a .NET SDK pin and one unit test under newer MSVC), 26,633 SPIR-V modules, a run at the documented frontier, slot 108 publishing 0, then a fault ten frames deep in title code. Asks whether that fault is the expected slot-108 consequence, and whether console GPU-VA dereference is this layer or the translator | External, older tree, not a duplicate. Q1 answered: the invalid slot-108 answer made the title's own render-target factory return null, fixed (`c3c7a7b83`, `cc7e044b2`), and current `main` boots past it. Q2 answered by measurement: the `0x100000000` read was our queue `GetDesc` struct-return clobber, not a console GPU-VA dereference (`4178c742c`). Draft corrected and held |
| #3 | bittertea0323 | open · ours to answer | FSPX corpus on a second host: 18/29 translate, 11 fail `fall-through into undecodable words` and cluster on wave32; per-program word counts; asks for a preferred report shape | Addressed by `0276f2b1f` (repair → 29/29); draft held. **Comment (NinosTech, 09:21Z):** four FSPX translations from an older upstream revision cleared four lookup misses and made the 42 compute-pipeline calls succeed, but the captured frame still had 0 non-black pixels of 896,232; not matched to our cases — on our tree the black frame is a later stop (O11/O12), not the FSPX set, see notes |

Closed: #1 — PR "Main"; #5 — closed as completed on `4178c742c` (its own stop was fixed; later frontiers are tracked as separate items, not a reopen).

## Draft replies (HOLD — not posted; awaiting repo-owner approval)

- **#2:** "Thanks — that is answered now. The slot-108 allocation answer was invalid, which made the title's own
  render-target factory return null; it is fixed (`c3c7a7b83`, then `cc7e044b2` for A8 render targets), and current
  `main` boots past that fault into its frame loop. The `0x100000000` read we chased turned out to be our own queue
  `GetDesc` struct-return clobber (`4178c742c`), so it was this layer, not the translator. Please re-run `main`;
  `PALMETTO_CRASH_REPORT` gives the faulting-thread dump."
- **#3:** "Thanks for the per-program counts — the build now repairs each FSPX listing before translating
  (`tools/widen_smem_decode.py`) and measures 29/29 passing `spirv-val` (22/29 without), so the wave32 cluster is closed
  in tree. Per-program stop word is still the report shape we would want for any surviving refusal."
- **#3/NinosTech:** "Thanks — that matches the split we measure: resolving the lookups moves the 42 creations to
  success, while the frame's black on our tree is a later, separate stop (the title presents a null surface of its own
  and half our frame submits are refused on an unresolved resource), not the FSPX set. We will check your four programs
  against ours."

## Our findings — open

| id | area | item | state | evidence |
|---|---|---|---|---|
| O3 | runtime/diagnostics | Debugging gaps from the public attempt | open, partial — crash-memory save `7c70724b7` (`PALMETTO_CRASH_SAVE`), faulting-thread unwinder `7eab981ad` (`PALMETTO_CRASH_REPORT`), per-run report `0e76519a0` (`tools/run_report.py`) landed | every-thread stack dump still missing (`docs/competitor_intel.md`); with the title now running, a fault-time all-thread dump is the highest-value remaining diagnostic |
| O5 | build (test hygiene) | `runtime/test/d3d12_existing_heap_test.cpp` fails on the last tracker build: `CreatePlacedResource` on an existing heap expects `E_NOTIMPL`, gets `E_INVALIDARG` (~L163) | open, low | a ctest target in `runtime/CMakeLists.txt`, but `_setup/build_tests.cmd` builds only `d3d12_fence_timing_test`, `xs_proxy_mock_test` and `unimplemented_policy_test` (verified this refresh), so it can rot again; not re-run in this refresh |
| O8 | build | Shader stage's completeness gate blocks the designed `--stages shaders` path: the translator refuses two duplicate-name variants the old corpus had, so stages had to be run individually | open | `62b510467`, `9271ef74c`; `docs/boot_frontier.md` (corpus section) |
| O9 | build/perf | Pipeline-library disassembly is quadratic: `llvm-mc` prints the whole input hex line once per invalid vcc-tuple word, so a 120 KB program becomes a 100–134 MB listing (one file 137 MB); 841 of 2,684 programs in 17 minutes, 9.4 GB | open | `docs/boot_frontier.md` (disassembly-cost section), `9271ef74c`, `62b510467` — not yet in `pipeline_program_source.md` |
| O11 | runtime/replay | Every draw-carrying submit is refused `ExecuteCommandLists failed: unknown resource; nothing was submitted`. Measured site: `prepare_draw`'s vertex-buffer lookup (`buffer_at` miss for the VA the draw binds) — the pipeline resolves, and the render-target and depth bindings resolve (the frame's views are null and skipped) | open — **the refusal itself; its clean fix is blocked on O17** | 150 s boot: 43,755 submits ok / **43,750 failed, one per recorded draw**; the missing VA is exactly `...0B00000 + 0x3DF10` inside the title's one 174 MiB placed buffer, and later draws advance `0x20000` each within it (`309f30380`, `docs/replay_resolution.md` §1–3). Bounded diagnostics name each site (`PALMETTO_REPLAY_MISS=0` silences them). Next measurement: re-run with O17's arena range plus the measured VA registration and count executed draws and TDRs |
| O12 | runtime/display | The title's own display surfaces carry a null resource: `CreateRenderTargetView` is called with `pResource = [surface+0x20] = 0`, and `PresentX` reads the same zeroed field, so the frame's clears and the present aim at nothing | open — **presentation frontier** | 120 s write watch: **23,819 writes of 0**, every one from `game+0xCD74DB`, all on the title's own present-to-display path (`docs/display_path.md` §2); the same null is the render-target view's resource (§3), so it is one field of one surface class, not two failures. Next measurement: `PALMETTO_POKE_LOG=325090` on the surface constructor to name which creator makes the null population, after the slot-133 path already measured creating and storing real surface resources |
| O13 | runtime | Console device slot 55 is forwarded unchanged to PC `ID3D12Device4::CreateReservedResource1`, which refuses the non-null session, so every pipeline state created on that path is null; the title does not test the HRESULT | open — measured mis-forward, not the current draw refusal | 41,545 `E_INVALIDARG` failures per 120 s; called from `game+0x247A5C2` with `riid = IID_ID3D12PipelineState` and the out pointer on the stack; the same shape as the earlier mis-forwarded console extensions (`docs/display_path.md` §4 side observation). The replay diagnostics show the executed draw's pipeline resolving, so these PSOs are not on that path. Next measurement: probe slot 55 as a pipeline-state creator (arity and out slot) before any forward, as queue slots 18 and 35 were probed |
| O14 | runtime/presentation | `PresentX` (console queue slot 32) is refused: the prepared presenter core (window presenter over the Vulkan swapchain, frame pacer, token bridge for the frame-event counter) has nothing to present | open | 36,084 presents per 140 s, all refused `E_NOTIMPL`; answering present with a loud stub sped the loop (30,786 → 42,515 submits) but changed no draw count at the time, so present success alone does not unlock the frame (`docs/display_path.md` §5, `docs/boot_frontier.md`). Order: O12 first (a surface to present), then register and wire `runtime/shims/xbox_present.*` |
| O15 | runtime | The 388-byte (380–520 B) fixed-function state blob is undecoded: every built pipeline carries `documented_default_state()` and the 1 x `R8G8B8A8_UNORM`, no-depth, sample-1 attachment stand-in | open | all 10,241 slot-57 records build with those stand-ins and the refusal histogram is empty, so no field refuses; rasterizer, blend, depth-stencil, sample description, topology, strip cut and the real RTV/DSV formats remain unmeasured (`docs/graphics_record.md` §4, `docs/graphics_pso.md` §4/§6). Next measurement: decode the state blob, then histogram its fields over the 10,241 records — a field measurement, not a boot refusal |
| O16 | runtime/memory | Title-placed buffer pages cannot be imported: the title's one 174 MiB placed buffer (device slot 133) sits on xmem physical pages that are committed lazily, so the whole range is not importable (the fallback answers `E_NOTIMPL`); the fallback is an ordinary stand-in buffer, the title's CPU writes do not reach the GPU, and the stand-in's registered VA is the arena VA, not the console VA the title placed it at | open — **measured cause of O11** | measured fallback on the 174 MiB placement; the stand-in's bytes are not the title's writes (`309f30380`, `docs/replay_resolution.md` §3). Next measurement: which page state blocks the import (uncommitted, write-combined, off-alignment) and whether a copy into the stand-in is the honest substitute for the page import |
| O17 | runtime/addressing | The address space: a translated shader computes `host_address = heap_base + console_va`, but a title-placed buffer is named by a two-terabyte title VA, not an arena offset. Registering the O11 stand-in under the console VA removes **all 43,750 refusals** and the first draws execute — then the GPU faults: TDR at 22.95 s, two `nvlddmkm` event-153 records, the replay logs the device lost, title `0xC0000005`. A control with every vertex binding redirected to the zero page TDRs at the same point, so it is the addressing, not the fetched data | open — **frontier, highest priority** | the candidate VA registration was measured and reverted, so a clean refusal is this tree's behaviour; the same fault mode is already recorded in `docs/d3d12_vulkan.md` §3.3 (`309f30380`, `docs/replay_resolution.md` §4–5). Next: give every title-placed buffer an arena range at creation and rebase every shader-visible title VA (root descriptor VAs at record time, title-written V# descriptors); anything that cannot be rebased must stay refused, never executed |

**Priority.** O17 is the frontier: the title-VA address space. O11 is its replay-side symptom and O16 its creation-side
cause, and both point at the one place where the arena range and the VA alias must be set. O12 is the presentation
frontier behind it (a surface to present). O13 is a measured mis-forward, but it is not on the executed draw path today.
O15 has to be decoded before the pipelines are faithful, but it refuses nothing today. O3's every-thread dump is the
highest-value debugging gap now that the title runs; O5, O8 and O9 are build/test hygiene.

## Our findings — done

| area | item | state | evidence |
|---|---|---|---|
| runtime | Slot-57 deserialized graphics record → real PC graphics pipeline: `graphics_pso` builder wired, then the input-layout element table decoded (count at `+0x4C`, length-prefixed names, 24-byte D3D12 attribute block, DXGI format words 2/6/16/28/30) — answers O4 | done `9b0d696b5`, `d0095ca70` | 1,074 built / 8,926 refused at the layout blob before the decode; after: **10,241 of 10,241** build, 0 PC failures, 42,721 `SetPipelineState` / 42,722 `DrawInstanced` recorded in the decode boot and 43,750 of each in the later replay baseline boot (`docs/input_layout.md` §6, `docs/slot57_wiring.md` §2, `docs/replay_resolution.md`) |
| runtime | Console clear (list slot 52) and console barrier (list slot 146) converted: a null-view clear is the no-op it is, a real view forwards unchanged; resource-less texture barriers become global barriers, real-resource entries pass through | done `cf825083d` | 60 s boot with no mutes: 30,999 `Close` S_OK / 0 failed, 30,376 reopens and submits, 75,937 barrier records; the frame rate is roughly half the muted control because the barriers are now real, with no submission lost (`docs/handles_barriers.md` §6) |
| runtime | Console command-list lifecycle: the proxy remembers the creating allocator and recording state and reopens a closed list before a forwarded recording call, refusing on a failed `Reset` | done `8dc42f405` | 4 reopens and 82,736 submits in 150 s; the first `Close` after a reopen started returning `E_INVALIDARG` (proof the recording reached PC validation) and exposed the two invalid calls above (`docs/list_lifecycle.md` §4–5) |
| runtime | Console queue slot 35 is the frame submit: forward `ExecuteCommandLists`; emulate the AMD-only `monitorx`/`mwaitx` pair on Intel | done `22555b249` | 0 → **78,745 submits per 140 s**, ALIVE, 2,292,332 traced D3D12 calls (`docs/boot_frontier.md`) |
| runtime | Console fence slot 12 answered with the address of the fence's completed value — the last stop before the title's own loop | done `77d2f6bba` | ALIVE 300 s / 400 s with no exception; 5,195,024 calls in 300 s; 245,524+ slot-12 answers (`docs/fence_slot12.md`) |
| runtime | Console queue slot shift: the console inserts two methods before `Signal`, so console 16/17/18/19/20 map to PC Signal/Wait/GetTimestampFrequency/GetClockCalibration/GetDesc — answers O10 | done `e1a40382d` | the 10^9 was our own `GetTimestampFrequency` answer written over the fence proxy's first qword; stop `game+0x4fb0aec` → `game+0x4fb0b03`, traced calls 12,085 → 12,087 (`docs/frontier_4fb0aec.md`) |
| d3d12 | A8_UNORM render targets accepted as R8_UNORM colour attachments with `RENDER_TARGET`, and the slot-108 bridge treats `SizeInBytes == UINT64_MAX` as unanswered — answers O7 | done `cc7e044b2` | the null container element was the title's own 3840x2160 1-sample A8 render target whose factory failed on the invalid allocation; "format not supported yet: 65" gone, stop `game+0xcc2861` → `game+0x4fb0aec`, calls 11,986 → 12,085 (`docs/container_element.md`) |
| build | `make_game` publishes the 1,266 graphics programs the title asks for (947 PS + 319 VS), with step ownership and shard-plan alignment | done `9271ef74c` | 1,266/1,266 found, translated and `spirv-val` clean; published as 1,337 fspx entries beside 29 FSPX + 42 library compute |
| runtime | Console queue slot 18 `GetDesc` struct-return order (the clobbered `rbx` at `game+0x2484133`; was O2, GH #5) | done `4178c742c` | the `0x100000000` read was a clobbered queue-desc buffer, not a GPU-VA dereference; stop moved to `game+0xcc2861`, calls 11,906 → 11,986 |
| build | Pipeline-library extraction and FSPX listing repair wired into `make_game.py` | done `0276f2b1f` | FSPX 29/29 with the repair (22/29 without); library 42/42 on the 42-program extraction, 1,047/1,684 on a real package subset |
| runtime | Console device slot 108 answers a real resource allocation info | done `c3c7a7b83` | null-object stop at `game+0xCC2861` gone; run continued to `game+0x24844A9` |
| runtime | Console queue slot 35 calibration answered (plus nonvolatile register dump) | done `818c739af` | stop unchanged at the time, so not the cause; the dump carries `rbx`, `r12`–`r15` |
| runtime | DirectStorage zlib stage inflates instead of failing with `E_NOTIMPL` | done `ae876f4b5` | 2,238 → 11,894 D3D12 calls; 0 zlib requests fail |
| runtime | AMD-only `clzero` emulated on Intel hosts | done `f76446847` | illegal-instruction fault gone; frame-loop event waits reached, 11,906 calls, 0 pipeline failures |

## Our findings — superseded

| id | area | item | superseded by | evidence |
|---|---|---|---|---|
| O1 | runtime | Renderer's three-slot record array (216 B; 72-byte records) is never initialised | the output-target measurement (`cc7e044b2`, `docs/container_element.md`): the stop is not reached on the current tree — the title boots into its frame loop, records its draws and submits them; the null-write was the renderer's null output-target element, fixed with the A8/slot-108 change | the 216-byte array's own population is not separately re-measured — a limit of this refresh, not a claim |
| O6 | runtime | Unified-memory requirement: the title CPU-dereferences console GPU image addresses; our fabricated VAs (from `1 << 32`) are unmapped | the queue slot 18 measurement (`4178c742c`): the `0x100000000` read was our own `GetDesc` struct-return clobber writing into the queue object, and no measured stop needs a CPU-visible fabricated VA. The remaining host-memory requirement is measured as O16 (title-placed pages not imported), not as a CPU dereference | `PALMETTO_BACK_IMAGES` was a diagnostic, never needed by the boot |
| O10 | runtime | New top stop `game+0x4fb0aec`: call through `qword [rax+0x60]` where `[rcx]` is `0x3b9aca00` (10^9) where a vtable pointer belongs | the queue slot shift fix (`e1a40382d`, `docs/frontier_4fb0aec.md`): the 10^9 was our own `GetTimestampFrequency` answer written over the fence proxy's first qword, because console queue slot 16 is not PC slot 16; the boot now runs and O14 is the presentation frontier | the two hypotheses the old row named (a size written as a pointer, or a title sentinel) are ruled out by the object dump |

## Notes (not bugs)

- **Measurement recipe at this refresh.** Retail image, shader corpus, Vulkan backend, `PALMETTO_SKIP_INT3=1`,
  `PALMETTO_SEED_GLOBAL=75DF268:D8`; one boot at a time, a boot runs to its 150 s timeout; harness
  `tools/boot_report.py`. Every number here is from the committed tree at `309f30380`.
- **Executing the title's un-rebased VA draws faults the GPU.** The measured VA registration that removes all 43,750
  refusals ends in a TDR (two `nvlddmkm` event-153 records) and a title `0xC0000005`; the zero-page control TDRs at the
  same point, so the addressing, not the data, is the fault. Keep an un-rebased VA refused until O17's arena range and
  rebase land: a refused submission is honest, an executed one is a device reset.
- **Incident: corpus lost and rebuilt today.** Fingerprint mismatch → retranslation killed mid-run when the agent turn
  ended → crashed shard repaired solo → 16 parts merged (26,501), stages re-run (fspx 29/29, bvh 52/52, graphics
  1,266/1,266) + 42 library compute republished → 27,890 loaded, baseline restored (`62b510467`). **A background job
  does not outlive an agent turn.**
- **TLS rewrite (loader).** 53,246 `gs:[58h]` reads are rewritten to the TEB; `PALMETTO_TEB_CHECK=1` confirms `0x1480 +
  8*slot` is right and the faulting thread's TLS cells are correct (`443639062`, `91a01b5ea`); the public attempt agrees.
- **Pipeline-library translation is partial by design.** 42/42 on the dedicated 42-program extraction, 1,047/1,684 (~62%)
  on a real package subset (`0276f2b1f`); the FSPX set needs the `widen_smem_decode` repair. Not a boot front today.
- **Historical: the eleven "missing" pipeline programs** were zeroed library reads from the refused zlib stage, not a
  second container or a load-time patch; resolved by `ae876f4b5` (see `docs/pipeline_program_source.md`).
- **External (GH #3, NinosTech): lookups can succeed and the frame still be black.** Four FSPX translations cleared four
  misses and the 42 compute calls succeeded, yet 0 non-black pixels of 896,232. On our tree that split still holds and the
  black is now measured at O11/O12, not in the FSPX corpus; matching his four programs to ours still needs his hashes.
- **Lifetime-tracker prototype (evidence, not a fix).** Implemented correctly and unit-tested, then reverted because the
  boot regressed 11,986 → 1,653 calls through a wrong vtable slot — evidence slot 57 is not a tracker (O4, now done).
- **The display path is the title's own.** No DXGI display path is called (the only DXGI call is `GetAdapter` three
  times); the only display-surface call is `PresentX`, and the slot-133 path measurably creates and stores real surface
  resources. The null population that reaches the present and the views is therefore a different set of surfaces (O12),
  not a missing shim answer.
- **docs.** `docs/competitor_intel.md` records the public attempt (<https://github.com/Marvisak/VIctory>): DXIL + host
  D3D12 vs our SPIR-V + Vulkan, prepare-then-map loader, crash reporter with stack unwind, mechanical asset policy;
  GPL-3.0 like ours, so reuse is permitted with attribution. README's progress tracker is generated from
  `docs/progress.json` by `tools/readme_progress.py`; at this refresh both predate the slot-57 wiring, the decode, the
  display path and the replay-resolution page, so the tracker is stale until `progress.json` is updated and the tracker
  regenerated (see the refresh report).
