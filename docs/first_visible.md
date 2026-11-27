# The peer ports' first visible frames, the ranked divergence with this tree, and the next five tests

**Status: research and ranking; no boot and no runtime code.** The peer ports publish first visible frames (first-run
screens, menus, a launch movie, a lit opening scene) while every frame this tree presents is black. Read against their
own records, that is not one picture bug. Their first visible pixels are the title's **2D UI, logo and video frames**,
drawn after their boot has left the background loop; the peers' documented earlier frames are black (four of the five
record it), and their 3D world is the *last* thing to become visible (five-star's still is not on screen). This page
quotes the peers' public status sentences as evidence, maps each claim onto this project's measured frontier, and ranks
the divergence:
**boot chronology and the content's producer first, the draw-state/colour feed third, the target/format measured equal,
the present path behind it, pipeline availability measured out for the passes we reach.** Section 6 turns the ranking
into five falsifiable tests; one is already being measured by the in-flight `contentblack` slice.

The evidence is each peer's public README, docs, status page and change notes, plus the public tracker snapshot. Peer
code, comment text, identifiers and tables are **not** copied into this tree; the sentences below are cited as another
project's published observation, and every conclusion is tied to a page of our own measurement. No game data, package
path, shader byte, extracted table or peer flag appears here. Tree `f22ff4d41` plus the in-flight slices' files; no boot
was run for this page, no CMake and no runtime file changed.

Peer clones and commits read (all outside the repository, read-only): five-star-recomp `0bfa755` (GPL-3.0-or-later),
XCL `6d12f58` (GPL-2.0-only), PercyScarlett `e44c465` (GPL-2.0-only), VIctory `13f91ca` (GPL-3.0-or-later),
victory-fork `9c184b3` (GPL-3.0-or-later), and the tracker snapshot (`page.txt`). The credit is in `NOTICE.md`.

## 1. The question, our measured state, and the two numbering systems

The reframed question is **"why do the peers' frames have visible content while ours are black"**, not "why is our
frame black in isolation". This tree's measured picture frontier, at the tree this page is written on:

| measurement | value (page) |
|---|---|
| the title's stage word `0x7446510` | reaches **3** (the running stage) 56.9-64.7 s into a content arm, then the boot stops 1.4-4.3 s later ([vertex_buffer_va.md](vertex_buffer_va.md) sections 10.3, 13.6, 16.5) |
| content command lists | 2-3 submissions, 6-9 draws, each a 3-draw submission: two draws into a 4K intermediate and one compose draw into a 4K display buffer, all format 37 at 3840x2160 ([vertex_buffer_va.md](vertex_buffer_va.md) sections 10.2, 13.6) |
| presented resource identity | the compose draw's target resource id is the id the present trace prints for the presented plane, so the draws bind the plane that is presented ([vertex_buffer_va.md](vertex_buffer_va.md) section 10.2) |
| content pixels | first capture per target reads `0 0 0`, `0/16,384`, up to 9 readbacks: no title pixel is measured in the window ([vertex_buffer_va.md](vertex_buffer_va.md) sections 10.2, 13.6) |
| copy path control | the layer's write clear on the same target reads `51 102 204`, `16,384/16,384`, peak 204, so the zero is content, not the copy ([vertex_buffer_va.md](vertex_buffer_va.md) sections 10.2, 13.6) |
| presenter captures | every capture is `mean rgb 0.00 0.00 0.00`; the stable pre-content loop's captures at the 2564x1421 window extent are the same ([frame_first_light.md](frame_first_light.md) section 4) |
| the draw's own inputs | the constant pass's 32-dword user data holds only the descriptor-table pair, the transform scalars `s8/s9/s10/s28` are unset, and the console driver-block fetch lands on the descriptor table ([frame_first_light.md](frame_first_light.md) section 6) |
| the constant pass's colour | the four dwords its pixel shader reads at `block + 0x20` are zero at the first frame and two later frames, and the byte-exact watch sees no write: black by design in the stationary loop ([colour_path.md](colour_path.md) sections 3.2, 5) |
| the compose pass's source | its only sampled T# resolves by exact resource id to the constant pass's own target, produced by our replay and empty ([frame_first_light.md](frame_first_light.md) section 5) |
| the console upscaler | the peer FSR precondition does not hold here: 0 of 29 FidelityFX programs is requested, the title's own upscale pass runs, and the layer's stretch stand-in leaves the frame black because its source is ([upscale_standin.md](upscale_standin.md) sections 2.2, 5) |
| the stop that ends the window | a title-side compile-time class 0xa: the image's own format table and a `mov eax,0xa` getter ask the title's allocator for 0 bytes and a 0x1000-byte copy overruns the block; no D3D12 description and no layer answer is in the chain ([vertex_buffer_va.md](vertex_buffer_va.md) section 16.4) |

Two numbering systems appear in this page and must not be mixed. **This tree's "stage 3"** is the title's own
game/stage word (`0x7446510`, meaning 1 initial, 2 init/load, 3 running, 4..6 later), decompiled and measured in
[state_machine.md](state_machine.md) section 2; the stage-2 handler and its boot sequence are [stage2_blocker.md](stage2_blocker.md).
**The tracker's "stage N"** is the public tracker's own seven-step reading of each project, not a title word. Its
legend, from the snapshot (standings): 1 Loads, 2 Starts up, 3 Graphics device, **4 First frames**, **5 Recognisable
picture**, **6 In-game**, 7 Playable.

| tracker step | projects the tracker places there (snapshot) | what our tree measures at the same step |
|---|---|---|
| 4 First frames | Leonida, Holy-Interoperability, **palmetto** | the frame loop and the content window: presents succeed, every capture and every content target is black ([frame_first_light.md](frame_first_light.md) section 4, [vertex_buffer_va.md](vertex_buffer_va.md) section 13.6) |
| 5 Recognisable picture | PercyScarlett, Scarlight, XCL, VIctory-macOS, five-star-recomp, Leonida Continued | not reached: the boot stops 1.4-4.3 s into the title's running stage ([vertex_buffer_va.md](vertex_buffer_va.md) sections 10.3, 16.4) |
| 6 In-game | klbf-lab, Flamingo, victory-fork, VI-BR-PORT, VIctory-Belobug-AMD and 21 more | not reached |
| 7 Playable | none | not reached |

The tracker's own row for this project reads: "Stage 4 of 7: First frames ... presents frames in its own window, with
the picture still black". The useful part of that ladder is the order: every project that shows a picture crossed the
black-first-frames step first, and the picture step is a *later* step with its own content and composition requirements.

## 2. What the peers' first visible frames actually are

| peer | first visible content, in the peer's own words | boot point and path | early black recorded | source |
|---|---|---|---|---|
| five-star-recomp | "It now draws its first real image: its UI layer and the intro video (Bink, decoded on the CPU and drawn with the game's own translated shaders) in the right colours. It then shows the Rockstar logo, the intro art and every first-run screen ... loads story mode and has shown the story intro's title card" | first-run screens answered by a script that reacts to what is on screen; "story load after about 57 s", and "after about 39 s" on a later boot with saved settings | "the game's window shows the plane, **black so far**" at the first present; a 120 s boot with pipelines "presents a frame every 33.3 ms ...; **the window stays black**" | status page lines 7-15; shim page, Present/Boot section |
| XCL | "its first menu is visible (2026-11-23) - the account prompt ... at 30 fps (632 frames in 30 s, 720 draws per 2 s)"; the README's status is "~4 s to its first frame, 30 fps in its menus" | first menu after the fix list; "first frame at 3.6 s" in the retail bring-up | "Then: 30 fps (1 449 frames in 60 s), **image black**" in the same section, before the menu | handoff, the retail bring-up section and the first-menu entry; README status |
| PercyScarlett | "reaches the opening prison scene, lit and textured"; "Menus, Bink2 video, save data and world streaming work" | press Enter through logos and menus; the opening scene is the first 3D content | "The first visit to a scene compiles its GPU pipelines: **the picture can be black or have missing pieces for a few minutes**"; the Series S world "was black" until the upscale takeover | prologue section "How the Series S black screen was fixed"; README status table |
| VIctory | first-boot screens (accessibility, language, settings, legal) at 30 fps; "Intro video with sound"; start screen, profile and saves; then the intro cutscene "textured and lit characters with whole hair, buildings, sky, depth of field" | menus at the title's 30 fps, then loading, then the cutscene; the prologue is reached at about 1 fps | no early-black statement in the status table; the recorded picture limits are "real queries, depth bounds, HDR and scaling are missing" | README status and subsystem tables |
| victory-fork | boot, menus and start screen at 30 fps; the opening cutscene "plays from start to finish, in order, lit"; the first gameplay scene "renders: characters, textures, sky, sunlit exterior; HUD, pause menu and pad input work" | same runtime as VIctory with picture and speed work on top | a rejected pipeline library saved empty made "the next run **drew black frames** at 0.7 to 1.5 frames a second while building them again"; a page-mapping change made "faces came out **black** or red" in 3 of 3 launches and was reverted | README status table and the newest-night sections |
| the public tracker's ledger | five-star "Shows every first-run screen, loads story mode and plays the opening cutscene's art ... and that the lit picture is still wrong"; XCL "goes on from its menus into its 3D world at about 5 frames a second"; PercyScarlett "reaches the opening scene ... lit and textured"; Leonida Continued "shows the launch movie, the first-run pages and the landing prompt" | the tracker's stage 5 for those four, stage 6 for the VIctory family | the tracker's 2026-11-24 10:27 entry: a project with "the first cutscene's opening credits and 5 minutes in the world" says "the 3D scene is drawn internally, but the lighting steps are not translated yet ... **The picture is still black**" | tracker snapshot, standings rows and the update entries |

Read as a statement, four things are consistent across the peers:

1. **The first visible pixels are 2D, not the world.** The order the peers publish is UI/video/logo/first-run screens
   first, then menus, then the cutscene's art and credits, and the 3D world last. five-star states the 3D position
   plainly: "The cutscene's 3D world now draws into the G-buffer (geometry is in place), but lighting and composition
   do not reach the screen yet" (README status). A drawn 3D scene is therefore not a visible 3D scene, and the tracker's
   10:27 entry records the same before the lighting passes translated: "the 3D scene is drawn internally ... The picture
   is still black".
2. **The peers record a black interval at their early presents.** The quotes in the fourth column are four of the five
   projects' own logs or guides, at their own boot points before the first-run flow (VIctory's README records no black
   frame; it lists missing HDR and scaling instead, and its forks record the black interval). The interval is not a
   property of a particular port: five-star records it per frame count (black at the first present and for a 120 s boot
   with pipelines), XCL per boot point (black at 30 fps before the menu), PercyScarlett per scene (black until the
   pipelines are built), victory-fork per regression (black while the pipeline library is rebuilt).
