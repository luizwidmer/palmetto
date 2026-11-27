# Clean-room feature programme: what the peer ports document, and what we implement next

The public tracker at <https://vuyp.github.io/gta6-pc-race/> reads the public repositories of the projects
running the same console build on a PC. This page records the feature claims those projects publish about
themselves, what each means for this runtime, and the clean-room slice we will implement next. It is a
policy page as much as a plan: the peer projects are **evidence**, not a code source.


## Implemented peer ports (audit trail)

The port campaign reads the five public ports as evidence and lands our own implementations one coherent slice at a time.
This table is the audit trail: each row names the port page section that records the peer evidence and commits, the commit
that landed our implementation, and whether the port was measured on a boot or is readiness only.

| port | page / section | landed in | measured status |
|---|---|---|---|
| Present-driven frame clock and flip token | `peer_port_first_ui.md` section 12 | `7c867c03e` | measured A/B: handshake moved, title state unchanged |
| Never build or wait for a pipeline on a title thread | `peer_port_streaming.md` section 10 | `8b7a800be` | measured A/B: title-thread waits removed, worker pool still parked |
| Per-submission plane refresh and measured plane composition | `peer_port_movie.md` sections 8-9 | `9a86418c4` | measured: refresh live and free, one plane per present |
| Cross-list memory wait and GPU-ordered bottom-of-pipe writes | `peer_port_streaming.md` section 11 | `81841a3a6` | measured: unexercised at this population, GPU form falls back |
| Present attestation and hang-watchdog pause hold | `peer_port_first_ui.md` section 13 | `c14a70c26` | measured: switches exercised, stage-3 question blocked by device loss |
| Terminal read outcomes | `peer_port_streaming.md` section 12 | `4970e3c23` | measured: success half live, conversion unexercised |
| GPU-view reservation pairing | `peer_port_movie.md` section 11 | `4b0821bcb` | measured: pair present, no population yet |
| Row-aware XG upload for movie planes | `peer_port_movie.md` section 12 | `6da738f85` | measured: records 3/3, uploads 1/3 before; readiness, no decode window yet |
| Physical-page completion delivery | `peer_port_streaming.md` section 13 | `fdfacfdf2` | measured: zero physical-page destinations at this population, latent negative |
| Measured display answers and UI page probe | `peer_port_first_ui.md` section 14 | `ef46566d1` | measured: UI layer runs, page flow does not; display answers console-correct |
| Aliased page watch and backing-identity delivery | `title_memory_model.md` section 10 | `3323f7834` | measured CPU: aliases tracked across remap, fanout by Section+offset; no boot evidence |
| Overlapped submission counters and bounds | `peer_port_streaming.md` section 15 | `5798bb070` | measured GPU: queues and holds on the card; no title A/B |
| MS-capable mesh publication | `mesh_stage.md` section 8 | `5798bb070` | measured: build-pipeline omission closed; 0 MS resolver keys in the current corpus |
| DPI-aware window fit plan | `peer_port_first_ui.md` section 16 | `f2d6736a2` | measured planner; opt-in, no title arm yet |
| BVH route plan wired into RT slots | `bvh_route.md` | `a19ce9aab` | measured GPU: route arm on update/postbuild/copy, 0/1000 ray diffs; helper rehydration unwired |
| Kind-7 upscaler consumer plan | `fsr_route.md` section 9 | `0148cbfeb` | readiness: shapes measured, no FFX dispatch/consumer observation yet |
| Viewport/scissor array capture and named refusals | `d3d12_vulkan.md` | `dfe2337ef` | measured: bounded capture, count>1 refused until pipeline dynamic states land |
| Title-VA windows on the device | `vertex_buffer_va.md` section 24 | `75e9991a2` | measured: raw-VA resolve/window unit evidence; GPU falsifier named |
| Bindless degradation override and telemetry | `bindless_degradation.md` section 9 | `33f20f0c2` | measured: default unchanged, legacy mode pinned; real exhaustion unmeasured |
| Native write-fault watch and shadow invalidation | `title_memory_model.md` section 12 | `805b42ed4` | measured: 545 xmem checks, synthetic fault stress; no title consumer yet |
| Synthetic audio decoder contract | `audio_decode.md` | `cd47ec306` | 18 Rust FFI contract tests; codec-header validation unproven |
| 120 Hz and kind-7 arms | `window.md` / `fsr_route.md` | `5746b8123` | measured negatives: no enumeration trace; no post-map FSPX consumer |
| Presenter attestation, fit rule and synthetic non-black control | `peer_port_first_ui.md` section 15 | `779ce8be0` | measured: synthetic bands reach the capture exactly; presenter is not the black cause |
| Movie draw-path probes | `peer_port_movie.md` section 13 | `2e9915cbb` | measured: draw path never issued (all six sites zero); downstream of the session-3 poll |
| Pull-request CI guarantees | board D-03 | `2bb13be8b` | measured: docs-only and runtime-path gaps closed and registered |

Every port is opt-in and off by default, keeps the switch-off path byte-identical, has unit/GPU tests and credits the peer
evidence with URLs and commits in its page and in `NOTICE.md`. No peer source, comment, identifier or data table is copied.

## Rules of the programme

1. **Documentation only.** The raw material is each project's public README, status page and the tracker's
   own summaries of them. We do not read or copy peer source code for these slices. Where a peer's public
   description names a mechanism, we re-derive the behaviour by measuring our own title and implementing our
   own code against that measurement.
2. **Every claim is re-measured here.** A peer's sentence is a hypothesis until our boot log, probe or
   readback shows what our tree does. The slice records both.
3. **Same gates as every other slice**: opt-in until measured where behaviour can change, tests, one boot,
   honest numbers (a negative is a result), documentation with the peer claim cited and the attribution in
   `NOTICE.md` where a mechanism itself came from a named upstream change.
4. **No game data, no peer code, no extracted assets** enter the repository or this page.

## The peer claims that matter to our next stage

| Peer claim (public wording, summarised) | What it implies for this runtime | Our target slice |
|---|---|---|
| Five Star Recomp: the loader resolves all imports, boots through start-up, **answers its first-run screens through a scripted gamepad**, then starts loading story mode; later, **loading the game's saved settings makes later boots skip the first-run screens** and reach the story load in about 39 s. | Our title currently reads no input and makes no save calls in 300 s. A first-run flow that waits on a button press and on a settings store would look exactly like our post-16 s idle. | A no-SDL input backend with XInput/keyboard-as-pad plus a scripted first-run press schedule; an offline signed-in user and an on-disk settings/save store that survives a kill. |
| VIctory and XCL: runtime services are **one signed-in offline user, licences, packages, saves that survive a kill at any step**, with menus, video and loading sound; input is XInput pads with a keyboard stand-in pad. | Our user shim answers creation but delivers no signed-in event by default and has no persistent store; our GameInput shim is not built when SDL3 is absent, so the canonical build leans on the host. | Same slices as above: user event on demand, persistent local storage, and the input backend that is always built. |
| PercyScarlett: the world was black because it upscales with the console FSR 4 build, whose pipelines have no PC equivalent; **the upscale is now a stretch of the colour image**, so the lit picture reaches the screen. | Our frame's compose pass is exactly that console upscale (it samples the constant pass's 4K sibling). We have FSR stand-in switches; their state and the measured compose output must be settled. | Verify and, if needed, complete the upscale stand-in so the compose pass writes the sampled colour when the console pipelines cannot be made. |
| VIctory fork: while streaming, about 850 title-memory page remaps, 260 texture uploads and 450 disk reads a frame make the card idle about 330 ms of a 490 ms frame; batching page mappings, keeping copies on the card, and in-place title memory are the measured fixes. | Our page resolver and slot-31 mapping exist but are not batched; streamed content will hit the same cost when it starts. | Batch the page-mapping path (one alias/copy per run instead of per page) and measure the cost before/after on our mapping route. |
| XCL reports the game's **Bink 2**, **XboxBvhBuilder**, and **AMD FSR 4 console DLL** running on the PC GPU; Five Star's loader still logs and skips BVH builds. The latest victory-fork status says its builder now runs and ray-traced shadows/light appear after a scene streams in; VIctory still reports no structures built, while Percy lists ray-traced lighting as next. | Our movie library decodes 515/515 streams but the title's movie start is stage-gated; our BVH route is synthetic and the measured boot requests no RT work — [movies.md](movies.md), [bvh_route.md](bvh_route.md). RT is no longer a universal peer gap: the fork reports light, at 0.2–3 fps and with large copy costs. | Keep the build/consumer route behind the content and memory gates; use the fork result as a dependency and performance hypothesis, not as a reason to move RT ahead of the current stage frontier. |
| Five Star: GPU hangs from **translated shader loops that never end** are bounded. | We landed the loop guard in the translator this week for exactly this class. | Done; keep the falsifier (a guarded loop exits) in the translator page. |

