# Remaining upstream mechanisms: ranked baseline and current measured status

**Status: read-only analysis with measured/landed overlays through rounds 16–20 (2026-11-27).** The original rank table,
mechanism notes and next-slice plan preserve the earlier hypothesis snapshot; their former “Our tree today” and “Measured
symptom” cells are historical baseline, not live status. Section 1.1 preserves the prior measured/landed overlay for ranks
1–9; section 1.2 adds rounds 16–17 and section 1.3 rounds 18–20, each taking precedence only for the mechanisms it names. No boot was run and no code,
game data or peer code changed for this documentation refresh; the earlier boot-arm results below are linked evidence.
Every number is either a measured count from a page of ours or a numstat line count from the public fork's clone at
`D:\leonida-work\leonida-fork`.

Inputs read: [upstream_port.md](upstream_port.md) (the porting policy), [d3d12_vulkan.md](d3d12_vulkan.md),
[story_load_probe.md](story_load_probe.md), [frame_first_light.md](frame_first_light.md),
[loading_gate.md](loading_gate.md), [user_event_probe.md](user_event_probe.md),
[directstorage_advanced.md](directstorage_advanced.md), [cpu_view_mapping.md](cpu_view_mapping.md),
[streamed_image_backing.md](streamed_image_backing.md), [streamed_consumer.md](streamed_consumer.md),
[readfile_first_touch.md](readfile_first_touch.md), [frontier_null_read.md](frontier_null_read.md),
[page_address_space.md](page_address_space.md), [root_arguments.md](root_arguments.md),
[command_signature.md](command_signature.md), [graphics_record.md](graphics_record.md),
[compute_slot67.md](compute_slot67.md), [pipeline_cache.md](pipeline_cache.md), [buffer_bounds.md](buffer_bounds.md),
[present_wiring.md](present_wiring.md), [long_boot.md](long_boot.md); and the fork's own documentation at
`273b3bdf9`, `18a22c1ae` and `ed94fbef9` (the fork's linux_proton.md, d3d12_vulkan.md, directstorage.md and
boot_leads.md).

The fork's story-load stop list is quoted in its squash message and in its own docs/linux_proton.md "What this branch
changes to get there" and "Where it stops". This page answers: of that list, what is still missing here, in what order
will it bite, and what is the smallest slice for each.

## 1. The ranked list

Rank is the likelihood that the mechanism gates the next observable phase. "Frontier" names the measured wall it
belongs to: `streamed content` = the story load once it starts; `memory` = the process footprint; `world` = the phase
after the load; `post-load` = polish once content exists.

The following eight-column table preserves the original prioritization snapshot. Its tree/symptom cells are historical;
sections 1.1–1.2 are layered current-status overlays, with section 1.2 taking precedence only for the mechanisms it names.

