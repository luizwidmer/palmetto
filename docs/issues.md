# Issue ledger

Ledger of **what people are sending** to the project's GitHub home, plus our own tracked findings for the boot port. Each row carries area, state and one-line evidence. Counts, error classes and commit ids only — no game data, package paths, shader bytes or hashes.
Maintained by the issue-tracker agent as a standing, read-only-plus-this-file role.

**Repo:** `palmetto`, branch `main`, HEAD `ee9ebc6ac` · **GitHub:** [luizwidmer/palmetto](https://github.com/luizwidmer/palmetto)
**Updated:** 2026-11-24 (refreshed after the page address space, the slot-127 surface fix, the PresentX wiring, the single-window adoption and the frame-capture measurements) · **Incoming:** 2 open (#2, #3) · **Ours open:** 9 (O3 partial, O5, O8, O9, O15, O18–O21) · **Done:** 22 · **Superseded:** 5 (O1, O6, O10, O12, O16) · **#5 closed:** 1 · **Open PRs:** 0.

**Policy: ledger only.** Do not open, comment on, close, delete or edit GitHub issues/PRs, or commit/push, without an
explicit ask. Draft replies below are **HOLD** — the repo owner has not approved public replies; nothing has been posted.

**Where the project stands (measured on one machine; one 150 s boot unless named).** The retail target console title
boots and runs through the runtime: **ALIVE past 300 s with no exception**; **all 10,241 slot-57 records build into real
PC graphics pipelines** and **41,545 derived pipelines** are created through the console slot-55 answer; after the
title's page VAs were rebased into one arena the frame's draws **execute with 0 refused submissions** (17,674 per 150 s
in the address-space boot, 27,935 with the page pull, 66,550 in a 300 s run); and the runtime **presents into the
title's own adopted window** (exactly one visible top-level window measured, 8,625 presents in a 90 s adoption boot with
0 failed). The presented 4K resource **is** the frame's draw target and is proven writable by a control draw, yet reads
back uniformly black: the frame's record is clean (one clear of the display resource before the draws, the draw is its
last writer, no copy or resolve anywhere), the fixed-function stand-in and the cull axis are falsified, the title's
vertex/geometry path produces no fragments even unculled, and the sampled textures are black. Evidence:
`docs/page_address_space.md` §4, `docs/page_content.md` §2-3, `docs/frame_capture.md` §2-8, `docs/window.md` §5.

## Incoming reports (GitHub)

| # | sender | state | what they sent | tracking |
|---|---|---|---|---|
| #2 | bittertea0323 | open · ours to answer | Independent reproduction, Windows 10 + RTX 4070 SUPER: two host build blockers (a .NET SDK pin and one unit test under newer MSVC), 26,633 SPIR-V modules, a run at the documented frontier, slot 108 publishing 0, then a fault ten frames deep in title code. Asks whether that fault is the expected slot-108 consequence, and whether console GPU-VA dereference is this layer or the translator | External, older tree, not a duplicate. Q1 answered: the invalid slot-108 answer made the title's own render-target factory return null, fixed (`c3c7a7b83`, `cc7e044b2`), and current `main` boots past it into its frame loop. Q2 answered by measurement: the `0x100000000` read was our queue `GetDesc` struct-return clobber, not a console GPU-VA dereference (`4178c742c`). Draft corrected and held |
| #3 | bittertea0323 | open · ours to answer | FSPX corpus on a second host: 18/29 translate, 11 fail `fall-through into undecodable words` and cluster on wave32; per-program word counts; asks for a preferred report shape | Addressed by `0276f2b1f` (repair → 29/29); draft held. **Comment (NinosTech, 09:21Z):** four FSPX translations from an older upstream revision cleared four lookup misses and made the 42 compute-pipeline calls succeed, but the captured frame still had 0 non-black pixels of 896,232; not matched to our cases — on our current tree that split still holds, and the black frame is now measured as the draw's own vertex/geometry path (O18) plus empty sampled content (O19), not the FSPX set |

Closed: #1 — PR "Main"; #5 — closed as completed on `4178c742c` (its own stop was fixed; later frontiers are tracked as separate items, not a reopen).

## Draft replies (HOLD — not posted; awaiting repo-owner approval)