## Slice order (why this order)

1. **Input backend + first-run press schedule** — the cheapest way to test the five-star observation
   ("reads input, starts its save system after a scripted button press") on our own title.
2. **Offline user + persistent settings/saves** — needed for the second half of that observation (later boots
   skip the first-run screens) and for the title's own session/save services.
3. **Upscale stand-in** — turns the compose pass's sampled colour into a presented picture when the console
   upscaler's pipelines cannot be made; this is the last picture step before ray-traced lighting.
4. **Batched page mapping** — the streamed-content performance feature; independent of the stage gate and
   measurable on our mapping route without waiting for content.
5. Deferred: Bink 2 playback route, XboxBvhBuilder, ray-traced light stand-in, audio decoding.

Each slice above gets its own page with the peer sentence quoted as the hypothesis, our measurement, the
implementation, the tests and the honest result. This page stays the ledger.

# Amendment and full parity audit (2026-11-25, evening)

## The rules, as amended

The rules at the top of this page stand, with one amendment to rule 1 and one added rule. The no-copy rule is not
relaxed: it is the reason the programme can cite the peers at all.

1. **Peer documentation and peer source may be consulted as evidence.** The raw material is each project's public
   README, status page, docs and the tracker's summaries; where a mechanism has to be understood, the peer's public
   source may be read as a reference. We still do not copy peer code, comment text, identifiers or tables into this
   tree: every implementation is our own, in our files, naming and tests, derived from our own measurements of our
   own title. Where a measurement disagrees with a peer, our measurement wins and the difference is recorded.
2. Every claim is re-measured here (unchanged).
3. The same gates as every other slice (unchanged).
4. No game data and no extracted assets enter the repository or this page (unchanged).
5. **Attribution is part of the slice.** A slice that takes a mechanism from a named peer records the project, its
   licence, the file and the commit read, in `NOTICE.md` and in its own page, per
   [upstream_port.md](upstream_port.md). The peers' licences are GPL-3.0-or-later (five-star, VIctory, victory-fork),
   GPL-2.0-only (XCL, PercyScarlett), GPL-3.0-or-later (g6shim), AGPL-3.0-or-later (OpenScarlett Community Edition)
   and Apache-2.0 (TuxBox).

## Reference material consulted for this audit

The first audit used tracker downloads made on 2026-11-25 17:34 and the detailed material named below at its then-current
clone pin. This refresh re-read the current peer README/status summaries and reviewed Five Star's documentation delta
`ce2b333b..34b4dfed`; the detailed non-README material from the first audit remains tied to its historical pins. The
current clone heads are recorded separately so a newer pin is not mistaken for a new measurement or a fresh source
review. Nothing here is a game file or an extracted asset.

| Peer | Repository and licence | Material consulted | Current clone head (commit date) |
|---|---|---|---|
| Five Star Recomp | <https://github.com/ThatRecompDev/five-star-recomp>, GPL-3.0-or-later | README; `docs/loader.md`, `docs/running.md`, `docs/wiki/Status.md`, `docs/status/README.md`; documentation delta `ce2b333b..34b4dfed` | `34b4dfed` (2026-11-27) |
| XCL | <https://github.com/xcl-dev/xcl>, GPL-2.0-only | Current README; prior audit: dated design spec, wave64 and shader-container pages at `6d12f58` | `623d02e` (2026-11-27) |
| VIctory | <https://github.com/Marvisak/VIctory>, GPL-3.0-or-later | Current README; prior audit: architecture page and runtime, GPU, memory/watch material at `13f91ca` | `63bcca4` (2026-11-26) |
| victory-fork | <https://github.com/fxceless/victory-fork>, GPL-3.0-or-later | Current README status sections; prior audit: README at `9c184b3` | `9a9091d` (2026-11-27) |
| PercyScarlett | <https://github.com/rpatty-dev/PercyScarlett>, GPL-2.0-only | Current README; prior audit: prologue guide and upscale-takeover/stretch modules at `e44c465` | `e44c465` (2026-11-25) |
| g6shim | <https://github.com/gookie-dev/g6shim>, GPL-3.0-or-later | README only (no clone) | — |
| OpenScarlett (Community Edition) | <https://github.com/OpenCommunityEdition/OpenScarlett>, AGPL-3.0-or-later | README only (no clone) | — |
| TuxBox | <https://github.com/brcly/TuxBox>, Apache-2.0 | README only (no clone) | — |

**Latest snapshots.** The five local clones are Five Star Recomp `34b4dfed` (2026-11-27), VIctory `63bcca4`
(2026-11-26), PercyScarlett `e44c465` (2026-11-25), victory-fork `9a9091d` (2026-11-27), and XCL `623d02e`
(2026-11-27). The Five Star range contains four commits; `docs/loader.md` and `docs/running.md` changed, while the
README and wiki Status page did not. The docs add NVIDIA shader-table safety and a keep-awake failure warning, not a
new boot-stage result. The latest victory-fork README now reports its ray-tracing builder running
and shadows/light appearing after a scene streams in, but at 0.2–3 fps; this supersedes its old “nothing is lit by it
yet” snapshot. These are peer-reported claims, not measurements of Palmetto.

The tracker itself is <https://vuyp.github.io/gta6-pc-race/>; its readings are quoted below where they add a stage
number. The full evidence table, with every row's URL, commit, functional decoding and caveat, is kept outside the
repository as the audit's working evidence; this page is the condensed ledger.

## The parity matrix

Refresher on the columns. **Claim** is the peer's public wording, shortened but not paraphrased where the wording
decides the row. **Functional meaning** is what the behaviour is for the title. **Our measured state** cites our page.
**Files** are the ones that would implement the equivalent here, not the peer's files. **Effort, testable** is a
planning estimate (S a day, M two to five days, L a week or more with a boot A/B) and whether the mechanism can be
proven without the content pass (unit, translator, creation-side or synthetic-GPU test). The **mechanism** column is
filled for the eight subsystems the user named for it: threading and task queues, XMem/title memory and aliasing,
D3D12.X device/queue/list semantics, first-run input/save flow, the Bink 2 video route, the FSR 4 upscale route,
BVH/ray tracing, and streamed page mapping/copies.