| # | Mechanism (upstream name) | Frontier | Upstream evidence (commits, files, +/−) | Our tree then (historical baseline) | Measured symptom then (historical baseline) | Effort | Independently testable |
|---:|---|---|---|---|---|---|---|
| 1 | DirectStorage **page-list resolver** and live page transfer | streamed content | `273b3bdf9` `shims/dstorage.cpp` +419/−94, `dstorage_staging.h` +104; `0aecc5efa` (#968) 3 files +151 (`d3d12_xs_device.cpp` +87, `xmem.cpp` +56, `xmem.h` +8) | plan and copy math implemented in `port/shims/dstorage_advanced.h` + `port/shims/dstorage.cpp`; a live request is refused by name ("no page resolver"); `port/shims/xmem.h` exposes commit only | page-list requests measured 0 (`dest-pages=0 source-pages=0`, 8,272 shapes in 300 s); mechanism inert, the refusal is named | S | yes - unit test through a synthetic resolver; promotion falsifier is a trace with `dest-pages` non-zero |
| 2 | **Bindless table degradation** and larger image arrays | streamed content | `273b3bdf9` `src/vulkan_bindless.h` +260/−65, `src/descriptor_table.h` +57/−12, `src/gfx10_descriptors.h` +11, `shims/d3d12_xs_desc_clean.h` +74 (new), `shims/d3d12_xs_alloc_size.h` +268 (new) | `port/src/vulkan_bindless.h` 979 lines: a failed replacing flush returns false, and the queue then refuses the whole submission (`port/src/vulkan_backend.h:3134`); no spares, no host fallback, no degrade; 2D-array/3D/1D arrays not grown | 0 refused submissions today because the image population is 32 images / 279.1 MiB; the fork's streamed population is ~38,000 images | S/M | yes - `port/test/d3d12_bindless_churn_test.cpp` plus an injected allocation failure |
| 3 | **Title buffers at raw addresses**: title-VA alias table and windows outside the arena | streamed content | `273b3bdf9` `src/title_va_alias.h` +314 (new), `shims/d3d12_title_buffer_windows.h` +119 (new), `src/buffer_va_index.h` +6; `ed94fbef9` (#1723) buffer windows +37/−5, `placed_image_map_note.h` +84 (new), `xg_xs_layout.h` +7/−1 | `port/src/console_va_map.h` 177 lines aliases the two measured title ranges, `port/src/buffer_va_index.h` 87 lines; no general title-VA table, no window when the arena has no usable offset, no persistent copies of streamed buffer views | the two measured ranges are rebased and draw (0 refused submissions); an unregistered title VA stays refused; the upload/readback V# bias is measured to fail (D3D-53) | M | yes - `port/test/buffer_va_index_test.cpp` plus a synthetic alias table and a draw that binds an upload buffer as a V# |
| 4 | **Native write faults beyond first-touch**: title write watch, fault retries under the tracker lock, shadow-copy invalidation | memory | `273b3bdf9` `shims/xmem.cpp` +400/−63, `xmem.h` +8, `src/title_page_watch.h` +700 (new), `src/title_geometry_watch.h` +123 (new), `src/pe_loader.cpp` +411/−66, `src/vulkan_device_fault.h` +429 (new); `237deb64ce` (#1539) +77/−10; `ed94fbef9` (#1723) `xmem.cpp` +1,584/−150, `arena_blocks.h` +238, `xmem.h` +49 | `port/shims/xmem.cpp` 1,170 lines: first-touch commit handler and physical-page sections; `port/shims/first_touch.h` 57 lines for the read/DS retry; no write-watch consumer, no copy invalidation, no retry under a tracker lock | first-touch class fixed (271,044,608 bytes over 4,136 pages; 25.6 s fatal stop becomes a 130 s frame loop); private bytes 16.7 -> 83.6 GB at 300 s (~94% of the ~89 GB commit limit, still rising) and the full-heap mirror refresh costs 3.4-4.4 ms per submission | L | yes - synthetic `GetWriteWatch` test, a remap-race test, and a boot timing the mirror refresh and the footprint |
| 5 | **Translator loop/packed-dot/buffer-bounds fixes** | streamed content + world | `273b3bdf9` `tools/rdna2spirv.py` +254/−72, `tools/spirv_loop_guard.py` +128 (new), `shader_ir.py` +21/−23, `shader_library.py` +54/−14, `lane_masks.py` +19, `wave64_emulate_opt.py` +72/−4, `wave64_halves.py` +7/−1, `pe_shader_containers.py` +48/−2, `fspx_unpack.py` +7/−2, `shader_disasm.py` +5/−2, `rdna2_decode.py` +19, plus `tests/semantic` fixtures | `tooling/rdna2spirv.py` 18,619 lines and its own lane-mask/wave64 modules; per-mode buffer bounds are implemented (`docs/buffer_bounds.md`); no loop-bounds guard, no packed-dot form for the eight FidelityFX programs | slot-67 library compute still refuses a named stock: 260 of 322 requested programs translate, 62 stay refused (36 fall-through, 16 work-group-info SGPR, 7 lane-mask bit-replicate, 3 other); 13 of 29 FidelityFX programs translate | M | yes - translator-only: refusals per reason, corpus counts, `spirv-val`, GPU-free |
| 6 | **Directly indexed heaps and root-signature packing** | world | `273b3bdf9` `src/root_signature.cpp` +22/−5, `root_signature.h` +14/−8, `shims/d3d12_com.cpp` +369/−41, `shims/d3d12_xs_pso_stream.cpp` +381 (new); `98efc22ca3` (#1064) +17/−2; `bfef489cb6` (#1232) +19/−3; `b07b9ecb09` (#974) 6 files +66/−20; `a2de554e56` (#1353) 3 files +213/−20 | `port/src/root_signature.cpp` 498 lines already packs the console stage bases s4/s8/s16 (D3D-448); the heap-base dword after the explicit arguments is **not** written ("SGPR slot open"); `port/shims/d3d12_xs_rootsig.cpp` 175 lines masks the Xbox flag byte; spills are refused | the measured frame's user data holds 2 of 32 non-zero dwords, `s8`/`s9`/`s10`/`s28` unset, both targets 0 of 8,294,400; `d3d12_vulkan.md` section 3.4 leaves the graphics-stage heap-base placement open | S/M | yes - a CPU-side packed-dword test plus a D3D12 draw with the directly-indexed flags set |
| 7 | **Mesh stages** and their heap offset / indirect path | world | `273b3bdf9` `shims/d3d12_geometry_views.h` +216 (new), `shims/xg_xs_layout.h` +410 (new), `shims/d3d12_xs_pso_stream.cpp` +381 (new); `ed94fbef9` (#1723) `d3d12_indirect_draw_policy.h` +20 (new), `d3d12_xs_list_values.h` +1,291 (new), `d3d12_geometry_views.h` +189/−25; `e79906a42b` (#939) `d3d12_xs_native_pso.h` +243 (new) | `port/src/graphics_pso.cpp:333` refuses a mesh stage inside a D3D12 graphics description; the 5th command signature is a named `DISPATCH_MESH` stand-in whose `ExecuteIndirect` is refused; no stream-description PSO path, no mesh indirect replay | no boot sample decoded with a mesh stage; the mesh signature's `ExecuteIndirect` never executed in the measured window; the fork itself states mesh stages do not get the heap offset yet | M | creation side GPU-free (`port/test/xs_proxy_mock_test.cpp`); replay needs a mesh-capable GPU |
| 8 | **Pipeline compile workers** | post-load | `3de22a1904` (#1547) 6 files +412 (`src/pipeline_workers.h` +173 new, `vulkan_replay.h` +136/−12, `d3d12_vulkan_device.cpp` +45/−7, `vulkan_backend.h` +29/−1); `273b3bdf9` `d3d12_pipeline_compile.h` +128 (new), `d3d12_pipeline_checkpoint.h` +97 (new), `pipeline_cache_store.h` +69, `pipeline_profile.h` +70; `ed94fbef9` `d3d12_pipeline_compile.h` +232/−28 | compilation is inline; our own `port/src/pipeline_cache.h` and the persistent/recipe cache exist; no worker threads | one 0-30 s burst: 51,786 graphics pipelines in 20,545.9 ms (driver 34.9%, layer 44.9%); nothing created later; 0 refusals | S/M | yes - fake compile timing in `port/test/pipeline_cache_test.cpp` and a boot with a cold driver cache |
| 9 | **Presentation pacing/tokens** (shared `WaitFrameEventX`/`PresentX` source) | post-load | `89e0ea9b55` (#402) 5 files +290/−71; `273b3bdf9` `src/frame_pacer.h` +19/−6, `src/d3d12_xs_frame_events.h` +47 (new), `shims/xbox_present.cpp` +48/−15, `src/vulkan_present.h` +378/−19; `afb14d56c` +38/−12, `ddafa07b46` +235/−57 | `port/src/frame_pacer.h` 436 lines, `port/shims/xbox_present.cpp` 665 lines, device slots 78/79/80 served, and a token bridge accepts the device proxy's reported token; the pacer is not the token source | 8,625 presents / 90 s with 0 failed; 33,275 presents / 300 s; the presented frame loop is alive and is not the gate | S | yes - `port/test/frame_pacer_test.cpp` and `port/test/xbox_present_test.cpp`; a boot counting token origins |

### 1.1 Current measured/landed status (2026-11-27)

This compact overlay is authoritative for the ranks below; the original detail and rationale remain available as history.
A status of “readiness” or an empty title population is not a claim of title behavior.

| # | Current status | Latest evidence |
|---:|---|---|
| 1 | **Landed; title demand absent.** A diagnostic boot arm exercised the real DirectStorage request path with a synthetic request: 100,000 bytes delivered through two mapped views and 25 page-watch trips; the title's own file reads still had 0 destination-page lists. | [page_resolver.md](page_resolver.md); [peer_port_streaming.md](peer_port_streaming.md), §§13–14; [`66b33d782`](https://github.com/luizwidmer/palmetto/commit/66b33d782); [`f495a6261`](https://github.com/luizwidmer/palmetto/commit/f495a6261) |
| 2 | **Landed; exhaustion remains latent.** Bindless replacement degradation and array growth are implemented; the measured population is 32 image allocations / 279.1 MiB with 0 refused submissions, so title-scale exhaustion is not demonstrated. | [bindless_degradation.md](bindless_degradation.md); [`61f544a1b`](https://github.com/luizwidmer/palmetto/commit/61f544a1b) |
| 3 | **Partial; title-VA mapping remains bounded.** The console VA map rebases the two measured title ranges; unregistered VAs remain named refusals, with production outside-arena placement and persistent streamed-buffer copies still unresolved. The measured GPU-view pair recorded 0 placed-texture resolutions and 0 pages in its CPU half, so production GPU aliasing is not demonstrated. | [page_address_space.md](page_address_space.md); [peer_port_movie.md](peer_port_movie.md), §11; [`4b0821bcb`](https://github.com/luizwidmer/palmetto/commit/4b0821bcb) |
| 4 | **Partial; no title write-watch result yet.** The alias-expanded watcher has CPU-only tests (496 checks); no P-30 title arm or title GPU-backed alias effect was measured. Plane refresh is a related texture-consumer slice, not the ranked native write-fault / shadow-copy invalidation mechanism: its 300 s arm tracked two 2×2, 64-byte textures over 8,191 pairs with 0 changed and 0 uploaded. Memory growth remains measured but unattributed. | [title_memory_model.md](title_memory_model.md), §§10–11 (**§11 in-flight**); [peer_port_movie.md](peer_port_movie.md), §§8.4–8.5; [story_load_probe.md](story_load_probe.md); [`3323f7834`](https://github.com/luizwidmer/palmetto/commit/3323f7834); [`9a86418c4`](https://github.com/luizwidmer/palmetto/commit/9a86418c4) |
| 5 | **Partial; translator fixes landed, corpus still partial.** At the prior overlay snapshot, the request-union scan validated 480/668 and left 188 refused; the later translator scans supersede that count in §1.2. The prior additive corpus publish records 444 entries; the later translation additions remain unrecorded as a newer corpus publish. | [compute_corpus_2.md](compute_corpus_2.md); [translator_fixes.md](translator_fixes.md), §§5–6, 12; [`1a48d38da`](https://github.com/luizwidmer/palmetto/commit/1a48d38da); [`e93cfe4e1`](https://github.com/luizwidmer/palmetto/commit/e93cfe4e1) |
| 6 | **Partial; measured feed landed, but did not produce a frame.** The opt-in vertex/transform feed supplies the measured bound vertex V#; the constant-pass target remains zero and two of three with-feed arms stalled before the frame loop. Directly indexed heap-base placement and spills remain open. | [vertex_constant_feed.md](vertex_constant_feed.md); [root_arguments.md](root_arguments.md); [`58472232b`](https://github.com/luizwidmer/palmetto/commit/58472232b) |
| 7 | **Opt-in stream/publication landed; selected MS population is empty.** There are 0 MS resolver keys against 7,498 library MS programs, no `ms` selection, and all 10,241 measured slot-57 calls remain vertex-family; no title mesh record or picture was observed. | [mesh_stage.md](mesh_stage.md), §8; [`5798bb070`](https://github.com/luizwidmer/palmetto/commit/5798bb070) |
| 8 | **Workers and hold measured; the pool/session gate did not move.** The hold A/B removed pipeline-wait time from title threads, but the five pool-2 events stayed unsignalled and state was unchanged at 298 s. The separate overlap A/B recorded no queue-room wait at bound 64; both on attempts lost the device, so it does not establish a causal improvement. | [deferred_compile.md](deferred_compile.md); [peer_port_streaming.md](peer_port_streaming.md), §§10.4–10.5 and §16 (**§16 in-flight**); [`375a9b14c`](https://github.com/luizwidmer/palmetto/commit/375a9b14c); [`8b7a800be`](https://github.com/luizwidmer/palmetto/commit/8b7a800be); [`5798bb070`](https://github.com/luizwidmer/palmetto/commit/5798bb070) |
| 9 | **Present-driven pacing landed; handshake changed, title state did not.** The 50 s A/B changed token ownership and measured cadence, with about 12% fewer presents, but state words and captures matched through 44 s before the loading transition; the separate 33,275-present / 300 s run shows the present path itself is live. | [peer_port_first_ui.md](peer_port_first_ui.md), §12; [window.md](window.md), §5; [`7c867c03e`](https://github.com/luizwidmer/palmetto/commit/7c867c03e) |

### 1.2 Rounds 16–17 measured/landed addendum (2026-11-27)

This later overlay supplements—not rewrites—§1.1; it supersedes earlier status only for the mechanisms named here. This
documentation refresh ran no boot or code; it records arm results from the separately coordinated tasks cited below.

**Rank reconciliation.** §1.1's numbers follow the preserved upstream-mechanism table: rank 4 is native write faults
beyond first-touch. `peer_port_movie.md`'s local rank-4 plane-refresh port is a separate slice, not a rank in this
inventory. This addendum uses taskboard IDs for the rank-owned work rather than repeating those rank numbers.

| Board ID / slice | Rounds 16–17 status | Latest evidence |
|---|---|---|
| P-44 — Bindless exhaustion | **The Task-236 arm attempted but did not measure bindless counters.** The synthetic sizing baseline is 32 image allocations / 279.1 MiB, not title-scale exhaustion. The earlier Task-228 arm did not launch after the P-40/P-43 two-loss stop. Task-236 P-44 ran 62.3 s then STOPped with title AV `0xC0000005`, one Vulkan loss, one exception and 177,657 D3D12 calls. The set-size line recorded `65536/3936/16384/4096/4096/4096`, but `degraded_flushes`/`spares_reused`/`keys_refused` snapshot was not emitted; captures were suppressed. At 60 s state was init2/loading2/session1/machine2→2. Bindless telemetry is unavailable, not zero, and no exhaustion negative was measured. | [bindless_degradation.md](bindless_degradation.md), §§9–11; [device_slot70.md](device_slot70.md), §13; [title_memory_model.md](title_memory_model.md), §13 |
| P-42/P-45/P-48 — Title-VA windows | **Synthetic seam passes; production BDA ordering now admits one candidate, but pixel acceptance is blocked.** Synthetic GPU lookup/pull readback returned `21 43 65 87`, then `9a bc de f0` after the source-page change. P-45's first controlled probe stopped before allocator because inherited `PALMETTO_ARENA_MB=1024` triggered the 512 MiB heap-OOM assertion. The corrected 256 MiB run placed the production candidate 0x90000 above arena end, registered the alias, pulled 5 times / 5.6 MiB, and submitted `ExecuteCommandLists` without a below-base refusal or device loss. The overall test exits 1 at Xbox Signal slot 14 proxy refusal before CPU pixel/readback acceptance; thus no pixel correctness result. An earlier qualitative `window_below_heap_base` observation is a separate below-base case, not the current candidate. Dedicated Title-VA GPU CTest remains deferred as P-48. | [vertex_buffer_va.md](vertex_buffer_va.md), §24; [`6351f6846`](https://github.com/luizwidmer/palmetto/commit/6351f6846); P-42/P-45/P-48 |
| P-43 — Native write faults | **No title write-watch result.** P-43 ended on a failed queue signal with one device-loss event (the diagnostic appeared twice); no consumer was wired and range/trip/epoch counters were unavailable, not zero. P-30 alias/watch was not run after the stop. | [title_memory_model.md](title_memory_model.md), §13; [peer_port_streaming.md](peer_port_streaming.md), §16 |
| Translator decode-gap rules | **Later scan supersedes the older count; gap recoveries add no whole-program closure.** The translator page's current-tree pre-rule scan is 491/668 validated / 177 refused, superseding §1.1's prior 480/668 snapshot. The `s_mov_b32` rule restores 12 measured gap words and exact `s_waitcnt lgkmcnt(0)` recovers 30 programs; each affected program still reaches a later unsupported gap, so both rules add 0 whole-program closures/status changes. All 491 previously-valid modules remain byte-identical. This is not a new corpus publish. | [translator_fixes.md](translator_fixes.md), §§14–15; [`13f423532`](https://github.com/luizwidmer/palmetto/commit/13f423532) |
| P-46 — Direct heaps | **Strict admission and the isolated GPU falsifier now pass; title use remains absent.** `PALMETTO_DIRECT_HEAP_STRICT=1` validates the full plan and both bases before writing; 96 CPU assertions pass. After correcting SDK masks and the shim-safe queue/fence signal, `root_signature`, `root_signature_oracle`, and the final `d3d12_compute` CTest pass (1/1). The strict child matched 64/64 outputs at s7/s8 bases `0x10100`/`0x10200`, known SRV `0x5a17c0de` and four S# dwords; both missing-base controls refused `E_INVALIDARG` with named diagnostics. Legacy complete and missing-sampler-safe dispatches also matched 64/64. The title trace still has no direct-index commands. | [root_packing.md](root_packing.md), §10; [`6351f6846`](https://github.com/luizwidmer/palmetto/commit/6351f6846); P-46 |
| Native MS publication | **Native MS publication path landed; real selection is empty.** Explicit `ms` selection now maps native code hashes into the runtime FSPX index, and all-refused translation prunes only the selected MS prefix. Synthetic tests pass; the library has 7,498 distinct MS programs but the current selection and resolver index have 0 MS keys, and the sampled boot's 10,241 slot-57 records were vertex-family. No real MS module, title mesh record or picture was measured. | [mesh_stage.md](mesh_stage.md), §§8, 10; [`3eebb1bae`](https://github.com/luizwidmer/palmetto/commit/3eebb1bae) |
| P-31 — Compile/submit overlap | **P-31 did not show a queue-room wait or improvement.** No `submit-flight: room-wait` appeared in any arm; overlap-off reached 130.3 s without device loss, while both overlap-on attempts lost the device at 64.3/68.5 s. This is a safety correlation, not proof the switch caused loss; the second loss stopped the series and P-30 was not run. | [peer_port_streaming.md](peer_port_streaming.md), §16; `5798bb070` (overlap implementation) |

**Adjacent board items outside the upstream nine-rank list.** P-32/P-41 strict multi-count viewport/scissor replay passed the direct counted-draw GPU readback at `(8,8)=(0,255,0,255)` with 0 validation errors and 10 readiness cases passing. A multi-count `ExecuteIndirect` carrying array state remains named-refused as `indirect_multi_count_viewport_state_not_captured` because its replay snapshot has no graphics state; the five measured title sites cap `MaxCommandCount` at 1. P-47's dedicated `d3d12_viewport_array_test` is now CMake-registered and the `d3d12_viewport_array` CTest passes 1/1 (1.6 s, validation layer on), so the earlier source-only status is retired (section 1.3). P-34's static disassembly resolves `XDspSubmitCommand` roles as RCX=context, RDX=0x20 descriptor, R8=command-id output; 136 synthetic checks pass and the title still has no live submit. Task-236 P-34 ran 90.3 s ALIVE/time-limit with no loss or exception and 179,177 D3D12 calls; no submit hit occurred. It saw 350 contexts and five 5,600-dword scans, all zero; the decoder DLL was missing. At 88 s state was init2/loading1/session0/machine2→2. | [d3d12_vulkan.md](d3d12_vulkan.md), C-06; [audio_decode.md](audio_decode.md), §§11–12; [`6351f6846`](https://github.com/luizwidmer/palmetto/commit/6351f6846); [`9d00b09d7`](https://github.com/luizwidmer/palmetto/commit/9d00b09d7) |

### 1.3 Round-18–20 loader-audit and viewport-test addendum (2026-11-27)

This later overlay supplements—not rewrites—§1.1 and §1.2. It records the inherited-loader audit, the CTest
registration of the rewrite phases and the P-47 viewport CTest; this documentation refresh ran no boot and changed no code.

| Board ID / slice | Rounds 18–20 status | Latest evidence |
|---|---|---|
| P-47 — dedicated viewport CTest | **Registered and passing.** The strict count-2 viewport/scissor draw test owns its device/list/PSO; `d3d12_viewport_array` passes 1/1 (1.6 s, validation layer on) and the earlier source-only status is retired. | [d3d12_vulkan.md](d3d12_vulkan.md), C-06; [handoff.md](handoff.md) |
| P-49/P-50 — parser, mapper, resolver | **Phases 0–2 landed and CTest-registered, and mapper parity passes.** `pe_image`, `pe_image_fuzz`, `pe_mapper` and `pe_resolver` all pass, and the round-20 mapper-vs-old-loader mapping/protection parity harness (`pe_mapper_parity`, task 253) reports 285 checks / 0 failures; the old loader's protection derivation was aligned to the measured rule, not the mapper weakened. | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-51/P-52/P-53/P-54 — inherited-loader audit | **Fixed and CTest-registered.** `loader_core_audit` 46 checks / 0 failures, `loader_bind_audit` 27 checks (20 contracts held, 0 faults still present), `loader_tls_audit` 45 checks / 0 reproduced (F1, F5, F6 report FIXED), `pe_loader_test` 149 checks, `loader_selfcheck` 203 checks / 0 faults; the CPU loader subset ran 16/16 on 2026-11-27. | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-55 — TLS falsifier | **Loader ruled out as the black-frame cause.** 103/103 light arms across 37 builds map the title at its preferred base, bind all 587 imports with 0 unimplemented, install TLS and report no loader failure; the `PALMETTO_TRACE_TLS=1` + `PALMETTO_TEB_CHECK=1` arm installs the per-thread GA view in every listed thread and runs 90.3 s ALIVE with zero exceptions. The frontier is content/presentation. | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-56 — V2 adapter and TLS F1 | **F1 hardened and the shadow wired; V2 still unwired.** A `/GA` image with no free `TlsAlloc` slot below 64 is refused by name (`GA_TLS_SLOT_UNAVAILABLE`) instead of logging and continuing, verified by the extended `loader_tls_audit` (45 checks, 0 reproduced faults, `FIXED AUDIT-F1`); `PALMETTO_PE_LOADER_SHADOW_PARSE`/`PALMETTO_PE_LOADER_SHADOW_STRICT` compare the parser with the old loader behind bounded named diagnostics, verified by `pe_loader_shadow_wiring` (6 checks); `PALMETTO_PE_LOADER_V2` still selects no new path. | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| P-57 — loader census | **Formatter registered; the loader hunks are not applied.** CTest `loader_census` covers the pure formatter (47 checks) and the exact `PALMETTO_LOADER_CENSUS` switch; the `pe_loader.cpp` insertion hunks are held for the loader owner and the lead. | [pe_loader_rewrite.md](pe_loader_rewrite.md) |
| TLS F4/F7 contract gate | **Open by design; the gate landed.** CTest `loader_tls_race` passes under its `--expect-faults` gate: 9 checks with 2 reproduced faults (F4 duplicate-install window in `install_on_existing_threads`, F7 thread-event TLS callback self-deadlock); the gate expects them, so a future fix flips them to FIXED. | [pe_loader_rewrite.md](pe_loader_rewrite.md) |

**Boot-arm ledger (evidence from the separately coordinated task runs; this doc update ran no boots).** Task-236 used all 3/3 launches (P-34/P-44/P-35).

| Arm | Result and limit |
|---|---|
| Task-228 P-40 | 65.6 s; one Vulkan loss preceded the title exception. No slot-70 summary or compose V# readback appeared; acceptance/refusal totals remain unavailable. |
| Task-228 P-43 / P-44 | P-43 was the second loss event (duplicate same-timestamp diagnostics); write-watch counters were not emitted. The two-loss stop left Task-228 P-44 unlaunched. |
| Task-236 P-34 | 90.3 s ALIVE/time-limit; 0 loss, 0 exceptions, 179,177 D3D12 calls. No submit hit: 350 contexts and five 5,600-dword scans were all zero; decoder DLL missing. At 88 s: init2/loading1/session0/machine2→2. |
| Task-236 P-44 | 62.3 s STOP/title AV `0xC0000005`, one Vulkan loss, one exception, 177,657 calls. Set-size line `65536/3936/16384/4096/4096/4096` appeared; `degraded_flushes`/`spares_reused`/`keys_refused` snapshot absent (unavailable, not zero). At 60 s: init2/loading2/session1/machine2→2. |
| Task-236 P-35 | 55.5 s STOP/title AV `0xC0000005`, one loss/exception (second consecutive loss-affected arm), 144,998 calls. XG slot 2 was observed twice, each `array_size=1` (2,048² and 1,024²); no `array_size>1` slot-2 call. The 1×1 format-28 array sizes 2/6/12 returned measured-linear `S_OK` through create/copy but had no slot-2 call. At 52 s: init2/loading2/session1/machine2→2. Evidence: [texture_layout.md](texture_layout.md), §9. |

All Task-236 captures/readbacks were suppressed. Separately, P-31's off/on/retry sequence above had no queue-room waits and stopped after the second on-arm loss; do not call it causal, and do not treat the omitted P-30 arm as a negative.

## 2. Original rationale and frontier ordering (historical baseline)

The four measured frontiers, in the order the last pages name them:

1. **First light / the black constant pass** ([frame_first_light.md](frame_first_light.md)): both targets read 0 of
   8,294,400 pixels; the content dies at the constant pass's own inputs. At the time of this original ranking, a
   vertex/transform-feed slice for the console driver block at `s0`/`s1` and root layout at `s8` was in flight; it has
   since landed, but the target remains zero and two of three with-feed arms stalled before the frame loop (current rank 6
   above). The first-light result remains the picture frontier; the heap-base/spill/flag residue is still a separate
   world-path question.
2. **State-2 idle** ([story_load_probe.md](story_load_probe.md) section 3, [loading_gate.md](loading_gate.md)):
   `0x7446510` stays 2 and `0x98f828c` stays 0 to 298.9 s after a fully answered user sign-in at 115.1 s. No mechanism
   in this list is named in the fork's history as a loader fix, so the ranking below does not claim to gate it.
3. **Streamed content once loading starts**: ranks 1, 2, 3 and 5. These are the fork's own measured stops at that phase
   (page mappings that never become readable, the 16,000-image allocation wall, title buffers at raw addresses,
   translator coverage), so they are the most likely walls immediately after the state step.
4. **Memory** ([story_load_probe.md](story_load_probe.md) section 2): rank 4, with rank 3 for the copy-invalidation
   half. The growth is measured but not attributed, so this rank is a hazard and a mitigation path, not a proven cause.
   At the time of this original ranking, an `alloc_trace` measurement was the proposed next attribution step. The current
   overlay still records the growth as unattributed and does not claim rank 4 as its cause.

The three "cheap and independently testable" ranks (1, 2 and 9) are S or S/M and need no boot to prove their own
mechanics; 3, 4 and 5 are the M/L work that needs the measured population to pay off.

## 3. Mechanism notes (historical baseline; current status in section 1.1)

### 3.1 Page-list resolver and live page transfer (rank 1, S)

**What it is.** A console DirectStorage request may name a destination by physical page numbers rather than a linear
buffer; the shim must resolve each 64 KiB page number to a CPU address, then transfer into it (scatter/gather), with
the page plan and copy math already computed. The upstream squash states "DirectStorage pinned reads, page-list
scatter/gather, BCPack and unshuffle with a locally generated predictor file"; its page-list path resolves pages
through `xmem`.

**Why the title needs it.** The streamed texture library is built for this request shape; a request whose destination
is not linear fails whole. The fork measured the consequence of not having the mapping: "tens of thousands of streamed
views wait for memory that never becomes readable" (before #968), and after #968 it still lists two gaps (mappings change
at call time rather than queue order; ranges no mapping call reached).

**Ours.** [directstorage_advanced.md](directstorage_advanced.md) sections 3 and 6: the page plan, boundary, short-list
and copy math are implemented and tested (213 checks in `port/test/dstorage_test.cpp`), and a live request is refused
by name because `port/shims/xmem.h` exposes only `commit_physical_range` / `commit_physical_pages`, not a page-number
resolver. [cpu_view_mapping.md](cpu_view_mapping.md) sections 5-6: queue slot 31 (`CopyPageMappingsBatchX`) is
implemented through an xmem hook and is not reached in a boot yet (0 calls in the A/B pair).

**Measured symptom.** [story_load_probe.md](story_load_probe.md) section 5: 8,272 request shapes, `dest-pages=0`
`source-pages=0`, refusals 0 - the boot never asks for the shape, so the mechanism is inert, not broken. The falsifier
that promotes it is a trace with either page counter non-zero.

**Acceptance test.** One resolver export in `port/shims/xmem.cpp` that turns a page number into the CPU address of its
64 KiB physical page (with the existing physical-page bookkeeping as the source of truth), wired into
`page_list_request`; unit tests through a synthetic resolver; a boot whose `PALMETTO_DS_PAGES=1` arm reports
`dest-pages` non-zero and every status `S_OK`.

### 3.2 Bindless degradation and larger arrays (rank 2, S/M)

**What it is.** The descriptor table's new-generation path: when device-local memory is exhausted, a new table
generation goes to host-visible memory, released generations become reusable spares, a generation that still cannot be
made is rewritten in place when idle and otherwise the table keeps its last uploaded contents with new keys at the
fallback index; **a flush never fails the submission**. The same change grows the 2D-array, 3D and 1D image arrays, and
adds the descriptor-hash finalizer option.

**Why the title needs it.** The streamed image population is the one that grows the bindless set: ~38,000 streamed
images in the fork's story load. A set that cannot be rebuilt at that pressure turns one allocation failure into a
refused submission (the whole command list), which is the class the upstream commit message calls out.

**Ours.** `port/src/vulkan_bindless.h` 979 lines already has generations, retirable sets and a fallback at index 0, but:
`flush()` returns false when a new generation cannot be made; the bindless-prepare lambda
(`port/shims/d3d12_vulkan_device.cpp:8151-8189`) propagates that, and `port/src/vulkan_backend.h:3134` returns
`ReplayResult::unsupported` for the submission; there are no spares, no host fallback and no in-place rewrite. The
default 2D sampled array is 4,096 with growth to 65,536 (2D only); there are no 2D-array/3D/1D growth caps like the
upstream 16,384 / 4,096 / 4,096. `port/src/descriptor_table.h` 349 lines, `port/src/gfx10_descriptors.h` 649 lines.

**Measured symptom.** [streamed_image_backing.md](streamed_image_backing.md) section 1: one 130 s boot creates 32 image
allocations totalling 279.1 MiB, all device-local; the measured driver's `maxMemoryAllocationCount` is 4,096. So the
failure path is unreachable at today's population and becomes reachable exactly at the streamed population. The fork's
story load registers ~38,000 streamed images; our 2D capacity can hold the descriptors but the *generation* path is what
fails, not the count alone.

**Acceptance test.** GPU-free: an allocation hook that fails the Nth new generation, then a flush that must succeed
with the submission still recorded (`port/test/d3d12_bindless_churn_test.cpp` and `port/test/vk_bindless_test.cpp`
already exercise churn and retire). GPU: a churn test that fills the set past its default arrays and keeps submits at
0 refused.

### 3.3 Title buffers at raw addresses (rank 3, M)

**What it is.** The title sometimes names its own memory directly: slot 133 places a buffer at the CPU address of its
pages; V#s and root views point at its own per-frame rings; those addresses are far above the arena, so
`heap_base + console_va` reads nothing the layer made. The upstream mechanism is a process-wide alias table that
replaces such an address, wherever the layer can see it (root view, created view, table V#, vertex view, index view),
by the console VA of a GPU copy: the arena offset of a window when one is available, a device address minus
`heap_base` when the copy has its own memory, and a *window outside the arena* when the title's reservations fragment
it. Optional persistent device-local copies of streamed buffer views are the same mechanism with a memory cap.

**Why the title needs it.** Every buffer a shader can address must be an arena range or have a copy; the alternative is
an unaddressed title VA and a refusal, or a wild shader address. The title's streamed content and per-frame constant
rings are exactly this shape.

**Ours.** [page_address_space.md](page_address_space.md) implements design B (VA rebasing) for the two measured
title ranges: `port/src/console_va_map.h` (177 lines) keeps title ranges and a kind-preferring rebase,
`port/src/buffer_va_index.h` (87 lines) knows the slot-133 buffers by VA, the console descriptor heap gets an arena
mirror, and unrebased VAs are refused at `draw.console_va` rather than executed. What is missing is the *general*
alias table (any title-memory address seen at the five view sites), the window store when the arena has no usable
offset, and the persistent copies. `d3d12_vulkan.md` section 3.3 records the measured failure of the cheap substitute:
biasing a V# by `-heap_base` fails for a V# base field (D3D-53), so a title-memory buffer behind a V# must be copied,
not biased.

**Measured symptom.** The current frontier is clean (0 refused submissions, 17,674 executed draws per 150 s in
[page_address_space.md](page_address_space.md) section 4) because the two ranges the title names today are registered.
The latent class is measured elsewhere: upload/readback buffers bound as V#s are measured to fault
([d3d12_vulkan.md](d3d12_vulkan.md) section 3.3), and an unregistered title VA stays refused. The fork's own doc names
the two cases that need the table: slot 133 placement, and V#s/root views into the title's per-frame rings.

**Acceptance test.** `port/test/buffer_va_index_test.cpp` extended with a synthetic process-wide table, and a D3D12
draw that binds an upload/readback buffer as a vertex or constant V# and must fetch the right bytes without a fault.

### 3.4 Native write faults beyond first-touch (rank 4, L)

**What it is.** After a GPU copy of title memory exists, the layer must know *which title pages changed* since the last
submission: where xmem reserved with `MEM_WRITE_WATCH`, the kernel answers (`GetWriteWatch`); elsewhere the copy's pages
are made read-only and the title's first write faults into the layer's handler. Two writes do not fault and are
reported instead: aliased physical pages (`XMemMapPhysicalPages` maps one page at many VAs) and kernel writes
(`ReadFile` into read-only pages fails rather than faulting). The fault handler must be able to wait for a tracker lock
without waiting on the process heap lock, because the faulting thread may hold it.

**Why the title needs it.** This is the correctness half of rank 3 and the memory half of the load: without the write
watch, the layer either copies whole title ranges every submission or keeps stale copies; with it, copies are
incremental and can be made device-local on demand. The fork's own measured memory result sits in the same story-load
work: deferred backing moved Windows A host private memory from 22-24 GB to 11.9 GB, and the Linux machine holds
~14.6 GB at the end of the load after the cuts.

**Ours.** `port/shims/xmem.cpp` (1,170 lines) already has the placeholder/physical-page model, the first-touch commit
handler (`physical_page_fault`, lines 727-748), the `XMemGetWriteWatch`/`XMemResetWriteWatch` exports (tested in
`port/test/xmem_test.cpp`), and the queue-slot-31 page mapping hook. It has no consumer of write watch, no copy
invalidation, no alias reporting, no fault retry under a tracker lock and no separate fault heap.
[readfile_first_touch.md](readfile_first_touch.md) and [frontier_null_read.md](frontier_null_read.md) measured and
fixed the first-touch class only (271,044,608 bytes over 4,136 pages, and the 25.6 s fatal stop becoming a 130 s frame
loop).

**Measured symptom.** [story_load_probe.md](story_load_probe.md) section 2: private bytes go 16.7 -> 83.6 GB over
300 s (about 94% of the ~89 GB commit limit) and are still rising, against 12.1 GB flat in the older 600 s boot; the
page states plainly that the growth is not attributed. On the copy side, [page_address_space.md](page_address_space.md)
section 4 measures the current full-heap mirror refresh: 728,676 us for the first, then 3.4-4.4 ms per submission (most
of the ~3.8 ms between submissions). That cost is the price of not knowing which pages changed.

**Acceptance test.** GPU-free: a synthetic write-watch test (write, read the dirty list, reset) and the upstream remap
race test class (`28f19186db`, +45 lines: a thread touching a page of a view being split waits for the remap). Boot: the
mirror refresh cost with and without write watch, and the process footprint at the same marks, on one A/B pair.

### 3.5 Translator loop/packed-dot/buffer-bounds fixes (rank 5, M)

**What it is.** Four translator changes from the squash: loop bounds on every emitted loop (5,128 compute modules have
loops), the eight FidelityFX programs that use a packed-dot form the disassembler skips (all 29 translate after it),
buffer bounds modes per view kind, plus a compaction option and the hash finalizer option.

**Why the title needs it.** Every compute pipeline the title asks for and the corpus cannot translate is a named
refusal on the frame loop's own pipeline path. The committed compute-library publish measured the size of this stock,
and the follow-up publish (in flight) extends it; the remaining refusals are decoder/emitter gaps, not missing data.

**Ours.** `tooling/rdna2spirv.py` is 18,619 lines with its own `lane_masks.py` (283 lines), `wave64_halves.py`,
`wave64_emulate_opt.py` and `fspx_unpack.py` (266 lines). Per-mode buffer bounds are already implemented and measured
on hardware ([buffer_bounds.md](buffer_bounds.md): RDNA2 follows the Mesa GFX11 column, 729 of 732 cases, with
`soffset` in the raw check). Missing/incomplete: loop-bounds guarding, the packed-dot disassembly form (13 of 29
FidelityFX programs translate), and the lane-mask forms behind the bit-replicate refusal.

**Measured symptom.** [compute_slot67.md](compute_slot67.md) section 2.4: of the 322 requested library compute programs,
260 translate and 62 are refused today - 36 `fall-through into undecodable words`, 16 `CS work-group-info SGPR use`,
7 `s_bitreplicate_b64_b32 on a lane mask`, 3 other - and the runtime's named refusal falls by exactly what is published
(326 -> 209 failure lines in the A/B). A second additive publish is in flight in this checkout (uncommitted), with the
same refusal classes in a larger requested set; nothing in it needs runtime code.

**Acceptance test.** Translator only: a wanted program set translates with the per-reason refusals report
(`tooling/rdna2spirv.py --refusals`), `spirv-val` clean, the 8 FidelityFX programs move the FSPX count from 13/29 to
29/29, and the corpus coverage line names each remaining refusal by reason. GPU-free.

### 3.6 Directly indexed heaps and root-signature packing (rank 6, S/M)

**What it is.** A root signature with `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` / `SAMPLER_HEAP_DIRECTLY_INDEXED` makes the
console driver place the bound heap base as a 32-bit pointer in an entry SGPR inside the user data; no stride arrives
(the stride is compiled into the shader). The upstream layer writes the heaps into the dword after the explicit root
arguments: compute from s4, VS/PS/GS from s8, mesh from s16 (mesh does not get it yet upstream either); it also models
the compute spill array behind `s[0:1]`, and it rewrites the root-signature blob's flags so the host serializer accepts
the signature while the layer still sees the heap-directly bits.

**Why the title needs it.** The fork measured this on the path that can lose the device: "culling compute at world
start reads buffers named by descriptors in the directly indexed heap; where the memory behind them is not mapped,
those reads leave mapped memory and the device is lost". Our own measurements already found the same user-data model
for the graphics stage: the translated stages read their console layout from `s8` and a descriptor table takes one
32-bit dword ([root_arguments.md](root_arguments.md) section 4, [d3d12_vulkan.md](d3d12_vulkan.md) section 3.4).

**Ours.** `port/src/root_signature.cpp` (498 lines, `pack_stage`) already places the console stage bases (s4 compute,
s8 vertex/pixel/geometry, s16 mesh) and the explicit parameters; `port/src/graphics_pso.cpp` packs the tables into the
graphics user data. The heap-base dword after the explicit arguments is not written: the code's own note says
"SGPR slot open" (`port/shims/d3d12_com.cpp:1543`), and `d3d12_vulkan.md` question 3 leaves the graphics-stage bases
open. `port/shims/d3d12_xs_rootsig.cpp` (175 lines) clears the Xbox high flag byte on the serialized blob; the
restore/flags half of the upstream #974/#1353 work is not carried. Spills above the 32-dword budget are refused, not
packed into `s[0:1]`.

**Measured symptom.** No boot has yet exercised a directly indexed heap: the current frame's two FSPX blit draws read
tables, not the heap index, and the measured user data holds 2 of 32 non-zero dwords
([frame_first_light.md](frame_first_light.md) section 6). So this is the *world-start* class, not the current black
frame's cause; a vertex/transform feed for the console layout is in flight now, and this item is its residue.

**Acceptance test.** CPU-side: `port/test/root_signature_test.cpp` extended with a directly-indexed signature and the
dword after the explicit arguments (per stage), plus a spill case. D3D12: bind a heap, set the flag, draw, read back
what a vertex and a pixel stage saw (the upstream equivalent is `d3d12_graphics_direct_heaps_test`); GPU-free for the
created dwords, GPU for the readback.

### 3.7 Mesh stages and their heap offset (rank 7, M)

**What it is.** Console graphics pipelines can carry a mesh stage, expressed on the PC only through the pipeline-state
*stream* description (the `MS` subobject); mesh draws and indirect mesh dispatch need their own replay path, and the
directly indexed heap offset for a mesh stage is not covered upstream either. The fork's later merges add the stream
records (`d3d12_xs_pso_stream.cpp`), a native-PSO path (`d3d12_xs_native_pso.h`), an indirect-draw policy
(`d3d12_indirect_draw_policy.h`) and a console list-value table (`d3d12_xs_list_values.h`).

**Why the title needs it.** The world phase's content passes are where a mesh stage appears; the title's indirect
family already has one `DISPATCH_MESH` signature created at startup, and the fork's own doc leaves the mesh heap offset
open.

**Ours.** `port/src/graphics_pso.cpp:333` refuses a mesh stage inside a `D3D12_GRAPHICS_PIPELINE_STATE_DESC` with a
named reason; there is no stream-description PSO path. The 5th command signature is a named dispatch-shaped stand-in
and `ExecuteIndirect` that names it is refused by name, never replayed as a dispatch
([command_signature.md](command_signature.md) sections 3 and 6). `port/src/vulkan_replay.h` can record a mesh draw if
the pipeline carries one, but no console record path builds one.

**Measured symptom.** [graphics_record.md](graphics_record.md): no boot sample decoded with a mesh stage, and
[command_signature.md](command_signature.md) section 5: the mesh signature's `ExecuteIndirect` never executed in the
measured window. So today this is a named, honest refusal on a path the title has not used yet.

**Acceptance test.** Creation side GPU-free: a stream-description record builds a mesh PSO (mock device in
`port/test/xs_proxy_mock_test.cpp`), and the indirect replay records a mesh dispatch rather than refusing. GPU side: a
mesh-capable device runs it.

### 3.8 Pipeline compile workers (rank 8, S/M)

**What it is.** Compile a submission's pipelines on worker threads instead of on the recording thread, with a
per-compile profile and a compile checkpoint, so a long pipeline burst does not stall the frame's record path.

**Why the title needs it.** #1547 landed upstream *after* their story load already worked, so it is not evidence of an
unblocker. It is adjacent to the idle-after-precompile question only in time: our measured burst is 51,786 creations in
20,545.9 ms inside the first 30 s, and nothing is created later, so it does not explain a 300 s idle.

**Ours.** Inline compilation with our own persistent `VkPipelineCache` and per-process recipe cache
([pipeline_cache.md](pipeline_cache.md)): 20.5 s -> 3.9 s with both switches on. Adding workers is a performance
change, not a gate; it becomes useful if a cold first boot ever hits the title's ~30 s watchdog (the fork's own reason
for the watchdog suppressor), which is why it is kept but deferred.

**Acceptance test.** `port/test/pipeline_cache_test.cpp` with a fake compile clock, plus a cold-driver-cache boot
measuring the recording thread's stall.

### 3.9 Presentation pacing and tokens (rank 9, S)

**What it is.** `SetFrameIntervalX` / `ScheduleFrameEventX` / `WaitFrameEventX` pacing, and one frame-token source
shared by the pacer and `PresentX` (the upstream `afb14d56c` / `ddafa07b46` pair).

**Why the title needs it.** A loading-screen tick that waits for a frame token the runtime does not issue can stall a
per-frame update while an independent blit loop keeps presenting. The fork's #402 work is a baseline merge before our
base, so most of it is already carried here.

**Ours.** `port/src/frame_pacer.h` (436 lines), `port/shims/xbox_present.cpp` (665 lines), device slots 78/79/80 served,
and a token bridge: `WaitFrameEventX` tokens come from the device proxy's own counter and are reported to the presenter,
which accepts them ([present_wiring.md](present_wiring.md)). The remaining difference is that our pacer is not the token
source; the measured frame loop presents 8,625 frames / 90 s and 33,275 / 300 s with 0 failed, so the difference is a
quality item.

**Acceptance test.** `port/test/frame_pacer_test.cpp` and `port/test/xbox_present_test.cpp`; a boot counting token
origins and wait timeouts.

## 4. Original three-slice plan (historical)

The original plan described the vertex/transform feed and second compute-corpus publish as in flight, then listed three
independent slices ordered by rank. Those two have since landed; ranks 1 and 2 are also implemented, while ranks 3 and 4
have only partial CPU/texture-refresh coverage. The read/build/acceptance details below are preserved as plan history,
not as current assignments or claims that those mechanisms remain wholly unimplemented; use section 1.1 for current status.

### Slice A - DirectStorage page resolver and live transfer (rank 1, effort S)

Read first: `port/shims/xmem.h` (30 lines, the public surface), `port/shims/xmem.cpp` (the physical-page and
placeholder bookkeeping, the slot-31 hook at lines 943-987), `port/shims/dstorage.cpp` (the page-list branch and its
named refusal at lines 2572-2610), `port/shims/dstorage_advanced.h` (the plan and copy math), and
`port/test/dstorage_test.cpp` (the existing page-plan section). Upstream evidence to read (not copy): commit
[`273b3bdf9`](https://github.com/ubiquitarianism/leonida/commit/273b3bdf9) files `runtime/shims/dstorage.cpp`,
`runtime/shims/dstorage_staging.h`, `runtime/src/dstorage_swizzle.h`; commit
[`0aecc5efa`](https://github.com/ubiquitarianism/leonida/commit/0aecc5efa) files `runtime/shims/xmem.cpp`,
`runtime/shims/xmem.h`, `runtime/shims/d3d12_xs_device.cpp`; and the fork's docs/linux_proton.md "Where it stops"
and docs/directstorage.md.

Build: one xmem resolver (page number -> CPU address of its 64 KiB page, valid for the process heap pages the title
registers), used by the existing page plan; keep the named refusal for out-of-range and unaligned pages; no change to
the default path. Acceptance: the existing 213 checks still pass, a new synthetic-resolver test transfers one request
end to end, and a `PALMETTO_DS_PAGES=1` boot reports `dest-pages` non-zero with every status `S_OK`.

### Slice B - Bindless degradation and array growth (rank 2, effort S/M)

Read first: `port/src/vulkan_bindless.h` (generations, `flush`, `release_retired`, `size_bindings`),
`port/src/descriptor_table.h`, `port/src/vulkan_backend.h:3134` (where a failed prepare becomes a refused submission),
`port/shims/d3d12_vulkan_device.cpp:8151-8189` (the prepare lambda), and the churn tests
`port/test/d3d12_bindless_churn_test.cpp` / `port/test/vk_bindless_test.cpp`. Upstream evidence:
[`273b3bdf9`](https://github.com/ubiquitarianism/leonida/commit/273b3bdf9) files `runtime/src/vulkan_bindless.h`,
`runtime/src/descriptor_table.h`, `runtime/src/gfx10_descriptors.h`, `runtime/shims/d3d12_xs_desc_clean.h`,
`runtime/shims/d3d12_xs_alloc_size.h`; and the squash message's bindless bullet.

Build: keep the last uploaded table when a new generation cannot be made (degrade), reuse retired generations as
spares, prefer the existing in-place rewrite when no submission can read the table, and grow the 2D-array/3D/1D arrays
inside the device's limits. The submission must never be refused for want of table memory; count and log the degraded
flushes. Acceptance: an injection test that fails the Nth allocation keeps every submission recorded; the churn tests
stay green; the array sizes are reported once.

### Slice C - Title VA alias, buffer windows and write-watch invalidation (ranks 3+4, effort M then L)

Read first: `port/src/console_va_map.h` (177 lines), `port/src/buffer_va_index.h` (87 lines),
`port/shims/d3d12_vulkan_device.cpp` (the console-heap mirror refresh and `create_at_host_address`),
`port/shims/xmem.cpp` (1,170 lines: placeholder, physical pages, fault handler, write-watch exports),
`port/shims/file_system.cpp`, `port/shims/dstorage.cpp`, and `port/test/buffer_va_index_test.cpp` /
`port/test/xmem_test.cpp`. Upstream evidence: [`273b3bdf9`](https://github.com/ubiquitarianism/leonida/commit/273b3bdf9)
files `runtime/src/title_va_alias.h`, `runtime/shims/d3d12_title_buffer_windows.h`, `runtime/src/buffer_va_index.h`,
`runtime/src/title_page_watch.h`, `runtime/src/title_geometry_watch.h`, `runtime/shims/xmem.cpp`,
`runtime/shims/xmem.h`, `runtime/src/pe_loader.cpp`; [`237deb64ce`](https://github.com/ubiquitarianism/leonida/commit/237deb64ce)
(the RADV import pins, which re-use the same watch); the post-story merge
[`ed94fbef9`](https://github.com/ubiquitarianism/leonida/commit/ed94fbef9) for `runtime/shims/xmem.cpp` and
`runtime/src/arena_blocks.h`; and the fork's d3d12_vulkan.md section 3.3.

Build in two steps so the first is independently useful: C1, a general title-VA alias table fed by the five view
sites, plus a window store for ranges the arena cannot hold, with unregistered VAs still refused; C2, the write-watch
consumer that tells the alias/shadows which title pages changed, replacing the full mirror scan, plus the kernel-write
and remap-race protections. Acceptance: `buffer_va_index_test` extended with the synthetic table; the mirror refresh
time and the footprint compared in one A/B pair; a draw that binds a title-memory buffer as a V# fetches the right
bytes.

## 5. Original dependency sketch (historical)

| Slice/item | Needs before it | What it unblocks |
|---|---|---|
| A page resolver | the existing page plan (already in `dstorage_advanced.h`); no other slice | live page-list transfers; rank 2's streamed population can then reach the table |
| B bindless degradation | nothing in this list; the churn test is the harness | the streamed image population (~38,000) without a refused submission |
| C1 alias/windows | nothing; the two measured ranges are the seed | title-named streamed buffers and descriptor tables |
| C2 write watch | C1's copy set (the thing to invalidate) | incremental copies instead of full scans; the memory mitigation path |
| Rank 5 translator | nothing; tooling only | compute-library coverage numbers |
| Rank 6 direct heaps | the in-flight console-layout feed (s8/s4), and the root-signature blob flag path | world-start culling compute; mesh heap offset |
| Rank 7 mesh | rank 6's heap offset for the mesh stage and the stream-description PSO path | mesh content passes |
| Ranks 8-9 | nothing; post-load | frame pacing and compile latency once content exists |

## 6. Cheap post-story-load changes, deferred in the original plan

The fork's post-#984 merges are mostly *after* its load worked, so they are deferred here and listed so the next
planner does not rediscover them. Sizes are numstat runtime +/− at the named commit.

| Change | Commit | Size | Why deferred |
|---|---|---|---|
| D16S8 UAV fallback for depth textures | [`bcaa13dad3`](https://github.com/ubiquitarianism/leonida/commit/bcaa13dad3) (#1558) | 2 files +49/−12 | only matters when a depth texture has no UAV; no measured path here |
| Device-fault report (last dispatches/checkpoints) | [`0bc810504a`](https://github.com/ubiquitarianism/leonida/commit/0bc810504a) (#1028) | 2 files +32 | a diagnostic; our `port/src/crash_unwind.cpp` is the current fault tool |
| Indirect-draw policy | `ed94fbef9` (#1723) | `runtime/shims/d3d12_indirect_draw_policy.h` +20 | world-phase indirect work; rank 7's replay path is the prerequisite |
| Compile-exit report | `ed94fbef9` (#1723) | `runtime/shims/d3d12_compile_exit.h` +13 | diagnostics for rank 8, not a gate |
| Placed-image map note | `ed94fbef9` (#1723) | `runtime/shims/placed_image_map_note.h` +84 | only after placed images are populated |
| Console list-value table | `ed94fbef9` (#1723) | `runtime/shims/d3d12_xs_list_values.h` +1,291 | larger than it looks; per-slot parity work for the world phase |
| Arena block allocator | `ed94fbef9` (#1723) | `runtime/src/arena_blocks.h` +238 | memory-management work that overlaps rank 3/4's needs; revisit with slice C |
| GDK lifecycle window hook | [`604ddff0bc`](https://github.com/ubiquitarianism/leonida/commit/604ddff0bc) (#769) | 3 files +291 | suspend/resume on window close; our window adoption is measured and stable |
| RADV import pins | [`237deb64ce`](https://github.com/ubiquitarianism/leonida/commit/237deb64ce) (#1539) | 3 files +77/−10 | Linux/RADV host-import pins; this host is native Windows, and the same file is reused by slice C |
| Stream-transfer barrier | [`fec45f0a13`](https://github.com/ubiquitarianism/leonida/commit/fec45f0a13) (#1099) | 1 file +23 | only for streamed copies; one small patch once slice A transfers |
| Silent spatial/output audio endpoint | [`42537e7b6f`](https://github.com/ubiquitarianism/leonida/commit/42537e7b6f) (#1598) | 5 files +1,395/−1 | already declined for this host ([silent_audio.md](silent_audio.md): the measured host audio path works) |
| Windows first-run UI / controller prompts | [`e79906a42b`](https://github.com/ubiquitarianism/leonida/commit/e79906a42b) (#939) | 45 files +6,799/−477 | our boot signs the user in on its own and requests no first-run page ([first_run_path.md](first_run_path.md)); world-phase polish |
| Pipeline compile workers | [`3de22a1904`](https://github.com/ubiquitarianism/leonida/commit/3de22a1904) (#1547) | 6 files +412 | rank 8: after content, or when a cold first boot hits the title watchdog |
| The rest of post-story runtime | [`ed94fbef9`](https://github.com/ubiquitarianism/leonida/commit/ed94fbef9) (#1723) | 59 files +19,790/−8,636 | per-file review only; the rows above are its cheap and relevant parts |

Already ported and therefore not repeated above: the barrier into `LAYOUT_UNDEFINED` (#1055,
[barrier_undefined.md](barrier_undefined.md)), the image memory pool and deferred backing (#1413,
[streamed_image_backing.md](streamed_image_backing.md)), queue slot 31 CPU-view mapping (#968,
[cpu_view_mapping.md](cpu_view_mapping.md)), and the advanced DirectStorage staging/pinning/unshuffle/BCPack paths
([directstorage_advanced.md](directstorage_advanced.md), [bcpack_runtime.md](bcpack_runtime.md)).

## 7. Limits

- The original rank ordering is a plausibility ranking from upstream commit sizes/messages, the fork's own docs and our
  measured pages; no new boot was run for this documentation update and no code was changed. A rank is a hypothesis with
  a named falsifier, not a measurement of the next wall.
- Our line counts were taken from the working tree; the fork's are numstat per commit and path scope, so
  a total can differ from another page that counts a different scope (for example the whole squash, or tests included).
- The memory growth of [story_load_probe.md](story_load_probe.md) is measured but not attributed. Rank 4 is placed high
  because it is the only carried mechanism aimed at that class, not because it is proven to be the cause.
- The fork's upstream paths are cited as evidence only; no upstream code, comment text or table was copied, and the
  implementation of any slice here stays ours ([upstream_port.md](upstream_port.md)).
- The original snapshot's in-flight labels for the vertex/transform feed and second compute-corpus publish are historical.
  Their landed/measured outcomes are recorded in section 1.1 ranks 6 and 5; the detailed mechanism plans above retain the
  context in which those slices were first ranked.

## 8. Attribution

The public GPL-3.0 project `ubiquitarianism/leonida` (<https://github.com/ubiquitarianism/leonida>, license
<https://github.com/ubiquitarianism/leonida/blob/main/LICENSE>) is cited above by commit and path for the behaviour,
file sizes and the statements in its own docs/linux_proton.md, d3d12_vulkan.md and docs/directstorage.md
that this page compares with. No code from that project is copied here or by the slices this page proposes; the
upstream project remains the copyright of its contributors and its license continues to apply to its own tree.