- **#2:** "Thanks — that is answered now. The slot-108 allocation answer was invalid, which made the title's own
  render-target factory return null; it is fixed (`c3c7a7b83`, then `cc7e044b2` for A8 render targets), and current
  `main` boots past that fault into its frame loop. The `0x100000000` read we chased turned out to be our own queue
  `GetDesc` struct-return clobber (`4178c742c`), so it was this layer, not the translator. Please re-run `main`;
  `PALMETTO_CRASH_REPORT` gives the faulting-thread dump."
- **#3:** "Thanks for the per-program counts — the build now repairs each FSPX listing before translating
  (`tooling/widen_smem_decode.py`) and measures 29/29 passing `spirv-val` (22/29 without), so the wave32 cluster is closed
  in tree. Per-program stop word is still the report shape we would want for any surviving refusal."
- **#3/NinosTech:** "Thanks — that matches the split we measure: resolving the lookups moves the 42 creations to
  success, while the frame's black on our tree is a separate, later frontier — the presented target is proven writable
  and is the draw's own target, so what is missing is the title draw's vertex/geometry coverage and its sampled content,
  not the FSPX set. We will check your four programs against ours."

## Our findings — open

| id | area | item | state | evidence |
|---|---|---|---|---|
| O3 | port/diagnostics | Debugging gaps from the public attempt: crash-memory save `7c70724b7` (`PALMETTO_CRASH_SAVE`), faulting-thread unwinder `7eab981ad` (`PALMETTO_CRASH_REPORT`) and per-run report `0e76519a0` (`tooling/run_report.py`) landed; the every-thread stack dump is still missing, and the unwinder's exception hook is implemented and tested on synthetic images but not wired | open, partial | `docs/crash_report.md`; with the title now running, a fault-time all-thread dump is the highest-value remaining diagnostic |
| O5 | build (full build) | A full configure+build of the runtime tree is red on a small set of pre-existing targets: `d3d12_console_descriptor_heap_test` (the local `Ref<>` helper has no `operator->`/`GetCPUDescriptorHandleForHeapStart`), `d3d12_m2_formats_test` (MSVC 14.50 internal compiler error C1001), and `d3d12_existing_heap_test` (stale expectation: `CreatePlacedResource` expects `E_NOTIMPL`, current behaviour is `E_INVALIDARG`); only the `palmetto` target and the gate tests are built, so the other test targets never compile | open — repair in progress; the seven gate targets stay green | full-build log: `palmetto` = `PALMETTO_OK`, gate tests = `TESTS_BUILT`; `docs/testing.md` is to record the fix or the documented exclusion and reason per target, and what would unblock it |
| O8 | build | Shader stage's completeness gate blocks the designed `--stages shaders` path: the translator refuses two duplicate-name variants the old corpus had, so stages had to be run individually | open | `62b510467`, `9271ef74c`; `docs/boot_frontier.md` (corpus section) |
| O9 | build/perf | Pipeline-library disassembly is quadratic: `llvm-mc` prints the whole input hex line once per invalid vcc-tuple word, so a 120 KB program becomes a 100–134 MB listing (one file 137 MB); 841 of 2,684 programs in 17 minutes, 9.4 GB | open | `docs/boot_frontier.md` (corpus section), `9271ef74c`, `62b510467` |
| O15 | runtime | The slot-57 fixed-function state record is undecoded: its container decodes on all 5,680 distinct blobs but no field is pinned to a D3D12 rasterizer/blend/depth-stencil/topology/strip-cut/sample field, so every pipeline carries the logged `documented_default_state()` and format stand-ins | open — fidelity work, **not** the black frame: the stand-in's cull axis and the attachments are falsified by measurement | `docs/state_blob.md` §2/§5/§8, `docs/graphics_pso.md` §4/§6, `docs/frame_capture.md` §6-8. Next: pin a field by behavioural probe only where that probe supports the exact value, and keep every unpinned field refused |
| O18 | port/replay (frame content) | The title's vertex/geometry path produces no fragments in the presented target: with the title's own vertices and pipeline recipe but the project's constant-output pixel shader, and again with culling off, the presented resource reads back black while the project's own full-screen triangle writes its exact colour into the same target | open — **frame-content frontier** | `docs/frame_capture.md` §5-8: presented resource read back after its last writer `mean 0 0 0, non-black 0/16,384`; control `32 160 240, 16,384/16,384`; cull-off repeat still black; the sampled constants block is a plausible transform matrix (`3f800000` on the diagonal). Next: dump the positions the translated vertex shader writes and the bytes its input layout reads, and compare them with the view's 24 non-zero bytes |
| O19 | texture/sampling | The six-image pixel shader's first sampled texture resolves to a real 4K image whose sampled window reads back black, and no upload path runs in the measured windows (0 copy, copy-source pull, dispatch, indirect command or reserved-resource creation in 150 s and 300 s boots) | open — a second, independent frame-content gap | `docs/frame_capture.md` §7, `docs/page_content.md` §7-8; the title's CPU fill destinations are in memory no placed texture names (`docs/texture_import.md` §3). Next: sample the other T#s of the same table and a window away from the origin, so a black window is not mistaken for an empty image |
| O20 | loader/streaming | The consumer of the streamed DirectStorage blocks is unnamed: 4,096 requests / 256 MiB decompress into ordinary process-heap 64 KiB blocks, the title polls its status array (`EnqueueSignal`/`EnqueueSetEvent` are never called), and no instrumented device call maps, views or copies a range overlapping those destinations in the observed window | open | `docs/wait_state.md` §3, `docs/texture_import.md` §4 (caller RVAs of every creation/view/`Map` site; `CreatePlacedResource*` = 0). Next: caller-RVA logging on the remaining consumer entry points (heap creation, existing-heap opens, placed resources, copy-queue calls) and the title allocation that owns the destination range |
| O21 | runtime (XG layout) | XG texture-layout slots 2 (`GetResourceLayout`, a 0x6C8-byte record) and 10 (the tile list) refuse by name, which keeps linear layout mode opt-in; answering the family does unlock the title's CPU upload path (65 subresource copies / 34,044 B) but nothing imports those pages into a sampled image | open | `docs/texture_layout.md` §2-4, `docs/texture_import.md` §5. Next: decode both records honestly, then decide whether linear mode can become the default |