### Boot and first run

| Peer | Claim (quote) | Functional meaning | Mechanism as the peer implements it (reference only) | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|---|
| five-star | "boots through its start-up, answers its first-run screens through a scripted gamepad ... and starts loading story mode" | the title's own boot machine reaches story load; input drives the screens | a full in-process GDK runtime (its own task queues, async blocks and user/store shims) under a loader that resolves every import; the first-run script drives screen *states* learned from the frames, not fixed timings (presses held 0.3 s, each select step retried until the screen changes) | boots to its own frame loop past 130 s, but the stage word stays 2 / loading 1, the machine stays 2/2 and the stage-3 request is never written; the frozen holder is a running stage job (title-side) — [stage2_blocker.md](stage2_blocker.md) | `port/src/main.cpp` probes, `port/shims/xgameruntime.cpp`, `port/shims/gameinput.cpp` | M, no |
| XCL | "starts in ~4 s ... shows its menus and goes on past the first one to its settings" | start to menus in seconds, stable | the loader protocol is served at the import-thunk level and the PC's own `xgameruntime` supplies task queues, async, networking and dialogs; the console D3D12.X object model is replayed on desktop D3D12 | first frame, draws and presents are reached (17,674-27,935 draws per 150 s, 0 refused submissions); the picture is black by design at this stage — [page_address_space.md](page_address_space.md), [colour_path.md](colour_path.md) | the same runtime files as the row above; the picture half is the content/state step, not a loader gap | M, no |
| PercyScarlett (tracker stage) | tracker: "reaches the opening scene ... lit and textured, at about 2 to 3 frames a second" | in-world state reached | its runtime is the VIctory/victory-fork loader and services (the mechanism rows for those projects below), plus a Series S display fix | behind: no stage-3 transition measured | — | —, no |

Five Star's root README still contradicts itself ("GTA VI does not run yet" and "does not get past its first screens
yet" against its table and narrative); its current wiki status, updated 27-Nov, claims the game boots to the world and
shows a lit scene. Treat both as that tree's self-reported status, not a specification. The tracker snapshot downloaded
on 25-Nov reads Five Star at stage 4 ("first frames ... the picture still black"); the tracker counts similar forks as
separate projects, so that dated stage is the tracker's reading, not the current peer status or our measurement.

### User, saves and settings

| Peer | Claim (quote) | Functional meaning | Mechanism as the peer implements it (reference only) | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|---|
| VIctory | "One signed-in offline user, licences, packages, saves that survive a kill at any step" | signed-in offline user; crash-safe saves | one offline local user served through the runtime's COM-style provider; a save update writes a complete new container version and a directory rename commits it, so a kill leaves the old or the new version, never a mix; persistent local storage is a virtual drive over a host folder | one user-change registration; the opt-in signed-in event is delivered and consumed but the title stays in init state 2; zero save calls measured — [user_events.md](user_events.md), [user_event_probe.md](user_event_probe.md) | `port/shims/xgameruntime_user.cpp`, `port/shims/xgameruntime_gamesave.cpp`, `port/shims/persistent_store.h` | assigned slice 104, partly |
| five-star | "loading the game's saved settings makes later boots skip the first-run screens" | persisted settings shorten later boots | the title's data root holds settings and saves behind the GDK shims; a boot with the saved settings skips the first-run states the scripted input would otherwise have to answer | one persistent-local-storage call at 3.6 s answered, no settings read back yet because no first run completes — [first_run_path.md](first_run_path.md), [persistent_store.md](persistent_store.md) | as the row above | assigned slice 104, partly |
| victory-fork | "Saves \| the game autosaves a checkpoint and loads it again" | the title's own save path runs in-world | inherits the VIctory save store mechanism above | zero save calls at our stage — [user_events.md](user_events.md) | as above | no |
| OpenScarlett | saves are a host folder behind the console drive | console-drive virtualization | the console's `C:` drive is mapped to a host folder per title; nothing about the save format is interpreted | our overlay and file system already map the console drives — [runtime_imports.md](runtime_imports.md) | none new | —, yes |

### Input

| Peer | Claim (quote) | Functional meaning | Mechanism as the peer implements it (reference only) | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|---|
| XCL | "one gamepad - the PC's XInput controller or a PlayStation controller, and keyboard and mouse as a gamepad" | pad plus keyboard/mouse stand-in, hot-plug | GameInput over XInput and SDL (DualSense), with a poll source that turns the host state into readings and runs connect/disconnect callbacks | a GameInput shim exists on SDL3 and is tested; the canonical build without SDL3 leaned on the host; the always-built backend is the assigned slice; the input gate measured zero reads in 300 s — [gameinput.md](gameinput.md), [input_gate.md](input_gate.md), [input_backend.md](input_backend.md) | `port/shims/gameinput.cpp`, `port/shims/gameinput_abi.h`, `port/CMakeLists.txt` | assigned slice 103, yes |
| victory-fork | "XInput pads, keyboard, keyboard stand-in pad \| Needs: Trigger rumble" | same surface as the row above | XInput slots 0-3 become the game's pads; while none is connected the keyboard drives one stand-in pad; a poll thread makes a new reading per change | as the row above | `port/shims/gameinput.cpp`, `port/shims/gameinput_abi.h`, `port/CMakeLists.txt` | assigned slice 103, yes |
| five-star | first-run screens answered "through a scripted gamepad" | deterministic scripted presses | the script is a sequence of screen-state blocks; each block waits for its learned screen, presses a mapped key or pad button with a hold, and a select step retries until the screen changes | after slice 103 the title reads presses; no save or stage move follows — [input_backend.md](input_backend.md) | as the row above plus the stage work | M, yes |

### Audio (XMA / Opus / mixing)

| Peer | Claim (quote) | Functional meaning | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|
| XCL | "Opus 0x704F decoded via MSOpusDecoder.dll; XMA1 / XMA2 decoded in pure Rust" | real decode into the host mixer | the audio shims answer with a handle, a zeroed buffer and a zeroed status, and drop every command; nothing is decoded or mixed — [audio_shims.md](audio_shims.md) | `port/shims/xapu_chad.cpp`, `port/shims/acphal.cpp`, `port/xma_decoder/src/lib.rs` | M, yes |
| five-star | XMA contexts "decode on the caller's thread" into the output ring; Opus decodes at 48 kHz | console `AcpHal` semantics with real output | same stand-ins; the package's AWC banks are decoded offline only — [audio.md](audio.md) | as the row above, plus `port/xma_decoder/palmetto_xma2_decoder.h` | M, yes |
| VIctory | "Menu, video and loading sound; XMA voices and Opus streams decode" | sound through the menus and video | silence at every point measured; the host's own endpoint is proven to work, so no silent endpoint ships — [silent_audio.md](silent_audio.md) | as the row above | M, yes |
| g6shim | "XMA2 decoding, Opus decoding, DSP queues \| Left: DSP transforms, SHAPE units other than XMA" | a peer's own split: decode first, DSP later | same class as ours | as the row above | M, yes |

### Video (Bink 2)

