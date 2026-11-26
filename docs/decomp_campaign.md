# Decompilation campaign: the map behind the first visible frame

**Status: static evidence only.** Every claim in the campaign pages comes from the retail image's code and data
(RVAs, structures, call graphs, counts) or from a named prior measurement; no boot was run for these pages, no code
was copied from another project, and no game data — strings, assets, decrypted content — is quoted anywhere in the
repository. The raw working material (Ghidra projects, decompile dumps, scanner output) lives outside the repository
under `D:\leonida-work\`, and is not committed.

The campaign exists because the picture programme had been advancing one crash at a time. After stage 3 became
reachable (see [vertex_buffer_va.md](vertex_buffer_va.md)), the open questions were no longer "what is the next
fault" but "which title path must run, which runtime answers does it consume, and where do our shims still stand in".
Six slices answer those questions for the stages, the first-run/UI/movie producer, the renderer graph, the job and
threading contract, the allocator, and the complete import surface.

## The six slices

| page | what it maps | strongest measured findings |
|---|---|---|
| [decomp_stage.md](decomp_stage.md) | Stage/session state words, writers, transition conditions, the phase protocol and the stage-3 job graph | The stage machine is `current/requested/pending` with a phase byte; stage 2→3 needs the stage-2 boot sequence to return; stage 3→4 has two conditions; the session word has a single 2-instruction writer that earlier pages called unwritten; the session-3 poll gates on a loading/streaming completion record. |
| [decomp_first_ui.md](decomp_first_ui.md) | Boot-media gate, first-run screens, UI and movie paths, and the runtime services each asks for | The boot-media gate is a byte latched by the module-init constructor; the AC handshake chain that starts the media/UI present pass is identified down to the event type; the movie module is bound but nothing calls its API in our measured boot; accessibility is the first screen-owner call the boot reaches. |
| [decomp_renderer.md](decomp_renderer.md) | Command decoder and node graph, the type-5 LUT family, the upscaler kind branch, compose and present | The decoder has six forms that emit a shared 0x48-byte node; the type-5 LUT family has exactly three producers and the missing class-0xa stride case is a title data/code contradiction; the upscaler kind is selected by a console-type registry read (kind 7 only for console type 6, kind 6 otherwise); the present path builds one per-plane block and calls the console present slot. |
| [decomp_jobs.md](decomp_jobs.md) | Job creator/executor/flush/submit-wait contract, pools, frame tick, heartbeat and watchdog | The renderer job is synchronous on the submitting thread; the job block is 0xa0 bytes and returns to its caller for destruction; the heartbeat is per frame with a 30-second hang budget; the title distinguishes inline and queued jobs by owner thread id. |
| [decomp_memory.md](decomp_memory.md) | Title heap, inner allocator, the XMem arena contract, graphics VA/page model | The title heap is anchored a constant offset from the one XMem pool answer; the arena is a committed private pool, not a placeholder import; a zero-byte allocation degenerates into a real zero-size chunk whose bytes are also free-chunk metadata, so the measured 0x1000-byte copy corrupts allocator metadata by construction — one producer for both crash families. |
| [decomp_imports.md](decomp_imports.md) | Every imported service by subsystem, call-site counts and phase, our shim slot/status, and the stage-3/first-run gate list | 587 imports across 68 DLLs and about 39,000 call sites; the stage-3 renderer/loader import surface is fully answered; the remaining stand-ins on the named paths are speech, offline user/sign-in and saves, input, audio, and the movie trigger — each with a measured refutation for the ones already ruled out. |

## What the campaign changed

1. **The class-0xa crash is title-side, twice over.** The class is a compile-time constant, the stride switch has no
   case for it, the allocator hands back a zero-size chunk, and the copy length is a literal. No runtime answer of
   ours participates; no shim fix is implied. The only runtime lever left is the renderer branch that chooses to build
   that command — and the campaign shows the branch is not selected by any answer we serve on PC.
2. **The black frame is a chronology problem, not a compose problem.** The compose draw's console table is fed (the
   earlier "empty matrix" was a publication-off arm), its control rasterises with culling off, and a correctly
   rasterising compose of a zero source is black. What is missing is the content producer that runs after stage 3
   stays alive.
3. **The first visible frame is a UI/movie frame.** Every peer port's first visible frame is a first-run/UI/menu or
   movie frame, not the 3D content pass; our boot never reaches the producer that draws one.
4. **The remaining stand-ins are enumerable.** The import map lists every stand-in or refusal on the stage-3,
   first-run, UI and movie paths, so the next implementation slices can be chosen from evidence.

## Consolidated roadmap (from the six rankings)

Frontier-first; each row names the page that justifies it and the files it would touch.

| # | slice | why it is next | files | falsifier |
|---|---|---|---|---|
| 1 | Complete the session-3 loading/streaming record | The session-3 poll gates on it; stage 4 needs the boot sequence to return | `port/shims/dstorage.cpp`, `port/shims/file_system.cpp` | a load-in-flight count reaching zero with the session word moving |
| 2 | Open the boot-media AC chain | It starts the media/UI present pass the peers show first | `port/shims/xmem.cpp`, `port/shims/library_loader.cpp`, `port/src/main.cpp` | the media present body executing and a non-black capture |
| 3 | Per-record rasterizer decode | The content window's own draws still depend on a stand-in cull state | `port/src/graphics_pso.*`, `port/src/graphics_record.*`, `port/src/vulkan_replay.h` | the recorded raster state matching the measured console fields |
| 4 | Speech surface + offline sign-in/save + input defaults | Cheapest remaining stand-ins on the first-run path; each has a measured refutation already | `port/shims/xgameruntime*.cpp`, `port/shims/gameinput.cpp` | more screen-owner calls; any present change |
| 5 | Movie trigger and frame-buffer mapping | The movie module is bound but never entered; peers show video first | `port/shims/*bink*`, D3D12 layer | the movie API executing, or player state 9 with no draw |
| 6 | Deferred: type-5 LUT consumer semantics and console-type branch | Both need a new probe; both are excluded from the default path until measured | `port/src/vulkan_replay.h`, `port/src/main.cpp` | a controlled probe naming the LUT's consumer draw |

There is also a **do-not-repeat list**: the campaign retires package readiness, networking hints, silent speech as
sole gate, XGameUi/XLauncher dialogs, XAppCapture, xg slot 10 tiling, DirectStorage BCPack shapes, store/save
containers, `XMemIsToolsMemAvailable`, and the renderer/memory hypotheses already measured negative in the earlier
pages.

## Measured follow-ups

The roadmap rows were taken up immediately after the campaign; this table records what the measurements retired and what
they renamed, so the page cannot be read as a list of open hypotheses that are already settled.

| roadmap row | measured outcome | evidence |
|---|---|---|
| 1. Session-3 loading/streaming record | **Negative for the storage path, writer named.** The storage shims delivered everything (both loader bursts done, status arrays complete, zero refusals, nothing in flight), and a per-thread write watch found the only writer of the completion dword is the loading record's own reset, storing zero. The completing producer had not been dispatched before the title's own stop. | decomp_stage.md sections 10-11 |
| 2. Boot-media AC chain | **Retired: the producer is dead code in this image.** The present body is ticked but exits on its two keys; the media module is armed; the only producer of the type-0x24 set event has no call, jump or data pointer anywhere in the image. A clean 300 s boot confirms the keys stay zero and every capture is black. | decomp_first_ui.md section 11 |
| 3. Per-record rasterizer decode | **Still open.** The content window's compose draw rasterises under a control when culling is off, and the source image is zero; the per-record state decode is still the next measured step for real content pixels. | vertex_buffer_va.md section 20 |
| 4. Speech, offline user/save and input defaults | **Landed and measured: consumed, not sufficient.** The accessibility screen owner executes with the new defaults, but no present follows and the machine stays at stage 2 in a clean 300 s boot. | decomp_first_ui.md section 12 |
| 5. Movie trigger and frame-buffer mapping | **Still open.** The movie module is bound and armed, its API is never entered in the measured window; the trigger path is the next measurement. | decomp_first_ui.md sections 3-4 |
| 6. Deferred LUT/console-type probes | **Console-type lever measured and retired.** A real Series X takes the same kind-6 own-upscaler as PC and reproduces the same class-0xa sequence; only the Series-S FSR branch (type 6) differs, and its shape now survives but has not yet coincided with stage 3. The LUT class is compile-time and no console type selects it. **Partially retired/renamed.** The type-5 LUT family's missing stride case is title-side; the remaining answer-shaped question is whether the console-type branch (kind 7 FSR) would skip the LUT command, which needs a registry-level console-type answer rather than a new renderer probe. | decomp_renderer.md sections 5-6, decomp_campaign.md roadmap |

| 7. Series-S FSR branch survival | **Shape survives, coincidence blocked by a GPU-side loss.** The R9G9B9E5 output shape is created (invalid-argument refusal gone); every post-reboot Series-S arm then loses the device on a driver event-153 fault, which also hits a control arm with no Series-S shape, so the loss is not the shape. No arm reached stage 3 with the Series-S answer, so the class-0xa question stays open; FSR programs remain 0 of 29. The fault report API itself crashes this driver; validation is too slow for the window. | decomp_renderer.md sections 12-13 |

The dominant remaining wall for the picture is unchanged: stage 3 dies a few seconds after entry on the title-side
class-0xa zero-byte overrun, so the first-run producers that follow stage 3 do not run. The campaign's own evidence says
there is no runtime answer in that overrun; the console-type branch is the only measured lever that might avoid the
command entirely, which is why it is the next experiment rather than another decode.

## Limits

- Phase attribution in the import map is inferred for part of the surface (console DLLs 85/97 sites, GDK 200/358);
  rows are marked measured or inferred.
- The Ghidra analysis is read-only and pinned to the extracted image; addresses are image-relative and can move with
  a different build.
- Static hypotheses are not fixes: every roadmap row still needs its own measured slice and gate.
- The campaign deliberately contains no strings, asset data, keys or decrypted content.

## Tooling

The six slices reused one analysed Ghidra project read-only, the existing decompiler scripts under
`D:\leonida-work\rea-tooling\ghidra_scripts` and `D:\leonida-work\job109\ghidra_scripts`, the repository's own
`tooling/import_call_sites.py` and `tooling/gdk_runtime_calls.py`, and static PE helpers (call-site scans, RIP-relative
xref scans, vtable dumps). The project files and dumps stay outside the repository.
