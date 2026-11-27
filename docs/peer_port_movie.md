# The peer ports' movie path: Bink 2 decode, the plane textures and the present

**Status: peer audit (sections 1-7, written as a read-only audit: documentation only, no boot, every page quoted) plus
four implemented ports (sections 8-9, 11 and 12, with their own tests and their light boots), their attribution
(sections 8.7, 9.6, 11.7 and 12.7) and the uncommitted file lists (sections 10, 11.8 and 12.8).** The five peer ports
were re-cloned and read
at the commits in section 1 to answer one question: how does a port get a movie from the title's
`BinkOpenWithOptions` to a pixel on screen, and
which part of that is missing in this tree? The answer is narrower than the earlier parity audit expected. Every peer
runs the title's own Bink 2 DLL, lets the title decode into its own CPU frame buffers, and has no movie trigger, no Bink
HLE and no fake decoder: the title's own state machine opens, decodes and draws the movie. What the peers do carry on the
graphics side is the part this tree does not: a per-submission refresh of a texture whose bytes the title writes in its
own memory (XCL's `title_textures`, VIctory's and victory-fork's texture upload path, Five Star's texture refresh
worker), and a present compositor for a second display plane (VIctory, PercyScarlett, XCL). Our chain reaches the
player's playing state `0x9` with a non-null handle and `buffers 0`, but frame-buffer registration `0x442F760` and
per-frame decode `0x2EF0E0` never run because the draw that issues them is never issued: the title's frame thread sits
in the session-3 loading poll ([decomp_first_ui.md](decomp_first_ui.md) sections 17-18). No peer port makes that draw
issue; their titles reach the movie because their loading and first-run flow completes. The ranked plan in section 4
therefore puts the movie-side instruments that can decide the draw question first, and the two real movie-specific
graphics ports (title-memory plane-texture refresh, extra-plane composition) behind them - and sections 8-9 are those
two graphics ports, implemented, tested and measured on this tree.

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
for a slice that ports a mechanism. This page was an audit that ported nothing; the two ports of sections 8-9 landed
with their own attribution there, and the lead still owes `NOTICE.md` the two entries (that file is outside this page's
write scope): the per-submission texture refresh (section 8.7) and the measured second-plane rule (section 9.6). The
peers' own copyrights and licences continue to apply to their trees; this repository carries none of their code.

## 8. Implemented port 1: the per-submission refresh of a title-written plane texture (section 4 item 4)

**Status: implemented behind opt-in switches, tested GPU-free and on the GPU, and measured on one light boot.** This is
the rank-4 port of section 4, item 4. It answers the gap section 3.2 (a) named: a texture whose bytes the title writes
itself is imported once into its stand-in and never refreshed, while a movie rewrites its planes every frame. The code is
our own, in our files ([plane_refresh.h](../port/src/plane_refresh.h) is the pure half;
[Resource::import_placed_pages](../port/shims/d3d12_vulkan_device.cpp) and `Resource::refresh_placed_pages` the impure
one); sections 2.2 item 3 and 5 of this page are the peer evidence it was planned from, and section 8.6 repeats the
attribution. No peer code was copied and no game data is in this port.

### 8.1 The measured shapes and the gap

Every number below is from existing boot logs outside the repository (the harness's boot directories under
`D:\leonida-work`, read only for this port; no new boot was needed to establish them). The pattern is the same in each:
the title-written texture population our layer can import is **small**, the sampled placed population is **large and
empty in the measured window**, and the import path is **one-shot**.

| what | measured | where |
|---|---|---|
| title-placed texture imports that succeed | 5-9 per boot, subresource 0 only: **512 B** into a 64x2 image, **65,024 B** into 127x128, **32,768 B** into 64x64, **57,600 B** into 160x90 (twice), **36,864 B** into 128x72 (three times, "more than one subresource: the rest is not imported yet") | the `boot-teximport*` boots' stderr |
| sampled placed spans a page watch watched | four spans, **16.0 MiB**, **28.2 MiB**, two more at 16 MiB and 33.2 MiB; all-zero head at the first sample, **no change line afterwards** (the title never wrote them in that window); the import refuses them at its default 16 MiB cap | `firstlight/arm-a` (`PALMETTO_TEXTURE_PAGE_WATCH=1`) |
| placed textures with the import cap lifted | **25** queued, **every one** "the pages' head is all zero, waiting for the texture-computer fill" for the bounded 512 attempts, so **0** imported | the same boot with `PALMETTO_TEXTURE_IMPORT_MAX=67108864` |
| the import's own shape | one bounded import per resource: 512 attempts while the fill has not run, a 16 MiB default cap, subresource 0 only, then the resource leaves the pending list for good | [texture_import.md](texture_import.md) section 1, the code above |
| the guard-page tracker's armed population | descriptor mirrors only (`PALMETTO_CARD_COPY_WATCH`): one range, 608 x 64 KiB views, 38.0 MiB; arming 0.604 ms, a guarded write ~10.3 us, a guarded read ~5.1 us; no texture consumer existed | [card_copies.md](card_copies.md) sections 2.3, 4.2 |
| a 4K movie plane in the import's row-major mode | **12.4 MB** for an R8 tri-planar plane (3840x2160, 1.5 bytes per pixel) and **33.2 MB** for RGBA8 - i.e. the peers' "~25 MB a presented frame where ~3 MB had changed" population, sized against the import's 16 MiB cap and this port's compare budget | arithmetic on the measured 3840x2160 presented target extent ([upscale_standin.md](upscale_standin.md) section 2.1) |

So the gap is exact: **a title-written texture that changes after its one import keeps the frame that import took**, and
the population that would exercise it (a 4K movie plane) is at or beyond the import's cap while the population the import
does take (512 B - 65 KB atlases and defaults) is small enough that comparing all of it every submission is cheap. The
port therefore has to do two different things well: keep the small tracked textures current for a bounded cost, and be
ready for the 12.4 MB plane case the moment a draw samples it.

### 8.2 The design

Our own mechanism, composed from parts this tree already owns - the one-shot import, the guard-page write tracker
([xmem.h](../port/shims/xmem.h), [card_copies.md](card_copies.md)) and the per-submission queue hook that already drains
the import. The peers' three structures (XCL's per-submit placed-texture upload with a content-hash skip, VIctory's and
victory-fork's detile-on-change/hash-what-cannot-be-watched, Five Star's page-hash worker) are evidence for the mechanism
class, not its implementation: this port keeps a CPU **shadow** of the bytes it imported, compares the title's pages
against it before the submission that could sample them, and re-uploads **only the rows the change touched**.

| switch | effect |
|---|---|
| `PALMETTO_PLANE_REFRESH` | unset, `0`, `off` or any unknown value: the previous path exactly - no shadow, no compare, no tracker call, one false branch in the per-submission hook. `hash`: compare every tracked texture's pages against its shadow and re-upload the changed row band (needs no tracker). `watch`: ask the guard-page tracker first and skip the compare when it answers "unchanged"; a tracker that answers "unavailable" falls back to the hash compare. A `watch` that would share the tracker with `PALMETTO_CARD_COPY_WATCH` falls back to `hash` (the mirror gate polls the tracker's one change flag first and suspends every range around its scan, so a texture write would be consumed by it or land in its suspended window) - logged once. |
| `PALMETTO_PLANE_REFRESH_TRACE` | the counters only, the path unchanged: one `[rt] d3d12: plane refresh: ...` line at the first walk, the 256th and every 4096th. |

- **The shadow is the reference and the narrow upload's source.** The import reads the title's pages once into the
  shadow, uploads from it, and remembers the subresource's row shape. A refresh compares the pages against the shadow
  (`changed_byte_range`), copies only the changed byte range into the shadow, maps that range to the copy-unit rows it
  touches (`changed_rows`) and re-uploads exactly those rows through the existing `WriteToSubresource` path
  (`cpu_copy`). A change that cannot be narrowed - block-compressed rows (a box's top must be a block multiple), more
  than one slice, or every row - re-uploads the subresource whole, which is always correct and only costs the copy.
- **The compare budget is bounded and never starves a texture.** A submission admits tracked textures in round-robin
  order up to `plane_refresh_compare_budget` = **16 MiB** (the import path's default cap, so one 4K tri-planar plane
  fits); a texture the budget did not admit is deferred to the next submission, and the round-robin start moves by one
  each submission, so the deferred texture is first next time. A texture larger than the whole budget is admitted alone
  when it reaches the front - it is never starved either.
- **The tracker is reused, never re-implemented.** In `watch` mode the import arms the existing guard-page tracker on
  the texture's pages (`page_watch_arm`), and each submission's walk polls it once (`page_watch_poll`): "unchanged"
  skips every compare, "changed" and "unavailable" both fall through to the compare (a write narrows the upload, an
  unavailable tracker is the hash path). The walk re-arms the pages its own reads tripped (`page_watch_rearm`). The arm
  is bounded by the measurement: only spans at or below `plane_refresh_watch_max_bytes` = **1 MiB** are watched, because
  a guarded page costs ~10.3 us per write and a plane a frame rewrites would pay that per written page; a larger texture
  is compared, which is the peers' "hash what you cannot watch" answer. An arm that fails (an uncommitted page, another
  owner's guard, the tracker's 8 range slots full) is the documented fallback to the hash compare.