| Peer | Claim (quote) | Functional meaning | Mechanism as the peer implements it (reference only) | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|---|
| XCL | "the game's Bink 2 console DLL decodes its movies ... the game's own YUV to RGB display shaders translate like any other" | the title's own codec DLL plus its own display shaders | the title's own Bink DLL is imported and mapped like any title DLL, so only its console imports are shimmed; the YUV-to-RGB conversion is the game's own pixel shader, translated like any other program | the library works: 515/515 streams decode frame 0 through the package's own Bink 2 library and playback/audio tests exist — [movies.md](movies.md) | `port/test/bink_play.cpp`, `port/shims/library_loader.cpp` | route exists, yes |
| five-star | "presents its logo, intro video and menus with complete UI text" | intro video inside the title's own flow | a call-recipe layer over the game's own Bink DLL (each call checked against its disassembly first), with the game's own display shaders doing the colour conversion | the title's movie library maps and binds, opens its archive twice at 2.7 s with 0 failed opens, but never runs past its entry and the movie-start sites never execute in 130 s — [movie_path.md](movie_path.md) | the stage work owns the start path; the decode route does not | M, no (stage) |
| TuxBox | "the story intro; the first cutscene does not appear yet" | a peer that also lacks the cutscene | its own native D3D12 backend plus the title's DLLs | we are behind even that (no stage 3) | — | —, no |

### Shader stages and coverage

| Peer | Claim (quote) | Functional meaning | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|
| five-star | "Every shader stage ... (vertex, pixel, compute, mesh, geometry, ray intersection), and all 25,908 programs compile" | complete stage coverage, structured control flow | driver-level translation is clean: 26,503 of 26,503 modules and pipelines, 0 failures — [pipeline_check.md](pipeline_check.md); a mesh stage inside a graphics description is still refused by name — [remaining_mechanisms.md](remaining_mechanisms.md) | `tooling/rdna2spirv.py`, `port/src/graphics_pso.cpp` | M, yes |
| XCL | "26 698 / 26 698 programs of the retail pack; 0 DXC errors on random samples" | full corpus translates | as the row above; our residual list is the FidelityFX and builder sets, not the game corpus | as the row above | —, yes |
| victory-fork | "World: all but under 300 of 24,387 programs ... Merged geometry stages (195 programs)" | residual coverage, merged geometry | our range is clean but the mesh/merged-stage path is not carried | `port/src/graphics_pso.cpp`, `tooling/rdna2spirv.py` | M, yes |

### D3D12 pipeline tables and driver background work

