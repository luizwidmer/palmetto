# Issue ledger

Ledger of **what people are sending** to the project's GitHub home, plus our own tracked findings for the boot port. Each row carries area, state and one-line evidence. Counts, error classes and commit ids only — no game data, package paths, shader bytes or hashes.
Maintained by the issue-tracker agent as a standing, read-only-plus-this-file role.

**Repo:** `palmetto`, branch `main`, HEAD `bcb030b44` · **GitHub:** [luizwidmer/palmetto](https://github.com/luizwidmer/palmetto)
*HEAD is the observed checkout for this sweep; the lead will refresh this field at integration.*
**Updated:** 2026-11-27 (live GitHub state and the task-board pointer; the pre-rename numbers stay historical) · **Incoming:** #1 closed (re-baselined), #2 closed (post-rename residue) · **Ours open:** 9 (O8, O9, O15, O18–O23) · **Done:** 26 · **Superseded:** 5 (O1, O6, O10, O12, O16) · **Open PRs:** 0.

**Policy: ledger only.** The task board and issue triage are the maintainer's lane; do not post replies to reporters
without the repo owner's approval - that part is still **HOLD**. Work claims are public on the board, not in this file.

## Current measured picture frontier (2026-11-27)

**Latest picture evidence (Windows 11/RTX 4090; later arms are separate):** On the default, unmitigated path, a clean
stage-3 arm reaches the title-side class-0xa zero-byte overrun and faults about a second later. The opt-in title-side
`PALMETTO_TITLE_CLASS0A_SIZE=1` experiment sizes that upload to 0x1000: a clean 300 s arm enters and holds stage 3 for the
remaining ~230 s, yet the first-run page owners remain at 0 hits; the movie player reaches state 9 with `buffers 0`, and
frame-buffer registration/per-frame decode both remain at 0 hits. Captures stay black. Separately, the presenter's
synthetic four-band control reaches capture with its expected values. The current visible-content frontier is therefore
the title-side UI/movie draw path, not the earlier stage-2 wait or a missing decoder shim/presenter. The mitigation is
off by default and does not establish console-faithful behavior. See [decomp_first_ui.md](decomp_first_ui.md) sections
16-18, [peer_port_first_ui.md](peer_port_first_ui.md) sections 14-15, and [first_visible.md](first_visible.md).

**Round-22 update (2026-11-28):** the session-3 completion wall was forced and measured. Writing 1 into the loading
record's slot-4 completion dword (`image+0x78ACB20`, the dword the 33 ms poll tests) took the title to **session 1 -> 13**
at 68.0 s with the stage-3 pump running; it then stopped at the AMD-only SSE4a `INSERTQ` instruction at `image+0x2E7FF1`
(a real half-float packing routine). A clean-room SSE4a INSERTQ/EXTRQ emulation now runs in the illegal-instruction
handler (`port/src/sse4a_emulation.h`, 25 synthetic checks), removing that stop by construction. The media/UI present-pass
keys, forced only in a stage-3 window, and the out-record state-2 redirect exist as opt-in diagnostics in
`port/src/title_frontier_patch.h`. The title's own window now exists and is visible, but its client area is pure black in
every window screenshot and every presenter capture so far; later long arms lost the Vulkan device before reaching
session 3, so no menu pixel is claimed. The visible-content frontier is now **post-session-3 execution on a stable host**,
followed by the media/UI present pass - not the session-3 wait. See [decomp_first_ui.md](decomp_first_ui.md) section 20.

### Historical issue-1 baseline (measured on one machine; one 150 s boot unless named)

The retail target console title
boots and runs through the runtime: **ALIVE past 300 s with no exception**; **all 10,241 slot-57 records build into real
PC graphics pipelines** and **41,545 derived pipelines** are created through the console slot-55 answer; after the
title's page VAs were rebased into one arena the frame's draws **execute with 0 refused submissions**; the runtime
**presents into the title's own adopted window**; and a full configure+build of the tree is **green (237 of 237
targets)**. The boot is parked in the title's stationary pre-content loop at stage 2: the stage-2 job-graph wait, where
the entry thread waits inside the boot sequence's completion wait on a tracker event that no measured producer signals
(`docs/stage2_blocker.md`). The frame that loop presents is **black by design**, not missing driver data: the constant
pass's only colour source is a title-owned block whose colour window is zero and which nothing in the loop writes
(`docs/colour_path.md` §5), so the content frontier is the title's stage step. The earlier frame-content causes - the
vertex path's degenerate transform (O22) and the sampled 4K placed texture whose pages are never written (O19) - are
measured limits of that same stationary loop and stay in the findings table below; the colour-path measurement is what
explains the loop's black. The streamed 256 MiB are read by a **title CPU deserializer and never named to the device**
(O20 is closed as an upload lead), the XG **slot-2 resource-layout record is decoded and the linear layout is now the
default** (O21 is slot 10 only), console **device slot 70 is decoded as a per-frame descriptor publication** whose
encoding is not modelled (O23), and the slot-57 fixed-function record's **8 fields cannot be pinned by a behavioural
probe** (O15). Evidence is page-driven: `docs/stage2_blocker.md`, `docs/colour_path.md`, `docs/issue1_baseline.md`,
`docs/frame_capture.md` §2-8, `docs/vertex_path.md`, `docs/texture_source.md` §6, `docs/streamed_consumer.md` §3-6,
`docs/texture_layout.md` §3-4, `docs/device_slot70.md`, `docs/state_blob.md` §9, `docs/testing.md` ("The full Windows
build").

## Incoming reports (GitHub)

Live issues on [luizwidmer/palmetto](https://github.com/luizwidmer/palmetto/issues):

| # | sender | state | what they sent | tracking |
|---|---|---|---|---|
| #2 | XccQ114 | closed (done) | Post-rename de-branding residue, checked against the tree and the GitHub API: the claim procedure and the "How to build" contact link still spell the repository as a literal placeholder in GitHub URLs and a `gh -R` command (`CONTRIBUTING.md`, `AGENTS.md`, `.github/ISSUE_TEMPLATE/config.yml`); this ledger cited issue numbers from the pre-rename repository; two de-branding leftovers remain (`docs/movie_path.md` author name, `docs/bcpack_runtime.md` env-var name); `cmake -S runtime` names a directory with no `CMakeLists.txt` (`AGENTS.md`), while the source directory is `port` | Being fixed in this pass: the real repository in the links and commands, the title-based search for the coordination issue with an open-it-yourself fallback, this ledger rewritten (the pre-rename numbers noted below), both leftovers retired, `-S port`; the same placeholder-phrase sweep also corrected `build_port.py`'s clone instruction, the dashboard's default repository and the two tests that asserted the old strings. No reply posted (HOLD) |
| #1 | marekpastuszczak-lang | closed (done) | A finding, not a fix: the black loading loop is the title's init state (2); four shim answers that differ from the console (silent speech, slot-25 depth-stencil + unordered-access sizing, sampled A8 1D/3D, device slot 74) and the two `int3` asserts those answers remove | Closed after re-baseline and implementation: [`docs/issue1_baseline.md`](issue1_baseline.md) re-measured the probe after the read repairs, landed the three deltas still absent at HEAD (slot-25 stand-in, sampled A8 1D/3D, device slot 74 `S_OK`), removed `game+0x1e6c2c5` and `game+0x344196`, and attributed the two remaining asserts to console device slot 41 (`CreateCommandSignature`); the issue's state words still hold (game state 2, session 0, still-black stationary loop) |
| [#4](https://github.com/luizwidmer/palmetto/issues/4) | XccQ114 | open - task board | The shared coordination issue, now the canonical task board: claims and statuses indexed by task ID, with the repo copy of the rules and backlog snapshot in [taskboard.md](taskboard.md). |
| [#5](https://github.com/luizwidmer/palmetto/pull/5) | XccQ114 | merged (done) | Crash report: dump every thread, not only the faulting one (O3) - verified on a test merge, approved and merged rebased. |
| [#6](https://github.com/luizwidmer/palmetto/pull/6) | XccQ114 | merged (done) | Documentation check of `docs/crash_report.md` against the merged every-thread dump (D-05): step 3 now places `OpenThread` outside the suspend window, both report switches are documented as read with `GetEnvironmentVariableW(..., 512)` (512+ characters are ignored), and the routinely-handled exception gate is six codes (fixed from five). Reviewed against `crash_unwind.cpp`/`startup_trace.h` on current main, docs checks green on the merge result, squash-merged as `bcb030b44`. |

**Pre-rename issue numbers are historical only.** Earlier versions of this page cited `#1`-`#5` from the repository
that existed before the rename: that repository was deleted, its issue objects are gone, and the re-created repository
reused the numbers for different objects (the live `#1` above is the closed black-loading finding, not the old `#1`;
the old `#5` has no counterpart here at all). Treat every such number quoted by an older page, note or commit message
as narrative history with no tracker link; the table above is the only live incoming list. The reports behind the old
numbers cannot be replied to any more: nothing was posted, and the old draft replies are withdrawn as historical.

## Task board

[taskboard.md](taskboard.md) is the repository copy of the board rules and backlog; the pinned coordination issue (#4) is
the live board. New work is claimed there by task ID (`P-`, `C-`, `D-` rows marked open; `F-` rows and maintainer rows need
the maintainer's OK), and the row links the evidence page that defines what done means.

## Our findings — open

| id | area | item | state | evidence |
|---|---|---|---|---|
| O8 | build | Shader stage's completeness gate blocks the designed `--stages shaders` path: the translator refuses two duplicate-name variants the old corpus had, so stages had to be run individually | open | `62b510467`, `9271ef74c`; `docs/boot_frontier.md` (corpus section) |
| O9 | build/perf | Pipeline-library disassembly is quadratic: `llvm-mc` prints the whole input hex line once per invalid vcc-tuple word, so a 120 KB program becomes a 100–134 MB listing (one file 137 MB); 841 of 2,684 programs in 17 minutes, 9.4 GB | open | `docs/boot_frontier.md` (corpus section), `9271ef74c`, `62b510467` |
| O15 | runtime | The slot-57 fixed-function state record cannot be pinned from this title's data: the container decodes on all 5,680 distinct blobs, and a deterministic GPU-free behavioural probe contract exists (seven scenes, a contract table), but every field group keeps several candidate places, fill mode has no candidate word, the sample-mask domain is fully open, attachments have no candidate place in the container, and the title's draws supply no observation (no fragments) — **0 of 8 fields pinnable**, so the decoder keeps refusing by name and every pipeline carries the logged stand-in | open — behaviour alone cannot decide it; a pin needs the serialized-layout source or a driver trace | `docs/state_blob.md` §2/§5/§9 (`run_probe()`/`kProbeContract`, `graphics_pso_test` 215 checks, `graphics_record_test` 158), `docs/frame_capture.md` §6-8. Keep every unpinned field refused |
| O18 | port/replay (frame content) | The presented 4K image is black although it is the frame's own draw target and is proven writable: the frame's record is clean (one clear of the display resource before the draws, the draw is its last writer, no copy or resolve anywhere), the fixed-function stand-in and the cull axis are falsified, and the target reads black at its origin and its centre while the V# constants read back a plausible transform matrix | open — **frame-content frontier**; the two measured causes are O22 (the vertex path is degenerate: the transform scalars never arrive) and O19 (the sampled content is never produced) | `docs/frame_capture.md` §2-8: presented resource read back after its last writer `mean 0 0 0, non-black 0/16,384`; control triangle `32 160 240, 16,384/16,384`; cull-off repeat still black; constants `3f800000` on the diagonal |
| O19 | texture/sampling | The six-image pixel shader's sampled T# is a placed 4K RGBA8 texture whose title pages are committed, readable and **never written**: the page-writer watch reads 0 non-zero bytes of 129,792 scanned (all 507 64 KiB pages, 256 B of each head) at every sample over 150 s, and the placed-texture import with its cap lifted to 64 MiB finds an all-zero head, waits out its budget and gives up — there is nothing to copy | open — the page-writer negative closes "a fill landed elsewhere": the pages were never filled; the next named place is the title-side producer that should write this resource | `docs/texture_source.md` §4/§6 (`PALMETTO_TEXTURE_PAGE_WATCH=1`, `PALMETTO_TEXTURE_IMPORT_MAX=67108864`); sampled-image readback stays `mean 0 0 0, 0/16,384`; the XG fill writes only the 32 KB atlas and 1x1 defaults (`docs/texture_layout.md` §5). Next: find that producer, or bound where the streamed-block CPU consumer stops (between requests 16 and 256) |
| O20 | loader/streaming | The consumer of the streamed DirectStorage blocks is **named**: all 4,096 blocks / 256 MiB land in ordinary process-heap addresses, a title CPU deserializer (`game+0x248F680`, reached as `[obj+0x18]` through the job wrapper `game+0x4F65820`) reads the first batch's blocks at ~2.3 s, and no device call names any of the 4,096 ranges — 0 range hits in the 14 argument words over 13,629 calls (13,568 of them console slot 70) and 0 in every argument-pointed structure. The earlier A/B block addresses are withdrawn: they were saved-register and caller-stack carry-over, not arguments | open — named, **no upload lead in the window**; the "do the streamed bytes reach a texture" hypothesis is closed | `docs/streamed_consumer.md` §3-6 (`PALMETTO_STREAM_CONSUMER=1`, `PAGE_GUARD` first-touch, three 130 s boots); the only block pointers near these calls sit in the caller's own frame structures (slot 19 calls 12-16, blocks #44/#45). Next: O23's publication encoding, the five slot-19 caller-stack structures, and the consumer stop between requests 16 and 256 |
| O21 | runtime (XG layout) | XG slot 10 (`GetResourceTiling`, D3D12's seven-argument shape, 12-byte subresource entries) still refuses by name: its structures are decoded, but a tile list only exists for a 64 KiB-tiled resource and the measured window creates none, so filling tile indices would be fabricated. Slot 2 (`GetResourceLayout`, a 0x1B40-byte record) is done and the linear layout is now the **default** | open — refused with the reason named; needs a title path that creates a tiled resource (or a measurement that the title never names such a list) | `docs/texture_layout.md` §2.2/§3; the default boot fills 2 slot-2 records, copies 65 slot-8 subresources (34,044 B), 0 exception/device-lost/TDR, frame still black |
| O22 | port/replay (geometry) | The title's vertex path is measured **degenerate**: the armed draw's translated FSPX blit vertex shader computes its clip position as one fetched vertex register times four push-constant scalars, and `s8`, `s9`, `s10`, `s28` are zero in the draw's recorded user data - every vertex lands at `(0,0,0,0)`, so `w = 0` and nothing rasterizes (forcing culling off cannot help a degenerate triangle). The vertex data and the bound input layout are sane, and the title issues 31,458 `SetGraphicsRootDescriptorTable` calls but **0** `SetGraphicsRoot32BitConstant*`, so the scalars can only come from the slot-57 record's root-argument block, which the record decode does not reconstruct yet | open - measured degenerate vertex path; the record's root-argument block is being decoded now (rootargs agent); **acceptance = the four scalars non-zero with a provenance, a `non-degenerate` verdict and a changed readback** | [docs/vertex_path.md](vertex_path.md) (it also carries the acceptance test lines: per-dword root-parameter provenance - `table`/`root`/`unset`, and `loose` for a record-filled dword no root parameter covers - and the blit transform's `degenerate` check; the measured boot prints `s8 unset s9 unset s10 unset s28 unset` and `clip-position scalars x 0 y 0 z 0 w 0 -> degenerate`), `docs/frame_capture.md` §6-8. The acceptance boot must print `non-degenerate` with a non-zero `w` and a provenance on `s8`/`s9`/`s10`/`s28`, and the measured readback must change |
| O23 | runtime (console device slot 70) | Console device slot 70 is decoded as the **per-frame descriptor publication**: 13,568 calls per boot (plus 74 at a wrapper site), three arguments - a record (byte size `0x1688`, element count at `+0x18`, a CPU block at `+0x28`, a format word at `+0x30`), a 32-byte destination slot in a shader-visible descriptor heap (zeroed before 12,214 of the first 12,288 calls), then a `clwb` cache-line publication. The default answer stays the measured refusal (`E_NOTIMPL`, default path byte-identical); the naive constant-buffer-view conversion is **falsified**: the descriptor it writes names the destination address rather than a buffer, the frame is refused and the boot stops at 29.5 s | open - decoded shape and ABI, one conversion falsified; the **publication encoding** is not modelled. Next: decode the record's encoding (fields `+0x18`/`+0x28`/`+0x30`, the 128-byte block and the format table at `game+0x4D9F3C0`) and publish it into the console heap's GPU mirror | [docs/device_slot70.md](device_slot70.md) (probe boot 140 s ALIVE, 594,356 calls; conversion boot stops at 29.5 s; default-path validation boot 100.1 s ALIVE, readback unchanged black), `docs/streamed_consumer.md` §5, `docs/boot_audit.md` §5 |

**Priority.** The frontier is the stage-2 job-graph wait (`docs/stage2_blocker.md`): it keeps the title in its
stationary pre-content loop, and the frame that loop presents is black by design (`docs/colour_path.md` §5). The
frame-content rows are measured limits of that loop: O22's vertex path is degenerate - the four transform scalars never
arrive, with the acceptance gate written down in `docs/vertex_path.md` - and O19's sampled 4K placed texture's pages
are never written, so the producer that should write them is the next named place once a content pass runs. O20 is
closed as an upload lead: the streamed bytes die in a title CPU deserializer
and no device call names them. O23 (slot 70's publication encoding) and O21 (slot 10) are the remaining unmodelled
console entry points on that path. O15 cannot be pinned by
behaviour and needs a layout source. O3's fault-time every-thread dump is the highest-value diagnostic while the title
runs; O8 and O9 are build/corpus hygiene. The full build is green (237/237), so O5 is done.

**Ledger note (2026-11-27):** the historical issue-1 baseline above and the findings below preserve their original measurements; they are not rewritten as the live frontier. Read the current measured stage-3/mitigation state above, and use the [task board](taskboard.md) for current ownership and next work.

## Our findings — done

| area | item | state | evidence |
|---|---|---|---|
| port/diagnostics | Debugging gaps from the public attempt: crash-memory save `7c70724b7` (`PALMETTO_CRASH_SAVE`), faulting-thread unwinder `7eab981ad` (`PALMETTO_CRASH_REPORT`), per-run report `0e76519a0` (`tooling/run_report.py`) and the every-thread dump (PR #5, `a2eabe94c`) landed; the documentation check (PR #6, `bcb030b44`) corrected the page to match the shipped sources | done `bcb030b44` (D-05) | `docs/crash_report.md`; `crash_unwind.cpp`, `startup_trace.h` |
| runtime | Slot-57 deserialized graphics record → real PC graphics pipeline: `graphics_pso` builder wired, then the input-layout element table decoded (count at `+0x4C`, length-prefixed names, 24-byte D3D12 attribute block, DXGI format words 2/6/16/28/30) — answers O4 | done `9b0d696b5`, `d0095ca70` | 1,074 built / 8,926 refused at the layout blob before the decode; after: **10,241 of 10,241** build, 0 PC failures, 42,721 `SetPipelineState` / 42,722 `DrawInstanced` recorded in the decode boot and 43,750 of each in the later replay baseline boot (`docs/input_layout.md` §6, `docs/slot57_wiring.md` §2, `docs/replay_resolution.md`) |
| runtime | Console clear (list slot 52) and console barrier (list slot 146) converted: a null-view clear is the no-op it is, a real view forwards unchanged; resource-less texture barriers become global barriers, real-resource entries pass through | done `cf825083d` | 60 s boot with no mutes: 30,999 `Close` S_OK / 0 failed, 30,376 reopens and submits, 75,937 barrier records; the frame rate is roughly half the muted control because the barriers are now real, with no submission lost (`docs/handles_barriers.md` §6) |
| runtime | Console command-list lifecycle: the proxy remembers the creating allocator and recording state and reopens a closed list before a forwarded recording call, refusing on a failed `Reset` | done `8dc42f405` | 4 reopens and 82,736 submits in 150 s; the first `Close` after a reopen started returning `E_INVALIDARG` (proof the recording reached PC validation) and exposed the two invalid calls above (`docs/list_lifecycle.md` §4–5) |
| runtime | Console queue slot 35 is the frame submit: forward `ExecuteCommandLists`; emulate the AMD-only `monitorx`/`mwaitx` pair on Intel | done `22555b249` | 0 → **78,745 submits per 140 s**, ALIVE, 2,292,332 traced D3D12 calls (`docs/boot_frontier.md`) |
| runtime | Console fence slot 12 answered with the address of the fence's completed value — the last stop before the title's own loop | done `77d2f6bba` | ALIVE 300 s / 400 s with no exception; 5,195,024 calls in 300 s; 245,524+ slot-12 answers (`docs/fence_slot12.md`) |
| runtime | Console queue slot shift: the console inserts two methods before `Signal`, so console 16/17/18/19/20 map to PC Signal/Wait/GetTimestampFrequency/GetClockCalibration/GetDesc — answers O10 | done `e1a40382d` | the 10^9 was our own `GetTimestampFrequency` answer written over the fence proxy's first qword; stop `game+0x4fb0aec` → `game+0x4fb0b03`, traced calls 12,085 → 12,087 (`docs/frontier_4fb0aec.md`) |
| d3d12 | A8_UNORM render targets accepted as R8_UNORM colour attachments with `RENDER_TARGET`, and the slot-108 bridge treats `SizeInBytes == UINT64_MAX` as unanswered — answers O7 | done `cc7e044b2` | the null container element was the title's own 3840x2160 1-sample A8 render target whose factory failed on the invalid allocation; "format not supported yet: 65" gone, stop `game+0xcc2861` → `game+0x4fb0aec`, calls 11,986 → 12,085 (`docs/container_element.md`) |
| build | `build_port` publishes the 1,266 graphics programs the title asks for (947 PS + 319 VS), with step ownership and shard-plan alignment | done `9271ef74c` | 1,266/1,266 found, translated and `spirv-val` clean; published as 1,337 fspx entries beside 29 FSPX + 42 library compute |
| runtime | Console queue slot 18 `GetDesc` struct-return order (the clobbered `rbx` at `game+0x2484133`; was O2) | done `4178c742c` | the `0x100000000` read was a clobbered queue-desc buffer, not a GPU-VA dereference; stop moved to `game+0xcc2861`, calls 11,906 → 11,986 |
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
| runtime | Fixed-function state: a deterministic, GPU-free behavioural probe contract (seven scenes, contract table) is built and tested; measured against the 5,680 distinct state records, every field group keeps several candidate places, fill mode has no candidate, and the title's draws supply no observation — so **0 of 8 fields are pinnable by probe** and the decoder keeps refusing by name | done `56f0de563` | `run_probe()`/`kProbeContract` in `port/src/graphics_pso.{h,cpp}`; `graphics_pso_test` 215 checks and `graphics_record_test` 158; boot ALIVE with 10,000/10,000 records decoded and 10,000 pipelines created (`docs/state_blob.md` §9) |
| runtime (XG) | Console texture-layout slot 2 `GetResourceLayout` decoded and filled: the record is a 0x1B40-byte layout (0x18 header + four 0x6C8-byte planes), not the 0x6C8 previously assumed; the linear layout answer is now the **default**, filling the two requested records exactly (2048x2048 and 1024x1024 single-level R8, every non-field byte zeroed) and refusing by name every description it cannot lay out honestly | done `d718a50ed` | default boot: 44 suggest / 24 create / **2 slot-2 records filled** / 65 slot-8 copies (34,044 B), ALIVE 150.2 s, 0 exception/device-lost/TDR, frame still black (content frontier separate); `xg_xs_test` 86 checks (16 in the refusal arm) (`docs/texture_layout.md` §3-4) |
| build | Full configure+build repaired: every target compiles and links (the console descriptor-heap test's local `Ref<>` API and expectations, the m2-formats C1001, and the targets that never linked); imported heaps are registered so `CreatePlacedResource` on an `OpenExistingHeapFromAddress` heap reaches the address branch and answers the documented `E_NOTIMPL` — answers O5 | done `8d36a4136`, heap fix `6a42fb445` | full build green **237/237 steps** (second run `ninja: no work to do`), gates `PALMETTO_OK` / `TESTS_BUILT`; three known failures documented with their unblocks (`d3d12_fence_timing_test` crash, `root_sig_test` two checks, `device_proxy_test` one check) (`docs/testing.md`, "The full Windows build") |
| port/replay (diagnostics) | Frame capture: one frame's command order and targets, the presented resource's id match, the corrected `page_content.md` §4 reading, a known-good full-screen control and a title-vertices/constant-PS control (`PALMETTO_FRAME_DUMP`, `_CONTROL`, `_PS_CONTROL`, `_SAMPLE`), plus the texture page-writer watch and import-cap override | done `0e1f06e47`, `ee9ebc6ac`, `9feab1b06`, `c9653e371` | the presented target reads back black after its own last writer, the control reads back its exact colour (`32 160 240`), the cull axis is closed, and the sampled 4K placed texture's pages never change (0 of 129,792 scanned bytes over 150 s); all switches off by default, recorded commands unchanged (`docs/frame_capture.md`, `docs/texture_source.md` §6) |

## Our findings — superseded

| id | area | item | superseded by | evidence |
|---|---|---|---|---|
| O1 | runtime | Renderer's three-slot record array (216 B; 72-byte records) is never initialised | the output-target measurement (`cc7e044b2`, `docs/container_element.md`): the stop is not reached on the current tree — the title boots into its frame loop, records its draws and submits them; the null-write was the renderer's null output-target element, fixed with the A8/slot-108 change | the 216-byte array's own population is not separately re-measured — a limit of this refresh, not a claim |
| O6 | runtime | Unified-memory requirement: the title CPU-dereferences console GPU image addresses; our fabricated VAs (from `1 << 32`) are unmapped | the queue slot 18 measurement (`4178c742c`): the `0x100000000` read was our own `GetDesc` struct-return clobber writing into the queue object, and no measured stop needs a CPU-visible fabricated VA. The remaining host-memory requirement became O16, and is now the page content work below | `PALMETTO_BACK_IMAGES` was a diagnostic, never needed by the boot |
| O10 | runtime | New top stop `game+0x4fb0aec`: call through `qword [rax+0x60]` where `[rcx]` is `0x3b9aca00` (10^9) where a vtable pointer belongs | the queue slot shift fix (`e1a40382d`, `docs/frontier_4fb0aec.md`): the 10^9 was our own `GetTimestampFrequency` answer written over the fence proxy's first qword, because console queue slot 16 is not PC slot 16; the boot now runs, executes its draws and presents | the two hypotheses the old row named (a size written as a pointer, or a title sentinel) are ruled out by the object dump |
| O12 | port/display | The title's own display surfaces carry a null resource: `CreateRenderTargetView` is called with `pResource = [surface+0x20] = 0` and `PresentX` reads the same zeroed field, so the frame's clears and the present aim at nothing | the slot-127 fix measurement (`804a01f06`, `docs/surface_population.md`): the null was the beyond-vtable stub not writing the out pointer; the display surfaces now carry real resources, the present plane's first resource is non-null and its id equals the frame draw's target, so clears and present name a real resource — and that resource is black for the reasons in O18/O22/O19, not because nothing was aimed at | `docs/frame_capture.md` §2; the 23,819-writes-of-0 watch in `docs/display_path.md` recorded the null surface of the old tree, not the presented path |
| O16 | port/memory | Title-placed buffer pages cannot be imported: the whole 174 MiB range is not importable (`E_NOTIMPL`), the fallback is an ordinary stand-in buffer, the title's CPU writes never reach the GPU, and the stand-in's registered VA is the arena VA, not the console VA | the page-address-space fix (`ea771ec60`, `docs/page_address_space.md` §3) and the page pull (`38c845193`, `docs/page_content.md` §1-2): the stand-in is registered under the console VA, every shader-visible title VA is rebased at replay, and the pull copies the title's pages into the stand-in with real content (24 non-zero bytes in the vertex view; 273 MiB per 300 s). The "writes never reach the GPU" limitation is now the content frontier (O18/O22/O19), not an import wall | the import itself is still refused and is not needed: rebasing plus the page pull supersede the import design (`docs/page_address_space.md` §1) |

## Notes (not bugs)

- **Measurement recipe at this refresh.** Retail image, shader corpus, Vulkan backend, `PALMETTO_SKIP_INT3=1`,
  `PALMETTO_SEED_GLOBAL=75DF268:D8`; one boot at a time, a boot runs to its timeout; harness `tooling/boot_report.py`.
  Every number here is from the committed tree at `c9653e371` or the increment named on the page.
- **Repository and publishing.** The published repository is this working tree: commits on `main` are the real ones,
  pushed directly (there is no mirror or publish helper step to record).
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
- **Lifetime-tracker prototype (evidence, not a fix).** Implemented correctly and unit-tested, then reverted because the
  boot regressed 11,986 → 1,653 calls through a wrong vtable slot — evidence slot 57 is not a tracker (O4, now done).
- **Waiting is not the blocker.** Thread samples in a 200 s boot put every thread in an ordinary pool, condition, frame or
  audio wait, and the DirectStorage queue is never asked for a fence signal or an event (0 `EnqueueSignal`, 0
  `EnqueueSetEvent`) - the process is not blocked, it is in a pre-content loop (`docs/wait_state.md`).
- **Acceptance gate for the pending root-argument fix.** `docs/vertex_path.md` carries it as test lines, not prose:
  the `PALMETTO_VERTEX_PATH=1` dump prints, for every push-constant dword, its root-parameter provenance (`table`,
  `root` or `unset`; a record-filled dword no root parameter covers prints `loose`), plus the blit transform's
  `degenerate` check. Acceptance = the `acceptance inputs` line shows `s8`, `s9`, `s10` and `s28` with a provenance
  (not `unset`), the transform line reads `non-degenerate` with a non-zero `w`, and the measured readback changes -
  it must not be claimed changed until it is measured.
- **docs and tracker.** README's progress tracker is generated from `docs/progress.json` by `tooling/readme_progress.py`;
  both are refreshed to this state (`--check` passes).
**Round-22 final measurement (windowshot4, binary `DA54119D`):** with the class-0xa mitigation, the forced session-3
completion and the stage-3-gated present keys, the title reached **stage 3 at 51.6 s and session 13 at 52 s** with zero
device losses and zero present failures; the SSE4a INSERTQ emulation carried execution past the previous stop. The
presented captures remain **9/9 black** and the readbacks are black plus the layer's own `51 102 204` write-control
frames - so the remaining wall is the content the media/UI present body chooses to present, not session completion or the
CPU-feature gap. No menu pixel exists yet; see [decomp_first_ui.md](decomp_first_ui.md) section 20.6.
**Round-3 probe (binary `FF7ECF51`, 300 s arm):** the stable stage-3/session-13 window repeated (ALIVE 300.2 s, 0 losses)
and the bounded present-key burst made the media/UI body take its present pass at 92 s; **presentation then froze**
(presents/submits 1,118/2,236 held for ~200 s) and all 23 captures stayed black. The 2D-batch probe recorded its first
**post-stage-3** hits (batch-build/submit 2 in the `>=3` bucket; rectangle/transform still 0) but no menu draw. The
remaining wall is the media/UI present pass waiting on its media/UI plane or worker event; next falsifier is a wait-trace
of the body during `body-present-pass` plus a target-binding probe on the UI batch submit path
([decomp_first_ui.md](decomp_first_ui.md) section 20.7).
