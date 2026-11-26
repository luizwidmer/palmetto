# The peer ports' movie path: Bink 2 decode, the plane textures and the present

**Status: read-only peer audit, documentation only. No boot was run, no code was changed, and every page named here is
quoted, not re-measured.** The five peer ports were re-cloned and read at the commits in section 1 to answer one
question: how does a port get a movie from the title's `BinkOpenWithOptions` to a pixel on screen, and which part of
that is missing in this tree? The answer is narrower than the earlier parity audit expected. Every peer runs the
title's own Bink 2 DLL, lets the title decode into its own CPU frame buffers, and has no movie trigger, no Bink HLE and
no fake decoder: the title's own state machine opens, decodes and draws the movie. What the peers do carry on the
graphics side is the part this tree does not: a per-submission refresh of a texture whose bytes the title writes in its
own memory (XCL's `title_textures`, VIctory's and victory-fork's texture upload path, Five Star's texture refresh
worker), and a present compositor for a second display plane (VIctory, PercyScarlett, XCL). Our chain reaches the
player's playing state `0x9` with a non-null handle and `buffers 0`, but frame-buffer registration `0x442F760` and
per-frame decode `0x2EF0E0` never run because the draw that issues them is never issued: the title's frame thread sits
in the session-3 loading poll ([decomp_first_ui.md](decomp_first_ui.md) sections 17-18). No peer port makes that draw
issue; their titles reach the movie because their loading and first-run flow completes. The ranked plan in section 4
therefore puts the movie-side instruments that can decide the draw question first, and the two real movie-specific
graphics ports (title-memory plane-texture refresh, extra-plane composition) behind them.

## 1. What was read and with what

One question per peer, answered from that peer's own code and documentation only:

- how the Bink decoder is loaded and called,
- where the decoded frames go and what keeps the GPU's copy current,
- how the movie reaches the display (texture, overlay plane, present),
- how the title's movie request is triggered and serviced,
- and which runtime surfaces the peer implements on that path.

The peer clones are working evidence outside the repository (`D:\leonida-work\peers`); nothing is copied from them.
The refresh this audit reads is newer than the one the previous parity audit used, and the differences matter where a
file was named in that audit: the old commits are listed beside the new ones below.

| Peer | Repository and licence | Read at (HEAD) | Previous audit read | Material read for this page |
|---|---|---|---|---|
| Five Star Recomp | <https://github.com/ThatRecompDev/five-star-recomp>, GPL-3.0-or-later | `45071d5216a424218ff8743ede1c3eec30214365` | `0bfa755` | README; middleware, gpu-resources and d3d12x-shim docs; status.json/manual.json; BinkRecipes.cs; the texture refresh worker and registry; the XMem reservation pairing; the present plane reader |
| VIctory | <https://github.com/Marvisak/VIctory>, GPL-3.0-or-later | `63bcca43c6a00a758510398fa43096ce7e4f7df0` | `13f91ca` | README; the texture upload module and its docs; the write watch; the host present compositor; the queue plane struct; the mfplat resolver module; the absent-title-DLL check |
| victory-fork | <https://github.com/fxceless/victory-fork>, GPL-3.0-or-later | `52ac4ec0f8a03f103c146e60228e53612759d435` | `9c184b3` | README (current night section); the texture upload module; the write watch |
| PercyScarlett | <https://github.com/rpatty-dev/PercyScarlett>, GPL-2.0-only | `e44c465e47ab5b29ca87777cd00875e2ae0d8c0d` | `e44c465` | README; CHANGELOG; TheBig6.md; the overlay-plane blend in its GPU host and queue; the media resolver; the inherited runtime's texture path |
| XCL | <https://github.com/xcl-dev/xcl>, GPL-2.0-only | `8b957bec6f82495aec940944c6bf47faa02db590` | `6d12f58` | README; HANDOFF; the Bink module; the GPU-view reservation mirror; the title-texture upload/watch module; the present compositor; the import resolver |

Our side of the comparison is cited from this tree's measured pages only: [decomp_first_ui.md](decomp_first_ui.md)
sections 14-18, [movie_path.md](movie_path.md), [decomp_imports.md](decomp_imports.md), [display_path.md](display_path.md),
[surface_population.md](surface_population.md), [texture_import.md](texture_import.md), [page_content.md](page_content.md),
[card_copies.md](card_copies.md), [streamed_image_backing.md](streamed_image_backing.md), [texture_layout.md](texture_layout.md),
[present_wiring.md](present_wiring.md), [d3d12_vulkan.md](d3d12_vulkan.md) and [movies.md](movies.md). The external
parity evidence is the working file D:\leonida-work\peer_parity.md, outside the repository (the condensed copy is
[cleanroom_peers.md](cleanroom_peers.md)). Limits and the no-copy rule are in sections 6 and 7.

## 2. The peers' evidence

### 2.1 One row per peer

The cells are this audit's reading of the peer material, not a specification. "Movie request" means how the title's own
request to play a movie is reached and serviced; no peer adds a trigger of its own.