| Peer | Claim (docs, summarised) | Functional meaning | Mechanism as the peer documents it (reference only) | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|---|
| Five Star Recomp (27-Nov docs) | An NVIDIA driver's internal shader tables have a 512 MiB ceiling; background recompiles can fill them and remove the device. The new docs also warn that a keep-awake dispatch lost an RTX 5090, while the no-loop control ran 660 s. | Trade completeness/performance for survival when a vendor pipeline table is near its limit; avoid injecting unmeasured work to pin GPU clocks. | The loader disables D3D12 background shader work by default; Windows may refuse that unless Developer Mode is on, in which case periodic measurement discard only slows growth. The NVIDIA-only guard trusts a driver-read table or a refused disable request; an estimate alone only warns. It warns near 400 MiB, holds unused first-use pipelines as stand-ins from 470 MiB, and gives idle pipelines up near 495 MiB while preserving clears, blits, UI and lighting. Separately, the optional keep-awake switch runs a 64-thread busy compute loop; the new docs report RTX 5090 device loss at 100–154 s with it versus 660 s without it, so leave it off unless measuring. | Our backend is Vulkan, not D3D12. `memory_growth.md` measures a different NVIDIA issue (compiler host arenas and a ~40 GB plateau after bounding live VkPipelines); `pipeline_cache.md` measures a warm VkPipelineCache with 0 misses. Neither establishes the peer's 512 MiB D3D12 table failure or a Vulkan analogue — [memory_growth.md](memory_growth.md#4-what-the-cap-recovers-and-what-it-cannot), [pipeline_cache.md](pipeline_cache.md#4-coldwarm-ab). | None new; the existing live-pipeline cap/cache are not the same vendor-table guard. Do not transplant without a target-side Vulkan device-loss measurement. | Conditional monitor only; GPU-free policy test if a relevant failure is measured |

### D3D12.X descriptors, barriers and command signatures

| Peer | Claim (quote) | Functional meaning | Mechanism as the peer implements it (reference only) | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|---|
| five-star | "its descriptors and barriers are translated" | full descriptor and barrier translation | descriptor heaps are plain descriptor memory handed out through the title's own allocator hook (32-byte descriptors); per-draw tables are copied by the title into a linear allocator; enhanced barrier groups go through list slot 146; bottom-of-pipe writes and memory waits in the queue path | surface measured: 116-slot export, 83/83 standard GUIDs; the barrier into an undefined layout is implemented with WARP parity; recording fixes measured — [d3d12_xs_surface.md](d3d12_xs_surface.md), [barrier_undefined.md](barrier_undefined.md), [handles_barriers.md](handles_barriers.md) | `port/shims/d3d12_xs_device.cpp`, `port/src/vulkan_replay.h` | S/M, yes |
| five-star | list slots 71/72 are "Write32/64BitValueBottomOfPipeX", slot 74 is "Wait64BitValueX" (waits on "a pending record's cell reaching the frame number") | the title's own GPU-to-CPU cell handoff and queue waits | a desktop immediate write at the game address in marker mode; waits met by a watcher thread that signals the queue's fence once memory holds; lists split at waits; waits released at a present | **not forwarded**: list slots 70, 71, 72 and 74 are answered zero — [console_slots.md](console_slots.md); the PC immediate-write conversion exists in the IR — [d3d12_vulkan.md](d3d12_vulkan.md) | `port/shims/d3d12_xs_device.cpp`, `port/src/vulkan_replay.h`, `port/src/vulkan_backend.h`, `port/test/xs_proxy_mock_test.cpp` | S, yes |
| five-star | D3D12.X "takes [descriptor memory] from the title's XMemAlloc hook ... breaks into the debugger unless slot 9 then returns that memory" | the title's allocator callback hands out descriptor, render-target and recording memory | the allocator slot 9 callback is registered and the allocator records the pair; the callback is never invoked | ours records `(callback, context)` and answers success; the callback is never invoked (84,323 calls in 140 s on the earlier tree) — [allocator_slot9.md](allocator_slot9.md) | `port/shims/d3d12_xs_device.cpp`, `port/test/xs_proxy_mock_test.cpp` | S, yes |
| XCL | "console descriptor heaps (T# / V# / S#), buffers at their console addresses, compute command lists recorded in the console format and replayed" | console recording model replayed | wrapper per interface with desktop slot remap and struct-return swap; the list never records inline packets because put pointer and limits are zero | command queue/list/fence/present surface measured; we pin the list write cursor at its limit so every command takes the vtable path — [d3d12_command_objects.md](d3d12_command_objects.md), [render_loop.md](render_loop.md) | existing | —, yes |
| PercyScarlett (credits) | "Console bottom-of-pipe write and memory-wait handling ported from xcl" | a second port with the same mechanism | as the five-star rows above and below | as the slot-74 row | as the slot-74 row | S, yes |

### GPU memory, aliasing, copies and page mapping

| Peer | Claim (quote) | Functional meaning | Mechanism as the peer implements it (reference only) | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|---|
| five-star | "a page mapped at two addresses is one page and a remapped page keeps its contents"; GPU visibility by opening the page sections as heaps, 1 GiB windows, system-memory budget | 64 KiB physical pages aliased into GPU-addressable memory, remapped without losing contents | pagefile sections of 1024 pages, placeholders split per 64 KiB slot and replaced by views of the page's section; each page's CPU address works as its GPU address where an alias exists, else a buffer placed over the contiguous run; the page table drives the shaders' address map; page mappings batched through queue slot 31 with range kinds | placeholder and physical-page model with first-touch commit (271,044,608 bytes over 4,136 pages); VA rebasing for the two measured title ranges, 0 refused submissions at the current frontier; GPU visibility is by copies plus a full-heap mirror refresh of 3.4-4.4 ms per submission; no write-watch consumer — [page_address_space.md](page_address_space.md), [remaining_mechanisms.md](remaining_mechanisms.md) | `port/shims/xmem.cpp`, `port/shims/xmem.h`, `port/src/console_va_map.h`, `port/src/buffer_va_index.h`, `port/shims/d3d12_vulkan_device.cpp` | L, partly |
| VIctory | title memory at raw GPU addresses via "the heap index of a pair of raw views (SRV, UAV) of the host copy holding the page"; writes found by read-only guards with aliasing reported by the runtime | shaders read and write title memory by computed address; copies stay current cheaply | a per-page table of host-copy raw views; each copy's pages are guarded, the first write faults into a vectored handler that marks the block changed in every copy holding it; aliased mappings are reported when they are made; kernel writes are declared before they write | as the row above (copies plus full mirror; no write tracking) | as the row above | L, partly |
| victory-fork (25-Nov measurement; historical row) | "about 850 remaps of title memory, 260 texture uploads and 450 disk reads in one frame"; batching pool pages "1.8 to 4.2 pages a view, remap time 140 -> 79 ms a frame, BUT faces came out black or red ... Reverted" | streamed page remapping dominates streaming frames; the batched form was **refuted** on picture correctness | remap through the xmem alias hook on the CPU at the queue's fence; batching was measured, halved remap time and broke the picture in three launches of three, so it was reverted | queue slot 31 (`CopyPageMappingsBatchX`) is implemented but the boot never reaches it (0 calls in the A/B pair); the DirectStorage page-list resolver is implemented behind a switch with 0 measured requests — [cpu_view_mapping.md](cpu_view_mapping.md), [page_resolver.md](page_resolver.md) | `port/shims/xmem.cpp`, `port/shims/d3d12_xs_device.cpp` | assigned slice 4 (with a picture gate), yes (latent) |
| victory-fork (25-Nov measurement; historical row) | "puts every copy in the card's memory: the title's lists went from 68.0 to 49.8 ms of card time a frame"; texture copy-back "22,900 read-backs landed in six minutes" | small title buffers live on the card; shader-written textures return to title memory | device-local copies of title buffers with on-card hashing for changes; shader-written pages copied back, marked in the list bitmap | neither is carried: the mirror is refreshed CPU-side and the sampled 4K texture's pages are never written by the title — [texture_source.md](texture_source.md), [page_address_space.md](page_address_space.md) | `port/shims/xmem.cpp`, `port/src/vulkan_backend.h`, `port/src/vulkan_replay.h` | M, yes (unit) |
| XCL | "title graphics memory shared by CPU and GPU without copies, up to 16 GB (4 GiB windows)" | unified memory without copies | a memory registry plus GPU-visible mappings | as the five-star row: we copy; VA rebasing covers the two measured ranges | as the five-star row | L, partly |

### Ray tracing and BVH

| Peer | Claim (quote) | Functional meaning | Mechanism as the peer implements it (reference only) | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|---|
| XCL | "BLAS / TLAS built on the GPU by the game's own XboxBvhBuilder; RDNA2 BVH node format; BVH intersection instructions emulated in shaders" | the game's builder DLL runs on the PC; traversal emulated inline | the game's DLL constructor builds compute pipelines through the console compute-pipeline slot; builds go through the batched build export; sizes from the DLL's own prebuild query; traversal by emulated intersection instructions | 565,596 chunks in the BVH archive (271,627 BVH4 streams) inventoried; the builder's 52 embedded programs extracted, 51/52 pass the validator; the XDXR build descriptions are decoded; a GPU BVH4 build is implemented; `image_bvh*` is emulated with native-RDNA2-measured box and triangle rules (716 sites) — [bvh.md](bvh.md), [embedded_shaders.md](embedded_shaders.md), [xdxr.md](xdxr.md), [design.md](design.md) | `port/src/xdxr_build.h`, `port/shims/d3d12_com.h`, `port/test/bvh_builder_test.cpp`, `tooling/xdxr_bvh.py` | L, yes (synthetic) |
| five-star | "the port must put structures in that layout at the console GPU addresses the game hands its shaders"; CPU design: sizes from the DLL's own prebuild, builds and rehydration at record time | console-sized structures written into game memory without the console GPU builder | rehydration on the CPU per build description from the game's own vertex data; bottom-up refit of box32 and box16 nodes; TLAS built on the CPU over instance bounds | as the row above; our alternative GPU build exists but is not wired | as the row above | L, yes |
| victory-fork (27-Nov snapshot) | "ray-traced shadows and light now show with the builder on, after a scene has streamed in" | the builder output is consumed by the title's RT passes; the picture is visible, but at 0.2–3 fps | the documented fix limits each list to the memory copies its dispatches name, tracks shader-written 64 KiB pages for copy-back, supplies the acceleration-structure address from the view description, and uses the peer's reported hardware node-hit semantics | our GPU builder and traversal exist in synthetic tests, but the title requests zero RT work at this frontier — [bvh_route.md](bvh_route.md#21-what-the-boot-requests); the dependency on write/copy tracking is distinct from the builder itself — [title_memory_model.md](title_memory_model.md#3-what-each-peer-mechanism-would-need-on-our-windowsvulkan-backend) | as the row above | L, conditional on content; yes (synthetic) |
| PercyScarlett / OpenScarlett | PercyScarlett: "shaded areas and interiors (ray-traced lighting)" is next; OpenScarlett: "Lumen hardware ray tracing is incomplete" | Percy still lists the RT picture as next; peer maturity now differs by project | — | the fork's current report disproves a universal RT wall; our own title remains stage-gated and unmeasured for RT — [bvh_route.md](bvh_route.md) | as the row above | L, no (content) |

### Upscaling and FSR

| Peer | Claim (quote) | Functional meaning | Mechanism as the peer implements it (reference only) | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|---|
| PercyScarlett | "the Series S world was black because the game upscales with the console build of AMD FSR 4 ... The upscale is now a stretch of the colour image" | when the console upscaler cannot run, stretch the colour image to the present target | the runtime takes over the upscale dispatch and records a plain stretch of the colour image; the rest of the game's FSR DLL runs as it is | the stand-in is implemented, tested and exercised: with the compose-draw control the presented target is painted through the stand-in path, and the frame stays honest-black because the constant pass's colour path is content-gated — [upscale_standin.md](upscale_standin.md), [colour_path.md](colour_path.md), [slot70_publication.md](slot70_publication.md) | already implemented | done, yes |
| XCL | "the game's AMD FSR 4 console DLL: context, the 29 pipelines ... translated, a frame's 20 passes recorded and run on the GPU" | run the title's own upscaler pipelines | a FidelityFX API shim over the game's own upscaler programs in the console driver's container; the console-only pipeline deserialization is the blocker the peer handles | 13 of 29 upscaler programs translate at this tree, and the console upscaler pipelines are not created in the measured window; the stand-in covers the picture case — [fspx.md](fspx.md) | `tooling/rdna2spirv.py`, `port/src/shader_library.h`, `port/shims/d3d12_xs_device.cpp` | M, yes |
| five-star | a PC FidelityFX API DLL can serve the game's FidelityFX calls | forward the API to a PC FSR build | a FidelityFX bridge, layout facts, jitter and a PC-library loader | not carried; our route is the stand-in | `port/src/shader_library.h` (route) or a host-DLL bridge | S, yes |
| VIctory | Series S: "the PC upscaler (`VICTORY_FIDELITYFX_DIR`)" and "the title's own upscaler DLLs: six of their passes are untranslated" | PC FSR directory versus the title's own DLLs | the FidelityFX API served by the host's PC DLL; only the upscaler, frame generation refused | as the XCL row | as the XCL row | M, yes |

### Presentation and frame pacing

| Peer | Claim (quote) | Functional meaning | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|
| XCL | "`PresentX` and frame pacing into an XCL window" | console present plus pacing | present is wired end to end: 8,625 presents per 90 s, 0 failed, one visible title window by adoption — [present_wiring.md](present_wiring.md), [window.md](window.md) | `port/src/frame_pacer.h`, `port/shims/xbox_present.cpp` | S, yes |
| five-star | per-frame submit then present, and "the throttle also releases [held waits] when it has waited 250 ms" | present throttling with waits released at a present | device slots for the frame interval, scheduled events and waits are served and the token bridge accepts the device proxy's tokens; the pacer is not the token source — [remaining_mechanisms.md](remaining_mechanisms.md), [present_wiring.md](present_wiring.md) | as the row above | S, yes |

### Performance and threading (task queues, worker lanes)

| Peer | Claim (quote) | Functional meaning | Mechanism as the peer implements it (reference only) | Our measured state (page) | Files that would implement it | Effort, testable |
|---|---|---|---|---|---|---|
| five-star | a task queue "has a work port and a completion port, each dispatched manually ... on a thread pool, serialised on one thread, or immediately" | full GDK task-queue semantics | queues and ports, composite queues, delayed callbacks, waiters, monitors and termination, all in-process | the threading thunks already forward to the vendored task-queue implementation; the measured task-queue and async calls are few and all early in the boot (9-99 across boots, before about +30 s) — [runtime_imports.md](runtime_imports.md), [loading_gate.md](loading_gate.md) | `port/shims/xgameruntime.cpp` (parity slice in flight), the vendored queue | S, yes |
| VIctory | "A null queue handle means the process queue. An async block's system area holds its status and result size" | the same API documented from the header | as the row above | as the row above | as the row above | S, yes |
| victory-fork | "The frame is a chain of waits: more cores alone give nothing"; the layer's submit thread "works 83 ms of the room's 122 ms frame" | in-world, the layer's submit path is the wall | submission prep on its own thread, copy queues, fast-core pinning, thread splitting | not our wall yet: the title's own job graph holds stage 2 and no runtime answer is missing there — [stage2_blocker.md](stage2_blocker.md) | not applicable before content | —, no |

## The gaps that matter most

1. **The console queue's bottom-of-pipe writes and memory-value waits are dropped.** Our list slots 70, 71, 72 and 74
   are answered zero, while the title calls the immediate-write shape tens of thousands of times per boot and every
   peer that reached the world implements these. This is the title's own GPU-to-CPU cell handoff, it is cheap, and it
   is testable without content.
2. **The allocator callback is recorded but never invoked.** The title's own allocator (the callback registered on
   the allocator) is what the peers use to hand out descriptor, render-target and recording memory; invoking it is a
   small, self-contained step and the precondition for matching their descriptor-heap model.
3. **Title memory is copied where the peers alias it.** The peers map 64 KiB physical pages into GPU-addressable
   memory and track writes with guards and aliasing reports; we copy and refresh a full heap mirror (3.4-4.4 ms per
   submission) with no write-watch consumer and no per-page write tracking. This is the streamed-content and memory
   frontier the mechanism page already ranks third and fourth.
4. **Small title buffers on the card, and texture write-back, are not carried.** The fork's 25-Nov published run
   measured this class (buffers: 68.0 to 49.8 ms of card time a frame; copy-back: 22,900 read-backs in six minutes,
   unblocking a mission). We have neither, and our memory figure (about 83.6 GB private bytes at 300 s) is the other
   side of the same class. Its 27-Nov RT-builder report makes selective list sync and page-granular write-back newly
   relevant, but still counts about 8 GiB of copies / 150 ms CPU work a frame; this is a dependency result, not a new
   performance win (see ranked slices 3, 4 and 6).
5. **The FSR 4 console route is translated but not yet routed by the title.** 29 of 29 upscaler programs translate and validate (measured on the current tree; the route harness maps the package DLL, creates its context, records 20 compute passes and serves all 29 pipeline creations), and the title itself requests none of them in the boot.
   console upscaler pipelines are not created, where XCL runs all 29 pipelines and 20 passes of a frame. The stand-in
   is already done, so this is the picture-parity step behind content.
6. **Audio is still a stand-in.** Every stage-5/6 peer decodes XMA and Opus; our XMA2 decoder exists but is not wired
   into the audio HAL and no Opus path exists, so menus and video are silent.
7. **Bink is not a missing mechanism.** The package's own library decodes 515/515 streams' frame 0 through our loader
   and a playback test exists; the title's movie-start sites never execute because the title does not leave stage 2.
8. **Ray tracing is a conditional picture gate, not a universal peer gap.** The latest victory-fork snapshot reports
   visible ray-traced shadows and light after streaming, but at 0.2–3 fps and with about 8 GiB of copies per frame;
   VIctory still says structures are not built, and Percy lists RT lighting as next. Our inline traversal and synthetic
   build route exist, but the title requests zero RT work at this frontier — [bvh_route.md](bvh_route.md). Keep it behind
   the content and memory gates; the fork's result makes write/copy behavior part of the RT prerequisite, not evidence
   that our title is ready for it.

## What we already do, or have measured and refuted (do not redo)

- The upscale stand-in is done and exercised; the remaining black is the constant pass's colour path, content-gated
  ([upscale_standin.md](upscale_standin.md), [colour_path.md](colour_path.md)).
- The per-frame slot-70 descriptor publication is implemented and committed; the compose matrix resolves and is causal
  under the control, so it is not the black-frame cause any more
  ([slot70_publication.md](slot70_publication.md), [device_slot70.md](device_slot70.md)).
- Bindless degradation and larger descriptor arrays are implemented and latent at today's population
  ([bindless_degradation.md](bindless_degradation.md)).
- The DirectStorage page-list resolver and live transfer are implemented behind a switch with a measured zero-request
  negative, and the advanced staging, pinning, BCPack and predictor paths are carried
  ([page_resolver.md](page_resolver.md), [directstorage_advanced.md](directstorage_advanced.md),
  [bcpack_runtime.md](bcpack_runtime.md)).
- Queue slot 31 page mapping is implemented and the boot does not reach it ([cpu_view_mapping.md](cpu_view_mapping.md)).
  The fork's attempt to map pool pages in runs as one view halved remap time but broke the picture in three launches of
  three; any batching work must carry a picture-correctness gate, not only a timing gate.
- Barriers into an undefined layout and the enhanced barrier groups are implemented with a WARP parity table and a
  measured fix to the recording path ([barrier_undefined.md](barrier_undefined.md),
  [handles_barriers.md](handles_barriers.md)).
- The five console command signatures are measured and implemented; the mesh signature is an honest stand-in whose
  indirect execution is refused by name, and no boot sample decoded a mesh stage yet
  ([command_signature.md](command_signature.md)).
- The translator loop guard and the measured translator fixes are landed ([translator_fixes.md](translator_fixes.md));
  the peers' "translated loops are bounded" claim maps onto that work.
- A silent audio endpoint was measured and declined: the host's own audio path works ([silent_audio.md](silent_audio.md)).
- Texture tiling and BCPack coverage are at or beyond the peers' documented numbers ([textures.md](textures.md),
  [bcpack_runtime.md](bcpack_runtime.md)) and the pipeline corpus is clean ([pipeline_check.md](pipeline_check.md)).
