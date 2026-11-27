# Task board

The shared board for contributors and maintainers: what is open, who holds what, and what is in progress. The live board
is the pinned coordination issue on GitHub
([issue #4](https://github.com/luizwidmer/palmetto/issues/4)); this page is the repository copy of the rules and a
snapshot of the backlog, so a checkout with no network still says what can be claimed and how.

The backlog is built from the project's own measured evidence: the boot/frame frontier pages
([vertex_buffer_va.md](vertex_buffer_va.md), [decomp_stage.md](decomp_stage.md),
[decomp_first_ui.md](decomp_first_ui.md), [frame_first_light.md](frame_first_light.md), [colour_path.md](colour_path.md)),
the peer-port plans ([peer_port_streaming.md](peer_port_streaming.md), [peer_port_movie.md](peer_port_movie.md),
[peer_port_first_ui.md](peer_port_first_ui.md)) and the remaining-mechanism inventory
([remaining_mechanisms.md](remaining_mechanisms.md)). Every board row names the page that defines what "done" means, so a
claim does not start with a guess.

## How to claim

1. Read the live board issue and the page the task names. Check the open pull requests too.
2. Post one claim comment in the board issue:

   ```
   claiming: <ID> — <one line on the approach and the files you expect to touch>
   ```

   Only `P-`, `C-`, `D-` and `F-` rows marked `open`, and only when nothing in the open pull requests already does the
work. Rows marked `maintainer` are the maintainer's lane: ask in the issue before starting one. The frame frontier is now
claimable one at a time: an open `F-` row can be claimed by any contributor with a short plan and the evidence page;
`F-01` and `F-02` stay blocked until the maintainer routes the diagnostic state-2 redirect.
3. Open a small pull request that names the task: `Task: <ID>` in the body, plus the disclosure and test/GPU statements
   [AGENTS.md](../AGENTS.md) asks for. One task per pull request; if the work needs a second task, claim that one
   separately.
4. When it is merged, post `done: <ID> -> #<PR>`. If you stop, post `release: <ID>` the same day so the row can go back
   to `open`. The maintainer keeps the board rows and statuses current; claimants keep their comments accurate.

## Statuses

`open` - claimable and not held. `claimed` - someone posted a claim; no pull request yet. `in progress` - a pull request
or a maintainer branch exists. `in review` - waiting on review/CI. `blocked` - the row's page names what it waits on.
`done` - merged and recorded.

## Rules that matter on the board

- **No game data, ever**, and no keys or decryption work: see [CONTRIBUTING.md](../CONTRIBUTING.md) and
  [AGENTS.md](../AGENTS.md).
- **One task, one scope.** List the files you expect to touch in the claim. If another claim already lists one of them,
  say so and split the work first; the maintainer sequences overlapping claims.
- **Opt-in until measured.** Behaviour changes land behind a switch with the old path byte-identical, tests, and a boot or
  GPU test that shows the change.
- **Measurements move the board.** A task whose falsifier was measured negative goes to `blocked` with the page updated,
  not to `done` by effort alone.
- **Do not repeat measured negatives.** The campaign follow-ups table in
  [decomp_campaign.md](decomp_campaign.md) lists hardware/streaming/UI hypotheses already refuted; a new claim on one of
  those must name what changed.

## Backlog snapshot

Rows the maintainer owns or that are running right now are marked in the owner column; everything else is claimable.
Priority order is top to bottom within each lane.

### Frame frontier (claimable, one at a time)

| ID | area | task | owner | status | evidence page |
|---|---|---|---|---|---|
| F-01 | frontier | Name and complete the title's session-3 out-record state so the loading record can complete; needs maintainer routing of the diagnostic state-2 redirect | maintainer | blocked | [decomp_stage.md](decomp_stage.md) sections 14-15 |
| F-02 | frontier | Get the movie draw to issue (frame-buffer registration and decode hits) once F-01 moves | maintainer | blocked | [decomp_first_ui.md](decomp_first_ui.md) sections 14-17 |
| F-03 | frontier | The media/UI present body still exits on two keys; its only type-0x24 producer is unreferenced - find the reachable path (static lead) | open | open (claimable: one at a time) | [decomp_first_ui.md](decomp_first_ui.md) section 11 |
| F-04 | frontier | Content producer and colour input: carry the content window's colour source once content runs | open | open (claimable: one at a time) | [colour_path.md](colour_path.md), [frame_first_light.md](frame_first_light.md) |
| F-05 | frontier | Class-0xa mitigation follow-ups: clean-host stage-3 pair, image-variant detection, keep off by default | open | open (claimable: one at a time) | [decomp_first_ui.md](decomp_first_ui.md) section 16 |
| F-06 | frontier | Watchdog pause hold: clean-host pair carried to stage 3 (A/B was device-loss-limited) | open | open (claimable: one at a time) | [peer_port_first_ui.md](peer_port_first_ui.md) section 13.5 |
| F-07 | frontier | Present attestation polish: byte-masked keys and transitions logged past the periodic cap | open | open (claimable: one at a time) | [peer_port_first_ui.md](peer_port_first_ui.md) section 13 |

### Peer ports (claimable)

| ID | area | task | owner | status | evidence page |
|---|---|---|---|---|---|
| P-01 | renderer | Presenter fit/scale/colour-space, plus a synthetic non-black present control through the presenter capture | maintainer | done (`779ce8be0`) | [peer_port_first_ui.md](peer_port_first_ui.md) section 15 |
| P-02 | platform | First-run screen-state input script (screen signatures/holds as the peers do), opt-in | open | open | [peer_port_first_ui.md](peer_port_first_ui.md) rank 5 |
| P-03 | movie | Bink import-level call witness and opt-in movie fast-advance | maintainer | done (`a32719e56`) | [peer_port_movie.md](peer_port_movie.md) section 14 |
| P-04 | movie | Movie draw-path probes (layer draw, draw submit, advance, decode control) | maintainer | done (`2e9915cbb`) | [peer_port_movie.md](peer_port_movie.md) section 13 |
| P-05 | platform | Task-queue/async parity test against the vendored implementation (GPU-free) | maintainer | done (`a32719e56`) | [task_queue_parity.md](task_queue_parity.md) section 9 |
| P-06 | renderer | Overlap submission with the card: flights/copy lanes instead of one ordered queue | maintainer | done (`5798bb070`) | [peer_port_streaming.md](peer_port_streaming.md) section 15 |
| P-07 | movie | Bink audio witness: what the decoder asks the audio shims for (cannot make the draw issue) | open | open | [peer_port_movie.md](peer_port_movie.md) rank 8 |
| P-08 | streaming | Terminal-read conversion diagnostic arm: an uncommittable destination, to exercise the conversion | open | open | [peer_port_streaming.md](peer_port_streaming.md) section 12 |
| P-09 | memory | GPU-view pairing population/refusal arm on a settled tree | open | open | [peer_port_movie.md](peer_port_movie.md) section 11 |
| P-10 | memory | Plane-refresh changed-content arm: a boot where an imported plane actually changes | open | open | [peer_port_movie.md](peer_port_movie.md) section 8 |
| P-11 | memory | Memory-wait unmet-cell arm: find a population where a slot-74 cell is short at record time | open | open | [peer_port_streaming.md](peer_port_streaming.md) section 11 |
| P-12 | renderer | XG layout/upload coverage for the movie-plane shapes | maintainer | done (`6da738f85`) | [peer_port_movie.md](peer_port_movie.md) section 12 |
| P-13 | platform | UI resource probe and measured display capability answers | maintainer | done (`ef46566d1`) | [peer_port_first_ui.md](peer_port_first_ui.md) section 14 |
| P-14 | streaming | Physical-page completion destinations for DirectStorage | maintainer | done (`fdfacfdf2`) | [peer_port_streaming.md](peer_port_streaming.md) section 13 |
| P-15 | platform | HDR-desktop boot: does the Enabled HDR answer reach the plane colour space or the capture | open | open | [peer_port_first_ui.md](peer_port_first_ui.md) section 14 |
| P-16 | platform | 120 Hz-host display answer: compare the consumed refresh when the mode answer is not the host's (dxgi path) | maintainer | done (`78b15c388`) | [window.md](window.md) |
| P-17 | platform | Probe the RBF0 handler one level below the dispatcher and the page-flow step's reachability | open | open | [peer_port_first_ui.md](peer_port_first_ui.md) section 14 |
| P-18 | platform | Deferral-acquiring boot: exercise the display timeout deferral end to end | open | open | [peer_port_first_ui.md](peer_port_first_ui.md) section 14 |
| P-19 | movie | Movie-plane decode-run census: the XG row rule and the placed-stride consumer when decode first runs | open | open | [peer_port_movie.md](peer_port_movie.md) section 12 |
| P-20 | renderer | Content-window texture descriptor census: most distinct shapes are refused at create | maintainer | done (`1fdd100bb`) | [texture_layout.md](texture_layout.md) section 8 |
| P-21 | streaming | Mapped physical-page destination arm: exercise the delivery through an armed page watch | maintainer | done (`f495a6261`) | [peer_port_streaming.md](peer_port_streaming.md) section 14 |
| P-22 | platform | UI 2D-batch probe arm (draw mode): the batch path behind the page flow | maintainer | done (`dc6e6d949`) | [decomp_first_ui.md](decomp_first_ui.md) section 19 |
| P-23 | movie | Session-3 completion arm must produce a layer-draw hit, or locate the gate in the renderer job list | open | open | [peer_port_movie.md](peer_port_movie.md) section 13 |
| P-24 | renderer | Decide the presenter fit default once a real content frame exists to judge (fill vs aspect) | open | open | [peer_port_first_ui.md](peer_port_first_ui.md) section 15 |
| P-25 | platform | DPI-unaware window: the title's window is oversized against the virtualized monitor; measure window fitting | maintainer | done (`f2d6736a2`) | [peer_port_first_ui.md](peer_port_first_ui.md) section 16 |
| P-26 | ci | Branch protection: require the guaranteed CI jobs in repository settings (not enforceable from the tree) | maintainer | open | [dev-tools.md](dev-tools.md) |
| P-32 | renderer | Full multi-viewport/scissor array replay (needs the d3d12_com array copy and dynamic viewport count) | maintainer | done (`6351f6846`, GPU PASS) | [d3d12_vulkan.md](d3d12_vulkan.md) |
| P-33 | memory | Title-VA device integration: outside-arena GPU windows, raw-VA buffer resolve, shader-visible rewrites and page pulls | maintainer | done (`75e9991a2`, GPU falsifier remains) | [vertex_buffer_va.md](vertex_buffer_va.md) section 24 |
| P-34 | audio | Measured audio effect descriptor and DSP send with formats/lifecycle | maintainer | first step done (`9d00b09d7`: submit pointer roles); live submit unmeasured | [audio_decode.md](audio_decode.md) |
| P-35 | renderer | XG array slot-2 layout observation (the row-major array layout is still a hypothesis) | open | open | [texture_layout.md](texture_layout.md) section 8 |
| P-36 | platform | DXGI simulate-120 light arm (does the consumer take 120/1 and set the flag) | maintainer | attempted (`5746b8123`, bounded negative) | [window.md](window.md) |
| P-37 | renderer | Presenter HDR10 surface arm (the ST.2084 surface was unavailable on the test host) | open | open | [present_wiring.md](present_wiring.md) section 9 |
| P-38 | platform | Device-health post-gate arm with the helper enabled | open | open | [device_health.md](device_health.md) |
| P-39 | quality | Triage the remaining full-suite failures: synthetic CRT file round trip, queue-proxy signal causes, console-heap precondition | maintainer | CRT fixture fixed (`688a4742e`); queue/console-heap causes documented | [testing.md](testing.md) |
| P-40 | renderer | Slot-70 strict-validation arm: do the measured dominant-site calls pass and is the matrix V# accepted by compose | maintainer | attempted (`a331289cf`), device-loss limited | [device_slot70.md](device_slot70.md) |
| P-41 | renderer | Pipeline dynamic viewport/scissor states and multiViewport capability (the other half of P-32) | maintainer | done (`6351f6846`) | [d3d12_vulkan.md](d3d12_vulkan.md) |
| P-42 | memory | Title-VA window GPU test and arbitrary shader-UAV writeback limit (named unsupported) | maintainer | test seam + synthetic GPU evidence (`6351f6846`); production BDA ordering open | [vertex_buffer_va.md](vertex_buffer_va.md) |
| P-43 | memory | Native write-watch title arm (the watch has no title consumer/auto-arm yet) | maintainer | attempted (`a331289cf`), device-loss stop; counters unavailable | [title_memory_model.md](title_memory_model.md) |
| P-44 | renderer | Bindless exhaustion arm (synthetic full-limit sizing only; real title degradation unmeasured) | maintainer | not run (two-loss stop) | [bindless_degradation.md](bindless_degradation.md) |
| P-45 | memory | Title-VA production BDA ordering: real-adapter window creation | maintainer | measured positive at 256 MiB (candidate 0x17490000, heap base 0x07400000, arena end 0x17400000, +0x90000 above end; alias registered, 5 pulls/5.6 MiB, submit success); pixel check blocked at slot-14 Signal -> P-48 | [vertex_buffer_va.md](vertex_buffer_va.md) section 24 |
| P-46 | renderer | Synthetic shader reads a strict direct-heap base dword and fetches a known descriptor | maintainer | done (`ce57adeb3`): GPU CTest 1/1, strict s7/s8 bases 0x10100/0x10200, 64/64 readbacks, missing-base E_INVALIDARG; SDK masks fixed to 0x400/0x800 | [root_packing.md](root_packing.md) section 10 |
| P-47 | renderer | Post-integration GPU draw-test target for multi-count viewport | maintainer | done (`e55f04d33` + this slice): `d3d12_viewport_array` CTest registered and passing (1/1, 1.6 s, validation layer on) | [d3d12_vulkan.md](d3d12_vulkan.md) |
| P-49 | platform | PE-loader rewrite phase 0: bounds-safe PE32+ parsing core | maintainer | done (`e597ce4c5`; 29 refusal + 6 edge shapes and the 48,659-case mutation test) | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-50 | platform | PE-loader rewrite phase 1: mapper over the parsed image with the old loader as the oracle | maintainer | mapper + unit tests + old-loader/parser parity landed; the old-loader vs new-mapper mapping/protection parity harness is planned round 19 (task-253) | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-51 | platform | Loader audit: core mapping fixes (page-tail copy, raw pointer 0, alignment validation, shared-page protection, PAGE_* derivation) | maintainer | done (this slice), `loader_core_audit` 46 checks | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-52 | platform | Loader audit: binding fixes (FirstThunk=0, bundled-missing-export fallback, ordinal mask, forwarder/non-PE, extensionless shadow, header export) | maintainer | done (this slice), `loader_bind_audit` | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-53 | platform | Loader audit: TLS/thread/unwind fixes (DllMain re-entry, per-EXE GA slot, index>=64, DllMain thread events, TLS dir size, chained unwind) | maintainer | done (this slice), `loader_tls_audit` 43 checks | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-54 | platform | Loader audit: base reservation relocatable predicate and header validation | maintainer | done (this slice), `pe_loader_test` +20 checks, `loader_selfcheck` 203 checks | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-55 | platform | Loader hypothesis falsifier: `PALMETTO_TRACE_TLS=1` + `PALMETTO_TEB_CHECK=1` arm | maintainer | done: per-thread GA views installed in every listed thread, ALIVE 90.3 s, no new stop; black frame not loader-caused | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-56 | platform | PE-loader V2 adapter: opt-in `PALMETTO_PE_LOADER_SHADOW_PARSE`/`_STRICT` wiring in map_image and the TLS F1 named refusal when no TlsAlloc slot below 64 is available | open (round-19 task-254, sole owner of pe_loader.cpp) | planned | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-57 | platform | Loader observability census: mapped module lines (base/size/relocated/preferred), bind summary, TLS attach/detach counts and VirtualProtect failures behind one switch, asserted by the parser/mapper tests | open (round-19 planned; additive pe_loader instrumentation) | planned | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-58 | renderer | Next measured D3D12X replay/memory parity gap outside d3d12_com/pe_loader: opt-in replay-side path or bounded readiness contract with synthetic tests and peer pin | open (round-19 task-255) | planned | [d3d12_vulkan.md](d3d12_vulkan.md) |
| P-59 | renderer | Next peer movie/overlay or first-run/UI slice: wire the movie_overlay_readiness contract at the second-plane decision behind an opt-in switch, or a plane identity/order contract, without a boot | open (round-19 task-256) | planned | [peer_port_movie.md](peer_port_movie.md), [movie_overlay_readiness.md](movie_overlay_readiness.md) |
| P-60 | tooling | Translator: next measured refusal class after the SGPR-move and LGKM-wait rules (491/668 translated, 177 refused) | open (round-19 task-257) | planned | [translator_fixes.md](translator_fixes.md) |
| P-61 | quality | Full-suite inventory refresh after the loader audit and the new loader/rewrite/P-47 targets | open (round-19 task-259) | planned | [testing.md](testing.md) |
| P-62 | docs | Docs parity/loader refresh for round 19: audit registration status, P-47 CTest result, cleanroom/remaining rows | open (round-19 task-260) | planned | [cleanroom_peers.md](cleanroom_peers.md) |

### Parity features (claimable with maintainer coordination)

| ID | area | task | owner | status | evidence page |
|---|---|---|---|---|---|
| C-01 | memory | Title memory model: GPU-visible aliasing/write-watch against the peers' shared-memory designs | maintainer | done (`3323f7834`) | [title_memory_model.md](title_memory_model.md) section 10 |
| C-02 | rt | Ray-tracing/BVH build route and inline-traversal wiring | maintainer | done (`a19ce9aab`, helper rehydration unwired) | [bvh_route.md](bvh_route.md) |
| C-03 | audio | Audio decode real data path: XMA2 and Opus wired to the title's own streams | maintainer | deferred (measured absence, `3bbb51497`; own falsifier) | [audio_decode.md](audio_decode.md) section 9 |
| C-04 | renderer | Console FSR upscaler route: the kind-7 consumer semantics behind the compose draw | maintainer | done (`0148cbfeb`, readiness only) | [fsr_route.md](fsr_route.md) section 9 |
| C-05 | renderer | Mesh: publish the library's MS programs under the resolver's key, then wire the mesh stream | maintainer | done (`5798bb070`, readiness: 0 MS keys) | [mesh_stage.md](mesh_stage.md) section 8 |
| C-06 | renderer | D3D12X command-replay parity gaps listed as open in the layer plan | maintainer | steps done (`5e8beeeb6`, `dfe2337ef`; multi-count replay still open) | [d3d12_vulkan.md](d3d12_vulkan.md) |
| C-07 | shaders | Translator refusal batches: a rule plus a semantic fixture per measured refusal class | maintainer | done (`e93cfe4e1`) | [design.md](design.md) contract table, [translator_fixes.md](translator_fixes.md) |
| C-08 | renderer | Presentation pacing/tokens beyond the frame clock: per-origin pacing as the peers implement it | open | open | [peer_port_first_ui.md](peer_port_first_ui.md) rank 1 follow-up |

### Docs, tooling and CI (claimable)

| ID | area | task | owner | status | evidence page |
|---|---|---|---|---|---|
| D-01 | coordination | Task-board maintenance and weekly triage: keep rows current, sequence overlapping claims | maintainer | in progress | this page |
| D-02 | docs | Refresh the parity matrix audit trail for the peers' newest commits | maintainer | done (`cf0fb8174`) | [cleanroom_peers.md](cleanroom_peers.md) |
| D-03 | ci | CI: run the GPU-free contract tests and the documentation checks on every pull request | maintainer | done (`2bb13be8b`) | [.github/workflows](../.github/workflows), [dev-tools.md](dev-tools.md) |
| D-04 | docs | Index/link sweep: stale HEADs, dead links and out-of-date frontier descriptions | maintainer | done (`9edd62c44`) | [issues.md](issues.md), [README.md](README.md) |
| D-05 | docs | Crash-report page follows the merged every-thread dump (O3): verify the page matches the shipped switch | maintainer | done (`bcb030b44`, PR #6) | [crash_report.md](crash_report.md) |
| D-06 | platform | Synthetic benchmark and launcher measurement correctness | spyro24955 | claimed | issue #4 claim comment |
| D-07 | platform | D3D12XboxCreateDevice COM proxy interface investigation | spyro24955 | claimed | issue #4 claim comment |
| D-08 | docs | Every-thread crash dump (O3): implemented and merged as #5 | XccQ114 | done | [crash_report.md](crash_report.md) |

## Board history

- The first claims used a free-form `claiming: <description>` convention in the coordination issue. The board keeps those
  claims (D-06, D-07, D-08) and moves new work to the IDs above so overlapping scopes are visible before a pull request
  exists.
- Please add a row when you land a port or a measurement that changes a lane: [docs/README.md](README.md) names the page,
  and the board row links it.