3. **The visible frames require the first-run/menu state, which requires more than the frame loop.** five-star's first
   visible frames are its first-run screens, answered by "a script that reacts to what is on screen", and its later
   boots skip them only because the saved settings exist; XCL's first menu is an account prompt; VIctory's are the
   first-boot screens and the intro video; PercyScarlett's are logos and menus. None of these is a frame the background
   loop draws.
4. **The peers' timing is not our timing.** Their published figures (XCL 3.6 s to the first frame, five-star story load
   at 57 s and 39 s) are their frames on their machines; the tracker marks them as the authors' own measurements. They
   are used here only to establish the *order* of the milestones, never as a target for our boot.

## 3. What their present path does that ours does not

All five peers and this tree present the title's own `PresentX` plane; the title creates no DXGI swapchain on either
side (this title creates none - [display_path.md](display_path.md) section 4 measures that the boot calls no
`CreateSwapChain*`), so each runtime owns the swapchain and the presenter.
The present paths are the same class of mechanism. The differences are conversion and composition downstream of the
plane, and they are worth naming because two of them are real picture differences:

| peer | how the title's plane reaches the window | resolution, format and colour space | upscale and video |
|---|---|---|---|
| five-star-recomp | its own flip-model swap chain on the title's own window; a copy when the client area matches the plane, else a full-screen triangle that samples the plane into the largest rectangle of its aspect ratio "over black (PresentScaler, black bars)" | boot plane is "the 3840x2160 render target ... (R8G8B8A8 family), color-space dword 0"; "FP16 gets scRGB, R10G10B10A2 with dword 12 ... gets HDR10, otherwise sRGB", set with the display path's colour space | a test-pattern present mode; the intro video is Bink decoded on the CPU and drawn with the game's own translated shaders |
| XCL | its own window and swap chain; the present hands the title's `PresentX` resource to the presenter "at once" | the presenter reads the console `PresentX` plane; the window is XCL's own | console FSR 4 not emulated: the FFX API is served with a simple upscale or a hand-off to the PC FSR; the package's Bink 2 decoder decodes the game's movies |
| PercyScarlett | "console present into a DXGI flip swap chain", with an overlay plane "blended over the game plane, with the full console plane description" | Series S profile renders at 720p and upscales to 1080p | the console FSR 4 pipelines cannot be made from the console-only deserialization, so the upscale dispatch is taken over and "a plain stretch of the colour image" is recorded |
| VIctory | "Draws, compute and presentation on host D3D12; only changed title memory is copied" | Series S uses the PC FidelityFX upscaler; the title's own upscaler DLLs have "six of their passes ... untranslated, so their picture is wrong" | an intro video with sound plays before the 3D content; HDR and scaling are listed as missing |
| this tree | the presenter adopts the title's own window (2564x1421 client measured) and blits the title's plane through a prepared presenter over the Vulkan swapchain ([window.md](window.md) section 5, [present_wiring.md](present_wiring.md) section 6); the plane's first resource is real and is the frame's own target ([surface_population.md](surface_population.md)) | plane format 37 at 3840x2160 (R8G8B8A8 family per [frame_capture.md](frame_capture.md) section 7), plane colour-space dword 0; the presenter has a real source-rectangle to destination-rectangle stretch ([upscale_standin.md](upscale_standin.md) section 2.3) | the console upscaler is requested by nobody in the window; the layer's stretch stand-in exists and is off by default; the Bink library is mapped and bound but the title never enters it ([movie_path.md](movie_path.md)) |