- The threading thunks already forward to the vendored task-queue implementation; a parity slice is in flight, so no
  second task-queue slice should be opened ([runtime_imports.md](runtime_imports.md)).
- The stage-2 stall itself is title-side and no runtime answer is visibly missing; the peer mechanisms are candidates
  for the *pending-record / cell* half of it, not a known fix ([stage2_blocker.md](stage2_blocker.md)).

## The next eight clean-room slices, ranked

Ordering rule: first the mechanisms that can touch our stage-2 / picture frontier or remove a measured negative that
the peers all cover; then the picture parity that content needs; then feature parity. The programme's original four
slices (input backend, offline user with persistent settings, upscale stand-in, batched page mapping) and the
in-flight task-queue parity slice are excluded here so this list does not duplicate work already owned.

### Slice 1 - console bottom-of-pipe writes and memory-value waits (effort S)

Every peer that reached the world implements the title's cell handoff. Our list slots 70 to 72 (the immediate-write
shape and the two bottom-of-pipe writes) and slot 74 (a 64-bit memory wait) are answered zero; the title calls the
immediate-write shape tens of thousands of times a boot. This is the first slice because it is small, it is the only
peer mechanism in our measured loader/frame path that is not carried, and it plausibly feeds the pending-record cells
whose completion the stage-2 job graph may be waiting on. Clean-room spec: forward the two-word immediate-write call to
the host list's own immediate-write command; translate the bottom-of-pipe writes to an immediate write at the same
address in a marker mode after the list's earlier work, with the timestamp forms left refused by name; implement the
64-bit memory wait as a recorded wait (ordered after the same list's earlier work, met in order) whose consumer reuses
the existing submission gating, so a wait that cannot hold yet cannot deadlock the recording thread. All four stay
behind one opt-in switch until a boot measures them.

- **Files:** `port/shims/d3d12_xs_device.cpp` (list slot arms and the wait bookkeeping), `port/src/vulkan_replay.h`
  (recorded wait), `port/src/vulkan_backend.h` (submission gating), `port/test/xs_proxy_mock_test.cpp` (slot
  translation and the wait ordering), plus the slice page.
- **Acceptance:** the mock test shows slot 70 translated to an immediate write, slots 71/72 ordered after earlier work
  and slot 74 as a wait, and the boot's slot-70/74 counters stop being refusals; one boot pair with the switch on, with
  the cell series (a bounded probe read) compared against the switch-off arm.
- **Falsifier:** the measured cell series is unchanged in both arms and the stage word stays 2; then record the
  negative and keep the default off.

### Slice 2 - invoke the title's allocator callback (effort S)

The allocator slot 9 answer already records the callback and returns success, but never calls it. Five-star's evidence
is that the console runtime calls it to hand out descriptor memory, and the callback's own body in our decompilation
allocates a block and stores it in the context. Clean-room spec: call the registered callback once per allocator
lifetime at the measured point, with the callback's own argument shape, return the block's address where the slot's
measured answer expects memory, free nothing the title owns, keep a bounded log and a refusal for a callback that
returns no memory. This should also make the title's own debug-time checks around descriptor memory meaningful.

- **Files:** `port/shims/d3d12_xs_device.cpp` (allocator proxy), `port/test/xs_proxy_mock_test.cpp`,
  `docs/allocator_slot9.md` (an addendum section), plus the slice page if it needs its own.
- **Acceptance:** a unit test with a synthetic callback proves it is invoked once, its return value is used, and a
  null return is refused by name; the boot A/B shows the descriptor memory block returned and no change in refused
  submissions.
- **Falsifier:** the callback is called and the boot's descriptor-heap population or submission behaviour does not
  move; record it as a parity-only step.

### Slice 3 - console page aliasing, write watch and GPU-visible page heaps (effort L)

This is the peers' central memory mechanism and our largest measured structural gap: 64 KiB physical pages that can be
mapped at several addresses, GPU addresses that follow the page table, and copies that are invalidated by changed
pages instead of a full-heap refresh. Clean-room spec in our structure: extend the existing physical-page bookkeeping
with an alias view list per page (the CPU address of every mapping of a frame), report a mapping change to every
consumer, add the write-watch consumer that today has no caller (the existing write-watch exports are tested but
unused), and replace the full mirror refresh with a changed-page pull; keep a bounded fallback to the full refresh when
the watch cannot answer. Clean-room only: no peer table or code is imported; the shapes are re-derived from our own
measured call sites.

- **Files:** `port/shims/xmem.cpp` and `port/shims/xmem.h` (alias list, mapping reports, watch consumer),
  `port/src/console_va_map.h` and `port/src/buffer_va_index.h` (alias-aware rebase),
  `port/shims/d3d12_vulkan_device.cpp` (the mirror refresh path), `port/test/xmem_test.cpp`, plus the slice page.
- **Acceptance:** a GPU-free test maps one physical range at two addresses, writes through one, and reads the change
  through the other and through a copy; the boot pair reports the mirror refresh cost and the process footprint at the
  same marks (the mirror refresh is measured at 3.4-4.4 ms per submission today).
- **Falsifier:** aliasing works but the refresh cost and footprint do not move at our content population; then the
  slice is recorded as readiness for streamed content, not a current win.

### Slice 4 - small title-buffer card copies and texture write-back (effort M)

The fork's 25-Nov published measurement of this class reports copies under 1 MiB staying on the card (title lists
68.0 to 49.8 ms of card time a frame) and textures that shaders write copied back into title memory (22,900 read-backs
in six minutes, unblocking a mission). Its 27-Nov RT-builder report adds a dependency result: selective list sync and
page-granular write-back allow the builder to proceed, but its run still copies about 8 GiB / spends about 150 ms CPU
per frame. Clean-room spec: place copies of title buffers in device-local memory under a measured size floor and a memory budget,
hash the changed ranges on the card so only changed 256-byte chunks are sent back, and for textures whose shaders wrote
them copy the written pages back into the title's pages, driven by the same changed-page information as slice 3 where
it exists and by a bounded explicit request otherwise.