- **Bounded holding.** A tracked texture keeps the shim's reference for the process's lifetime, which is what makes the
  walk safe without a lock around a title release; the list is bounded to **16 textures** and **64 MiB** of shadows, and
  a texture past either bound keeps the one-shot import. This is a documented limit (section 8.5), not a leak a boot
  grows with: the measured imported population is 5-9 textures / under 0.5 MB.
- **Default path unchanged.** With the switch unset the port allocates no shadow, arms nothing, counts nothing and does
  not call the tracker: the import's own bytes and the boot's log lines are the tree's, byte for byte.
- **Where it runs.** The device already registers one hook the queue calls before it records a submission (the one-shot
  import); the walk rides the end of that hook, so the refreshed rows are in the stand-in before anything in the
  submission can sample it.

### 8.3 Tests

`port/test/xmem_test.cpp` (GPU-free) gained `test_plane_refresh_plan`, **51 new checks** (the binary now prints
`xmem_test: OK (341 checks)`):

| check group | what it proves |
|---|---|
| switch parsing | unset/empty/`0`/`off` are off; `hash`, `HASH`, `1`, `watch`+trace parse to the documented plan; an unknown word is off, never a guess |
| the compare budget | a texture that fits is admitted whole; one that does not fit behind an admitted texture is deferred; an oversized texture is admitted alone and nothing else fits behind it; a zero budget admits nothing |
| changed byte range | identical spans report no change; one middle byte reports exactly that byte; a later change extends the range; a full rewrite reports the whole span; a null span is not a change |
| changed rows | byte offsets map to copy rows; a range across rows covers both; a whole-subresource change names every row (the caller re-uploads whole); no row size, no rows, an empty range and a past-the-end offset are refused |
| the step decision | hash mode always compares; watch mode skips only on the tracker's "unchanged"; "changed" and "unavailable" fall through; a texture the budget did not admit is deferred |
| the report | one line names the tracked, compared, changed, uploaded, narrow/full, tracker-skip, deferred and unreadable counts; a null destination is a no-op |

`port/test/d3d12_vulkan_device_test.cpp` (GPU) extends the title-placed texture case of the image-memory checks, with
`PALMETTO_PLANE_REFRESH=hash` set for that run: an 8x8 placed texture is imported from title pages as before, then (a) a
**one-row rewrite** must appear after the next submission while the other seven rows keep their imported bytes (the narrow
band is the tested unit), (b) a **whole-texture rewrite** must appear after the next submission (the full re-upload), and
(c) a submission with **no write** must leave the image at the last rewrite (the skip must not corrupt). The run prints
the existing summary line and passes.

### 8.4 Boot measurement