The measured equality matters for the ranking: the plane is a 4K R8G8B8A8-family image on both sides with colour
space 0, and our presented resource is the target the frame draws (verified by resource identity). A colour-space,
10-bit, HDR or letterbox difference *can* make a correct image display wrongly, but it cannot turn a target that reads
`0 0 0` on every texel into a visible picture. That is why the present path ranks below the boot chronology even though
the peers do more in it.

## 4. Mechanisms the peers name as required, split into "any non-black pixel" and extras

| mechanism | peers that name it | required for any non-black pixel? | this tree's measured state |
|---|---|---|---|
| the title's own 2D UI / first-run / logo draws | five-star (UI layer first real image, then logo, intro art, screens), VIctory (first-boot screens), XCL (account-prompt menu), PercyScarlett (logos, menus) | **yes, for their first visible frame**: it is the producer of that frame | not reached: our stage-3 window holds the two-pass background/upscale draws only ([vertex_buffer_va.md](vertex_buffer_va.md) section 10.2); no first-run UI class is asked for and the title never polls input ([first_run_path.md](first_run_path.md) section 2.2, [input_gate.md](input_gate.md)) |
| the title's movie (Bink) path | five-star (intro video in the first real image), VIctory (intro video with sound), XCL (Bink works; movies decode), PercyScarlett (Bink2 video works), victory-fork (cutscene plays) | **yes when the title's first content is a movie**, as in five-star's order | the library is mapped, its imports are bound, the archive opens, and no decoder entry point runs in 130 s ([movie_path.md](movie_path.md)) |
| the first-run / input / user / save progression | five-star (scripted gamepad answers the screens; saved settings skip them), XCL (goes past the first menu to its settings), VIctory (one signed-in offline user, saves that survive a kill) | **yes as a gate**: the visible frames are behind it | not reached: the title asks for no first-run UI object and polls no input in the measured window ([first_run_path.md](first_run_path.md) section 2.3, [input_gate.md](input_gate.md)) |
| the composition / lighting pass that turns a G-buffer into a picture | five-star ("the final composite fills the screen" after the game's own descriptor records are mirrored past the root signature's range; the lighting's R9G9B9E5 textures read through typed views), the tracker's 10:27 entry (3D drawn, lighting not translated, picture black), victory-fork (picture work gets the first room right) | **yes, for any visible 3D content** | no content pass exists in the window: the draws we see are the background/upscale pair ([frame_first_light.md](frame_first_light.md) section 3, [upscale_standin.md](upscale_standin.md) section 2.1) |
| the descriptor model: views as values, one shared shader-visible heap | XCL ("Why every frame was black: descriptors are values": a CPU-only heap's copied T# named a descriptor of the wrong heap, so shaders read zeros and the presented buffers stayed 0 including alpha) | **yes where the title copies descriptors into a heap** | our title's descriptor model is carried and measured (the console heap mirror, the per-draw tables and the resolved T#s); the measured gaps are named in [frame_first_light.md](frame_first_light.md) section 6 and [vertex_constant_feed.md](vertex_constant_feed.md), not a heap-index identity failure |
| format/view and pixel-format conversions (typed image views, target format views) | five-star (typed R9G9B9E5 lighting views), XCL (a UAV in B5G5R5A1 of a target the PC makes R8G8B8A8 removed the device; shader views take the PC format) | **yes where a shader reads a format the host view does not match** | the content/background passes' formats are translated and the readbacks run; no view-format stop is measured in the window |
| pipeline availability at the first frames | PercyScarlett (black or missing pieces while the first-visit pipelines compile), XCL (black or half-drawn until a pipeline is built; "not a bug" when the cache is cold), victory-fork (black frames while the pipeline library rebuilds) | **yes at the first frames**: an unbuilt pipeline draws nothing | measured out for the passes we see: slot 57 converts 10,241 of 10,241 records with 0 refusals and the graphics population is 51,786 ([frame_first_light.md](frame_first_light.md) section 2, [upscale_standin.md](upscale_standin.md) section 2.2) |
| an upscale takeover when the console upscaler cannot run | PercyScarlett (stretch), XCL (FFX served with a simple upscale), five-star (PC FidelityFX bridge), VIctory (PC upscaler directory) | **no for non-black**: it is needed for the correct upscaled size when the console upscaler is the only path | implemented as our own opt-in stretch and exercised; the frame stays black because the colour image is ([upscale_standin.md](upscale_standin.md) sections 3, 5) |
| present scaling, aspect ratio / letterbox, overlay plane, colour space | five-star (aspect-ratio scaler over black bars, sRGB/scRGB/HDR10), PercyScarlett (overlay plane), XCL (present window) | **no**: presentation correctness, not the producer of pixels | our presenter stretches and presents the plane; colour space 0 and 8-bit on both sides ([window.md](window.md) section 5) |
| HDR / 10-bit targets, depth bounds, real queries | VIctory lists them as missing; five-star's present maps FP16/R10G10B10A2 when the title asks | **no** | the title's plane is 8-bit format 37 in the measured window |
| ray tracing and streamed texture batching | every peer in the world still has black shadows; victory-fork's batching and card-copy work are speed and correctness of content already drawn | **no** for first light | deferred with content ([cleanroom_peers.md](cleanroom_peers.md) "The gaps that matter most") |

## 5. The ranked divergence with this tree

Ranked by how much of the measured gap each explains, strongest first. Every row is falsifiable by one measurement
named in section 6.

| rank | divergence | what the peers' evidence says | our measurement that bears on it | verdict |
|---|---|---|---|---|
| **D1** | boot chronology: the visible frame belongs to a later state, and our window after the running stage is 1.4-4.3 s | each peer's first visible frame is a first-run/UI/menu/movie frame, and four of the five peers record black at their earlier presents (section 2); a drawn 3D scene still needs the composite/lighting pass to be visible | the stage word first reads 3 at 56.9-64.7 s, the boot stops 1.4-4.3 s later, and only 2-3 content submissions exist ([vertex_buffer_va.md](vertex_buffer_va.md) sections 10.3, 13.6, 16.5); the background loop's black is by design ([colour_path.md](colour_path.md) section 5) | **strongest**: the peers' equivalent frames are black too; our black window is too short and too early for the frames they show |
| **D2** | the content's producer: the peers' first visible pixels are the 2D UI/video path, ours are the two-pass background/upscale loop | five-star's first real image is its "UI layer and the intro video"; VIctory's are first-boot screens and the intro video; XCL's is the account prompt; PercyScarlett's are logos and menus | our content window is 3-draw submissions into a 4K intermediate plus a 4K display buffer, the measured background/upscale pair ([vertex_buffer_va.md](vertex_buffer_va.md) section 10.2); no first-run UI class is asked for and no Bink entry runs ([first_run_path.md](first_run_path.md) section 2.3, [movie_path.md](movie_path.md)) | **strong**: the producer of "theirs visible" is a pass class our window does not contain yet, not a conversion of the pass we do contain |
| **D3** | draw state and colour feed: even a content pass would be black on our current data | the peers' enabling fixes at this layer are feed-shaped: five-star's buffer-view descriptor record was unfilled at first, so a UI batch program read row 0 of its CBV for every row and every UI vector collapsed onto one line; XCL's descriptor-heap identity fix took the presented buffers from 0 including alpha to visible | the constant pass's user data is 2 of 32 non-zero, the transform scalars `s8/s9/s10/s28` are unset, the driver-block fetch lands on the descriptor table, and its colour dwords are zero ([frame_first_light.md](frame_first_light.md) section 6, [colour_path.md](colour_path.md) section 3.3) | **real, but downstream**: it explains why a content pass would still be black after D1/D2 change, not why the frames we see are black now |
| **D4** | the target and its format | five-star's boot plane is the same 3840x2160 R8G8B8A8-family plane with colour-space dword 0; XCL and PercyScarlett present the console plane or a stretch of it | our presented resource is the frame's own 4K format-37 target, verified by resource id, and the copy path writes `51 102 204` into the same target ([vertex_buffer_va.md](vertex_buffer_va.md) section 10.2) | **measured equal**: target size, format family and plane identity are not the divergence |
| **D5** | the present path: own compositor, colour space, letterbox, overlay | five-star scales into an aspect rectangle over black with sRGB/scRGB/HDR10 colour-space selection; PercyScarlett blends an overlay plane; XCL presents through its own window | our presenter adopts the title's window and blits the title's plane; the plane's colour space is 0 and its format 8-bit ([window.md](window.md) section 5, [present_wiring.md](present_wiring.md) section 6); the presenter is now attested end to end with a synthetic non-black control and a measured fit/colour rule ([peer_port_first_ui.md](peer_port_first_ui.md) section 15) | **measured**: the synthetic control reaches the capture exactly, so a zero-source plane is the only reason the title frame stays black; fill/fit and the DPI-unaware window are recorded as opt-in/next (P-24/P-25 on the task board) |
| **D6** | pipeline and format availability at the first frames | PercyScarlett, XCL and victory-fork all record black while pipelines are missing or being rebuilt, and XCL records a format-view mismatch as the black/device-loss cause | our passes' records convert with 0 refusals and the graphics population is 51,786, and the window's measured stops are the title's own crash family, not a view-format refusal ([upscale_standin.md](upscale_standin.md) section 2.2, [frame_first_light.md](frame_first_light.md) section 2, [vertex_buffer_va.md](vertex_buffer_va.md) section 16.5) | **measured out for the passes we see**, untested for a content pass we cannot reach (T1) |

The boot chronology is therefore the answer to the reframed question, and it has a measured cause on our side: the
title's running stage begins and the boot stops 1.4-4.3 s later on a title-side zero-byte allocation, so the window in
which the first-run/UI/movie frames could be drawn never exists. The peers' own records show black frames at their
earlier presents, and their first visible content is a later state's; that comparison is what makes the chronology the
strongest divergence. The draw state is the next real gap, but it is the gap that would keep a *content* frame
black; it cannot make the title's own pre-content frame non-black, because that frame's colour input is zero by design.

## 6. The next five falsifiable tests, frontier-first

Each test names the mechanism, the file that would carry the measurement, what to measure and the observable that
decides it. None of them needs game data in the repository; all run on the user's own package and work folder.

| id | falsifiable test | mechanism | our file | what to measure | expected observable | in-flight coverage |
|---|---|---|---|---|---|---|
| **T1** | extend the life of the running stage and read the next content frames | the stop ends the window at 1.4-4.3 s after the stage word reads 3; the question is whether the title's own frames stay black past it | `port/src/main.cpp` (bounded probe/arm), `port/shims/d3d12_xs_device.cpp` (the class/allocator sites; owned by the classa slice) | a boot that lives at least 10 content submissions past stage 3 (or a no-op that lets the executor pass the 0x1000-byte copy site), with the first-capture readback of every target | stays `0 0 0` at the 4th and later submission: the title's frames are black that far, so chronology alone is not the whole answer; first non-black or a new pass's pipeline population: the early frames were transition/background frames | `contentblack` (task-135) measures the per-draw table and first-capture readback in the current window; `classa` (task-134) names the stop's class origin but does not extend the window |
| **T2** | ask whether the first-run/UI/movie producer exists in the title's state at all | the peers' first visible frames are the first-run UI/video path; ours would need the first-run/UI and movie producers to run | `port/src/main.cpp` (existing state trace), `port/shims/xmem.cpp` (the existing movie reach signal), `port/shims/xgameruntime_user.cpp` | the boot-media/UI active bytes (`0x71B90AD`/`0x71B90AC`), the first-run/UI call count, Bink entry counts, and the pipeline ids of the content lists | none of them: no visible-class frame can exist in our window, and the producer is behind the boot stage, not behind a layer conversion; any of them: an instrumentable producer appears and the next test is its draw | not covered: `first_run_path.md`, `movie_path.md` and `input_gate.md` already measured the negatives this test extends |
| **T3** | a synthetic non-black present control | the presenter and its colour-space/format conversion | `port/src/vulkan_backend.h` (presenter capture) and the existing layer write-control clear | write a known non-black value into the exact presented plane resource after the title's content draw, then read it back through a presenter capture (not through the copy path) | capture reads the written value: the present path and its colour space are not the divergence, and the black is source content; capture stays `0 0 0`: the presenter/conversion path is a real divergence and the next test is its format/colour-space mapping | **answered** by [peer_port_first_ui.md](peer_port_first_ui.md) section 15: the synthetic four-band pattern reaches the presenter capture with its exact values through the title's own present, so the presenter and its colour space are not the divergence and the black is source content |
| **T4** | the colour window on the first content frame | the constant pass's four colour dwords are the whole colour input of its pixel shader | `port/src/vulkan_replay.h` (the existing colour reader) and the existing colour-path watch | the block at `block + 0x20` and its byte-exact watch at the content draws, not only at the stationary-loop draws | still zero: the title has not populated the colour it would draw, which is the D1/D2 chronology; non-zero while the target stays black: the draw-state/feed gap is the cause | not covered (the existing measurement is the stationary loop's, [colour_path.md](colour_path.md) section 4) |
| **T5** | the content pass's draw state and feed | the unset transform scalars and the driver-block vertex-stream descriptor keep a pass's vertex stage degenerate | `port/src/vulkan_replay.h`, `port/shims/d3d12_com.cpp`, `port/src/graphics_pso.cpp` (as named in [frame_first_light.md](frame_first_light.md) section 10) | the vertex-path dump on the first content submission: `clip-position scalars`, the `s0/s1` fetch decode, and the 32-dword user data | degenerate scalars and the driver-block fetch landing on the descriptor table: a content frame would be black even after T1; non-degenerate: the draw state is clean and the next gap is the colour image or the pass order | `contentblack` (task-135) already records the content draws' root arguments, viewport/scissor and host pipeline state, which is this test's measurement |

Ordering rule: T1 and T2 answer the reframed question (whether the peers' visible frames are reachable at all in our
window); T3 closes the present path honestly with a control; T4 and T5 are the picture work that would be needed after
the window exists.

## 7. Limits, and what this page does not claim

- **The peers' stage-word series is not published.** The chronology argument rests on their documented content order
  (UI/video first, menus, cutscene, then 3D), their own black-frame records at earlier presents, and the tracker's
  step ordering - not on a stage-word measurement of theirs. It is a strong reading, not an equivalence proof.
- **Their timings are their machines.** The 3.6 s first frame, the 57 s and 39 s story loads and the frame rates are the
  authors' own figures on different hardware; this page uses them for the order of milestones only.
- **The tracker is a third party's reading.** Its rows are marked "source: project README" and its steps are its own
  scale; it is cited as one more reading of the same public claims, not as an authority.
- **Our measured state is the cited pages'.** No boot was run for this research slice; the content window, the black
  content pixels and the stop are the numbers of [vertex_buffer_va.md](vertex_buffer_va.md) sections 10-16, the frame
  content is [frame_first_light.md](frame_first_light.md), and the colour data is [colour_path.md](colour_path.md).
  The two slices in flight (`classa`, `contentblack`) own their files and are extending those measurements; this page
  does not duplicate or pre-empt them.
- **Their 3D picture is not claimed to be right either.** five-star's lit image is "still wrong" (its own status page)
  and every in-game peer still has black shadow or missing ray-traced light; the comparison here is about first visible
  content, not picture parity.
- **No peer code or text is copied.** The quotes in section 2 are the peers' published sentences cited to their files
  and commits; no code, identifier table, comment or measurement table from their trees is reproduced here, and
  nothing from their trees was written into this repository.

## 8. Files and gates

- **This page**: research and ranking only. No runtime, build, test or data file changed; no boot was run, so no
  `PALMETTO_OK`/`TESTS_BUILT` pair is claimed for this slice (the tree's build state is the other slices').
- **Index and credit**: one row in `docs/README.md` next to the picture-frontier pages, and the peer-evidence credit
  section in `NOTICE.md`.
- **Documentation checks** (`tests/test_docs_*.py`): all twelve pass on the tree this page was written on, run with
  the session's git config environment cleared. The tree also carries the `classa`/`contentblack` slices' uncommitted
  files (`port/src/main.cpp`, `port/src/vulkan_replay.h`, `port/src/vulkan_backend.h`, `port/shims/d3d12_*`,
  `docs/vertex_buffer_va.md`, `port/test/frame_dump_test.cpp`); none of them was touched here.