- **Files:** `port/shims/xmem.cpp` (copy placement and write-back hook), `port/src/vulkan_backend.h` (sync lists and
  copy-back), `port/src/vulkan_replay.h` (write marking), `port/test/xmem_test.cpp`, plus the slice page.
- **Acceptance:** a unit test proves only changed chunks are copied and that a write-back lands in the title's page;
  a boot pair with the switches on reporting gated card time and read-back count, with the picture unchanged.
- **Falsifier:** the card time or the read-back count does not move, or the picture changes; record it and keep off.

### Slice 5 - the FSR 4 console-upscaler route (effort M)

The stand-in is done; the parity step is running the title's own upscaler where its pipelines can be made. The
measured gap was the translator: 29 of 29 upscaler programs now translate (the earlier 13-of-29 figure predates the widen repair). XCL reports all 29 pipelines translated and a
frame's 20 passes run. Clean-room spec: translate the remaining upscaler programs (the parity failure classes are
already named in the translator pages), make the console pipeline-deserialization answer create the measured upscaler
pipelines on the host, and route the upscale to the title's own programs when they exist, keeping the stretch stand-in
as the fallback and the control arm.

- **Files:** `tooling/rdna2spirv.py` (the parity classes), `port/src/shader_library.h` (program lookup),
  `port/shims/d3d12_xs_device.cpp` (pipeline creation path), the stand-in's own header and the fallback selection in
  `port/src/vulkan_backend.h`, plus the slice page.