**Priority.** O18 is the frame-content frontier: the presented target is proven writable and correctly bound, so the
question is what the title's vertex stage fetches and writes. O19 (sampled content) is a second, independent gap behind
it; O20 (the streamed consumer) and O21 (XG upload pages) are the two measured ways content could arrive. O15 is fidelity
work that refuses nothing today. O3's fault-time every-thread dump is the highest-value diagnostic while the title runs;
O5, O8 and O9 are build/test hygiene.

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
| build | `build_port` publishes the 1,266 graphics programs the title asks for (947 PS + 319 VS), with step ownership and shard-plan alignment | done `9271ef74c` | 1,266/1,266 found, translated and `spirv-val` clean; published as 1,337 fspx entries beside 29 FSPX + 42 library compute |
| runtime | Console queue slot 18 `GetDesc` struct-return order (the clobbered `rbx` at `game+0x2484133`; was O2, GH #5) | done `4178c742c` | the `0x100000000` read was a clobbered queue-desc buffer, not a GPU-VA dereference; stop moved to `game+0xcc2861`, calls 11,906 → 11,986 |
| build | Pipeline-library extraction and FSPX listing repair wired into `build_port.py` | done `0276f2b1f` | FSPX 29/29 with the repair (22/29 without); library 42/42 on the 42-program extraction, 1,047/1,684 on a real package subset |
| runtime | Console device slot 108 answers a real resource allocation info | done `c3c7a7b83` | null-object stop at `game+0xCC2861` gone; run continued to `game+0x24844A9` |
| runtime | Console queue slot 35 calibration answered (plus nonvolatile register dump) | done `818c739af` | stop unchanged at the time, so not the cause; the dump carries `rbx`, `r12`–`r15` |
| runtime | DirectStorage zlib stage inflates instead of failing with `E_NOTIMPL` | done `ae876f4b5` | 2,238 → 11,894 D3D12 calls; 0 zlib requests fail |
| runtime | AMD-only `clzero` emulated on Intel hosts | done `f76446847` | illegal-instruction fault gone; frame-loop event waits reached, 11,906 calls, 0 pipeline failures |
| runtime | Console device slot 55 answered as `CreateDerivedPipelineStateX` instead of being forwarded to PC `CreateReservedResource1` — answers O13 | done `b00248cae` | 41,281 `E_INVALIDARG` mis-forwards became **41,545 derived pipeline states, all `S_OK`, 0 `CreateReservedResource1` calls**, ALIVE with no device removal; the frame stayed black, so this slot is not the black frame's cause (`docs/device_slot55.md`) |
| runtime | Page address space: the title's page VAs rebased into one arena — the placed buffer's VA registered as an alias, the console descriptor heap gets an arena mirror, every shader-visible copy is rebased at replay, and anything unrebased still refuses — answers O11 and O17 | done `ea771ec60` | 150 s boot: **17,674 draws execute, `ExecuteCommandLists` 35,349 ok / 0 failed / 0 refused**, 0 TDR, no device lost, 0 failed `Close` (`docs/page_address_space.md` §4) |
| runtime | Title page contents pulled into the GPU stand-ins: per-draw pull of the vertex/index/constant views and of copy sources, with request/pull counters and an A/B switch | done `38c845193` | the pull carries the title's real content (24 non-zero bytes in the 0xFF0-byte vertex view, 78 in the 0x1E0-byte index view) and 131,072 pulls / 273.0 MiB per 300 s; the presented frame stayed black, which moved the frontier to the draw's own output (`docs/page_content.md` §1-3, §8) |
| port/display | Console device slot 127 creates the display resources: the generic beyond-vtable stub used to answer it with `S_OK` without writing the out pointer, so the title kept a surface whose resource field was null — answers O12 | done `804a01f06` | the present plane's first resource is non-null, the frame's clears reach the command list, and the presented resource id equals the frame draw's target (`docs/surface_population.md`, `docs/frame_capture.md` §2) |
| port/presentation | `PresentX` wired end to end: the registration hook the proxy can use without linking Vulkan, the frame-token bridge, and the presenter window — answers O14 | done `523d94759` | the runtime presents the title's own display resources: 17,674 `PresentX` ok / 0 failed in the address-space boot (`docs/present_wiring.md`, `docs/page_address_space.md` §4) |
| port/presentation | Single-window adoption: the present service adopts the title's own `sgaWindowClass` window instead of creating a second one, with the own-window path unchanged as fallback and an env switch for the A/B | done `914e0adc4`, measured `d13d94b59` | the adoption boot has exactly one visible top-level window in the process, 8,625 presents / 0 failed, ALIVE, no exception; the title's window keeps its own title, size, visibility and window procedure (`docs/window.md` §2/§5) |
| runtime | Slot-57 fixed-function state container decoded (two size families, a stage block, a shared block, a variable tail) while every field stays refused by name | done `92cedb55a`, wired `804a01f06` | the container decodes exactly on all **5,680** distinct state records referenced by 25,471 graphics records, 0 refused; **0 of 8** fixed-function fields pinned, so the stand-in stays and the boot numbers do not move (`docs/state_blob.md` §1/§5) |
| port/replay (diagnostics) | Frame capture: one frame's command order and targets, the presented resource's id match, the corrected `page_content.md` §4 reading, a known-good full-screen control and a title-vertices/constant-PS control (`PALMETTO_FRAME_DUMP`, `_CONTROL`, `_PS_CONTROL`, `_SAMPLE`) | done `0e1f06e47`, `ee9ebc6ac` | the presented target reads back black after its own last writer, the control reads back its exact colour (`32 160 240`), and the cull axis is closed; all switches off by default, recorded commands unchanged (`docs/frame_capture.md`) |

## Our findings — superseded

| id | area | item | superseded by | evidence |
|---|---|---|---|---|
| O1 | runtime | Renderer's three-slot record array (216 B; 72-byte records) is never initialised | the output-target measurement (`cc7e044b2`, `docs/container_element.md`): the stop is not reached on the current tree — the title boots into its frame loop, records its draws and submits them; the null-write was the renderer's null output-target element, fixed with the A8/slot-108 change | the 216-byte array's own population is not separately re-measured — a limit of this refresh, not a claim |
| O6 | runtime | Unified-memory requirement: the title CPU-dereferences console GPU image addresses; our fabricated VAs (from `1 << 32`) are unmapped | the queue slot 18 measurement (`4178c742c`): the `0x100000000` read was our own `GetDesc` struct-return clobber writing into the queue object, and no measured stop needs a CPU-visible fabricated VA. The remaining host-memory requirement became O16, and is now the page content work below | `PALMETTO_BACK_IMAGES` was a diagnostic, never needed by the boot |
| O10 | runtime | New top stop `game+0x4fb0aec`: call through `qword [rax+0x60]` where `[rcx]` is `0x3b9aca00` (10^9) where a vtable pointer belongs | the queue slot shift fix (`e1a40382d`, `docs/frontier_4fb0aec.md`): the 10^9 was our own `GetTimestampFrequency` answer written over the fence proxy's first qword, because console queue slot 16 is not PC slot 16; the boot now runs, executes its draws and presents | the two hypotheses the old row named (a size written as a pointer, or a title sentinel) are ruled out by the object dump |
| O12 | port/display | The title's own display surfaces carry a null resource: `CreateRenderTargetView` is called with `pResource = [surface+0x20] = 0` and `PresentX` reads the same zeroed field, so the frame's clears and the present aim at nothing | the slot-127 fix measurement (`804a01f06`, `docs/surface_population.md`): the null was the beyond-vtable stub not writing the out pointer; the display surfaces now carry real resources, the present plane's first resource is non-null and its id equals the frame draw's target, so clears and present name a real resource — and that resource is black for the reasons in O18/O19, not because nothing was aimed at | `docs/frame_capture.md` §2; the 23,819-writes-of-0 watch in `docs/display_path.md` recorded the null surface of the old tree, not the presented path |
| O16 | port/memory | Title-placed buffer pages cannot be imported: the whole 174 MiB range is not importable (`E_NOTIMPL`), the fallback is an ordinary stand-in buffer, the title's CPU writes never reach the GPU, and the stand-in's registered VA is the arena VA, not the console VA | the page-address-space fix (`ea771ec60`, `docs/page_address_space.md` §3) and the page pull (`38c845193`, `docs/page_content.md` §1-2): the stand-in is registered under the console VA, every shader-visible title VA is rebased at replay, and the pull copies the title's pages into the stand-in with real content (24 non-zero bytes in the vertex view; 273 MiB per 300 s). The "writes never reach the GPU" limitation is now the content frontier (O18/O19), not an import wall | the import itself is still refused and is not needed: rebasing plus the page pull supersede the import design (`docs/page_address_space.md` §1) |

## Notes (not bugs)

- **Measurement recipe at this refresh.** Retail image, shader corpus, Vulkan backend, `PALMETTO_SKIP_INT3=1`,
  `PALMETTO_SEED_GLOBAL=75DF268:D8`; one boot at a time, a boot runs to its timeout; harness `tooling/boot_report.py`.
  Every number here is from the committed tree at `ee9ebc6ac` or the diagnostic increment named on the page.
- **Executing an un-rebased title VA faults the GPU — which is why the rebase exists.** The old one-registration candidate
  ended in a TDR and a title `0xC0000005`; the fix now rewrites every shader-visible title VA to an arena VA before replay,
  and the draws execute with no TDR (`ea771ec60`). Anything the map cannot rebase still refuses, never executes.
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
  misses and the 42 compute calls succeeded, yet 0 non-black pixels of 896,232. On our tree that split holds: the black is
  now measured in the title draw's own vertex/geometry path (O18) and its empty sampled content (O19), not in the FSPX
  corpus; matching his four programs to ours still needs his hashes.
- **Lifetime-tracker prototype (evidence, not a fix).** Implemented correctly and unit-tested, then reverted because the
  boot regressed 11,986 → 1,653 calls through a wrong vtable slot — evidence slot 57 is not a tracker (O4, now done).
- **Waiting is not the blocker.** Thread samples in a 200 s boot put every thread in an ordinary pool, condition, frame or
  audio wait, and the DirectStorage queue is never asked for a fence signal or an event (0 `EnqueueSignal`, 0
  `EnqueueSetEvent`) - the process is not blocked, it is in a pre-content loop (`docs/wait_state.md`).
- **docs and tracker.** README's progress tracker is generated from `docs/progress.json` by `tooling/readme_progress.py`;
  both are refreshed to this state (`--check` passes). The public-comparison page earlier revisions linked no longer
  exists in the sanitised tree, and the tracker and the docs index no longer reference it.