One 300 s light boot (this port's budget: one arm, no A/B, because it is readiness behind a switch), the section-12
content environment of `D:\leonida-work\fsrstable\boot_arm.ps1` plus the class-0xa mitigation
`PALMETTO_TITLE_CLASS0A_SIZE=1`, the boot slot held by `_setup\boot_lock.ps1`, with the port's switches on:
`PALMETTO_TEXTURE_IMPORT=1`, `PALMETTO_PLANE_REFRESH=hash`, `PALMETTO_PLANE_REFRESH_TRACE=1`,
`PALMETTO_PRESENT_PLANE_DUMP=1` and `PALMETTO_PRESENT_PLANE_RULE=measured`. Outputs are outside the repository
(`D:\leonida-work\fsrstable\boot-plane-port`: `summary.txt`, `stderr.log`, `boot.jsonl`;
`capture-plane-port/index.txt`).

| measure | value |
|---|---|
| run | **ALIVE 300.3 s**, killed by the timeout as usual, **0 exceptions**, **0 device-lost** |
| state at the stop | init 3, loading 2, **session 3** (unchanged for the last ~228 s): the session-3 wall of section 3.3, so the movie draw was not issued and no frame sampled a plane texture |
| submissions | 1,024 at 16.3 s, 4,096 at 49.7 s, **8,192 at 279.5 s** (the frame thread parked after it) |
| presented frames | at least **2,201** and fewer than 10,000 (12 captures, one every 200th, the last frame 2,201); every capture 2564x1421 with mean rgb **0 0 0** - the same honest-black frame the earlier pages measured |
| the plane dump | 11 sampled calls (1-10 and 1,000), **every one "1 plane(s)"**; plane 0 at each: ResourceCount 1, colour space 0, ScaleFilter 0, Flags 0x0 - **0 two-plane presents** |
| title-written textures imported | **2** (both 2x2, 64 B, subresource 0), the small-atlas population of section 8.1; the boot also logged **32** "pages' head is all zero" waits and **8** over-cap refusals (29,491,200 B and 33,177,600 B placements) |
| the refresh counters | the report lines reached walk 4,096: **8,191 tracked texture-submission pairs, 0 watched, 8,191 compared (0.5 MB in all), 0 changed, 8,191 unchanged, 0 uploaded, 0 tracker skips, 0 deferred, 0 unreadable**; the next report was due at walk 8,192 and the boot ended at the title's 8,192nd submission, so it never printed |
| what it proves | the per-submission walk is live, bounded and cheap: **64 B compared per submission** at this population (0.5 MB over 8,191 walks), **no upload, no deferral, no unreadable page**, and nothing on the tree's default path - the boot's state, presents and captures are the ones the earlier pages measured on the same environment |
| what it does not prove | the measured population did not change, so **no upload ran on the title's own textures** (the narrow and full upload paths are covered by the GPU test of section 8.3); and no frame sampled a tracked texture, because the draw that would is the session-3 wall |

The counter line to compare a later boot against is the walk line above: a `changed`/`uploaded` pair that is not zero is
the event that says the title started rewriting a texture our import keeps - the falsifier section 8.5 names.

### 8.5 Limits and the next falsifier

- **The population that would exercise this in a real frame has not run yet.** The movie draw is not issued at this
  frontier (section 3.3, [decomp_first_ui.md](decomp_first_ui.md) sections 17-18), so no tracked texture a frame samples
  was refreshed by a frame in the measured window: the mechanism is readiness, measured on the tests above and on the
  boot's own counters, not on movie pixels.
- **No sampling-set narrowing.** The walk does not know which of a submission's T#s sample which tracked texture; it
  refreshes every tracked texture every submission, bounded by the compare budget. At the measured population
  (5-9 textures, under 0.5 MB) that is cheaper than resolving the submission's descriptors would be; at the 12.4 MB
  plane case the budget admits one texture a submission and defers the rest, which is the honest cost of not reading
  the draw's tables here. The next falsifier: a boot where the spent compare time is a visible share of the submission
  (the `plane refresh` line's compared bytes against the boot's submissions) - then the walk needs the descriptor
  resolution, not a bigger budget.
- **The guard-page watch's cost bound is the arm's size, not the write count.** In `watch` mode a frame that rewrites a
  watched span pays one guard exception per written 4 KiB page (~10.3 us each at the measured rate); the 1 MiB cap
  bounds that at ~2.6 ms for a fully rewritten span. That is why the default mode for a movie-sized texture is the hash
  compare, and why `watch` is an experiment for the small population.
- **A queued write can land between the compare and the submission.** The compare reads the title's pages, then the
  submission is recorded: a title thread writing in that window is the same race the descriptor mirror has
  ([page_address_space.md](page_address_space.md) section 5) and the same one the peers' per-submit upload has. It is a
  one-frame staleness bound, not a correctness claim.
- **The tracked hold is process-lifetime.** A tracked texture's shim reference is released only when the walk drops it
  (a shape change or a refused upload); at process end the OS reclaims it. The bounds (16 textures, 64 MiB) are what
  keep that finite.
- **Multi-subresource and multi-slice textures are not tracked** (the import already takes subresource 0 only, its
  order is unmeasured): they keep the one-shot import. The next falsifier is a movie plane described with more than one
  subresource - then the shadow and the row map need the subresource table, which is [texture_layout.md](texture_layout.md)
  item 7's measurement.
- **The measured population is zero-change in the window.** The page watch saw all-zero heads with no change on the
  sampled placed spans, so the boot's refresh counters are expected to show compares and no uploads; a change line in a
  later boot is the event that says the title started writing one of these.
- **The `watch` pre-filter's runtime path has no boot.** The one boot of section 8.4 ran `hash` mode - the mode a GPU
  test binary without `xmem.cpp` can exercise - so the arming call and the tracker verdict at the per-submission walk
  are covered by `xmem_test`'s tracker cases and the pure step tests, not by a boot with `watch` on. The next arm that
  wants the pre-filter should be a second light boot with `PALMETTO_PLANE_REFRESH=watch` (and `PALMETTO_CARD_COPY_WATCH`
  off, or the port's own guard against sharing the tracker applies), reading the `watched` and `tracker skips` counts.

### 8.6 Cross-check against the peers (reference only, no code copied)

| peer mechanism (section 2.2 item 3) | what this port does | difference |
|---|---|---|
| XCL `title_textures`: upload a placed texture at creation and again before the next execute when the CPU wrote any of its pages; a content-hash skip when unchanged; measured ~25 MB a presented frame where ~3 MB had changed | refresh before the submission, narrowed to the changed rows, with the changed byte range found by comparing the shadow | our upload is row-narrowed where theirs re-uploads the texture and relies on the change signal; our compare is the signal when the tracker is unavailable, theirs hashes |
| VIctory / victory-fork `texture.rs`: detile the whole texture when its memory changed; hash the memory that cannot be watched | the shadow compare is the "cannot be watched" path; the tracker's verdict is the watched path; a changed texture re-uploads only the changed rows | the peers' whole-texture detile is simpler and was measured cheap on their structures; our narrow band is what keeps a 12.4 MB plane's refresh from being a 12.4 MB upload |
| Five Star texture refresh worker: hash pages on a worker thread, detile only the affected subresources | the compare is on the recording thread (the measured population is small) and the narrowed unit is the row band in the import's row-major layout | no worker thread: the walk's cost at the measured population is bounded by the budget, and a background thread would be the next step only if the falsifier above fires |
| all three: a title-written texture is current before the submission that samples it | same rule; the stand-in is the imported image and the walk rides the existing per-submission hook | our import keeps a CPU shadow, which the peers' in-place/heap structures do not need - the cost of our copy-and-mirror model ([title_memory_model.md](title_memory_model.md) section 2) |

### 8.7 Attribution

This port was implemented from this project's own measurements and boot logs. The behaviour was cross-checked against
the public GPL projects named in section 1 at the commits there - **XCL** (`8b957be`, `crates/xcl/src/hle/d3d12x/title_textures.rs`),
**VIctory** (`63bcca4`, `crates/victory-gpu/src/texture.rs` and `watch.rs`), **victory-fork** (`52ac4ec`, the same texture
module) and **Five Star Recomp** (`45071d5`, the texture refresh worker and registry) - as evidence for the mechanism
class. **No code from those projects was copied.** The upstream projects remain the copyright of their contributors and
their licences continue to apply to their own trees. The `NOTICE.md` row for this port is owed by the lead (that file is
outside this page's write scope): the licence text of [upstream_port.md](upstream_port.md) section 6, listing the four
projects, the file paths above and the commits.

## 9. Implemented port 2: the second-plane composition rule (section 4 item 6)

**Status: implemented behind an opt-in switch, tested GPU-free and on the GPU, and measured on one light boot.** This is
the rank-6 port of section 4, item 6. It answers the gap section 3.2 (b) named: the composition of a second present
plane exists, but the gate that decides whether the title's second plane is shown is our invented
`xbox_plane_flag_overlay` bit, whose value no measurement supports, while the peers compose the plane. The port replaces
that gate, when the switch is on, with a rule stated over the plane fields our present path has measured, and keeps
refusing the cases no measurement covers. The pure decision is [plane_compose_rule.h](../port/src/plane_compose_rule.h);
the caller is `XboxPresentation::present_x` in [xbox_present.cpp](../port/shims/xbox_present.cpp). No peer code was
copied; section 9.6 repeats the attribution.

### 9.1 The measured plane words and the gate's population

Every sampled `PresentX` in every boot log read for this port presents **one plane** and the same words. The existing
sampled dump ([d3d12_xs_device.cpp](../port/shims/d3d12_xs_device.cpp)) prints them at calls 1, 2, 1,000 and 10,000:

| field | measured value | boots |
|---|---|---|
| plane count | **1** at every sample | the `boot-teximport*`, `boot-moviepath-*` and `boot-present-watch` boots' stderr |
| plane 0 Token | the frame token, rising with the present count | same |
| plane 0 ResourceCount | **1** | same |
| plane 0 colour space | **0** (`DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709`, SDR) | same |
| plane 0 ScaleFilter | **0** (the default) | same |
| plane 0 Flags | **0x0** | same |
| plane 0 resource | the **same resource object** at every sampled call (the frame's target is reused) | same |

So in the measured window: **a second plane has never been presented**, and the invented overlay bit has **never been
set on any plane** - the composition path with two shown planes has population 0. That is exactly why the gate had to be
replaced by a rule over fields that *are* measured rather than by another guessed bit, and why the port also adds the
sampled plane-word dump itself (`PALMETTO_PRESENT_PLANE_DUMP`): the next boot that presents a second plane will print
its words instead of leaving the gate to a hypothesis.

### 9.2 The design

The rule, in order (pure, `palmetto::compose_second_plane`):

1. **One plane** - not requested; the present path is the tree's.
2. **More than two planes** - refused (the UMD limit, always refused).
3. **ResourceCount other than 1** - refused as unmeasured: every observed plane had one resource, the console's
   multi-resource plane has no measured population here, and the composition reads one resource per plane.
4. **Plane 1 names a placement** (a sane `pDestPlacementBase`, a sub-rectangle of the output) - **compose** it over
   plane 0. Placement is the field whose meaning this present path already measured for plane 0: it is one of the
   measured compose triggers for a single plane.
5. **Plane 1 asks for its own filtering** (point or linear `ScaleFilter`) - **compose** it.
6. **Anything else** (no placement, the default filter) - **refuse by name**: that is the unmeasured GameDVR-copy shape
   of the public HDR sample, where showing the second plane would draw the frame twice with an alpha never meant to be
   blended.

The blend is unchanged and measured: the compose pass composites plane 1 **premultiplied over** plane 0 (the existing
`One`/`InvSrcAlpha`-equivalent the shader implements and the pixel model checks); the straight-alpha bit still selects
unassociated alpha and is still documented as a hypothesis in its own constant. The rule is a **decision**, not a new
composition path: `PresentX` still composes the same `PresentPlanes` it composed for the invented flag.

| switch | effect |
|---|---|
| `PALMETTO_PRESENT_PLANE_RULE` | unset, `0` or any value other than `measured`: the previous path exactly - plane 1 is composed only with the invented `xbox_plane_flag_overlay` bit. `measured`: the rule above replaces the flag gate, and the invented flag is ignored. |
| `PALMETTO_PRESENT_PLANE_DUMP` | diagnostic only: the sampled plane-word dump (first calls, powers of ten to a million, and the first calls carrying a second plane, another colour space/filter, a placement or any flag bit), written to stderr; never changes a present. |

- **Refusals are logged once per kind**, as the present path already logs its unmeasured fields: a composed plane 1 says
  why it was composed (placement or filter), a refused one says it is the unmeasured no-placement/default-filter shape.
- **Default path unchanged.** With `PALMETTO_PRESENT_PLANE_RULE` unset, the flag gate and its once-only "GameDVR copy"
  line are the tree's, byte for byte; the dump is off unless asked for.
- **Test seam.** The switch is read once per process (like every switch on the present path); a small
  `XboxPresentation::set_plane_rule_measured(bool)` lets the pixel test compare both gates on one GPU.

### 9.3 Tests

`port/test/xbox_present_test.cpp` (GPU, presents into a window and reads the swapchain back) gained:

| check group | what it proves |
|---|---|
| the pure rule (16 checks) | one plane is not requested; three planes refused; ResourceCount 2 refused; no placement + default filter refused; placement composes; point and linear filters compose; an unknown filter is refused (the caller normalises it to the default, so it is never a measured ask); the switch parses only `measured` (case-insensitive), never `0`, `flag` or a longer word; every decision carries a reason |
| the GPU cases (with the rule on) | plane 1 with a placement and **no invented flag** is composed (its pixels match the same CPU model as the flagged cases above, within 1 level); plane 1 with its own point filter and no placement is composed (exact); plane 1 with neither, **even with the invented flag set**, is not shown - the frame is plane 0 only, exactly; ResourceCount above 1 is still refused before composition; switching the rule off restores the flagged gate |

### 9.4 Boot measurement

The same one 300 s light boot as section 8.4 (one boot, both ports' switches), with `PALMETTO_PRESENT_PLANE_DUMP=1`
and `PALMETTO_PRESENT_PLANE_RULE=measured`:

| measure | value |
|---|---|
| presents sampled by the dump | 11 calls (1-10 and 1,000); **every one "1 plane(s)"** |
| plane 0's words at every sampled call | ResourceCount **1**, colour space **0** (SDR G22/P709), ScaleFilter **0** (default), Flags **0x0**, and the same resource object at every call |
| two-plane presents | **0** - the rule's two-plane branch was not entered and the invented overlay flag was not set on any plane in the window |
| the rule's once-per-kind lines | none printed: neither the "composed" line nor the "refused unmeasured" line has a case at this stage, which is itself the measured statement that the title presents one plane here |
| what it proves | the dump works (the words above are its output), and the rule costs nothing on the one-plane path: the boot's presents, captures and black frames are the ones the earlier pages measured |
| what it does not prove | the title's own two-plane descriptor: the rule is ready and GPU-tested (section 9.3), not observed on the title |

The falsifier section 9.5 names is the same line: a `present planes` dump with **2 plane(s)** is the event that gives the
rule a real case, and its plane-1 words decide whether the refusal or the composition is what this title needs.

### 9.5 Limits and the next falsifier

- **The rule's two-plane branch has no population on this title yet.** The measured window has one plane, so the rule
  is tested (GPU, synthetic plane words) and ready, not observed on the title's own descriptor. The next falsifier: a
  boot that reaches the movie window and presents two planes (the dump prints their words). If plane 1 there has no
  placement and the default filter but the title clearly expects it shown, the rule refuses what the console would
  show - then the measured answer is the peers' compose-all-planes rule, and the dump's words are the evidence to
  change it with.
- **The placement/filter rule is an inference from measured field semantics, not a measured two-plane present.** It is
  stated that way in the code and here: `pDestPlacementBase` and `ScaleFilter` are what this present path measured as
  "place and filter this plane", and the unmeasured case is refused rather than guessed.
- **The flag values remain unmeasured.** `xbox_plane_flag_overlay` and `xbox_plane_flag_straight_alpha` stay our
  placeholders in the header; the port does not make them measured, it stops the *decision* from resting on one.
- **Series S.** The console type changes what the title presents (section 3.3 item 4); the one A/B named in section 4
  item 6 has not been run, so whether the Series S profile presents the overlay plane is still the open question the
  plane dump is there to answer.

### 9.6 Attribution

This port was implemented from this project's own measurements and boot logs. The behaviour was cross-checked against
the public GPL projects named in section 1 at the commits there - **VIctory** (`63bcca4`, the host present compositor's
opaque plane 0 + overlay planes), **PercyScarlett** (`e44c465`, the premultiplied Series S overlay blend in its GPU host
and queue) and **XCL** (`8b957be`, `crates/xcl/src/hle/d3d12x/present.rs`, the opaque first plane and blended later
planes) - as evidence that the console's second plane is a real, composed movie surface. **No code from those projects
was copied.** The upstream projects remain the copyright of their contributors and their licences continue to apply to
their own trees. The `NOTICE.md` row for this port is owed by the lead (that file is outside this page's write scope),
listing the three projects, the paths above and the commits, in the form of [upstream_port.md](upstream_port.md)
section 6.

## 10. Uncommitted files of the two ports

Implementation: [plane_refresh.h](../port/src/plane_refresh.h) (new),
[plane_compose_rule.h](../port/src/plane_compose_rule.h) (new), [d3d12_vulkan_device.cpp](../port/shims/d3d12_vulkan_device.cpp),
[xbox_present.cpp](../port/shims/xbox_present.cpp), [xbox_present.h](../port/shims/xbox_present.h).
Tests: [xmem_test.cpp](../port/test/xmem_test.cpp), [d3d12_vulkan_device_test.cpp](../port/test/d3d12_vulkan_device_test.cpp),
[xbox_present_test.cpp](../port/test/xbox_present_test.cpp). Docs: this page. Nothing else is touched:
`port/src/vulkan_backend.h`, the CMake files, `NOTICE.md`, `docs/README.md` and the other docs pages are unchanged by
this port. No git write was made and no peer code is in the tree.

## 11. Implemented port 3: the GPU-view reservation pair for placed texture VAs (section 4 item 5)

**Status: implemented behind an opt-in switch, tested GPU-free and on the GPU, and measured on one light boot that
stopped at the title's stage-3 frontier at 60.4 s.** This is the rank-5 port of section 4, item 5. It answers the gap
section 4 item 5 names: the title reserves the same page span twice, a CPU-readable
reservation and a `PAGE_GRAPHICS_NOACCESS` one, and places GPU resources by the second while its CPU writes the first.
This tree mapped only the first; a placed resource whose console VA is in the second has no pages at that VA on the
host. The port records the pair (our own rule, stated over the measured reservations), resolves a placed texture's
console VA through it before the import reads anything, and keeps that resolved source for the per-submission refresh of
section 8. The pair was recorded in the measured boot with exactly the measured shape; **no placed texture resolved
through it** in the window the boot ran, because the title placed all 64 sampled creations in its committed heap and the
arm stopped before the movie window. That is a measured readiness result, not a fix observed on a title resource, and
section 11.4/11.5 say so. The code is our own: the pure rule and bounded table are in the section 11.2 files below, the
recording and the resolve hook in [xmem.cpp](../port/shims/xmem.cpp), the consumer in the placed-texture import and
refresh of [d3d12_vulkan_device.cpp](../port/shims/d3d12_vulkan_device.cpp). No peer code was copied and no game data is
in this port; section 11.6 repeats the attribution.

### 11.1 The measured shapes and the gap

The pair is measured (existing boot logs outside the repository, read only for this port; no new boot to establish it).
A 60 s boot with `PALMETTO_XMEM_LAYOUT=log` prints the two reservations; the light arm that carried this port's switches
(section 11.4) prints the same pair again, and the port's own record line names it:

| what | measured | where |
|---|---|---|
| CPU half of the pair | a reserve-only 64 KiB-page window of **0x600000000 B** (24 GiB), host `PAGE_READWRITE` (console protect `0x4`), attribute word `0x21`, title caller `game+0x1e1d00f` | `boot-xmemlayout1` and `boot-gpuview2` stderr, the `xmem layout` line |
| GPU view of the pair | the same size, host `PAGE_NOACCESS` (`PAGE_GRAPHICS_NOACCESS` \| `PAGE_NOACCESS`, console protect `0x801`), attribute word `0x3`, title caller `game+0x1e1d03e` (0x2f after the CPU caller) | same |
| the two ranges | **contiguous**: CPU base + size == GPU-view base in both boots (`0x1e1bc2a0000 + 0x600000000 = 0x1e7bc2a0000` in the port's boot) | same |
| presence | both allocations appear in **every** boot read: the 300 s rank-4 boot's attribute census records two `XMemVirtualAlloc` answers of 25,769,803,776 B with attribute words `0x21` and `0x3` from the same two callers | `boot-plane-port` `boot.jsonl`, events 13-14 |
| pages behind it, measured window | **0** pages mapped into the pair's CPU half by 60 s: the title's page pool (**99,776** 64 KiB pages, 6.54 GB) is mapped into a separate CPU-readable placeholder, and the boot's first page-mapping batch names that pool | `boot-gpuview2` stderr: `xmem layout` for the 0x185c00000-B placeholder, the slot-31 first-call line |
| placed texture population, same window | **64** creation lines (the log's own bound; the separate 60 s layout boot has 30, all of them inside the committed 2 MiB-page heap), **0** in either half of the pair, 64 inside the committed heap (`0x283`-attribute large pages, 0x89800000 B served) | both boots' `placed texture: address` lines |
| a placed buffer | the measured 182,452,224 B slot-133 placement is inside a mapped physical-page window (the address is one of the five window starts the same boot records), not in the pair | `boot-xmemlayout1` stderr |
| imports in the window | **0** imported: 32 all-zero-head waits, 24 give-ups after the bounded 512 attempts, 5 refusals at the 16 MiB cap; the rank-4 300 s boot imported 2 small (64 B, 2x2) textures and none of its 4K planes | `boot-gpuview2` and `boot-plane-port` stderr |

The gap is exact, and it is a host-side one: **the GPU-view window holds no pages on this host**. The title maps the
physical pages into the CPU reservation only (the measured pair is the peers' arrangement, section 11.6), so a placed
resource whose console VA is inside the GPU view reads a window with nothing committed in it, while its bytes sit at
the same offset in the CPU half. The one-shot import reads the console placement VA once per texture
([texture_import.md](texture_import.md)) and the section-8 refresh compares that same address, so both would read the
empty window for such a texture. At this frontier that texture has not been observed: every sampled placement is in
committed CPU-addressable heap, the movie draw is not issued (section 3.3) and the GPU view's population is 0. The port
is therefore the mechanism the movie planes would need (the peers document the video planes placed in that second
reservation), built and bounded now, measured for the population it does have.

### 11.2 The design

Our own mechanism in the tree's own terms. The pure half is [xmem.h](../port/shims/xmem.h) (`GpuViewPairTable`, the
rule, the translation, the switch); [xmem.cpp](../port/shims/xmem.cpp) records each reserve-only 64 KiB-page window as
XMemVirtualAlloc makes it and fills the resolve hook; the device resolves a placed texture's console VA through the hook
before the import's commit/content gate and keeps the answer as its source.

| switch | effect |
|---|---|
| `PALMETTO_GPU_VIEW_PAIR` | unset, empty, `0`, `off` or any unknown word: the tree before this port - nothing is recorded, the hook answers "unavailable", the import reads the console placement VA and the refresh compares it, byte for byte. `on`, `1`, `yes` or `true` (case-insensitive): record the pair and resolve placed-texture VAs through it. |
| `PALMETTO_GPU_VIEW_PAIR_TRACE` | more of the mechanism's evidence: the CPU candidates and the first pages mapped into a paired CPU view. The pair's record line, the device's resolve/refusal lines and the counters print with the switch on either way, bounded; the path is unchanged. |

- **The rule is stated over the measured reservations, and refuses what the measurement does not cover.** A GPU-view
  reservation pairs with the CPU-readable reservation recorded just before it when the two have the same size and are
  **contiguous** (`cpu.base + cpu.size == gpu.base`, exactly the measured arrangement). The peers pair by "the CPU
  reservation made just before it" and the measured boot's pair is also contiguous; this port keeps the narrower
  measured rule, so a same-size pair that later appears non-contiguous is recorded as unpaired and named by the refusal
  rather than translated on a guess (section 11.5 names that as the next falsifier).
- **The table is bounded and append-only.** Eight CPU candidates (newest kept), eight pairs and eight unpaired GPU-view
  reservations. The shim never sees the title's `VirtualFree` of these windows, so a record lives for the process's
  lifetime; the bounds, not a free-list, are what keep that finite. A reservation past a bound is counted and logged
  once, and the caller keeps the pre-port path.
- **One resolution per texture, used by the import and the refresh.** The import resolves the console VA before its
  first read, keeps the CPU address in `placed_source_pages_`, and every later attempt, the content gate, the shadow
  compare and the page watch use it. This is the "one-shot vs paired" difference: the refresh of section 8 compares the
  paired view, so a title rewriting the CPU half stays current in the image without a second resolution.
- **Page pull on first sample.** A resolution can succeed while the CPU half's pages are not committed yet (the title
  maps them lazily). The import's existing committed/readable gate makes it wait and retry on the next submission
  (bounded to 512 attempts), so the first sample after the pages appear imports them; the alternative, reading the
  uncommitted CPU address, would fault and is never done.
- **Refusals are named, not guessed.** A request inside a GPU view that is unpaired, or that crosses the pair's end
  (the whole subresource must fit the reservation), resolves to `gpu_only`: the import leaves the committed stand-in,
  logs the refusal once (bounded) and never reads the window. The import's own size cap still runs first, so pairing
  does not lift the 16 MiB bound of section 8's population.
- **Default path unchanged.** With `PALMETTO_GPU_VIEW_PAIR` unset no table entry is made, no mapping is counted, the
  hook answers "unavailable" and the import/refresh read the console VA; the boot's logs are the tree's. The resolve
  hook is null in binaries that link no xmem (the GPU test replaces it with a stub, section 11.3).
- **Where it runs.** Recording rides the two reserve-only window paths of `XMemVirtualAlloc` under the physical-page
  table's mutex; resolution is a hook call on the recording thread that drains the placed textures; the pairing counters
  reach the boot's report on the section-8 refresh milestone line (`GPU-view pairing: ...`).

### 11.3 Tests

`port/test/xmem_test.cpp` (GPU-free) gained `test_gpu_view_pairing_table` and `test_gpu_view_pairing_runtime` - **60 new
checks** (the binary now prints `xmem_test: OK (401 checks)`, from 341):

| check group | what it proves |
|---|---|
| switch parsing (11) | unset/empty/`0`/`off`/an unknown word are off; `on`, `1`, `ON`, `yes`, `true` are on; a longer word is not; the trace switch is separate and `0` is off |
| the reservation shapes, the pair rule and the translation (16) | only the two measured protections are recorded; the four conditions (same size, CPU side readable, GPU side `PAGE_NOACCESS`, contiguous) pair, and each one refused on its own; a wrapping CPU range is refused; the translation is the same offset and never past the reservation's end |
| the bounded table (17) | the measured sequence pairs and resolves to the CPU view; the answer is persistent; a request crossing the end and a VA in an unpaired view are the named refusal; a VA outside every view keeps the caller's source; the newer of two candidates pairs; pairs, unpaired ranges and candidates are bounded and a past-bound record is refused; `clear` forgets everything; the CPU-half span test the boot's page count uses |
| the runtime pair through the shim (16) | with the switch off the same two windows are made and a GPU-view VA is **not** resolved and nothing is recorded; with it on the CPU window followed by the GPU view is recorded as a pair, a physical page mapped into the CPU half is read through the resolved GPU-view VA (the title's bytes, not the window's), a past-end request is the refusal, the counters move, turning the switch off makes the recorded pair inert and turning it on again answers from it (process lifetime), and a lone GPU-only window with no same-size CPU candidate resolves to the refusal |

`port/test/d3d12_vulkan_device_test.cpp` (GPU) extends the image-memory case with the pairing path, using the test's own
resolve stub (the binary links no xmem.cpp): a placed texture at a VA the stub maps to a CPU sibling is imported from the
**sibling** (whose texels are the expected pixels while the GPU-view buffer holds a different colour), the per-submission
refresh follows a rewrite of the sibling, and a texture at a VA the stub answers `gpu_only` for is **not** filled from
the window. The earlier placed-texture checks still exercise the pre-port path (the stub answers "unavailable" for their
addresses). The run (`PALMETTO_IMAGE_MEM_ONLY=1`) exits 0 and prints `pooled and deferred image memory on the GPU: 21
small textures shared blocks and round-tripped, a title-placed texture was backed at its first submission and imported
its pages`.

### 11.4 Boot measurement

One 300 s light arm (`boot_arm.ps1` content environment, `-Light`, the class-0xa mitigation `PALMETTO_TITLE_CLASS0A_SIZE=1`,
the boot slot held by `_setup\boot_lock.ps1`) with the port's switches: `PALMETTO_TEXTURE_IMPORT=1`,
`PALMETTO_PLANE_REFRESH=hash`, `PALMETTO_PLANE_REFRESH_TRACE=1`, `PALMETTO_GPU_VIEW_PAIR=on`,
`PALMETTO_GPU_VIEW_PAIR_TRACE=1` and `PALMETTO_XMEM_LAYOUT=log` for the reservation bases. Outputs are outside the
repository (`D:\leonida-work\fsrstable\boot-gpuview2`: `summary.txt`, `stderr.log`, `boot.jsonl`, and
`capture-gpuview2/index.txt`). A first arm was discarded before this one: its extra-environment list was passed as one
comma-joined string, so only the first entry reached the process (the pairing switches and the class-0xa mitigation never
did); it ran with the pairing off and stopped in the class-0xa family at 62.5 s, and is not part of this measurement.

| measure | value |
|---|---|
| run | **STOP 60.4 s**, one exception: `0xc0000005` execute at `0x0` at trace t 59.1 s; **0 device-lost lines**; 1,549 `PresentX` ok and 2 failed (the failures at the stop) |
| the pair, recorded | **1 pair**: `GPU-view reservation 0x1e7bc2a0000 (0x600000000 bytes, PAGE_NOACCESS at caller game+0x1e1d03e) is the view of the CPU reservation 0x1e1bc2a0000 (0x600000000 bytes)` - the measured shape, recorded by the port's own rule |
| candidates recorded | 7 CPU-readable reservations: the 24 GiB CPU half, the five physical-page windows the title reserves, and the 0x185c00000-B page-pool placeholder |
| placed textures | **64** creation lines in the window (the log's bound; the 60 s layout boot has 30): **0** inside either half of the pair, **64** inside the committed 2 MiB-page heap (`0x1ee05800000`, 0x89800000 B served) |
| the pairing's answers | **0** placed VAs resolved through the pair, **0** refused (a GPU view with no pair), **0** CPU-half pages mapped: the pair had no population in this window |
| imports | **0** imported: 32 all-zero-head waits, 24 give-ups after 512 attempts, 5 refusals at the 16 MiB cap - the rank-4 population and answers, unchanged |
| stage and state | the stopped arm had run the stage-3 content draws (51,786 graphics PSOs created, submissions 1/16/256/1024 at 0.0/3.7/7.2/15.9 s); the last state trace at 58 s reads movie player state `0x3`, handle 0, `buffers 0` - **the movie window (state `0x9`, 118 s+) was not reached** |
| captures | 8 presenter captures (frames 1-1401), each 2564x1421 with mean rgb **0 0 0** - the honest-black frame the earlier pages measured |
| the stop | a title-side null execution at `0x0`, not a device loss and not a layer refusal; the discarded pairing-off arm stopped at 62.5 s on the documented `game+0x13360a3` read, and this arm's stop is a different title-side null execution |

**The verdict is a measured readiness negative.** The mechanism is live and bounded: it recorded the title's measured
pair, named its candidates, resolved no placed texture (0 resolved, 0 refused: every attempt got the pre-port answer)
and left the boot's present/import population exactly as the rank-4 boot measured it. What it could not show is the
fix: no placed texture of this window is in the GPU view, and the arm stopped before the movie window where the peers
place the video planes there. The counters that would show the fix are the ones the next arm reads: a `resolved` count
above 0 and a `GPU-view reservation ... resolves through` line, or a `changed`/`uploaded` pair on a texture resolved
through the pair.

### 11.5 Limits and the next falsifier

- **The movie window was not reached.** The arm stopped at 60.4 s on the title's own null execution (the documented
  stage-3 stop family), before state `0x9`; the pair's population where the peers document the video planes is
  **unmeasured** here. The next arm: a 300 s light boot on a settled tree with the same switches, reading the pairing
  line and the `resolved`/`refused`/CPU-half-page counters.
- **The title's page pool is not in the pair.** In the measured window the 99,776-page pool the page-mapping call names
  is mapped into a separate CPU-readable placeholder, and 0 pages are mapped into the pair's CPU half. Whether a later
  streaming phase maps into that half is unmeasured; the `gpu_view_cpu_map_pages` counter and its first-window line are
  the instrument (they printed no line in this arm).
- **The rule is the narrow measured one.** A same-size CPU/GPU reservation pair that is not contiguous is recorded as
  unpaired and named by the resolve refusal; if a later boot shows that shape while the title clearly places resources
  there, the measured answer is the peers' order-only rule and the boot's line is the evidence to change it with.
- **The table is append-only and bounded** (8 candidates, 8 pairs, 8 unpaired views), because the shim never sees the
  title's `VirtualFree` of these windows; a re-created span within the process lifetime is a record, not a translation
  of freed address space.
- **A texture larger than its reservation** is refused before any read (the request must fit the pair); the import's
  own 16 MiB cap still runs first, so pairing does not change the population section 8 sizes.
- **Buffers are not covered.** The measured slot-133 placed buffer is in a mapped physical-page window; the placed-buffer
  path keeps its own import/refusal and a buffer placed in the GPU view would still be refused (its next falsifier).
- **The GPU test's stub** proves the device path (resolve, import from the sibling, refresh through it, refusal) on
  synthetic addresses; only the boot can prove the title's own addresses.
- **The port's default is off** and stays off until a boot shows a placed resource in the pair; the sentence that would
  make it the default is "the movie planes are placed in the GPU-view reservation and the imported image matches them",
  which is a pixel claim this arm cannot make.

### 11.6 Cross-check against the peers (reference only, no code copied)

The peers were read at the refreshed commits in section 1's table (Five Star `ce2b333b`, PercyScarlett `e44c465`,
VIctory `63bcca4`, victory-fork `9a9091d`, XCL `623d02e`); their code is evidence for the mechanism class, and none was
copied.

| peer mechanism | what this port does | difference |
|---|---|---|
| XCL `kernelx::mirrors` (`623d02e`): a 24 GB reservation of 64 KiB pages made twice, `PAGE_READWRITE` then `PAGE_NOACCESS` \| `PAGE_GRAPHICS_NOACCESS`, physical pages mapped only into the first, resources placed at the same offset in the second, `source()` naming the CPU view behind a GPU-view address | the same measured arrangement recorded by our own table; the texture import and its refresh read and compare the CPU address at the same offset | their title-texture path reads and watches through `source` for any title-written texture; ours resolves only a placed texture's console VA, in the import/refresh the section-8 port built. Their pair rule also takes the most recent same-size CPU view without requiring contiguity; ours keeps the measured contiguity and refuses the rest by name |
| Five Star `XMemReservation.CpuView` (`ce2b333b`): "the CPU reservation of the same size made just before it", with the GPU reading its pages at the same offset | the same rule, with the measured contiguity added and the table bounded | their reservation objects own the split/coalesce machinery; ours only records the pair and never maps the GPU view, which their host must |
| VIctory `63bcca4`, victory-fork `9a9091d`, PercyScarlett `e44c465`: the inherited runtime's title-texture path (section 2.2 item 3) and the overlay plane | nothing of their texture path is re-used here; the per-submission refresh they carry is section 8's port, which this one feeds | the fork's `inplace` heap is a different mechanism (title pages opened as a heap), not a pair |

One agreement is worth recording as a measurement rather than a class: XCL's written figure is a 24 GB pair, Five
Star's is a 6 GiB streaming range; **this title's measured pair is 24 GiB** (`0x600000000` B), which is XCL's figure and
not Five Star's.

### 11.7 Attribution

This port was implemented from this project's own measurements and boot logs. The behaviour was cross-checked against
the public GPL projects named in section 1 at the commits there - **XCL** (`623d02e`,
`crates/xcl/src/hle/kernelx.rs`, the `mirrors` module) and **Five Star Recomp** (`ce2b333b`,
`src/FiveStar.Loader/Shims/XMemReservation.cs`, `CpuView`) - as evidence for the mechanism class, with
**VIctory** (`63bcca4`), **victory-fork** (`9a9091d`) and **PercyScarlett** (`e44c465`) read for the inherited texture
path the refresh of section 8 already credits. **No code from those projects was copied.** The upstream projects remain
the copyright of their contributors and their licences continue to apply to their own trees. The `NOTICE.md` row for
this port is owed by the lead (that file is outside this page's write scope): the licence text of
[upstream_port.md](upstream_port.md) section 6, listing the two projects named here with the paths and commits.

### 11.8 Uncommitted files of this port

Implementation: [xmem.h](../port/shims/xmem.h), [xmem.cpp](../port/shims/xmem.cpp),
[d3d12_vulkan_device.cpp](../port/shims/d3d12_vulkan_device.cpp). Tests: [xmem_test.cpp](../port/test/xmem_test.cpp),
[d3d12_vulkan_device_test.cpp](../port/test/d3d12_vulkan_device_test.cpp). Docs: this page. Nothing else is touched:
`port/src/console_va_map.h`, `port/src/buffer_va_index.h`, the CMake files, `NOTICE.md`, `docs/README.md` and the other
docs pages are unchanged by this port (the working tree's other modified files belong to the other in-flight slices).
No git write was made and no peer code is in the tree.

## 12. Implemented port 4: the XG layout and upload coverage for the movie-plane shapes (section 4 item 7)

**Status: implemented behind an opt-in switch, tested GPU-free, and measured with the census instrument on one light
A/B pair of boots that stopped at the shared tree's stage-3 fault at 63-65 s (plus one earlier arm).** This is the
rank-7 port of section 4, item 7. It answers the conditional in section 3.1's "XG plane fill" row: whether the movie's
three linear planes need a shape the shim refuses. The measured answer has two halves. **The records are already
covered:** the movie planes are three single-level, single-slice R8 linear textures (one 3840x2160 luma plane and two
1920x1080 chroma planes), so `XGCreateTextureComputer` accepts all three and the slot-2 record describes them, the
chroma pair at the linear layout's 2048-byte padded row pitch. **The upload was not:** slot 8 placed the caller's bytes
flat and used neither the caller's row pitch nor the record's, so the two 1920-wide planes' rows would land at 1920
bytes instead of the 2048 its own record reports. The port places a served description's one level row by row at the
layout pitch behind `PALMETTO_XG_MOVIE_PLANES`, names the calls it refuses, and leaves every shape its linear model does
not lay out on the tree's flat path. The code is our own: [xg_xs.cpp](../port/shims/xg_xs.cpp), the small
[xg_xs.h](../port/shims/xg_xs.h) switch/test seam, and the slot-2 record and its existing switch are unchanged. No peer
code was copied and no game data is in this port; section 12.6 repeats the attribution.

### 12.1 The measured coverage and the movie-plane shapes

The coverage half is read from this tree's shim and its tests (docs/texture_layout.md is the earlier measurement of the
record and of the refused calls). The shapes half is measured elsewhere and only cross-checked here, because the movie
draw is still not issued: the public reference's movies page measured the launch movie's three rotating plane sets -
**one 3840x2160 and two 1920x1080 linear R8 planes per set** - into the slot-133 texture uploader, with
`VS_Bink`/`PS_BinkNoAlpha` binding the three plane descriptors; this tree's own archive index measured exactly one 4K
stream, the 450-frame publisher stinger ([movies.md](movies.md)), and a 4:2:0 3840x2160 stream's planes are one
3840x2160 luma plane and two 1920x1080 chroma planes. Our own player fields pin where our chain stops instead: in
every arm of this port the player state byte read `0x3` with handle 0 and `buffers 0` (the earlier long arm's playing
state `0x9`, [decomp_first_ui.md](decomp_first_ui.md) sections 17.4 and 18.4, is not reached at this frontier), and
frame-buffer registration `0x442F760` and the per-frame decode `0x2EF0E0` did not run, so **no movie-plane description
was created or uploaded in any measured window.** The gap is therefore established by the record/upload arithmetic
above, not by a title call; section 12.5 states that limit.

| shape the family is asked about | slot-2 record before this port | slot-8 upload before this port | slot-8 upload with this port |
|---|---|---|---|
| one-level, one-slice 1D/2D texture, explicit linear layout `0x100`, whole-byte single-plane format (the shape class of every served shape in the measured window) | filled (header + one plane + one mip level) | flat copy of the caller's byte count at the source's offset; the caller's row pitch is not read | the record's rows are placed row by row: source stride = the caller's row pitch, destination stride = the record's padded row pitch, `row_bytes` per row written, the padding left alone |
| the movie luma plane 3840x2160 R8 | filled: row pitch 3840 B, level size 8,294,400 B | correct by construction: the packed row pitch 3840 B equals the layout pitch | same bytes (the rule is the identity when the two pitches agree) |
| each movie chroma plane 1920x1080 R8 | filled: row pitch 2,048 B, level size 2,211,840 B | **wrong: a 1,920 B packed source row is written at 1,920 B, 128 B short of every layout row after the first** | rows land at 2,048 B; 2,211,840 B of layout, 128 B of padding per row never written |
| multi-mip, arrayed, 3-D, MSAA, render-target/depth-stencil, swizzled layout (`0x10a` and the rest), planar/96-bit formats | refused by name (existing, unchanged) | flat copy (the upload loop ignores the create HRESULT; existing, unchanged) | flat copy, byte for byte (the switch does not change a shape its model has no layout for) |
| slot 10 `GetResourceTiling` (a 64 KiB-tiled resource's list) | n/a | refused by name (existing, unchanged) | refused by name (unchanged) |

The gap in one line: **3 of 3 movie-plane records are covered and 1 of 3 movie-plane uploads is covered**; the
uncovered pair is the two 1920-wide chroma planes, 2 of the 3 planes in each of the three rotating sets.

The census instrument (section 12.2) measures what the title's upload path actually asks for in a boot. In the
content-window arm (`boot-xgplan3`, switch off, 64.8 s) it registered **128 distinct description shapes** (the
instrument's bound) and printed 123 complete shape lines (the rest were lost to interleaved stderr writes): **116
refused by name at the create** - 54 swizzled layout (`0x10a`, the 64 KiB modes), 42 mip counts, 19 array sizes, 1
3-D - and **7 served** linear one-level shapes (256x128 R8; three 1x1 four-byte defaults; a 1D 256x1 four-byte image;
2048x2048 and 1024x1024 one-byte images). At the last printed counter milestone the family had seen **suggest 104,
create 68, slot-2 2, slot-8 137 copies / 332,156 bytes, slot-10 8**; the copy census attributes **42 of those 137
copies to served shapes** (26 of the 1D 128x1 one-byte image at a 128 B source row pitch against the record's 256 B
row, 14 of the 1x1 defaults, one 256x128 R8, one 1D 256x1) and **95 to shapes the model refuses** (arrays and mip
chains, flat path in both arms). Every served copy is a single-row copy except the 256x128 R8, so the only shape whose
source row pitch differs from the record's pitch is one row tall: the row-aware rule and the flat copy are the identity
there, which the A/B confirms byte for byte. The 1920-wide chroma shape is **not** in the window: no measured copy
places a second row at a pitch different from its source row pitch (the one shape whose source pitch differs is one
row tall), so the padding placement the port exists for is unexercised on the title.

The later-population arm that reached further before the shared tree's fault (`boot-xgplan1`, 60.7 s) saw a larger
population - **suggest 112, create 270, slot-2 4, slot-8 146, slot-10 179, 332,340 B** - and is where the census's
larger bound and the two refusals in section 12.4 come from. The **slot-10 calls are the surprise**: the earlier
150 s layout page measured 0 in its window, and the content environment reaches **179** `GetResourceTiling` calls,
which the shim refuses by name because it models no 64 KiB tile list. That is a content-texture gap, not a movie-plane
shape (a linear R8 plane has no tiles), and it is named as a next slice in section 12.5.

### 12.2 The design

Our own mechanism in the tree's terms: the slot-2 record already says where a linear subresource's rows are, and the
upload now places the caller's rows at exactly that geometry. One model - `LinearLayout` in
[xg_xs.cpp](../port/shims/xg_xs.cpp) - decides both the record and the copy, so the two cannot disagree again.

| switch | effect |
|---|---|
| `PALMETTO_XG_MOVIE_PLANES` | unset, empty, `0`, `off`, `no` or any unknown word: the tree before this port, byte for byte (the flat copy, its trace, its counters). `on`, `1`, `yes` or `true` (any case): a description the linear model lays out is copied row by row at the record's pitch. Read once, on the first copy. |
| `PALMETTO_XG_TRACE` | unchanged switch. It now also prints the census: one line per distinct description shape (its words, and the linear model's verdict with the row pitch and level size it would report), a line at each power-of-two copy number of a shape (plane, subresource, the caller's row pitch and byte count, sources after the first, negative sources), and the shape count on the counters line. Trace-only: with the switch unset nothing is counted and nothing is printed. |

- **Rows, not bytes.** The served description names one level of one slice: `row_bytes` = padded width in elements x
  element bytes (a 4x4 block row for the BC formats the model serves), `rows` = padded height in elements, the
  destination stride is the record's `pitch_bytes`, the source stride is the caller's row pitch. Only `row_bytes` per
  row is written; the padding bytes the layout adds are not touched.
- **The measured shapes stay byte-identical.** The window's copies are 128 rows of 256 B at 256 B, and single rows of
  4 B, 128 B and 1024 B - the rule and the flat copy write the same bytes for all of them, and the A/B's 18 counter
  lines are numerically identical (section 12.4).
- **Shape coverage is unchanged.** A description the linear model refuses (multi-mip, array, 3-D, MSAA, target,
  swizzled layout, planar/unknown format) keeps the tree's flat path, byte for byte. The switch does not change a
  shape it has no layout for; the census names those shapes and the two refusals below are the only calls the rule
  itself declines.
- **The refusals are named and write nothing.** A served description asked for a plane other than 0, a subresource
  other than 0, a second source after the first, a source row pitch smaller than a row (or 0), a byte count that does
  not hold the description's rows, or a destination/source span that is not committed announces the field and the
  caller once (bounded to the first 8 of each kind) and returns `E_NOTIMPL` without touching a byte. The measured
  window's two such calls (section 12.4) are a source row pitch half the description's row; the console's own layout
  models refuse a row pitch smaller than a row as `E_INVALIDARG`, and the title's upload loop does not read slot 8's
  HRESULT, so a console-shaped library would leave those pages unfilled too.
- **The instrument is a measurement, not a behaviour.** The census counts only with `PALMETTO_XG_TRACE=1`; the
  default path's log and bytes are the tree's. The instrument is bounded: 128 shapes, the first 8 of each refusal kind,
  and a copy line only at powers of two per shape.
- **Where it runs.** In the same slot-8 entry the family already answers; no allocation, one cached bool branch on the
  default path. The switch is off until a boot shows a movie plane uploaded through it.

### 12.3 Tests

`port/test/xg_xs_test.cpp` (GPU-free) gained `checks_movie_plane_upload`, **36 new checks** (the binary prints
`122 checks, 0 failures` in the linear layout, up from 86; the refuse-mode run still prints 16, 0 failures):

| check group | what it proves |
|---|---|
| switch parsing (2) | unset, empty, `0`, `off`, `no` and an unknown word are off; `on`, `1`, `ON`, `yes`, `true` are on |
| the movie-plane records (9) | the two movie-plane descriptions are laid out and slot 2 fills them: 3840x2160 R8 with size 8,294,400 B, a 3840 B row pitch and 3840 padded width; 1920x1080 R8 with size 2,211,840 B, a 2,048 B padded pitch and its real 1920x1080 element extent - the record's own pitch is the number the copy must use |
| the row-aware copy (4) | every 1920-byte source row lands at the 2,048-byte row pitch (all 1,080 rows compared); the 128 B of padding per row is never written; the copy writes exactly the layout's 2,211,840 B and nothing past it |
| byte-identical measured shapes (11) | the aligned 3840x2160 luma shape and the window's 256x128 R8 shape produce the same destination bytes with the switch on and off, only the measured bytes are written, and the on/off comparison happens on two computers built from the same description |
| the named refusals (6) | a plane, a subresource, a second source, a row pitch under a row and a byte count under the description's rows are refused and write nothing |
| the tree's-path fallback (3) | a two-level description is still refused at the create, keeps the tree's flat copy with the switch on, and that fallback is the tree's byte range |
| the harness entry (1) | the group ran (the binary's own check line) |

The existing groups are unchanged: the layout record's 26 field checks, the nine create refusals, slot 8's flat
subresource-offset behaviour, slot 10's refusal and the release/slot-count checks all still pass. The switch's
environment read and its parser live in [xg_xs.h](../port/shims/xg_xs.h) so the test binary and the runtime share them.

### 12.4 Boot measurement

The shared tree was under the other in-flight slices' edits (this port's working tree carries the other agents'
`main.cpp`, `xmem` and `dstorage` changes; they are not this port's), so the arms are the honest shared-tree
measurement: one 150 s light boot budget was intended, the tree stopped earlier on its own stage-3 fault, and three
arms were run around it. Environment for every arm: `boot_arm.ps1`'s section-12 content environment, `-Light`, the class-0xa
mitigation `PALMETTO_TITLE_CLASS0A_SIZE=1`, `PALMETTO_XG_TRACE=1`; the boot slot held by `_setup\boot_lock.ps1`, one
arm at a time. Outputs are outside the repository (`D:\leonida-work\fsrstable\boot-xgplan1`, `-2`, `-3` with their
`capture-*`/`readback-*` dirs).

| measure | `xgplan1` (switch on, first instrument) | `xgplan2` (switch on, final instrument) | `xgplan3` (switch off, final instrument) |
|---|---|---|---|
| run, verdict | STOP **60.7 s**, 1 exception: `0xc0000005` read at `game+0x4d9f636` address `0x169000007d6`; 0 device-lost lines | STOP **63.1 s**, 1 exception: `0xc0000005` write at `game+0x1db3ce6` address `0x9cd15a8e74`; 0 device-lost lines | STOP **64.8 s**, 1 exception: `0xc0000005` read at `game+0x28cf870` address `0x5c`; 1 `[rt] replay: the Vulkan device was lost` line at trace t 67.4 s before it |
| trace / d3d12 calls | 6,616 events / 138,434 | 6,644 events / 133,916 | 7,180 events / 135,666 |
| last printed XG counters | suggest 112, create 270, slot-2 4, slot-8 **146** (144 copied), slot-10 **179**, copied **332,340 B**, 32 shapes then + 197 over that instrument's bound | suggest 104, create 68, slot-2 2, slot-8 **137**, slot-10 8, copied **332,156 B**, 21 shapes at that milestone | the same 18 counter lines, numerically identical to `xgplan2` |
| census | 32 shapes (bound) | 128 shape lines (bound) | 128 shape lines (bound) |
| the rule's refusals | **2 of 146**: a source row pitch 1024 B for a 2048 B row and 512 B for a 1024 B row (the description words were not printed by that instrument) | none reached (the arm stopped before those calls) | n/a (switch off) |
| state at the last sample | player state `0x3`, handle 0, `buffers 0` at 60.0 s | player state `0x3`, handle 0, `buffers 0` at 60.0 s | player state `0x3`, handle 0, `buffers 0` at 62.0 s |
| presenter captures | last frame 1601, every capture 2564x1421, mean rgb **0 0 0** | 9 captures, last frame 1601, every one 2564x1421 mean rgb **0 0 0** | 10 captures, last frame 1801, every one 2564x1421 mean rgb **0 0 0** |

**The A/B verdict is a measured readiness negative with one proved property.** The switch-on and switch-off arms
produced **numerically identical XG counters at every one of their 18 printed milestones** (suggest, create, slot-2,
slot-8, slot-10, copied bytes and shapes) and identical state, stop family and black captures: on the population the
title actually asks for in this window, the movie-plane rule changes no byte and costs no call. The rule ran on the
served copies (the trace's 42 served copies in `xgplan2`: 41 single-row ones and the 128-row atlas whose source pitch
equals its layout pitch) and wrote what the flat path wrote. What the
arm cannot show is the fix: the movie window is upstream of the draw that never issues (player state `0x3`), so the
1920-wide chroma shape was not requested and the row-aware placement is GPU-free-tested, not boot-proved.
`xgplan1` is the arm that reached the later population, and its two refusals are the rule's only observed behaviour
change: a caller passed a source row pitch half the description's row, which the rule names and declines (the
console-shaped model refuses that argument too, and the title ignores the HRESULT), where the tree's flat path would
have copied a mis-strided range. The later arms did not reach those two calls; the census now prints the description
words of any such refusal, so the next arm that reaches them names the shape.

The stop is not this port's: the shared tree's arms in this window stopped on title-side faults at
`game+0x86f9df`/`0x86f981`/`0x4d9f636` (the other slice's control arms), one pre-hybrid binary on the same host ran
ALIVE 130.2 s, and the three arms here stopped on three different title sites (the switch-off arm after one host
device-loss line, the other two without). The two final arms ran the **same binary** (`palmetto.exe` sha256
`5B13C561...`), so the A/B is one binary with one switch changed; the gate build's binary differs from it only by the
census's trace-only gating (the trace-off path counts nothing), not by the switch-on path these arms measured. The
measured XG population and the A/B identity do not depend on where the arm stopped; the movie window does.

### 12.5 Limits and the next falsifier

- **Readiness, not an observed fix.** No movie-plane description was created in any measured window (the draw that
  would register the frame buffers and create the planes is not issued), so the port's row-aware placement is tested
  on synthetic subresources and measured for identity on the title's own served shapes, not observed on a movie plane.
  The next falsifier: a boot in which `0x442F760` (frame-buffer registration) or the decode wrapper `0x2EF0E0` runs.
  The census then prints the movie plane's own description words and the slot-8 rule line names its rows; if the
  chroma planes' source row pitch is 1920 B the rule is what makes their pages agree with the record.
- **The movie-plane shapes are a second-source measurement.** They come from the public reference's measured movies
  page (three rotating sets of linear R8 planes) and this tree's own archive index (one 4K 450-frame stream, hence
  one 3840x2160 luma and two 1920x1080 chroma planes at 4:2:0). Our own title has not been measured creating them.
- **The 256-byte linear row padding is peer/reference evidence, not our own record read.** Every record our title
  reads in a measured window has a row bank that is already 256-byte aligned (2048, 1024, 256, 128-wide-but-padded
  cases are the one-row 1D images), so the title has not exercised the padding either. Falsifier: a boot that reads a
  layout record for a texture whose packed row is not 256-byte aligned and whose CPU code copies at the record's
  pitch, or a console-library oracle answer for such a description. If the console's linear layout does not pad, the
  record and the row-aware placement would both be 128 B wrong for the 1920-wide plane and the measured answer would
  be the packed pitch.
- **The consumer's stride is a second file.** The per-submission import of section 8 reads a title-placed texture's
  pages as a tightly row-major footprint (`r.row_bytes * r.rows`, docs/texture_import.md), while the XG record and the
  slot-8 placement now use the 256-byte padded pitch. For the 1920-wide plane the two disagree by 128 B a row. This
  port fixes the XG side; the import (section 8's file, not this port's) inherits the same geometry when a sampled
  movie plane actually runs. Falsifier: a boot whose tracked placed texture imports pages the XG rule wrote at the
  padded pitch; the import then needs the record's pitch, which is a named next slice.
- **The two refusals of `xgplan1` are an unmeasured shape.** A source row pitch smaller than the description's row is
  declined by name (the console-shaped model refuses it), where the tree's flat path copied it. The description words
  of those two calls are missing from that arm (the earlier instrument did not print them); the current instrument
  does, and the next arm that reaches the later population names the shape.
- **The census is bounded and trace-only.** With 128 shapes it filled its bound in the content window, so any further
  shape is counted but unnamed; the instrument is a diagnostic, not a coverage promise.
- **Slot 10 is the content window's real coverage gap, and it is not movie.** `GetResourceTiling` is called 179 times
  in `xgplan1` where the earlier page measured 0, and the shim refuses every call (no 64 KiB tile list is modelled).
  The failed creates for the 54 swizzled `0x10a` shapes are the same family; neither is on a linear movie plane's
  path. The next slice is the tiling/64 KiB cover for content textures, not this port.
- **The switch stays opt-in.** The sentence that would make it the default: a boot in which the movie display job runs
  and its plane slot-8 calls pass a source row pitch equal to the packed plane width; the rule then writes the
  record's rows and the import shows the plane. Until then it is off, and its switch-off path is the tree's.

### 12.6 Cross-check against the peers (reference only, no code copied)

The peers were read at the refreshed commits in section 1's table; their code is evidence for the mechanism class and
none was copied. The XCL and Five Star XG material is on the same retail executable's call sites (the RVAs in
[texture_layout.md](texture_layout.md) match the XG page of Five Star's docs), which is why it can be cross-checked
against our own measured call shapes.

| peer mechanism | what this port does | difference |
|---|---|---|
| Five Star's XG page and its `Shims/Xg` writers (`ce2b333b`): the layout rules its writers fill, including "linear (mode 0, or a 12-byte element): mips in order from mip 0, rows padded to 256 bytes, mips on 512 bytes"; the same game call sites (`0x344235`, `0x979c75`, `0x16f7141`, `0x5121a5e`, `0x4eccca2`, `0xc4b3ce`) | the same linear row rule in our record, and the upload now places the caller's rows at that pitch instead of by a flat copy | Five Star lays out the whole console tiling (mip tails, 4 KiB/64 KiB modes, depth-stencil second planes); our model keeps the measured linear case only and refuses the rest by name - deliberately, because our backend's textures are row-major and the peers' full model is not needed to make a linear R8 plane's rows right |
| XCL `crates/xcl/src/hle/xg.rs` (`623d02e`): the layout record written from its GFX10 model, and `copy_into`/`copy_from` copying the linear rows row by row with the caller's row pitch into the layout's pitch | the same row-by-row placement for the served shape, with the geometry taken from the record our own model writes | XCL answers swizzled modes from its model; we refuse them. XCL's `title_textures` upload (the per-submission refresh) is section 8's port, which this one feeds |
| the public reference's `docs/movies.md` at `273b3bdf9`: the launch movie's three rotating sets of linear R8 planes (one 3840x2160 and two 1920x1080) reaching the slot-133 uploader | the movie-plane shapes this port is sized against, cross-checked with this tree's own archive index (the one 4K 450-frame stream) | the reference reaches the movie window; our tree does not yet. This is a shape measurement, not a code mechanism |

One disagreement is recorded as a measurement: the content window's 54 swizzled shapes all name layout `0x10a`
(64 KiB mode 10 in the peers' numbering), which confirms the peers' swizzle-mode encoding in our own title's data
while our shim refuses that mode by name - the refusal is the earlier page's measured linear-only answer, not a new
gap.

### 12.7 Attribution

This port was implemented from this project's own measurements and boot logs. The behaviour was cross-checked against
the public GPL projects named in section 1 at the commits there - **XCL** (`623d02e`,
`crates/xcl/src/hle/xg.rs`, the layout record and the row-by-row linear copy) and **Five Star Recomp** (`ce2b333b`,
its XG page and `src/FiveStar.Loader/Shims/Xg/`, the measured linear row/pitch and mip rules on the same executable)
- as evidence for the mechanism class, and the movie-plane shapes against the public GPL-3.0 reference
`ubiquitarianism/leonida` `docs/movies.md` at `273b3bdf9` (the page [movie_path.md](movie_path.md)
already cites). **No code from those projects was copied.** The upstream projects remain the copyright of their
contributors and their licences continue to apply to their own trees (XCL GPL-2.0-only; Five Star and the reference
GPL-3.0). The `NOTICE.md` row for this port is owed by the lead (that file is outside this page's write scope): the
licence text of [upstream_port.md](upstream_port.md) section 6, listing XCL and Five Star with the paths and commits
above.

### 12.8 Uncommitted files of this port

Implementation: [xg_xs.cpp](../port/shims/xg_xs.cpp), [xg_xs.h](../port/shims/xg_xs.h) (new). Tests:
[xg_xs_test.cpp](../port/test/xg_xs_test.cpp). Docs: this page. Nothing else is touched: the slot-2 record fill, the
CMake files, `NOTICE.md`, `docs/README.md` and the other docs pages are unchanged by this port (the working tree's
other modified files - `main.cpp`, `xgameruntime_offline.cpp`, `dstorage*`, `xmem*` - belong to the other in-flight
slices). No git write was made and no peer code is in the tree.