- **Acceptance:** the upscaler program count goes to 29 of 29 with a validator-clean set, and a GPU test arm shows the
  upscaler pass writing a non-black target from a synthetic source; the boot reports which upscaler pipelines are
  created or refused.
- **Falsifier:** the remaining programs cannot be translated safely (name each refusal by reason) or the console
  pipeline creation cannot be answered; the stand-in stays and the negative is recorded.

### Slice 6 - the BVH build route and inline-traversal wiring (effort L)

Our translator already emulates the traversal instructions with native-measured rules; the missing half is building
the structures in the node format at the addresses the title's shaders walk, and wiring the game's own builder
exports. The peer routes now differ: Five Star documents a CPU build/rehydration path from game vertex data and
prebuild sizes; XCL runs the builder's own GPU programs; the 27-Nov victory-fork README reports the builder producing
visible ray-traced shadows/light after a streamed scene. That fork's result followed a narrower memory-sync rule (only
copies named by each list's dispatches plus page-granular shader-write tracking and copy-back) and fixes to the
structure-view address and node-hit semantics; it also reports about 8 GiB of copy work / 150 ms per frame and only
0.2–3 fps. This is evidence that builder, memory-sync and consumer semantics must be tested together, not that bulk
synchronization is a portable fix. Clean-room spec: answer the builder's prebuild and build exports from our own measured
shapes (sizes from the prebuild path, structures built into the title's pages by our GPU build path where it fits the
console size, and a bounded CPU build for the fits-in-console-size case), route copy and rehydrate to our own
implementations, and keep the ray-tracing pipelines and traversal otherwise unchanged.

- **Files:** `port/src/xdxr_build.h`, `port/shims/d3d12_com.h` (build export answers), `port/test/bvh_builder_test.cpp`,
  `tooling/xdxr_bvh.py`, plus the slice page.
- **Acceptance:** the existing synthetic BVH test still walks a built structure bit-exactly against a brute-force
  reference, a build through the builder-shaped export lands in the title's pages at the console size, and the boot
  reports builder export calls served with no device loss.
- **Falsifier:** the built structures cannot be made at the console sizes the title allocates, or a boot builds them
  and no RT pass consumes them; record the negative and leave the path off by default.

### Slice 7 - the mesh and merged-geometry stage path (effort M)

The peers translate every stage including mesh and merged geometry; our graphics pipeline path refuses a mesh stage
inside a graphics description by name, and the mesh command signature is a stand-in whose indirect execution is
refused. Clean-room spec: build the graphics pipeline state through the stream-description form when a decoded record
carries a mesh stage (creation side first, GPU-free), replay a mesh indirect record as a mesh dispatch rather than
refusing it, and keep the merged-geometry stage work to the translator residual list named on the shader page. The
default path must stay byte-identical for records without a mesh stage.

- **Files:** `port/src/graphics_pso.cpp` and `port/src/graphics_pso.h` (stream description), `port/src/vulkan_replay.h`
  (mesh dispatch record), `port/test/graphics_pso_test.cpp` and `port/test/xs_proxy_mock_test.cpp`, plus the slice page.
- **Acceptance:** a mock device builds a stream-description mesh pipeline from a synthetic record and a synthetic
  mesh indirect record records a mesh dispatch; the boot A/B reports no change for the current record population.
- **Falsifier:** no boot record carries a mesh stage, so the slice is readiness only; record that and leave it
  default-off.

### Slice 8 - audio decode: wire XMA2 and add Opus (effort M)

Every peer at stage 5 or 6 decodes XMA and Opus; ours answers with a stand-in and drops every command. Our tree already
owns an XMA2 decoder crate and its C header, and the existing audio HAL shim has the measured message shapes. Clean-room
spec: call the decoder from the audio HAL's XMA context messages at the measured points (reset, move-and-validate,
stop), write the decoded PCM into the context's output ring with the measured block layout, add an Opus path for the
audio processor's stream commands, keep a bounded log of formats and block counts, and refuse by name any shape the
measured calls do not cover. The host endpoint already works, so the output can be heard without a silent stand-in.

- **Files:** `port/shims/acphal.cpp`, `port/shims/xapu_chad.cpp`, `port/xma_decoder/src/lib.rs`,
  `port/test/acphal_test.cpp` and `port/test/xapu_chad_test.cpp`, plus the slice page.
- **Acceptance:** unit tests decode a synthetic XMA2 stream and an Opus packet into the measured ring shapes; one boot
  reports decode counts per context and no change in refused submissions; if the title has reached its menus by then,
  a capture with sound is the win.
- **Falsifier:** the title never enables an XMA context or never sends a stream command at our stage; record the count
  and keep the code for the content pass.

## Beyond the eight

- **Bink 2 playback route** is not a slice here: the package library decodes and a playback test exists; the title's
  movie-start sites need the stage step first ([movie_path.md](movie_path.md)).
- **Streamed page-mapping batching** is the programme's assigned fourth slice; the victory-fork row of the memory
  matrix is its warning - the fork batched runs of pool pages, halved the remap time and broke the picture, so the
  slice must include a picture-correctness gate or stay off.
- **Frame-pacing token source** is small and post-load: our present path is alive and the token bridge works; making
  the pacer the token source is quality work once content runs ([remaining_mechanisms.md](remaining_mechanisms.md)).
- **Task-queue parity** is in flight and is not repeated here ([runtime_imports.md](runtime_imports.md)).
- **Merged geometry stage coverage** (the fork's 195 programs) stays on the translator residual list; slice 7 covers
  the pipeline path, not the translator residual.
- **NVIDIA D3D12 shader-table safety is conditional, not a new top-eight slice.** Five Star's new guard is specific to
  that D3D12 driver-table failure and its fallback may skip draws; our Vulkan pipeline cap/cache address different
  measured resources. Keep the eight-slice order unchanged unless a supported Vulkan run shows the same device-loss
  boundary — [D3D12 pipeline-table row](#d3d12-pipeline-tables-and-driver-background-work), [memory_growth.md](memory_growth.md#4-what-the-cap-recovers-and-what-it-cannot), [pipeline_cache.md](pipeline_cache.md#4-coldwarm-ab).

## Limits of this audit

- This 27-Nov refresh ran no boot and changed no code; every "our measured state" entry is an existing page's
  measured claim, not a re-measurement. A stale page would make a row stale with it.
- Peer claims are self-reported; Five Star's README contradicts itself and the dated tracker snapshot double-counts
  similar forks (section on boot claims). The current victory-fork row uses its 27-Nov README at `9a9091d`, which
  supersedes the older night-of-25 snapshot; its RT result remains a peer report, not a Palmetto measurement.
- The mechanism column is a reference for understanding, not a design we are copying; where our host differs (Vulkan
  versus desktop D3D12; copies versus page aliasing) the row says so, and every slice remains our own implementation.
- Effort sizes are planning estimates; the order is justified by our measured frontiers, not by the peers' release
  order.
- The first audit read peer source only where a mechanism had to be understood (module headers and docs). This refresh
  inspected only peer READMEs and documentation/status pages; no source code or comment text was copied. Each slice that
  lands adds its attribution to `NOTICE.md` and its own page, with the peer project, licence and commit named here.