| Peer | Decoder load and call | Where the decoded frames go | Overlay, texture and present path | The title's movie request | Runtime surfaces implemented |
|---|---|---|---|---|---|
| Five Star Recomp `45071d5` | The shipped `Bink2Scarlett.dll` runs under its loader with every import resolved: two `xmem.dll` allocators to its xmem shim, `CreateFile2` and `AddVectoredExceptionHandler` to loader shims, five VCRUNTIME140 functions to the shipped CRT, the rest to System32 (XAudio2_9 ordinal 1 is `XAudio2Create`); no `d3d12_xs` or `xg_xs` import. Its middleware doc records CPU decode into frame buffers the game registers, the game drawing the planes itself, the GPU-assisted export unused, `BinkSetSoundSystem` given the game's own open function, files through `BinkSetOSFileCallbacks` and memory through `BinkSetMemory`. A call-recipe layer checks `BinkUtilCPUs`/`BinkGetError`/`RADTimerRead`/`BinkUtilMalloc` against the disassembly; recommendation: run the shipped DLL, no PC Bink DLL and no new shims | Title CPU memory: the game's own YUV display shaders sample the planes. The D3D12.X shim keeps a desktop texture per console GPU address and copies the memory into it (slot 133 is "a buffer at a game address, or a texture registered by address"). The 6 GiB streaming range is reserved twice, a CPU-readable range and a `PAGE_NOACCESS | PAGE_GRAPHICS_NOACCESS` GPU-only range paired by `XMemReservation.CpuView`, and address lookups go through the pair. A refresh worker hashes each page against the uploaded page hashes and detiles only the subresources whose pages changed; `MarkWritten` is fired by a DirectStorage write or a page mapping. Its own comment: "a video frame reaches the GPU at most a frame later", off the submitting thread | One plane only: the present plane reader takes `Resources[0]` and the presenter copies or scales it into the back buffer and presents with vsync; there is no second-plane/overlay compositor. The movie is therefore the game's own draw into plane 0 | Title-side. The README status says "First image: the intro video" is done, and the loader/input programme answers the first-run screens so the title reaches the movie; no Bink trigger, no fake player | Full GDK runtime (task queues, user/store), D3D12.X shim on desktop D3D12, XMem physical-page store, texture registry and refresh worker, XG tiling/layout writers, present |
| VIctory `63bcca4` | No Bink HLE: the title's own `Bink2Scarlett.dll` is loaded by the loader (the `run` command checks it among the package's title DLLs), and Bink's imports reach its shims: `xmem.dll` for memory, `CreateFile2` (its files module says "Used by Bink"), host runtime/CRT elsewhere. The media module is an mfplat source-resolver proxy that keeps URLs local; it is not on the Bink path | Title memory; the GPU module's texture module is explicit: "Texture contents the title writes itself (decision 0007): streamed pages, layout-library copies, movie frames, glyph atlases. Such a texture lives in console memory in its tiled layout; before a submission samples its host copy, the memory is detiled into a staging buffer when its content changed, and the staging buffer is copied into the host texture. 'Changed' means a page of the memory was changed since the last detiling (watch, which follows writes through every mapping of the same physical pages)". The shortcut is stated too: a change detiles the whole texture, not only the written pages; memory it cannot watch is hashed at every sampling submission | Multi-plane PresentX: the queue's plane struct keeps token, resource count and resources, and the host has a built-in fullscreen-triangle compositor with an opaque pipe state for plane 0 and an overlay blend state for the later planes, "e.g. Series S presenting a 1080p scene plane with a 4K UI overlay plane"; up to eight planes, swapchain sized to the largest. A bindless test comment notes the title "rewrites both each time, as Bink rewrites its planes every frame" | Title-side; README: "Intro video with sound: works", the intro cutscene plays. No movie trigger or Bink call layer | Loader, GDK services (user, saves, audio, input), D3D12.X replayed on host D3D12 with guarded page write tracking, title-texture upload/caching, present compositor, shader recompiler |
| victory-fork `52ac4ec` | The same runtime lineage: the title's Bink DLL under the loader, the same shim imports. No Bink-specific code | The same texture module, with one difference named in its doc: "changed" is a page changed in write-watched memory; in physical-page views, which cannot be watched, a whole-memory content hash at every submission that samples it. The README's GPU: menus row is the same mechanism ("only changed title memory is copied") | The same inherited multi-plane present compositor | Title-side; README: intro video with sound works, "Opening cutscene: plays from start to finish, in order, lit"; its measured cutscene runs are the loudest evidence that the title's own draw path can present the movie | As VIctory, plus the fork's measured memory/copy work (`VICTORY_TEXTURE_BACK`, card buffers) |
| PercyScarlett `e44c465` | Its runtime is the victory/victory-fork lineage; `Bink2Scarlett.dll` is loaded and checked at start (TheBig6.md lists it among the package DLLs that map and run: 20 imports, 74 exports, the xmem pair and XAudio2 ordinal 1). No Bink HLE | Inherits the victory texture path (one host texture per texture memory, uploaded from the title's pages, re-uploaded when the tracked pages changed) | The explicit Series S overlay: its queue takes the second plane of `PresentX` ("Series S: an overlay plane (interface, movies) over the game's") and its host blends it: "The console's display hardware lays its overlay plane (interface and movies, 3840x2160 on a Series S) over the game plane; this pipeline does that on the host: the overlay, premultiplied, blended onto the back buffer" (fullscreen triangle, sample, premultiplied alpha over the back buffer). The CHANGELOG names it as its own change: "Series S overlay plane blended over the game plane, with the full console plane description" | Title-side; README: "Menus, Bink2 video, save data and world streaming work". No movie trigger | The inherited runtime plus its overlay blend and Series S profile work |
| XCL `8b957be` | The title's own DLLs are mapped by its PE loader (`Bink2Scarlett.dll` maps and its `DllMain` runs in its console-DLL end-to-end test), and a real `Bink2Scarlett.dll` decodes a movie from the package (`bink2_console_dll_decodes_a_game_movie`: an extracted `.bk2`, `BinkOpen` then `BinkDoFrame`/`BinkCopyToBuffer(BINKSURFACE32)`/`BinkNextFrame`; 1280x720, 60 frames, correct image). Its only Bink-specific code is an opt-in skipper: `XCL_SKIP_BINK` wraps the playback exports at import/`GetProcAddress` time, records the measured `BINK` header layout (`width` +0x00, `height` +0x04, `frames` +0x08, `frame_num` +0x0C, `last_frame_num` +0x10, `frame_rate` +0x14, `frame_rate_div` +0x18, `read_error` +0x1C, `open_flags` +0x20; `BINKWILLLOOP` 0x80) and fast-advances a non-looping movie to its last frame; open/close, buffer sizing, `BinkGetFrameBuffersInfo`, `BinkRegisterFrameBuffers` and the memory hooks still go to the real DLL | Title memory pages, then a placed texture the video shader samples. Its GPU module documents the console arrangement exactly: the title reserves a range of 64 KiB pages twice, a CPU-readable reservation and a `PAGE_NOACCESS | PAGE_GRAPHICS_NOACCESS` GPU-view reservation; pages are mapped only into the first, resources are placed at the same offset in the second, and "its Bink video planes: the decoder writes the first view, the video shader samples textures in the second". On the console the second reservation is a GPU-only view of the first; on the host it was separate empty memory, and "the video was its colour of zero luma and chroma, a green screen" until the mirror paired the two and the texture upload read the CPU view. `title_textures` uploads a placed texture at creation and again before the next `ExecuteCommandLists` when the CPU wrote any of its pages (write-watched), streamed pages included; it measured the retail title's video planes "up again for every submit, ~25 MB a presented frame where ~3 MB had changed" and added a content-hash skip | A general `PresentX` compositor: it reads up to eight `(resource, source rect)` plane pairs and draws each plane with a fullscreen triangle into the swapchain, opaque for plane 0 and `ONE/INV_SRC_ALPHA` blend for the rest; a change in plane count is logged as "the console's display composes them - a UI plane over the game's" | Title-side: "Titles that ship `Bink2Scarlett.dll` ... play splash/intro movies at real-time frame cadence through `BinkOpen*`/`BinkWait`/`BinkNextFrame`/`BinkDoFrame*`"; the title polls `FrameNum`/`LastFrameNum` for the end of the movie. No trigger, no fake decoder | PE loader for game DLLs, HLE modules (d3d12x, kernelx, xg), title-texture upload with write watch, video/sparse memory, present, shader recompiler |

### 2.2 What the peers agree on

1. **The decoder is the title's own DLL, and no peer shims it.** Every peer maps the package's Bink 2 DLL and serves its
   few non-Windows imports (the `xmem.dll` pair mainly, plus `CreateFile2`); the title installs its own memory and file
   callbacks (`BinkSetMemory`, `BinkSetOSFileCallbacks`) and handles the file itself. XCL's skipper is the only
   Bink-specific code in any tree, and it is an instrument that still forwards buffer sizing and registration.
2. **The decoded frames are title CPU memory, and the game's own shaders draw them.** Decoding is on the CPU into frame
   buffers the game registers; the game draws the planes itself (Five Star's middleware doc says so outright, XCL's
   green-screen note shows the sampling texture placed over the page range the decoder writes). No peer copies the
   frames into a port-owned surface and no peer runs a video compositor of its own for the movie content.
3. **The host keeps a texture per console address and refreshes changed pages before the sampling submission.** This is
   the one movie-specific graphics mechanism all three graphics-capable peers implement, each in its own structure:
   XCL uploads a placed texture at creation and again per submission when its pages were written (write watch or a
   skipped re-upload when a content hash is unchanged); VIctory and victory-fork detile a title-written texture into a
   staging buffer when the memory changed since the last upload, and hash memory they cannot watch; Five Star keeps a
   registry of desktop textures by address, marks pages written, hashes and detiles only the affected subresources before
   a submission. The peers also agree on the cost shape: a movie rewrites its planes every frame, so "uploaded once at
   creation" is not enough.
4. **The console's second display plane is a real movie surface and the peers compose it.** VIctory has an opaque plane
   0 plus an overlay blend for the rest; PercyScarlett names it for what it is (Series S interface and movies over the
   game plane, premultiplied); XCL composes planes 1 and up the same way. Five Star takes plane 0 only, which is enough
   on a profile where the movie is drawn into the game frame.
5. **No peer ports the movie request.** The trigger, the state machine, the metadata/playback opens, the frame-buffer
   registration and the draw are all title code. The peers' visible movies are a consequence of their title's own
   loading/first-run flow running to the point where it asks for the movie; XCL states the title drives
   `BinkOpen*`/`BinkDoFrame*` itself and only intercepts it when asked to skip.

### 2.3 What the peers do not have

- **No movie trigger and no decoder stand-in.** Nothing in the five trees fabricates a `BinkOpenWithOptions` result, a
  frame buffer, a registration or a decode call for the title.
- **No port-side pixel conversion.** The YUV-to-RGB display shaders are the game's own pixel shaders, translated like
  any other program (XCL and Five Star both say it in those words); the peers do not add a video shader.
- **No movie-specific present path.** The movie reaches the same `PresentX` (or the same game-plane present) as the rest
  of the frame; the only movie-specific present work is the second plane's composition, which is the console's general
  display-plane feature, not a video path.
- **No audio path specific to video** beyond letting the title's Bink DLL open its own sound system: Five Star records
  `BinkSetSoundSystem` receiving the game's own open function, and the peers let the DLL's XAudio2 import bind to the
  host (XCL's Bink DLL imports XAudio2_9 ordinal 1).

## 3. Comparison with our chain

### 3.1 Link by link

Our measured state is quoted from this tree's pages; the "difference" column is the audit's reading, not a new
measurement.

| Link | Peer position | Our measured state | Difference |
|---|---|---|---|
| Decoder module and imports | Title DLL under the loader, imports resolved ([movie_path.md](movie_path.md) section 2.2 pattern) | `Bink2Scarlett.dll` maps at +0.86/+0.92 s and its `DllMain` returns 1; 8 shim / 52 host / 0 module / 0 unimplemented imports; the library's async threads are in the stage-3 census ([decomp_first_ui.md](decomp_first_ui.md) section 14.4, [movie_path.md](movie_path.md) section 2.2) | **Same.** The decoder layer is not the gap |
| Bink memory/file callbacks | Title installs them; no shim call ([movie_path.md](movie_path.md)) | `BinkSetMemory`/`BinkSetOSFileCallbacks` callbacks are title code; `PALMETTO_XMEM_CALLERS`/`FILE_TRACE` never see the decoder even at state `0x9` ([decomp_first_ui.md](decomp_first_ui.md) sections 15.5, 18.2) | **Same.** The decoder allocates and opens through the title's own memory |
| Request arming | Title-side | Movie slot armed at 112-116 s (entity non-null, active 1, armed 1, media id 0x540714f3) ([decomp_first_ui.md](decomp_first_ui.md) section 17.4) | **Same** |
| Metadata/probe open | Title-side | Ready 26 s; metadata/probe open `0x442EA70` 3 hits; playback open `0x199EF20` 1 hit; `BinkOpenWithOptions` returns non-null ([decomp_first_ui.md](decomp_first_ui.md) sections 15.3, 18.4) | **Same.** The title has serviced its own request |
| Player state machine | Title-side | State byte `entity+0x1c4`: 0x3 at 112 s, 0x5 at 116 s, **0x9 from 118 s to 298 s**, handle non-null, probe 0 / open 1 / error 0 ([decomp_first_ui.md](decomp_first_ui.md) sections 15.1, 17.4, 18.4) | **Same.** The player is in the playing state the decoder gates on |
| Frame buffers | Title-side; the game registers Bink frame buffers | The state trace's `buffers 0` is the title player's `entity+0x1b8` decode-gate count, consistent with the registration never having run. The decoder's own `HBINK+0x1b8` is a different field of a different object: it is the open block allocator's answer, measured valid and non-zero on a clean arm (one track, status 0), and the only decoder fault was in a device-loss window ([decomp_first_ui.md](decomp_first_ui.md) sections 18.1-18.5) | **No layer answer is missing.** The player's count is 0 because registration has not run, not because the decoder was refused memory |
| Frame-buffer registration `0x442F760` | Title-side, called by the decode wrapper | **0 hits** in every arm; its only caller is inside the per-frame decode wrapper `0x2EF0E0` ([decomp_first_ui.md](decomp_first_ui.md) section 15.1) | **Downstream of the missing draw**, not a registration answer |
| Per-frame decode `0x2EF0E0` | Title-side | **0 hits** in the long window ([decomp_first_ui.md](decomp_first_ui.md) sections 16.4, 17.4, 18.4) | **The draw that would call it is never issued** |
| The draw itself | Title-side; peers' titles issue it because their loading/first-run flow completes | The movie display job is the first renderer job after state `0x9`; the frame thread is parked in the session-3 poll (`Sleep(33)` at `game+0x21B7070`) at every stage-3 pump sample, the loading task waits on a title semaphore, and the session-3 loading record's completion dword is never observed ([decomp_first_ui.md](decomp_first_ui.md) section 17.2-17.3) | **The one real difference, and it is title-side.** No peer port sits on it; their own loading completes |
| Plane textures and their content | Host texture per console address, refreshed when pages changed (section 2.2 item 3) | A title-placed texture's pages are imported **once**, bounded, behind `PALMETTO_TEXTURE_IMPORT` (off by default), after a zero-head gate; there is no per-submission re-import for a texture the title rewrites ([texture_import.md](texture_import.md) section 1, [streamed_image_backing.md](streamed_image_backing.md) section 4). A guard-page write tracker exists but is armed on the descriptor-heap mirrors only, not on texture pages ([card_copies.md](card_copies.md) sections 4-5) | **A real movie-specific gap.** A decoded plane rewritten every frame would keep its first imported frame |
| XG plane fill | XCL runs the console layout library; Five Star has XG layout writers | Our XG shim answers the title's texture-computer path honestly only for a linear, single-plane description; planar and multi-plane formats are refused by name, and the earlier note says the placed-texture stand-in "does not read the title's pages" ([texture_layout.md](texture_layout.md), [texture_import.md](texture_import.md) sections 2-3) | **Conditional gap:** whether the movie's three linear planes need a shape the shim refuses is not measured here |
| Present path | 1-plane copy, or 2+ plane composition; the second plane is the console's overlay | PresentX is wired end to end (window, pacer, token bridge; 33,275 presents per 300 s) and composes up to two planes, but plane 1 is blended **only** when it carries our invented `xbox_plane_flag_overlay`, and `ResourceCount != 1` is refused as unimplemented ([present_wiring.md](present_wiring.md), [d3d12_vulkan.md](d3d12_vulkan.md) section 4.5, `port/shims/xbox_present.cpp`) | **Partly carried.** The composition exists; the gate that decides whether the title's second plane is shown is a hypothesis |
| Pixels | The peers report visible movies and cutscenes | Every presenter capture and every full-target readback is black; the draw's own target is a real 4K image and is writability-proven ([decomp_first_ui.md](decomp_first_ui.md) sections 16.4, 17.5) | Consistent with the draw never issuing; not a movie pixel question yet |

### 3.2 Verdict

**The movie chain is not blocked by a missing decoder, a missing file or memory answer, or a missing video surface in
this layer.** The title's own DLL is mapped, bound and running its service threads; the title installs its own Bink
memory and file callbacks; it has serviced its own request (metadata/probe open three times, playback open once,
`BinkOpenWithOptions` non-null) and holds the playing state `0x9` for 196 s with no exception. Frame-buffer registration
and per-frame decode are title-side steps whose only caller is the per-frame decode wrapper, so their zero hits mean the
movie draw is not issued, not that a shim refused them. There is no `port/shims/*bink*` file in this tree and this audit
finds no reason to add one: no peer has one either, and a Bink shim would only be able to fabricate the title's own
states.

**The draw is gated on the title's own loading/frame path, and that is the same wall sections 17-18 measured.** The
movie's per-frame update runs on the frame thread that is parked in the session-3 poll with a loading task on a title
semaphore; the loading record's completion dword never arrives, so the frame pump that would issue the movie display
job stops. The peers' titles show movies because their loading and first-run flows complete, not because of a movie
port; none of the five trees has anything that would make our title's draw issue.

**Behind that wall, two movie-specific layer gaps are real and named.** (a) A plane texture the title rewrites every
frame would keep the one frame imported at its first host write: the import is single-shot and off by default, and no
consumer of the guard-page write tracker exists for texture pages, while all three graphics-capable peers refresh a
title-written texture before the sampling submission. (b) If the title presents the movie on the second display plane
(the Series S overlay is documented that way in PercyScarlett and VIctory), our composition of that plane is gated by an
invented flag value and `ResourceCount != 1` is refused, while the peers compose the plane. Both are invisible today
because the draw is not issued; both would have to be settled before a movie-picture claim.

### 3.3 What would make the movie draw issue

The chain that must run is: the title's per-frame movie update -> the movie display job -> the draw that calls the
decode wrapper `0x2EF0E0`, which calls `BinkDoFrameAsync*`/`BinkNextFrame` and registers the frame buffers
`0x442F760`. Our measurements put the movie update on the frame thread, which the session-3 loading poll holds. The
candidate gates, each with a falsifier that would move the attribution:

1. **The session-3 loading record's completion** (`image+0x78ACB20`, predicate `0x41E8C0`) is never consumed, so the
   loading state machine stays at session 3. Falsifier: a long post-stage-3 arm with the hardware write watch on that
   dword and the wait trace on the loading semaphore showing a signaller; if a completion arrives and the movie draw
   still does not, the draw is gated elsewhere. (This is the wall of [decomp_first_ui.md](decomp_first_ui.md) section
   17.3, not a movie question.)
2. **The movie display job is issued but skipped by a frame-ready flag.** Falsifier: execution probes on the layer
   entry `0x23495A0`, the player draw `0x2349D80` and the second per-frame advance `0x51B45B0`; a hit on the draw with
   decode at 0 separates "issued and skipped" from "never issued", and names the branch.
3. **The movie itself is upstream of the stall**: the title waits for the intro movie to finish before it completes the
   loading record. Falsifier: the Bink import-level witness plus the `XCL_SKIP_BINK` experiment (fast-advance the
   non-looping intro); if the skip moves the session word or the draw hits, the movie is a gate, not a consequence.
4. **The profile decides the movie's display path.** On the Series S profile the movie can be the console's overlay
   plane (section 2.2 item 4), which the title may issue from a different pass than the game-plane draw. Falsifier: the
   same probes under the Series S console type (`PALMETTO_CONSOLE_TYPE=6`) against the default profile, reading the
   plane count and plane words at the movie window.

## 4. The ranked port plan

The plan is ordered by what it can decide: first the instruments that can make the movie draw issue or prove what
gates it, then the two movie-specific graphics mechanisms, then the conditional surfaces. "Boot" is the boot cost of
the item's test, not of the implementation; effort is S (about a day), M (two to five days), L (a week or more),
matching [cleanroom_peers.md](cleanroom_peers.md). There is no `port/shims/*bink*` target in the plan because there is
no Bink shim to write: the decoder is the title's own DLL, its allocator and file callbacks are title code, and the one
Bink-specific thing a port can usefully own is the loader-side export wrapper of item 1.

| # | Candidate | Kind | Mechanism from (commit) | Main target files | Effort | Boot |
|---|---|---|---|---|---|---|
| 1 | Bink import-level call witness and movie fast-advance | Port + diagnostic | XCL `bink.rs` (`8b957be`) | `port/src/pe_loader.cpp`, `port/src/pe_loader.h`, `port/src/main.cpp`; tests `port/test/pe_loader_test.cpp` | S | A/B pair |
| 2 | Movie draw-path probes | Measurement | XCL frame tracing, Five Star draw probes, VIctory watch/poke (`8b957be`, `45071d5`, `63bcca4`) | `port/src/main.cpp` (existing poke-log and movie-probe machinery) | S | One long arm |
| 3 | Session-3 loading-record completion and semaphore signaller | Measurement | No peer port; the peers' completed loading is the evidence | `port/src/main.cpp` (existing record sampler, wait trace, write watch) | S/M | One long arm |
| 4 | Per-submission refresh of a title-written plane texture | Port | XCL `title_textures.rs`; VIctory/victory-fork `texture.rs`; Five Star texture refresh worker (`8b957be`, `63bcca4`, `52ac4ec`, `45071d5`) | `port/shims/d3d12_vulkan_device.cpp`, `port/src/vulkan_backend.h`, `port/shims/xmem.cpp` and `port/shims/xmem.h`; tests `port/test/d3d12_vulkan_device_test.cpp`, `port/test/xmem_test.cpp` | M | GPU test + one boot |
| 5 | GPU-view reservation pairing for placed texture VAs | Port | XCL `kernelx.rs` mirrors; Five Star `XMemReservation.CpuView` pairing (`8b957be`, `45071d5`) | `port/shims/xmem.cpp`, `port/shims/d3d12_vulkan_device.cpp`; test `port/test/xmem_test.cpp` | M | Unit + one boot |
| 6 | Second-plane composition measured, invented flag gate removed | Port | VIctory host compositor; PercyScarlett overlay blend; XCL present compositor (`63bcca4`, `e44c465`, `8b957be`) | `port/shims/xbox_present.cpp`, `port/shims/d3d12_xs_device.cpp` (the sampled plane dump), `port/src/vulkan_present.h`, `port/src/vulkan_present_compose.h`; test `port/test/xbox_present_test.cpp` | S | GPU test + one boot |
| 7 | XG layout and upload coverage for the movie plane shapes | Port | XCL `hle::xg` + `title_textures`; Five Star XG layout writers (`8b957be`, `45071d5`) | `port/shims/xg_xs.cpp`, `port/test/xs_proxy_mock_test.cpp`; measurement via `PALMETTO_XG_TRACE`/`PALMETTO_XG_DUMP` | S/M | Unit + one boot |
| 8 | Bink audio witness on the same wrapper | Diagnostic | Five Star middleware (`BinkSetSoundSystem`), XCL Bink imports; peers' movie sound claims (`45071d5`, `8b957be`) | Same wrapper as item 1; no new file | S | One arm at the movie window |

### Item 1: Bink import-level call witness and movie fast-advance (S)

- **Mechanism.** XCL wraps the title DLL's playback exports where its resolver hands the address to the title
  (`resolver.rs`, `bink.rs`), keeps the measured `BINK` header layout, and when asked fast-advances a non-looping movie
  by writing `frame_num` and `last_frame_num` to `frames`; `open`/`close`, `BinkGetFrameBuffersInfo`,
  `BinkRegisterFrameBuffers` and the memory hooks still reach the real DLL. Our loader binds every import of the title
  and its bundled modules in `bind_imports`, so the same instrument belongs there: wrappers around the 23 Bink imports
  that forward by default and log the call, the `BINK` header fields (width, height, frames, frame number, flags) and the
  call count per export; a proposed opt-in switch (name it at implementation time) turns on fast-advance.
- **Why first.** It is the cheapest witness of what the decoder is actually asked to do after state `0x9`, independent
  of the wrapper probes that already see the metadata/playback steps, and it is the only way to run the "the movie is
  itself the gate" experiment from section 3.3 item 3 without guessing at title addresses.
- **Our target files.** `port/src/pe_loader.cpp` (`bind_imports` and the module's import record), `port/src/pe_loader.h`
  (a wrapper table/registry), `port/src/main.cpp` (switch logging). A proposed include-only helper under `port/src`
  holds the Bink header layout and the pure advance rule so it is unit-tested without the package.
- **Test/falsifier.** Unit: a synthetic export table resolves to the wrapper, the wrapper forwards to the same address
  with the same arguments, and the advance rule moves only the documented fields and only for a non-looping handle.
  Boot A/B (off/on): off gives per-export counts and the header values at the movie window; on with fast-advance gives
  the same counters plus a state/session comparison. Falsifier: no wrapper call in a 300 s arm while the player sits at
  `0x9` (the title does not call the exports in that window at all), or the advance happens and the session word and
  the draw stay where they were (the movie is a consequence, not the gate).
- **Boot need.** One A/B pair of light arms. No game data: counts and header fields only.

### Item 2: Movie draw-path probes (S)

- **Mechanism.** The peers all instrument the draw frame rather than the decoder (XCL's frame trace and title-texture
  notes, Five Star's draw probes, VIctory's watch/poke). Our tree already has the machinery: the `PALMETTO_MOVIE_PROBE`
  poke groups (`player`, `trigger`, `frontend`, `player2`). A fifth group adds the movie's own draw path: layer entry
  `0x23495A0`, player draw `0x2349D80`, the second per-frame advance `0x51B45B0`, and the decode wrapper `0x2EF0E0`
  as the control. That answers section 3.3 item 2 directly.
- **Our target files.** `port/src/main.cpp` only (the probe group and its log lines).
- **Test/falsifier.** One long arm past stage 3 with `PALMETTO_MOVIE_PROBE` on and the state trace: draw at 0 and decode
  at 0 means never issued (title-side gate); draw > 0 and decode 0 means issued and skipped by a flag (record which
  register/flag); decode > 0 means the registration question moves to item 4. Falsifier for the whole movie inquiry: a
  boot where decode fires and the presented capture is still black - then the gap is the plane content or the present,
  not the draw.
- **Boot need.** One long arm (300 s, light). This is a measurement, not a port.

### Item 3: Session-3 loading-record completion and semaphore signaller (S/M)

- **Mechanism.** No peer port: this is the wall the peers do not have. Their titles complete loading and therefore
  issue the movie. Our measured instruments already exist: the record sampler (`PALMETTO_DS_RECORD`), the wait trace
  (`PALMETTO_WAIT_TRACE`), the hardware completion watch on the record's completion dword, and the state trace. The gap
  in the evidence is a single clean post-stage-3 arm carrying the direct dword watch as the only heavy probe (section
  17.8's named next arm).
- **Our target files.** `port/src/main.cpp` only (switches and one probe at the loading-task semaphore's signaller
  sites); no shim answer is expected, so nothing else changes.
- **Test/falsifier.** A long arm in which the completion dword is watched directly and the loading semaphore's signaller
  is named. Falsifier: the dword completes and the frame thread leaves the poll but the draw still does not fire (then
  the gate is in the movie component, not the loading record); or the loading tick keeps advancing with no completion
  in a clean window (reproduces section 17.3 and keeps this a title-side wall).
- **Boot need.** One long arm. This decides whether the movie remains worth chasing before items 4-8 are built.

### Item 4: Per-submission refresh of a title-written plane texture (M)

- **Mechanism.** The one movie-specific graphics mechanism the peers carry in three variants (section 2.2 item 3).
  XCL's shape is the closest to ours: a PC texture holds a detiled copy of a placed texture's pages, uploaded at
  creation and again before the next `ExecuteCommandLists` when the CPU wrote any of its pages; a content-hash skip
  avoids re-uploading an unchanged texture (its measured movie case: ~25 MB re-uploaded per presented frame where ~3 MB
  changed). VIctory's and victory-fork's shape is a per-submission detile of the changed texture; Five Star's is a page
  hash per subresource on a worker thread. Our machinery already exists in pieces: the single-shot
  `import_placed_pages` (a bounded import after the zero-head gate, behind `PALMETTO_TEXTURE_IMPORT`), the guard-page
  write tracker in `port/shims/xmem.cpp` (what `PALMETTO_CARD_COPY_WATCH` arms for the descriptor mirrors), and the
  per-submission `sync_console_heaps_measured` hook. The port is to arm the tracker on a sampled title-placed texture's
  pages, poll it before the submission that samples the texture, and re-import only on change; the import path and its
  size bound are already measured.
- **Our target files.** `port/shims/d3d12_vulkan_device.cpp` (the placed-texture import and its per-submission call
  site), `port/src/vulkan_backend.h` (the submission hook next to the console-heap sync), `port/shims/xmem.cpp` and
  `port/shims/xmem.h` (arm/poll reuse, no new mechanism), tests `port/test/d3d12_vulkan_device_test.cpp` and
  `port/test/xmem_test.cpp`.
- **Test/falsifier.** GPU test without a boot: create a title-placed texture through the slot-133 path, write a known
  pattern into its pages, take a submission, read back the image; write a second pattern, take another submission, and
  require the second readback to be the second pattern (and an unchanged third submission to skip the copy, if the hash
  skip is carried). Boot: one arm with the switch on; verify the import counter grows with the movie's frames while a
  draw runs. Falsifier: the draw fires and the sampled plane stays at its first imported frame (refresh unchanged), or
  the refresh costs a submission and changes the presented pixels for the worse (then the cost bound is wrong, not the
  mechanism).
- **Boot need.** A GPU test plus one boot. No game data: patterns and counts only.

### Item 5: GPU-view reservation pairing for placed texture VAs (M)

- **Mechanism.** XCL pairs the title's GPU-only 64 KiB-page reservation with the CPU-readable reservation made just
  before it, and any placed resource at a GPU-view address resolves its bytes through the pair; Five Star does the same
  with `XMemReservation.CpuView`/`XMemPhysicalMemory` and its texture registry. Our tree maps pages into GPU-only
  windows at the CPU (the `view_protect` path makes them readable) but does not pair reservations, and the placed
  texture import reads the placement address as if it were the title's page; if the title places its plane textures in
  the GPU-only range while mapping the pages only into the CPU range, our import reads nothing (or the wrong memory).
- **Our target files.** `port/shims/xmem.cpp` (reservation bookkeeping in the `XMemVirtualAlloc` path; the physical-page
  windows already have the shape), `port/shims/d3d12_vulkan_device.cpp` (resolve a placed texture's source through the
  pair before the import); test `port/test/xmem_test.cpp` and the GPU test of item 4.
- **Test/falsifier.** Unit: reserve two same-size windows (CPU then GPU-only), map a page only into the CPU window,
  place a texture at the GPU-view address, write through the CPU view, and require the import to read those bytes.
  Boot: the placed-texture imports at the movie window resolve (import lines name a source inside the paired CPU range).
  Falsifier: the title never places a texture in a GPU-view reservation (then the pairing is a no-op and is recorded as
  readiness, not a fix; our measured slot-133 placement addresses are in the title's own xmem region).
- **Boot need.** Unit test plus one boot. Conditional on item 4.

### Item 6: Second-plane composition, and the invented flag gate (S)

- **Mechanism.** VIctory composites plane 0 opaque and every later plane with an overlay blend; PercyScarlett blends the
  Series S overlay premultiplied over the game plane; XCL draws every plane with an opaque pass for the first and a
  `ONE/INV_SRC_ALPHA` blend for the rest. Our compositor exists (`present_planes`, the compute pass, up to two planes,
  HDR/SDR/alpha routes) but a second plane is shown only when it carries our own `xbox_plane_flag_overlay`, and a plane
  with `ResourceCount != 1` is refused. The port is to measure the title's own plane words at the movie window (extend
  the sampled `PresentX` dump to plane 1 and the flags word) and to decide the gate from that measurement, with the
  peers' "compose the extra plane" as the default if the fields cannot be decoded.
- **Our target files.** `port/shims/xbox_present.cpp` (plane read and gate), `port/shims/d3d12_xs_device.cpp` (the
  sampled `PresentX` plane dump that has to grow a plane-1/flags line), `port/src/vulkan_present.h` and
  `port/src/vulkan_present_compose.h` (the composition already exists), test `port/test/xbox_present_test.cpp` (it
  already covers 1- and 2-plane composition: add the real title's plane-word cases as recorded constants).
- **Test/falsifier.** GPU test: two planes, one without the invented flag, composed as an overlay (and a manifest
  duplicate of plane 0 still handled sanely); boot: one arm dumping the title's plane count and words at the movie
  window, one A/B of the default profile against the Series S console type. Falsifier: the title never presents two
  planes in the movie window (then composition is not the movie path and the one-plane blit is the whole present story).
- **Boot need.** GPU test plus one boot (one A/B if the profile changes the plane count). No game data: plane words and
  counts only.

### Item 7: XG layout and upload coverage for the movie plane shapes (S/M)

- **Mechanism.** XCL runs the console layout library for its placed textures; Five Star has XG layout writers that fill
  the layout record and the texture-computer copy. Our XG shim answers the title's texture-computer path with a linear,
  single-plane layout and refuses planar and multi-plane formats by name. The movie's planes are documented by the
  earlier reference page as three linear planes through the slot-133 uploader ([movie_path.md](movie_path.md) section
  1), so the shim may already cover them if they are three single-plane textures; if they are one planar/multi-plane
  description, the XG fill would be refused and the plane pages would never be written.
- **Our target files.** `port/shims/xg_xs.cpp` (the slot-2 layout record and slot-8 copy for the measured shapes),
  `port/test/xs_proxy_mock_test.cpp` for the pure argument/record cases; no new file.
- **Test/falsifier.** One boot with `PALMETTO_XG_TRACE`/`PALMETTO_XG_DUMP` at the movie window: the movie's plane
  descriptions and their destinations are logged; a unit test builds the slot-2 record for the measured shape. If the
  planes are three single-plane linear textures that the current shim already fills, record the negative and close the
  item (no change). Falsifier for the gap: an XG create/copy for a movie plane is refused by name in the window.
- **Boot need.** Unit test plus one boot. Conditional on the measured plane shape.

### Item 8: Bink audio witness on the same wrapper (S)

- **Mechanism.** The movie's sound is the title's own Bink sound system: Five Star records `BinkSetSoundSystem` getting
  the game's own open function, and the Bink DLL imports XAudio2_9 itself (XCL's DLL inspection), while our tree binds
  that import to the host's XAudio2. The console audio HAL is a stand-in, so the honest question is whether the movie's
  audio path is the host XAudio2 one (in which case little or nothing is missing for the movie) or the title's
  console-side sound system (in which case the stand-in would silence it). The wrapper of item 1 already sees all Bink
  imports; adding `BinkSetSoundSystem`, `BinkSetSoundTrack`, `BinkOpenTrack`/`BinkGetTrackType`/`BinkGetTrackData` to
  its log answers which path the title takes.
- **Our target files.** Same wrapper as item 1; no new file and no shim change unless the witness shows a console path.
- **Test/falsifier.** One arm at the movie window with the wrapper counts and the audio HAL's own counters. Falsifier:
  no Bink audio export is called (then sound is not on the movie's path at our stage) or the track opens through
  XAudio2 with the host binding and is still silent (then the audio HAL stand-in is the cause, a separate slice). This
  item cannot make the draw issue and is ranked last for that reason.
- **Boot need.** One arm. No game data: track counts and types only.

## 5. Dependencies and what not to redo

- Item 1's wrapper is the observation point for items 3 and 8; it touches only the loader, so it does not conflict with
  the movie probes in `port/src/main.cpp` if the two are separate commits.
- Item 4 reuses the guard-page tracker and the one-shot import; do not add a second write-tracking mechanism
  ([card_copies.md](card_copies.md) sections 4-5 name the same consumer for the descriptor mirror, and its measured costs
  apply).
- Item 5 is only worth building after item 4's unit test can show a placed texture reading title pages at all; our
  measured slot-133 placements are in the title's own xmem region and the pairing may be a no-op.
- Item 6 must not weaken the present path's honesty: `ResourceCount != 1` and the overlay-flag values are hypotheses,
  and the peers' compose-all-planes behaviour is evidence about a different console field set, not a measurement of our
  title's plane words. Measure first, then relax.
- Do not port a decoder, a fake player or a movie shim: every peer runs the title's own DLL, our offline decode already
  covers 515/515 streams through it ([movies.md](movies.md)), and the movie's gating is title-side.
- Do not edit CMake, README or NOTICE for this page: the docs index row belongs to the lead, and NOTICE carries the
  attribution sentence of section 7 once a mechanism is actually ported.

## 6. Limits

- **Read-only peer audit.** No boot was run for this page and nothing was re-measured. Every "our measured state" cell
  is an existing page's claim, and a stale page would make its cell stale.
- **Peer material is self-reported plus source-read.** The mechanisms are read from source and docs at the commits in
  section 1, but a peer's numbers (frame rates, uploads, "works") are its own measurements on its own package and
  structures; the code names agree, the numbers are not comparable.
- **The names of the decoder DLL differ across trees.** The retail executable imports its 23 functions from a DLL the
  peers and our decompilation call `Bink2Scarlett.dll`; our offline decode, ABI and play tests map the package's
  `Bink2Console.dll` (74 exports). They are treated here as the package's Bink 2 decoder family, not as proven-identical
  files; the audit's claims are about the mechanism, not the file name.
- **No game data.** No package path, movie name, shader byte, extracted table or address of the user's copy is added;
  RVAs, counts, field offsets and outcomes only.
- **The plan's effort sizes are planning estimates**, not measurements, and the ranking is justified by our measured
  frontier ([decomp_first_ui.md](decomp_first_ui.md) sections 17-18), not by the peers' release order.
- **A peer contradiction is quoted, not resolved:** Five Star's README has self-inconsistent status lines (as the earlier
  parity audit also noted); this page quotes only its mechanism documentation and its per-item status entries.

## 7. No-copy statement and attribution

This page is a read-only audit. **No peer code, comment text, identifier or table was copied into this repository.**
Every mechanism described above was read as evidence and is re-derived here in this project's own terms; every
implementation proposed in section 4 is this tree's own, in this tree's files, naming and tests, and would be
implemented from our own measurements of our own title. The peers are evidence, not a code source
([upstream_port.md](upstream_port.md), [cleanroom_peers.md](cleanroom_peers.md) rules as amended).

The evidence read for this page, with the licence each repository states and the commit read:

- **Five Star Recomp** <https://github.com/ThatRecompDev/five-star-recomp>, GPL-3.0-or-later, clone
  `45071d5216a424218ff8743ede1c3eec30214365`: README; its own middleware, gpu-resources and d3d12x-shim docs; its
  status files; `src/FiveStar.Loader/Calls/BinkRecipes.cs`; the texture refresh worker and registry under
  `src/FiveStar.Loader/Shims/D3D12X/Resources/`; `src/FiveStar.Loader/Shims/XMemReservation.cs` and
  XMemPhysicalMemory.cs; `src/FiveStar.Loader/Shims/D3D12X/Present/PlaneReader.cs` and Presenter.cs.
- **VIctory** <https://github.com/Marvisak/VIctory>, GPL-3.0-or-later, clone
  `63bcca43c6a00a758510398fa43096ce7e4f7df0`: README; `crates/victory-gpu/src/texture.rs`, watch.rs, host.rs, queue.rs,
  bindless.rs; `crates/victory-hle/src/modules/media.rs`; `apps/victory/src/run.rs`.
- **victory-fork** <https://github.com/fxceless/victory-fork>, GPL-3.0-or-later, clone
  `52ac4ec0f8a03f103c146e60228e53612759d435`: README (current night section); `crates/victory-gpu/src/texture.rs` and
  watch.rs.
- **PercyScarlett** <https://github.com/rpatty-dev/PercyScarlett>, GPL-2.0-only, clone
  `e44c465e47ab5b29ca87777cd00875e2ae0d8c0d`: README; CHANGELOG.md; docs/TheBig6.md; the overlay-plane blend in
  `PercyPort/crates/percyport-gpu/src/host.rs` and queue.rs; `PercyPort/crates/percyport-hle/src/modules/media.rs`; the
  inherited runtime's texture path under `runtime/victory/`.
- **XCL** <https://github.com/xcl-dev/xcl>, GPL-2.0-only, clone
  `8b957bec6f82495aec940944c6bf47faa02db590`: README; HANDOFF.md; `crates/xcl/src/hle/bink.rs`; the GPU-view mirror in
  `crates/xcl/src/hle/kernelx.rs`; `crates/xcl/src/hle/d3d12x/title_textures.rs`; `crates/xcl/src/hle/d3d12x/present.rs`;
  `crates/xcl/src/resolver.rs`; the console-DLL end-to-end test.

The project's attribution rule ([upstream_port.md](upstream_port.md) section 6) puts the same sentence in `NOTICE.md`
for a slice that ports a mechanism. This page is an audit that ports nothing yet; when an item of section 4 lands, its
slice repeats this attribution and the lead adds the `NOTICE.md` entry (that file is outside this page's write scope).
The peers' own copyrights and licences continue to apply to their trees; this repository carries none of their code.
