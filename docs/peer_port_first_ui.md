# The peer ports' first-run, UI and present flow: per-peer evidence, the comparison with this tree, and the ranked port plan

**Status: research and ranking; no boot, no runtime code, no CMake, README or NOTICE change, no git write.** This page
maps how each of the five peer ports drives its title's first-run, UI and present flow from their own trees, what makes
their first visible frame appear, how that compares with this tree's measured flow (the screen owners run and nothing
presents; the movie player reaches playing with no buffers; the media present body exits on its keys; the type-0x24
producer is unreferenced), and which mechanisms are worth a measured slice here, in what order. It is written under
[upstream_port.md](upstream_port.md) and the amended peer programme in [cleanroom_peers.md](cleanroom_peers.md): the
peer trees are evidence only, every claim is re-measured here before it becomes a slice, no peer code, comment text,
identifier table or measurement table is copied, and the peer projects keep their own copyright and licences.
No game data, package path, extracted table or shader byte appears.

## 1. Scope, inputs and provenance

The peer clones were refreshed for this audit and read read-only outside the repository (`D:\leonida-work\peers`). The
commits below are the clones read; the peer files linked in this page are cited at those commits. The earlier peer pages
([first_visible.md](first_visible.md), [cleanroom_peers.md](cleanroom_peers.md), [bottom_of_pipe.md](bottom_of_pipe.md))
quote older clone commits; this page re-read the refreshed ones.

| peer | repository and licence | commit read | material read for this page |
|---|---|---|---|
| Five Star Recomp | <https://github.com/ThatRecompDev/five-star-recomp>, GPL-3.0-or-later | `45071d52` | first-run input script and its engine, screen signature, scripted desktop input, GDK user/UI/speech/accessibility/display shims, the D3D12.X present folder, the D3D12.X shim page, the runtime and status pages |
| XCL | <https://github.com/xcl-dev/xcl>, GPL-2.0-only | `8b957be` | GameInput, the PresentX path and window, the present preparation screen, the xgameruntime object classes and GDK table, the handoff document, the README status table |
| VIctory | <https://github.com/Marvisak/VIctory>, GPL-3.0-or-later | `63bcca4` | frame clock and queue PresentX, the host presenter and window, GameInput, the user, game UI, speech and display modules, the README status table |
| victory-fork | <https://github.com/fxceless/victory-fork>, GPL-3.0-or-later | `52ac4ec` | the README status and newest-night sections, the frame-rate box, the fork's diagnostics |
| PercyScarlett | <https://github.com/rpatty-dev/PercyScarlett>, GPL-2.0-only | `e44c465` | the prologue guide, the runtime it builds on, the upscale takeover and stretch, its own D3D12.X present, the scripted virtual pad, the prologue runner script, the README proof and status |

This tree's numbers are not re-measured here: every "our state" entry is a measurement already recorded on the page
named beside it, chiefly [decomp_first_ui.md](decomp_first_ui.md) (sections 11-18), [decomp_stage.md](decomp_stage.md)
(sections 5, 10-15), [first_visible.md](first_visible.md), [present_wiring.md](present_wiring.md), [window.md](window.md),
[frame_capture.md](frame_capture.md), [bottom_of_pipe.md](bottom_of_pipe.md) and
[task_queue_parity.md](task_queue_parity.md).

Rules applied. A peer's sentence is a hypothesis, never a specification; where our measurement disagrees with a peer,
our measurement wins and the disagreement is written down. A mechanism named here is described in our words from the
peer's public code and docs; no line of peer code or comment is reproduced. Their own switches and flag names are
quoted only because they are the evidence, and they are the peers' flags, not ours.

## 2. The evidence table: how each peer drives first-run, UI and present

Each cell is the peer's own mechanism, cited; the linked files are the peer's, at the commit in section 1.

| peer | input injection and scripts | first-run screen owners | UI draw path | present handshake | display and overlay | first visible frame producer |
|---|---|---|---|---|---|---|
| Five Star Recomp | a text script of timed presses ([first-run.input](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/first-run.input)); blocks begin when a **screen signature learned from presented frames** matches or a watched panel changes ([InputScriptWatcher.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Input/InputScriptWatcher.cs), [ScreenSignature.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Input/ScreenSignature.cs)); presses held 0.3 s, a select step retried until it takes; one scripted source drives the keyboard and the virtual gamepad ([ScriptedDesktopInput.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Input/ScriptedDesktopInput.cs)) | the title's own first-run pages (press-wait screen, logos, intro art, option screens, subtitles, legal notices, terms, story load); the loader supplies the GDK services each asks for ([running.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/running.md)) | the title's own 2D/Scaleform batch programs through the D3D12.X shim; the shim's measured UI bug was a buffer-view record field left zero, so the batch programs read row 0 of their constant buffer and the first-run screens' vector shapes collapsed ([d3d12x-shim.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/d3d12x-shim.md), Present and descriptor-window sections) | queue slot 32 `PresentX` on a flip-model swapchain on the game's own window; the token is per frame origin and a present uses the plane's token; the GPU throttle waits for the GPU two presents back and **releases held waits at a present**; copy when sizes match else a full-screen scaler with black bars ([d3d12x-shim.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/d3d12x-shim.md), Present) | the game's own window, per-monitor DPI aware, oversized window fitted, scaler as aspect bars; swap-chain colour space by the plane's format family; a test-pattern mode instead of the plane | the title's own UI/logo/intro-art draw; the first present is recorded as the game's plane, black at first, and the first real image comes when the boot mode advances and the 2D batch draws reach the plane ([Status](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/wiki/Status.md), [README](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/README.md)) |
| XCL | its own GameInput v0 COM objects: one always-connected gamepad from XInput (or a PlayStation controller), keyboard as a pad while a window of the process has focus, a ~4 ms poller and a half-second reading history ([gameinput.rs](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/gameinput.rs)); scripted presses at listed seconds for tests (`XCL_PRESS_A_AT`) | the title's own front end and menus; the first menu is the title's account prompt and the flow goes on past it to settings ([README](https://github.com/xcl-dev/xcl/blob/8b957be/README.md), status table) | the title's console command lists replayed onto PC D3D12; the measured cause of a black title screen was the console descriptor model: a CPU-only heap's copied descriptor named a descriptor of another heap and shaders read zeros ([HANDOFF.md](https://github.com/xcl-dev/xcl/blob/8b957be/HANDOFF.md), "Why every frame was black: descriptors are values"); draws wait for pipelines still being made | `PresentX` (console command queue slot 30) presents plane 0's resource at once in XCL's own window and marks the frame token flipped for its frame clock; before a title's first `PresentX` a fallback presents the latest back-buffer-like render target, rate-limited; the copy is queued before the flip is reported ([present.rs](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/d3d12x/present.rs)) | XCL's own window is the display; the title's top-level windows are made frameless and cloaked ([user.rs](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/user.rs)); a GDI "preparing shaders" screen is painted on the same window before the guest starts ([preparation.rs](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/d3d12x/present/preparation.rs)); the window caption carries the frame rate and compile state | the title's own menu/title-screen draws once its pipelines exist and its descriptor/view model is right; the recorded black frames are waiting on pipelines, not on the present ([HANDOFF.md](https://github.com/xcl-dev/xcl/blob/8b957be/HANDOFF.md), XDefiant and Unicorn Overlord entries) |
| VIctory | XInput pads plus a keyboard stand-in pad and a keyboard device, a poll thread that makes a reading per change and runs connect/disconnect callbacks ([gameinput.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-hle/src/modules/gameinput.rs)); `VICTORY_KEYS` holds a key 0.2 s at a scripted second with or without window focus | the title's own first-boot screens (accessibility, language, settings, legal), start screen and profile/saves ([README](https://github.com/Marvisak/VIctory/blob/63bcca4/README.md)) | the title's own draws and dispatches replayed on host D3D12; only changed title memory is copied, the swap-chain grows from the title's own state ([README](https://github.com/Marvisak/VIctory/blob/63bcca4/README.md)) | `PresentX` is a queue slot whose handler pumps held work and calls the host presenter with all presented planes; the frame clock ticks once per interval and the token is the frame number, with the origin polled token-on-timeout and a **blocking flip wait before each present** ([frame.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-gpu/src/frame.rs), [queue.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-gpu/src/queue.rs)); a flip-model swapchain on the title's own visible top-level window, own window as fallback ([host.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-gpu/src/host.rs)) | the window caption carries the frame rate; multiple `PresentX` planes are composited (the Series S profile presents a scene plane and a UI overlay plane); `VICTORY_WINDOW_MAX` bounds the title's window ([host.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-gpu/src/host.rs)) | the title's own menu and first-boot-screen draws; the README records the menus at the title's own 30 fps, the intro video with sound and then the cutscene | 
| victory-fork | inherits VIctory's GameInput and scripted keys; the fork's additions are picture and measurement switches | same as VIctory: first-boot screens, menus, start screen, intro video | same D3D12.X replay as VIctory, with the fork's in-world picture and frame-time work | same `PresentX` to the host presenter; the fork adds a frame log, captures and a driver watch ([README](https://github.com/fxceless/victory-fork/blob/52ac4ec/README.md)) | the frame rate is **drawn into the top-left of every presented frame** by the layer (a capture carries it, the window caption does not) ([fpsbox.rs](https://github.com/fxceless/victory-fork/blob/52ac4ec/crates/victory-gpu/src/fpsbox.rs)); window caption frame rate too | the same title first-boot screens and menus as VIctory; the fork's own layer-drawn frame-rate box is a layer pixel but not the title's first visible frame |
| PercyScarlett | the runtime it builds on has VIctory's GameInput and `VICTORY_KEYS`; its own earlier layer has a virtual gamepad over keyboard/mouse while the title's window has focus and a **scripted pad file re-read every 30 ms, usable without focus** ([vpad.rs](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/crates/gameinput/src/vpad.rs)) | the title's own logos, menus and screens; the prologue guide says to press Enter through the logos and menus ([PROLOGUE.md](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/docs/PROLOGUE.md)) | the runtime it builds on replays the title's command lists on PC D3D12; its measured picture fix is the Series S console upscaler taking over: the upscale dispatch becomes a plain stretch of the colour image so the lit picture reaches the screen ([ffx.rs](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/runtime/victory/crates/victory-hle/src/modules/ffx.rs), [stretch.rs](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/runtime/victory/crates/victory-gpu/src/stretch.rs)) | the runtime's `PresentX` to the host presenter, as VIctory; its earlier layer copies the `PresentX` plane into a DXGI flip swapchain bound to the title's own top-level window and presents with vsync, **which also paces the title** ([present.rs](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/crates/d3d12_xs/src/present.rs)) | the earlier layer's own description lists a Series S overlay; the runtime answers a display mode for the Series X profile (`VICTORY_DISPLAY_MODE=1920x1080`, set by the prologue runner) ([play-prologue.ps1](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/scripts/play-prologue.ps1)) | the title's own menus/logos first, then the opening scene once the upscale is taken over; the guide's early black or incomplete picture is the first-visit pipeline build ([PROLOGUE.md](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/docs/PROLOGUE.md)) |

The last column of the table is the important one for this audit: in every peer the first visible pixels are the
title's **own 2D UI, logo or menu draws reaching a presented plane**, not a layer-drawn UI and not a conversion of the
layer's background pass. Four of the five peers also record black at their earlier presents (five-star's first present
"black so far"; XCL's black until pipelines are built; PercyScarlett's first-visit black or incomplete picture; the
fork's black frames while a rejected pipeline library rebuilds, [README](https://github.com/fxceless/victory-fork/blob/52ac4ec/README.md)).
Their visible frames are therefore a **state** milestone as much as a pixel milestone.

## 3. The runtime answers and events each peer provides

The columns are the service areas the first-run screens consume; "none delivered" means the shim registers or answers
successfully but no change event ever arrives.

| peer | user, saves and storage | UI dialogs and text entry | speech and accessibility | display | task queues and async | input devices |
|---|---|---|---|---|---|---|
| Five Star Recomp | one signed-in offline player; saves and local storage in folders under the save directory with the console's quota; store and package offline ([Console-Runtime.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/wiki/Console-Runtime.md)) | system dialogs complete at once, text entry and web sign-in complete as cancelled, the player picker picks nobody ([XGameUiShim.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Gdk/XGameUiShim.cs)) | no speech voices installed, so synthesizer creation fails; captions and high contrast follow the Windows setting ([AccessibilityService.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Gdk/AccessibilityService.cs)); the user shim registers a change event that never fires because the user never changes ([UserService.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Gdk/UserService.cs)) | HDR reported from the desktop, a screen-keep-awake request held while a deferral is open ([DisplayService.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Gdk/DisplayService.cs)) | an in-process task queue with work and completion ports, composite queues, delayed callbacks, waiters, monitors and termination ([TaskQueueService.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Gdk/TaskQueueService.cs)); the whole GDK is served through `QueryApiImpl` from the user's own GDK ABI table (161 functions) | one gamepad device from the host pad or the keyboard stand-in; every method the title calls is implemented ([Input folder](https://github.com/ThatRecompDev/five-star-recomp/tree/45071d52/src/FiveStar.Loader/Shims/Input)) |
| XCL | one local user, saves in the XCL data folder, persistent local storage as a drive root, a fully installed package, the title id ([xgameruntime](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/xgameruntime/mod.rs)); change-event registration succeeds and **no event is delivered** ("offline: the user never changes") | the dialogs and the in-game text entry are forwarded to the PC's own XGameRuntime classes ([api.rs](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/xgameruntime/api.rs)) | closed-caption properties and high-contrast queries ([accessibility.rs](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/xgameruntime/accessibility.rs)) | display timeout deferrals and URI launch ([api.rs](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/xgameruntime/api.rs)) | `XAsync`, `XTaskQueue` and `XThread` forwarded to the PC runtime when it is installed, XCL's own fallback otherwise ([api.rs](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/xgameruntime/api.rs)) | its own GameInput with one always-connected gamepad, keyboard-as-pad, connect/disconnect callbacks and reading history |
| VIctory | one signed-in offline user served as the user provider's objects; crash-safe versioned save containers; persistent local storage as a virtual drive ([xuser.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-hle/src/modules/xuser.rs)); change-event registration returns a token and **never delivers** (same file) | a profile card closes at once; text entry opens a small host dialog and completes cancelled (`E_ABORT`) when dismissed ([xgameui.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-hle/src/modules/xgameui.rs)) | speech is silent: the synthesizer answers valid WAVE audio of no length ([speech.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-hle/src/modules/speech.rs)) | HDR mode, screen-timeout deferrals, URI launch and screenshots in the display module ([display.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-hle/src/modules/display.rs)) | its own task queues and async implementation ([xasync.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-hle/src/modules/xasync.rs)); the fork measures the submit thread as the in-world frame's limit | XInput pads plus the keyboard pad; scripted keys for diagnostics |
| victory-fork | inherits VIctory's answers | inherits VIctory's | inherits VIctory's | inherits VIctory's; adds a layer-drawn frame-rate box and captures | inherits VIctory's; the fork's own changes are in render, memory, streaming and diagnostics | inherits VIctory's |
| PercyScarlett | the runtime it builds on inherits VIctory's answers; its earlier layer serves the same areas over the PC Gaming Runtime and registers the title identity with that runtime ([README](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/README.md)) | as the runtime it builds on | as the runtime it builds on | as the runtime it builds on; its earlier layer answers offline networking hints and device association | as the runtime it builds on | the runtime's GameInput plus the earlier layer's scripted virtual pad |

Two reads from this table matter for this tree. First, every peer's offline user is **static**: none of them delivers a
user change event, and none needs one for its first-run screens. Second, speech is absent or silent in every peer (a
failed create at five-star, a valid empty stream at VIctory and PercyScarlett), and their screens still appear. This
tree does strictly more than the peers on both counts: it delivers one `SignedInAgain` and answers a synthesizer create
with one silent voice ([decomp_first_ui.md](decomp_first_ui.md) sections 12.2, 12.3), and the stage word still does not
move. Those two services are therefore measured *not* to be the first-run gate.

## 4. What makes each peer's first visible frame appear, read as one statement

1. **The first visible pixels are the title's own 2D UI, logo or menu draws.** Five Star Recomp's first real image is
   its UI layer and intro video; XCL's is its title screen and account prompt; VIctory's and victory-fork's are the
   first-boot screens and menus; PercyScarlett's are logos and menus. None of these is a layer-drawn screen.
2. **The layer's job is to keep the title's state machine running and to present the plane it fills.** Every peer
   answers the first-run services (user, saves, storage, dialogs), presents the title's own plane into a window, and
   scripts the input that the screens wait on. The D3D12.X replay and present path is the same class of mechanism on
   both sides of the comparison.
3. **The visible frame is behind a state step, and the peers record black before it.** Their own logs and guides name
   black or incomplete frames while pipelines build, while a rejected pipeline library is rebuilt, and at the first
   present before the boot mode advances. A presented frame is not the same thing as a produced frame.
4. **Time alone does not answer a screen.** Five Star Recomp's script exists because timing alone missed presses while
   pipelines compiled; it waits for the *screen* and retries a select until the panel changes. The other peers' simpler
   timed keys work because their title reaches the screen being pressed.

## 5. This tree's measured first-run/UI/present flow

The numbers are the cited pages' measurements; no boot was run for this page.

| measurement | value and page |
|---|---|
| stage and session words | in the long content arm `init 3 / loading 2 / session 3`, machine current/next 3/3; stage 3 at 114.4-116.0 s and nothing moves for the next 180 s ([decomp_first_ui.md](decomp_first_ui.md) section 17.2) |
| the frame thread and the loading task | the frame thread parks in the session-3 poll's 33 ms wait at `game+0x21B7070` at every stage-3 sample; the loading task waits on a title semaphore; the loading tick keeps advancing, so the loader is running ([decomp_first_ui.md](decomp_first_ui.md) sections 17.2, 17.8) |
| the session-3 loading record | its completion dword is never observed non-zero in 300 s; the only writer measured is the record reset, and the completion entry points stay at zero ([decomp_first_ui.md](decomp_first_ui.md) section 17.3, [decomp_stage.md](decomp_stage.md) sections 10-15); the stream job's open gate never reaches the state the engine step needs, and no shim answer is on the path ([decomp_stage.md](decomp_stage.md) sections 13.6, 14.5-14.7) |
| first-run screen owners | accessibility create 1 hit, sign-in owner 1 hit, text-entry owner 0, profile-card owner 0 in the two probe arms; the runtime services they call answer and are consumed ([decomp_first_ui.md](decomp_first_ui.md) sections 12.3, 17.6) |
| first-run services | synthesizer create `S_OK` with one silent voice at ~24.5 s, one neutral pad enumerated with `GetCurrentReading`/`GetGamepadState` at ~24.75 s, save provider initialised at ~24.75 s, one `SignedInAgain` at ~1.9 s, user-ready refresh; nothing after them ([decomp_first_ui.md](decomp_first_ui.md) sections 12.2, 12.3) |
| the boot-media/UI present body | 4,556 body entries in 130 s (4,529 from the title's task-queue pump) and it exits at its first two compares every time; the keys `0x71B90AD`/`0x71B90AC` read 0 in every sample; the semaphores `0x71B9078`/`0x71B9088` read `0x714`/`0x71c` from the 10 s sample ([decomp_first_ui.md](decomp_first_ui.md) section 11.1) |
| the type-0x24 producer | the only in-image producer of the set event, `0x2218180`, has no reference outside its own selector thunk and 0 probe hits; a synthetic producer would be inventing title state ([decomp_first_ui.md](decomp_first_ui.md) sections 11.2, 11.4) |
| the front-end manager | created, ready and running from ~26 s, seven service holders live, its render called 1,786 times with a valid holder; the reported crash family was a stray overwritten vtable word, not a layer refusal ([decomp_first_ui.md](decomp_first_ui.md) sections 14.1-14.5) |
| the movie player | request binds slot 0; metadata/probe open 3 hits and playback open 1; player state reaches `0x9` with a non-null Bink handle and **`buffers 0`**; per-frame decode `0x2EF0E0` and frame-buffer registration `0x442F760` have 0 hits ([decomp_first_ui.md](decomp_first_ui.md) sections 15.3, 17.4, 18.4) |
| the class-0xa stop and the mitigation | the movie's first display job builds a class-0xa 256x1 constant upload with a zero-byte request and a 0x1000-byte copy into the live command object; with the opt-in title-side mitigation the request is 0x1000 and the executor pass survives, but the decode is still not issued ([decomp_first_ui.md](decomp_first_ui.md) sections 15.4, 16.3, 16.4) |
| presented pixels | about 9,001 frames presented by the 300 s kill (~30/s); 30 presenter captures 2564x1421, every one mean rgb `0.00 0.00 0.00`; six full-4K content-target readbacks `0/8,294,400` non-black; the layer's own write-control clear on the same target reads `51 102 204` in the control arms ([decomp_first_ui.md](decomp_first_ui.md) sections 16.4, 17.5, [frame_capture.md](frame_capture.md) section 7) |
| present path identity and handshake | the presented plane is the frame's own 4K format-37 target by resource identity ([vertex_buffer_va.md](vertex_buffer_va.md) section 10.2); the wired present arms record 8,625 presents in 90 s and 33,275 in 300 s with 0 failed ([window.md](window.md) section 5, [remaining_mechanisms.md](remaining_mechanisms.md) rank 9); the device proxy issues the frame tokens and the pacer is not the token source ([present_wiring.md](present_wiring.md) section 3) |
| task queues at the measured stall | the complete task-queue/threading surface is 26 calls, all setup, and the last is at +25.51 s; zero submit, dispatch, waiter, monitor, terminate and `XAsync` calls; the stall is inside the title's own job graph ([task_queue_parity.md](task_queue_parity.md) sections 2-3) |
| the bottom-of-pipe slice | slots 71/72/74 are implemented behind switches and A/B-booted; 4,088 of 4,088 slot-74 waits were already met at record time, so the cross-list queue hold is unobservable at this content population and not carried ([bottom_of_pipe.md](bottom_of_pipe.md) sections 5.1, 6) |

## 6. The comparison, dimension by dimension, and the most plausible difference

| dimension | the peers | this tree | verdict |
|---|---|---|---|
| **input timing** | a script: simple timed keys at one peer (XCL's `XCL_PRESS_A_AT`, VIctory's `VICTORY_KEYS`, PercyScarlett's pad file) or screen-state blocks with learned signatures and retried selects (Five Star Recomp); a press is held 0.2-0.3 s | one neutral pad, one first-run read at ~24.75 s with no press; the scripted schedule is opt-in, starts its first press at +70 s and was not reached in the arm that stopped before it ([decomp_first_ui.md](decomp_first_ui.md) sections 12.3, 17.7) | **differs, but behind the producer**: a script cannot advance a screen that is not drawn. Five Star Recomp's own design note says the same in the other direction: the screen has to exist for a screen-state block to match |
| **display answers** | HDR reported from the desktop, display mode answered for the Series X profile, dialogs dismissed or cancelled, speech absent or silent | synthesizer create `S_OK` with a silent voice, one voice enumerated, no narration stream requested; HDR disabled `S_OK`; no display-mode answer measured; the screen owners' calls are consumed | **not the gate by measurement**: every answer the screen owners asked for was consumed and no screen or pixel followed; a display-mode difference could change the title's output path, but no peer names it as the first-frame cause |
| **UI resource creation** | the peers create no title UI resources; the title's own Scaleform/page data and batch programs draw, and the layer's share is descriptor heaps, views and root signatures | the same translation and replay are carried; the package's RBF0 UI/Scaleform definitions are decoded offline ([rbf0.md](rbf0.md)); nothing measures whether the title asks for the UI pages or creates the batch pipelines in the measured window | **measured equal class, unmeasured here**: not a resource-format gap; the question is whether the UI producer runs at all, which the port plan's UI probe settles |
| **present and pacing** | the peers tie the title's frame events and tokens to the present: a token per origin and a flip wait before each present (VIctory), a token used up by a present and held waits released at a present (Five Star Recomp), a present marking the token flipped (XCL), a vsync present pacing the title (PercyScarlett's earlier layer) | presents succeed (8,625 in 90 s and 33,275 in 300 s, 0 failed in the wired present arms), the presented plane is the frame's own 4K target by identity, and the present path has a real movie-frame test that puts a decoded frame on the plane ([d3d12_boot_movie_test.cpp](../port/test/d3d12_boot_movie_test.cpp)); but the device proxy issues the frame tokens and the pacer is not the token source | **measured equal for a copy, differs in who owns the handshake**: a zero-source plane cannot become visible in any presenter (Five Star Recomp's own copy/scaler would show the same zero), but the handshake ownership is a real difference and the first port below |
| **content producer** | the first visible pixels are the title's later-state 2D UI/menu/logo draws reaching the plane | the window contains the stationary background/upscale loop only; the boot-media/UI present body exits on its keys; the front-end manager's render runs with live services but no presented UI frame; the movie player is at the playing state with `buffers 0` and the draw that would decode is not issued; the frame thread parks in the session-3 loading poll | **the strongest divergence**: the producer, not the present, and it is behind the title's own state step |
| **state chronology** | the peers' screens are early boot states; their own black intervals are pipeline builds and their first real image follows the state step `5 Recognisable picture` on the public tracker's ladder | our title enters its first-run service sequence at ~24 s and then reaches session 3, where the loading record's completion never arrives and the frame thread parks; the first-run/UI draws are above that park | **strongest single explanation**: the peers' visible frame belongs to a state our window does not reach, and the present path ranks below it |

**The most plausible difference.** The present path is measured not to be the cause of black pixels: our presented plane
is the frame's own target, the presents succeed, and the path already puts a decoded movie frame on the plane in a
test. The peers' first visible pixels are the title's own 2D UI/menu draws; ours never produce one, and the reason is
one layer up: this title's own first-run/UI frame producer does not run in the measured window. Its boot-media/UI
present body exits at its first two compares because the keys only an unreferenced in-image producer sets, and the
movie/UI draw that would fill the presented plane is not issued while the frame thread parks in the session-3 loading
poll. The peers do not carry any producer we lack for those pixels; they reach the state in which their title's own
producer runs. The two mechanisms the peers do carry that could move the title into that state, and that our tree does
not, are the **present-driven frame clock with held waits released at a present** and the **title's own hang-watchdog
hold**; the input script is the gate behind the screen, not in front of it. The sibling audits trace the same producer
gap one level down: the streaming audit names the title-side worker and pipeline-wait chain as the most plausible
reason their equivalent loading completes, and the movie audit finds every peer runs the title's own movie trigger and
none carries an HLE decoder. This page agrees with both and does not restate their ports. That is the ranking behind
this plan: measure the present handshake and the present body, hold the watchdog, probe the UI pages, then the input and
the presentation answers. It is a ranking of what can decide the difference, not a claim that any one of them fixes it.

## 7. The ranked port plan

Effort is the repository's scale: S is about a day, M is two to five days, L a week or more, each including its page,
its tests and one boot A/B. "Boot need" says whether the test needs a boot (`tooling/boot_report.py`, one at a time
under `_setup/boot_lock.ps1`) or can be decided GPU-free. Every mechanism is re-derived from our own measurement and
implemented in our files; the peer file is the evidence of the behaviour, never a code source.

| rank | port | mechanism as the peers do it | our target files | effort | test and falsifier | boot need |
|---|---|---|---|---|---|---|
| **P1** | present-driven frame clock and flip event (the pacer owns the token) | VIctory's clock ticks once per interval, the token is the frame number, the origin is polled token-on-timeout and a blocking flip wait sits before each present ([frame.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-gpu/src/frame.rs)); Five Star Recomp issues a token per origin and a present uses the plane's token, with held waits released at a present ([d3d12x-shim.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/d3d12x-shim.md)); XCL marks the token flipped after queuing the present copy ([present.rs](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/d3d12x/present.rs)); PercyScarlett's vsync present paces the title | `port/src/frame_pacer.h`, `port/shims/xbox_present.cpp`, `port/shims/d3d12_xs_device.cpp` (device slots 78-80), `port/src/vulkan_backend.h` (the present-completion hook, if the release needs it) | S | unit: the pacer issues token k only at origin k, a blocking flip wait returns only after the present that flipped it, one present consumes one token, and the title's measured interval is the pacer's interval (`port/test/frame_pacer_test.cpp`, `port/test/xbox_present_test.cpp`); boot A/B with `PALMETTO_STATE_TRACE` and presenter captures, comparing the media/UI body tick count `0x313560`, the token series and the captures. Falsifier: the cadence and token series are unchanged and the body's keys still read 0, so the handshake is not the difference | yes, one A/B pair |
| **P2** | present-side attestation of the media/UI body and the presented plane | every peer's first visible frame is its title's own draw reaching the presented plane, so the present side can say which of three states a boot is in: the body never ran, the body ran but no UI/movie draw was issued, or a draw was issued and the plane stayed black. Our arms already show the first and third (the body exits on its keys; every capture is black) and need the middle named per present | `port/src/main.cpp` (the present-side line of the existing state trace), `port/src/vulkan_backend.h` (the presenter capture hook), `port/test/vk_presenter_capture_colour_test.cpp` | S | one light arm printing per present: the body tick count and the two keys, the presented plane's resource, format and state, and the capture mean; falsifier: the attestation is identical to the existing arms, so the present side adds nothing and the sibling movie/streaming probes own the next look | yes, one light arm |
| **P3** | hold the title's own hang watchdog, opt-in | the title ends itself when its frame heartbeat stops for ~30 s; PercyScarlett writes the title's own pause byte every 200 ms and keeps the watchdog on with `VICTORY_WATCHDOG` ([run.rs](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/runtime/victory/apps/victory/src/run.rs)); VIctory exposes `VICTORY_POKE`, which writes a title byte twice a second for exactly this and names the 30 s tick ([run.rs](https://github.com/Marvisak/VIctory/blob/63bcca4/apps/victory/src/run.rs)); Five Star Recomp documents the same flag and that the type-0x24 event sets it ([debugging.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/debugging.md)) | `port/src/main.cpp` (an opt-in install after the image is mapped, the pattern of `port/src/title_class0a.h`), a new include-only header under `port/src`, `port/test/code_hooks_test.cpp` if the switch parser is a pure helper | S | unit: the parser and the install scope, refusing an install whose target byte is not in the measured data range; boot A/B with the class-0xa mitigation on and the movie/first-run environment, comparing the boot window, the state words, the movie play-step counters and the captures. Falsifier: the pause byte is set and the boot length, state words and pixels are unchanged from the control; record it as parity-only, since this tree's boot already survives 300 s | yes, one A/B pair |
| **P4** | a UI page/resource probe: does the title ask for its front-end UI at all | the peers' visible frames are title UI draws; nothing in their layer creates UI resources, so the question is whether the title's page data and batch pipelines are requested | `port/src/main.cpp` (a bounded trace of the RBF0/UI page load, the text-entry/profile wrappers `0x2234A40`/`0x3DAF310` and the UI batch program creation); no shim change | S | one probe arm over the first-run window: the page load call count, the UI batch pipeline ids and the draw counts; falsifier: no UI page or batch ask exists in the window, which makes the producer a state step above the frame loop and retires the UI-resource branch of the comparison | yes, one light arm |
| **P5** | first-run input: screen-state script with learned signatures | Five Star Recomp's script blocks begin on a learned 48x27 luminance signature or a watched panel change, presses are held 0.3 s and a select is retried until it takes ([InputScriptWatcher.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Input/InputScriptWatcher.cs), [ScreenSignature.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Input/ScreenSignature.cs)); the other peers' timed keys are the simpler form ([gameinput.rs](https://github.com/xcl-dev/xcl/blob/8b957be/crates/xcl/src/hle/gameinput.rs), [VIctory](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-hle/src/modules/gameinput.rs), [vpad.rs](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/crates/gameinput/src/vpad.rs)) | `port/shims/gameinput.h`, `port/shims/gameinput_nosdl.h`, `port/src/active_input.h`, `port/src/fake_input.h`, `port/src/main.cpp` (the script switch), the presenter capture path in `port/src/vulkan_backend.h` (a signature sample per capture) | M | unit: the script parser and the signature distance on synthetic 48x27 frames, a press schedule that a synthetic clock drives, and a refused malformed script (`port/test/gameinput_nosdl_test.cpp`, `port/test/active_input_test.cpp`); boot after a screen presents, comparing screen signatures, press counts and the state words. Falsifier: the presses land at the first-run read and no signature changes, so the screen producer, not the input, is the gate | yes, after a producer presents |
| **P6** | display capability answers | HDR reported from the desktop (enabled with the display's luminance when the desktop is HDR10, else disabled), a display mode answered for the Series X profile, and shapes outside the measured ones refused by name ([DisplayService.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/src/FiveStar.Loader/Shims/Gdk/DisplayService.cs), [play-prologue.ps1](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/scripts/play-prologue.ps1)) | `port/shims/xgameruntime_offline.cpp` (the display answers), `port/test/xgameruntime_offline_test.cpp` | S | GPU-free: the answers follow the host's real state, an unknown shape is refused by name, and the switch restores the old answer; falsifier: the answers are consumed and the state words and pixels are unchanged from the current boot (as HDR already measures), so they are recorded as parity-only | optional: the unit test decides; one A/B only if an answer changes the title's path |
| **P7** | presenter window fit, scale and colour space with a synthetic non-black control | Five Star Recomp fits an oversized window to the monitor, scales the plane into the client area with its aspect ratio over black bars, and picks the swap-chain colour space from the plane's format family ([d3d12x-shim.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/d3d12x-shim.md), Present); ours adopts the title's window and blits the plane ([window.md](window.md), [present_wiring.md](present_wiring.md)) | `port/shims/xbox_present.cpp`, `port/src/vulkan_backend.h` (presenter), `port/src/vulkan_present.h` and `port/src/vulkan_present_compose.h`, `port/test/xbox_present_test.cpp`, `port/test/vk_presenter_capture_colour_test.cpp` | S/M | GPU-free: a synthetic non-black source presented through the capture path reads back its value and a resize changes only the scale; falsifier: the capture stays black from a non-black source, which makes the presenter a real divergence, or the fit/scale answers move nothing and are recorded as parity-only | no for the control; one A/B if the fit/scale changes |
| **P8** | retire the task-queue and user-change-event hypotheses (no code) | every peer's offline user never changes and delivers no event, and no peer's task queue is what unblocks its title; our 26-call task-queue surface is setup only and the stall is title-side ([task_queue_parity.md](task_queue_parity.md) sections 2-3); our delivered `SignedInAgain` does not move a stage word ([decomp_first_ui.md](decomp_first_ui.md) sections 12.2, 12.3) | none; the record goes in this page and the two cited pages stay the evidence; the parity test itself is the streaming audit's port, not this page's | — | falsifier that would reopen them as first-run/UI/present gates: a boot in which a dispatched task or a delivered acquisition event precedes a presented first-run screen; until then no slice spends effort here | no |

Ordering rule: P1-P2 are the cheap present-side measurements that decide whether the present handshake or the present
body is the difference at all; P3-P4 are the title-side probes (the watchdog and the UI pages) that sit between the
present and the state; P5 is the input that only matters once a screen presents; P6-P7 are presentation answers and
correctness, valuable after a non-black plane exists; P8 closes two hypotheses the peers' own answers and our boots
already refute. A negative result in any of P1-P7 is a valid result and is recorded on this page's successor, per
[upstream_port.md](upstream_port.md) section 2: the measurement wins, and the disagreement is written down.

The movie and streaming peer audits in the same working tree already own the ports this page would otherwise
restate, so they are deliberately not duplicated here: [peer_port_movie.md](peer_port_movie.md) owns the Bink import
witness and movie fast-advance, the movie draw-path probes, the session-3 completion witness, the per-submission plane
refresh and the second-plane composition gate; [peer_port_streaming.md](peer_port_streaming.md) owns the
never-block-a-title-thread pipeline wait, the load-read and completion delivery, the cross-list memory wait, the
GPU-ordered bottom-of-pipe write, the task-queue parity test and the submission overlap. Both are the programme's other
uncommitted slices' pages and are committed with this one. This page's plan is the first-run/UI/present end of the same
programme, and its P1/P2 are the present-side measurements those audits can arm in their boots.

## 8. What this plan does not port, and why

- **No synthetic type-0x24 event and no synthetic state-2 ring item.** The only in-image producer of the boot-media/UI
  set event is unreferenced code, and the probe boot measured it never running
  ([decomp_first_ui.md](decomp_first_ui.md) sections 11.2, 11.4). A runtime that queued that event or that ring item
  would be inventing title state, not answering a runtime call; the peer mechanisms are title-side and do not do it
  either.
- **No user-change-event work.** Five Star Recomp's event never fires, XCL's and VIctory's registration delivers
  nothing, and our tree's one delivered `SignedInAgain` leaves the stage word unchanged. The negative is the
  measurement.
- **No Bink decode port.** The package's own library decodes 515 of 515 streams' frame 0 through our loader and a
  playback test exists ([movie_path.md](movie_path.md)); the title's entry never runs, and the missing piece is the
  title's own trigger and draw, which the sibling movie audit probes; P2 only attests which present state the boot is in.
- **No title UI resources created by the layer.** The peers create none; the RBF0 UI/Scaleform page data is decoded
  offline ([rbf0.md](rbf0.md)); the layer's share of the UI path is the descriptor/view/root-signature replay we
  already carry.
- **No second task-queue slice.** The parity slice measured the whole surface and found it idle
  ([task_queue_parity.md](task_queue_parity.md)); P8 records the retirement.
- **No peer code, comment text, identifier table or measurement table.** See section 11.

## 9. Limits, and what this page does not claim

- **No boot was run for this page.** Every "our state" entry is a cited page's measurement, and every peer entry is the
  peer's published or source-visible mechanism, read at the refresh commit in section 1.
- **Peer claims are self-reported.** Five Star Recomp's README and status text contradict each other on how far its
  title runs, and the public tracker reads the optimistic side; PercyScarlett's runtime is built on VIctory,
  victory-fork and the ScarlettPort upscale, so the five peers are a network, not five independent implementations.
  Their agreement on a mechanism makes it credible; their own numbers are not our targets.
- **The mechanisms are described, not specified.** A peer's flow is evidence of what worked for its own title tree on
  its own host; our host presents through Vulkan and a prepared presenter, not through the peers' D3D12 swap chains,
  and where my measurement and a peer disagree, my measurement wins.
- **The ranking is a ranking of falsifiers, not a causal proof.** The "most plausible difference" in section 6 is the
  reading the evidence supports; P1-P7 are the measurements that can retire it, and none of them is claimed to fix the
  picture until its boot shows a first-run/UI producer run or a first non-black capture.
- **The stage-3 reach is an environmental race.** The long arm's stage 3 arrived at 114.4-116.0 s and the other two arms
  lost the host GPU before it; the numbers quoted are that one clean arm's, as the cited pages state.

## 10. Files, gates and the index row

- **This page**: `docs/peer_port_first_ui.md`, the only repository file this slice writes. No runtime, build, test, data,
  CMake, README or NOTICE file changed; no boot was run; no git write.
- **Index**: [docs/README.md](README.md) needs one row next to the peer and first-visible pages
  ([first_visible.md](first_visible.md) and [decomp_first_ui.md](decomp_first_ui.md) are the neighbours), and the same
  row is the place to point at the two sibling peer-page audits in this working tree, which are committed together with
  this page ([peer_port_movie.md](peer_port_movie.md) and [peer_port_streaming.md](peer_port_streaming.md)). That file
  is outside this slice's write scope, so the lead adds the row.
- **Credit**: [NOTICE.md](../NOTICE.md) already carries the peer-port first-visible-frame evidence section; this page
  records the refreshed commits (`45071d52`, `8b957be`, `63bcca4`, `52ac4ec`, `e44c465`) and the files read for this
  page. Updating that section with the refreshed commits is the lead's follow-up at commit time, since this slice does
  not edit NOTICE.
- **Gates**: this page is written to pass the docs checks (`tests/test_docs_links.py`, `tests/test_docs_repo_paths.py`,
  `tests/test_docs_table_rows.py` and the rest of `tests/test_docs_*.py`); it names no build or test result, so no
  `PALMETTO_OK`/`TESTS_BUILT` pair is claimed for this slice.

Section 10 above is the record of the audit slice that wrote this page only. Section 12 is the later, separately gated
implementation of the page's P1 port: it does change runtime and test files, it did run boots, and its own files and
gates are listed there.

## 11. No-copy and attribution statement

This page and the plan it carries were written from this project's own measurements of the user's own image and boots,
and from reading the peers' public source and documentation at the commits named in section 1. **No code, comment
text, identifier table or measurement table from any peer project was copied into this repository or into this page**;
the mechanics are described in our words, every peer file and commit is cited with its link, and every implementation
the plan proposes stays ours, in our files, naming and tests, derived from our own call sites, boots and readbacks, as
required by [upstream_port.md](upstream_port.md). The peers remain the copyright of their contributors and their licences
continue to apply to their own trees:

- Five Star Recomp, GPL-3.0-or-later, <https://github.com/ThatRecompDev/five-star-recomp>, commit `45071d52`.
- XCL, GPL-2.0-only, <https://github.com/xcl-dev/xcl>, commit `8b957be`.
- VIctory, GPL-3.0-or-later, <https://github.com/Marvisak/VIctory>, commit `63bcca4`.
- victory-fork, GPL-3.0-or-later, <https://github.com/fxceless/victory-fork>, commit `52ac4ec`.
- PercyScarlett, GPL-2.0-only, <https://github.com/rpatty-dev/PercyScarlett>, commit `e44c465`.

No game data, package path, shader byte, extracted table, save file or account value appears in this page; RVAs, counts,
HRESULTs, API names, times and image means only.

## 12. The implemented port: a present-driven frame clock and flip token (P1)

This section is the record of the P1 slice in section 7. It was built from this tree's own measurements, with the peers'
mechanisms read as evidence only, as [upstream_port.md](upstream_port.md) requires; section 12.7 repeats the credit.

### 12.1 What was measured before the port

The device frame handshake and the present path were measured on the current tree with a light boot of the section-12
content environment, the class-0xa mitigation on, `PALMETTO_XS_CALLERS=1` with the caller profile and
`PALMETTO_PRESENT_PACING=1` (the measurement switch this slice adds; it changes no answer). The call sites and argument
words are the title's own, at the image RVAs below.

| measurement | value and where |
|---|---|
| interval call | `game+0x247ABDE` calls device slot 78 `SetFrameIntervalX` with (output null, period `0x8235` = 33,333 us, latency 3, flags `0x100000000`); the device proxy stored both numbers in its own atomics and no clock read them |
| schedule calls | `game+0x247AC02` schedules event 0; `game+0x2478DEA` schedules event 2; both were answered `S_OK` by the device proxy, which keeps one "scheduled" flag |
| origin poll | `game+0x248244E` calls device slot 80 `WaitFrameEventX(event 0, timeout 0, flags 2, &[image+0x75E0E88])`; the submit at `game+0x2482660` reads that word back as the frame token, so this poll is the token handshake |
| flip wait | `game+0x248240B` calls device slot 80 `WaitFrameEventX(event 2, timeout INFINITE, flags 0, ...)` in the same frame function, before the origin poll: the blocking flip wait of the peers' handshake |
| token source and series | the device proxy's own counter issued every token, both call sites included, so a frame presented tokens 1, 3, 5, 7, 9, 11, 13 ... after the first; the pacer never issued one, never flipped one, and `PALMETTO_PRESENT_PACING` printed `mode grid cadence 0 us flipped 0x0` at every present |
| present path | `PresentX` (queue slot 32) once per frame through the registered window presenter; because no interval reached the pacer, `sync_interval` was 0 (unpaced: not FIFO) and the frame-token bridge accepted the reported tokens |
| frame-pacing effect at boot | the 50 s control arm traced 1,338 `PresentX ok`; 1,320 presents at 27.898 s, about 47 presents per second at that sample; captures every 200 frames, seven of them, all mean rgb `0.00 0.00 0.00` |

### 12.2 What was implemented

**The pacer is the clock** (`port/src/frame_pacer.h`, `set_present_driven`; on with `PALMETTO_PRESENT_DRIVEN=1` in the
presentation core). In this mode:

* **A wait hands out the token.** Token 1 is issued at once, before any present: the layer has no displayed frame to
  follow yet and must not stall the title before its first frame. After that one token is issued per origin, never
  reused.
* **The present marks its token flipped.** `commit_present_token` is the frame's flip: it marks the record presented and
  displayed at the present's own time, frees one of the `latency` in-flight slots, and notifies the waiters. This is the
  "present marks the frame token flipped" of XCL and the "held waits released at a present" of Five Star Recomp, in our
  state machine.
* **The blocking flip wait is a separate call.** `wait_flip` issues no token; it returns when a present has flipped an
  in-flight frame, so a caller that keeps `latency` frames queued is released exactly at that present. The measured
  event-2 INFINITE call is that wait.
* **The pacing follows the observed display cadence.** The first present anchors the phase; the period seed is
  `SetFrameIntervalX`'s 33,333 us and every tokened present replaces it with a smoothed (1/8 weight) interval between
  presents, clamped to a quarter-to-four-times the declared period. A repeated or stale token, and a blank present,
  carry no new frame and never move the clock. The measured ON arm's cadence settled at 29,167 us at the last sample.
* **A dead-man keeps the boot alive.** A flip wait whose present never comes is released after four observed cadences
  (counted in `present_clock_fallbacks()`); a title that waits without presenting cannot hang on the handshake.

The pacer stays the existing grid implementation when the switch is off, byte for byte: the early return of the old
branches, the same tokens, the same statistics. The existing tests are unchanged and pass, which is the off-state proof.

**One clock for the device and the presentation** (`port/shims/xbox_present.cpp`). A process-lifetime `FramePacer`
(real steady clock) answers device slots 78/79/80 and is the pacer the runtime's `XboxPresentation` commits its presents
into; a presentation constructed with a test clock still owns its own pacer. `port/shims/xbox_present.h` adds the
hook-only `XboxFrameEventBackend` (backend record version 2) with the interval, schedule, origin-wait, flip-wait and
last-token calls.

**The two measured frame waits** (`port/shims/d3d12_xs_device.cpp`, slots 78-80). With the switch on, the device proxy
forwards the handshake to that pacer instead of its atomics: `SetFrameIntervalX` registers the interval, the event-0
schedule arms the origin, the event-0 poll hands back a new origin token or, when no origin is due, the last token with
`S_FALSE` (the public flag-2 rule: the token is written on the timeout); the event-2 wait is the pacer's flip wait. Any
hard refusal (not registered, suspended) falls back to the proxy's old fresh-token answer, so a foreground frame call
can never wedge a boot. The interval now reaches the pacer, so registered presents also go through the FIFO present mode
(sync interval 1) instead of the unpaced path: the "vsync present paces the title" of PercyScarlett's layer.

`PALMETTO_PRESENT_PACING=1` is the measurement aid for the A/B: the present path prints the first eight presents and
every thirtieth (time, token, mode, cadence, flipped watermark, dead-man count), and the device proxy prints the same
series for the frame waits with the answering side (`pacer` or `device-proxy`). It is off by default and changes no
answer.

### 12.3 Tests

* `port/test/frame_pacer_test.cpp`: new pure state-machine cases for present-driven mode - the first token before any
  present and the flip wait before a present both return at once; the first present anchors the phase and the next
  origin is exactly one declared period later; the present and display statistics both carry the present's time; a
  zero-timeout origin poll times out and leaves the token alone while `last_issued_token()` still names the held one; a
  repeated and the null token do not move the flipped watermark or the cadence; one present consumes one token; the
  blocking flip wait is released exactly at the present that flips the frame and not by time passing; a 1 ms timeout
  still times out; the cadence dead-man releases a held token wait with the fallback counted; a late present widens the
  cadence (observed 30,000 us, smoothed 17,750 us) and the next origin follows it; suspend fails a held wait and
  resume keeps the mode. The existing grid cases are unchanged and still pass.
* `port/test/xbox_present_test.cpp`: a present-driven case on its own window, Vulkan presenter and `ManualFrameClock` -
  `WaitFrameEventX(event 0, 0)` returns token 1 with no present, `WaitFrameEventX(event 2)` returns at once, the
  `PresentX` of token 1 marks it flipped and fills its display statistic at fake time 0, the zero-timeout poll then
  returns `HRESULT_FROM_WIN32(WAIT_TIMEOUT)` with the token untouched, the next origin arrives one cadence later, and
  the event-2 flip wait is held while token 2 is unpresented and released by token 2's present.
* `port/test/xs_proxy_mock_test.cpp`: a GPU-free forwarding case with a fake registered `XboxFrameEventBackend` - slots
  78 and the event-0 schedule are forwarded (the PC slots never reached), the event-2 schedule keeps the proxy's record,
  the event-0 flag-2 poll returns `S_FALSE` with the pacer's last token, an event-0 success passes the pacer's token
  through, event 2 reaches the flip wait with the token untouched, and with nothing registered the old counter answers
  exactly as before.

Results: `frame_pacer_test: all cases pass`, `xbox_present_test: all cases pass`, `xs_proxy_mock_test: PASS`; the gates
printed `PALMETTO_OK` and `TESTS_BUILT`.

### 12.4 A/B measurement

One light pair, one boot at a time under `_setup/boot_lock.ps1`, the section-12 content environment from the boot
template, the class-0xa mitigation (`PALMETTO_TITLE_CLASS0A_SIZE=1`), `PALMETTO_PRESENT_PACING=1` in both arms and
`PALMETTO_PRESENT_DRIVEN=1` in the ON arm only; 50 s limit per arm, stopped at the harness timeout in both (no device
lost inside the window; the first two attempts at the full 130 s both hit the host device-loss race the page records,
`[rt] replay: the Vulkan device was lost`, at 57.7 s and 58.2 s, so the pair was shortened before that point, per the
stop-after-two-losses rule).

| measurement (same 50 s window) | OFF arm (control) | ON arm (present-driven) |
|---|---|---|
| `PresentX ok` at the kill | 1,338 | 1,177 |
| presents at the last pacing sample (27.9 s) | 1,320 | 1,170 (about 12% fewer) |
| present cadence the pacer used | 0 us: grid mode, the pacer was not the clock | 33,333 us seed, then 32,769 -> 31,523 -> 29,167 us; it followed the presents |
| presented token series | 1, 2, 3, 5, 7, 9, 11, 13 ... (a fresh device-proxy token per wait call, then +2 per frame) | 1, 2, 3, 4, 4, 5, 5, 6 ... (one pacer origin token per frame; the flag-2 poll holds the last token, so a repeated token is presented again) |
| event-0 origin polls | answered by the device-proxy counter (`device-proxy`), token per call | answered by the pacer: a new token when the origin is due, the held last token with `S_FALSE` otherwise |
| event-2 INFINITE waits | answered by the device-proxy counter (`device-proxy`), fresh token, never blocks | the pacer's flip wait (`pacer`), token 0x0, released at the present that flips an in-flight frame |
| frame waits at 28.7-29.0 s | 2,640 | 2,340 (fewer because fewer frames) |
| state words in the window (last sample 44 s) | init 2 (change at 22.0 s), loading 1 (22.0 s), session 0, machine current/next 2/2, boot-media gate 0x2 active 0xff000000 flag 0x0, movie slot 0 entity 0, front-end ready 1 running 1 | identical to the control at the same sample: init 2 (22.0 s), loading 1 (22.0 s), session 0, machine 2/2, gate 0x2 active 0xff000000 flag 0x0, movie entity 0, front-end ready 1 running 1 |
| captures and pixels | 7 of the every-200-frame captures, all mean rgb `0.00 0.00 0.00` | 6 captures, all mean rgb `0.00 0.00 0.00` |
| stop site / device lost | ALIVE at the 50 s timeout, 0 exceptions, no device-lost line | ALIVE at the 50 s timeout, 1 trace exception `0x406d1388` (a thread-name exception from the display driver, not a stop), no device-lost line |

**Verdict: the present handshake moved, the title's state did not.** The pacer owned the tokens, the two measured waits
went through the flip-token state machine, and the present cadence became the observed one (about 12% fewer presents in
the same window); the loading/session/stage words, the boot-media keys, the movie slot and the capture means are the
control's exactly. The frame heartbeat is not in this table: the stage-3 pump never armed in a 50 s window, so no
heartbeat advance count exists for either arm. This is the P1 falsifier's shape read the other way round from a
null result: the handshake is measurably different and the first-run/UI producer is still behind its state step, which
keeps the producer ranking of section 6.

### 12.5 Limits and the next falsifier

* **The beat is one 50 s window per arm, and it is shorter than the state step.** The loading 1 -> 2 and session 0 -> 1
  transitions in this content arrive at about 54 s; the pair ended before them. A slower or faster arm would move the
  state words without the port being the cause.
* **The heartbeat count is unmeasured.** The stage-3 pump is the only probe that counts it and stage 3 was not reached;
  the next run should carry the pair to the first stage-3 sample and compare "advances since stage 3" between ON and
  OFF.
* **The event-2 wait's exact console condition is a model.** The measured call site gives the event, the INFINITE
  timeout and the position in the frame function; that it is a latency/flip wait and not a wait on a later point of the
  same frame is our reading, tested as a state machine but not against the console.
* **The forwarding can fall back silently by design.** A refusal of the pacer (suspended, not registered) leaves the old
  device-proxy answer, so an ON arm that never reaches the pacer would look like the control; the ON arm's wait trace
  shows `pacer` on both call sites, so that fallback did not run here.
* **`GetFrameStatisticsX` (device slot 81) still answers the beyond-vtable stub**, so the title's statistics calls
  cannot observe the new flip times yet. That is the next small slice after the heartbeat measurement.
* **The pacer's own presentation is unpaced when the switch is off on purpose**, and the off arm is the proof that the
  default path is unchanged.

### 12.6 Files, gates and the index row

The slice changes `port/src/frame_pacer.h`, `port/shims/xbox_present.h`, `port/shims/xbox_present.cpp`,
`port/shims/d3d12_xs_device.cpp` (slots 78-80 and the measurement probe only), `port/test/frame_pacer_test.cpp`,
`port/test/xbox_present_test.cpp`, `port/test/xs_proxy_mock_test.cpp` and this page. It does not edit the files another
slice owns (`port/src/deferred_compile.h`, `port/src/vulkan_backend.h`, `port/shims/d3d12_vulkan_device.cpp`), the
README or NOTICE. Gates: `PALMETTO_OK` for the runtime with these sources, `TESTS_BUILT`, the three tests above pass,
and of the docs checks the ones this page can affect pass (`test_docs_links.py`, `test_docs_list_items.py`,
`test_docs_repo_paths.py`, `test_docs_encoding.py`, `test_docs_repeated_lines.py`, `test_docs_table_rows.py`,
`test_docs_table_ids.py`, `test_docs_design_rows.py`, `test_docs_pr_template.py`, `test_docs_python_deps.py`); the two
failures the suite still reports are other slices' (`_setup/xcl/HANDOFF.md` CLI flags and the `PALMETTO_PIPELINE_HOLD*`
variables a concurrent slice introduced), not this page's. No git write was made; the lead commits the uncommitted list
from the slice report.

The index row for [docs/README.md](README.md) is the lead's: one line next to the other peer-port pages pointing at
this page. The [NOTICE.md](../NOTICE.md) row for this slice is the attribution in 12.7, adding the peer files and
commits to the existing peer-port first-visible-frame section.

### 12.7 Attribution

This slice was implemented from this project's own measurements. The behaviour was cross-checked against the public
GPL-3.0 projects named in section 11, files and commits as follows: VIctory's `crates/victory-gpu/src/frame.rs` and
`queue.rs` at `63bcca4` (a token per origin and a blocking flip wait before a present), Five Star Recomp's
[d3d12x-shim.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/d3d12x-shim.md) at `45071d52`
(a token used up by a present, held waits released at a present), XCL's
`crates/xcl/src/hle/d3d12x/present.rs` at `8b957be` (a present marks the frame token flipped, and the flag-2 token on
timeout), and PercyScarlett's `runtime/victory/apps/victory/src/run.rs` at `e44c465` (a vsync present pacing the title).
No code, comment text, identifier or table from those projects was copied into this tree: the state machine, the
callbacks and the tests are ours, derived from the call sites and boots above. The peers remain the copyright of their
contributors and their licences continue to apply to their own trees.

## 13. The implemented port: present-side attestation and an opt-in title hang-watchdog pause hold (P2, P3)

This section is the record of the P2 and P3 slices in section 7. Both are implemented in this tree's own structure from
this project's own measurements of the user's image, with the peers' mechanisms read as evidence only, as
[upstream_port.md](upstream_port.md) requires; section 13.7 repeats the credit. Status in one statement: **implemented,
opt-in and off by default, with pure rule/parser/thunk tests and the gates green; the A/B pair was taken but both arms
lost the host GPU before stage 3 (two losses, the task's stop rule) and stopped in the tree's post-device-loss crash
family, so the pair measured the hold engaging on the measured loading wait and the attestation counting the present
side, and it did not answer the stage-3 watchdog question. That question is section 13.5's next falsifier.**

### 13.1 What was measured before the port

Everything below is a measurement of the retail image (RVAs, relative to the preferred base 0x140000000) and of the two
arms in 13.4; no game data is quoted. The watchdog's engine-level shape was already recorded in
[3d_textures.md](3d_textures.md) section 10.6 and corrected in section 11.2; this slice re-read it to the byte it writes.

| measurement | value and where |
|---|---|
| hang watchdog | `game+0x2215A60`, a thread created by `image+0x2215A20` at the image's run-once point (0xA000 stack); each pass reads QueryPerformanceCounter and sleeps 1000 ms; after `0x1E` passes without the heartbeat advancing it calls the callback at `image+0x71B6348` and executes the deliberate null write `game+0x2215C35` (`mov dword ptr [0], 0`) then int3 |
| heartbeat | the qword at `image+0x71B6338`, petted by `image+0x22159F0` from the per-frame tick call sites `0x120799` and `0xC9E368`; the section-10.6 address `0x1471B6338` is the Ghidra operand address, not an RVA (section 11.2's correction) |
| pause byte | the **byte at `image+0x71B6330`** (the first byte of the block). The watchdog reads it at `image+0x2215B50` (`mov cl, byte ptr [rip+disp32]`); while bit 0 is set it stores the current counter into the heartbeat and resets the pass budget, so the abort is deferred instead of taken |
| pause setter | `image+0x2214A90`, a 7-byte leaf: `mov byte ptr [image+0x71B6330], 1; ret` (bytes `C6 05 99 18 FA 04 01`). Direct callers measured in the image: `image+0x21B6969` and `image+0x2214814`, both in the title's own loading/transition code |
| pause release/teardown | `image+0x22161B0` clears the pause byte and sets the watchdog's **stop byte** at `image+0x71B6340`; a set stop byte makes the watchdog thread tear down at `image+0x2215AB8` instead of deferring |
| budget and write shape | 0x1E one-second passes (about 30 s) and one writable byte; the peers' `+0x71b6330` pause byte and `+0x2214a90` setter are therefore this image's own word and instruction, re-measured here |
| media/UI present body | `image+0x22185A0`; its measured 16-byte entry prefix is `56 48 83 EC 40 80 3D 01 0B FA 04 01 0F 85 E8 00` (`push rsi; sub rsp,0x40; cmp byte [image+0x71B90AD],1; jne exit; cmp byte [image+0x71B90AC],1; jne exit`), and the disp32 in bytes 7..10 is the check that this is the measured body |
| present-pass keys and gate | `0x71B9048` (the boot-media gate, 2 = the stage the stage-2 wait releases on), `0x71B90AD` (first key), `0x71B90AC` (second key); the body exits at its first two compares until both keys read 1, as [decomp_first_ui.md](decomp_first_ui.md) sections 11 and 17 measured |
| presented-frame counters | the console queue proxy's slot 32 (PresentX) and slot 35 (submit) call counters and the allocator proxy's slot 9 per-presented-frame calls are readable from the runtime process, so the present side can be attested without touching a shim or the presenter |

Three readings made the design decisions:

1. **The peers' pause byte is this title's own switch, not an invented one.** The peer runtime holds
   `image+0x71b6330` by writing its own setter's byte repeatedly; this image's watchdog reads that byte and its own
   loading/transition code writes it through the same setter. A port that writes the same byte while a load the title
   is waiting for is legitimately outstanding is re-asserting the title's console behaviour, not synthesising state.
2. **Console-faithfulness is a window, not a switch.** The measured loading word `image+0x7453ED0` reads 2 for a load
   that is running (2..5 in the state trace's own meaning) and the state machine's current stage (`[[image+0x71273C8]
   +0x10]`) reads 2..3 during the boot's load; outside that window, and whenever the title's own stop byte is set, a
   hold would mask something that is no longer a load. The rule therefore holds only inside that window, only after the
   per-frame heartbeat has been seen advancing at least once, for a caller-chosen hard bound, releases as soon as the
   wait resolves, refuses by name when the title's watchdog is already stopped or the words are outside the measured
   band, and does not re-arm after a bound expiry while the same wait is still outstanding.
3. **The present side can name its state from counters it already has.** The body's entry counter plus the body's own
   gate and two keys separate "the body never ran", "the body ran and exits on its keys" (the measured state) and "the
   present pass was entered"; the console proxy counters separate "the layer presented" from "nothing yet". A draw that
   was issued but produced no pixel stays the presenter capture path's measurement (section 13.5).

The peers' five-star evidence (its documented type-`0x24` set event, [debugging.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/debugging.md))
is about the present-pass keys this tree already measured as unreachable in this build ([decomp_first_ui.md](decomp_first_ui.md)
sections 11.2 and 11.4); this slice samples those keys but does not synthesise their producer.

### 13.2 What was implemented

| change | what it is |
|---|---|
| a new include-only header under port/src ([title_watchdog.h](../port/src/title_watchdog.h)) | the measured watchdog/body constants, a byte-shape validation of the mapped image with named refusals, the two switch parsers, the pure pause-hold rule (phases, actions, hard bound, named reasons), the present-side classification, and the counting-thunk builder/installer for the body entry |
| `port/src/main.cpp` | the opt-in install and the two monitor threads: a 100 ms watchdog monitor that applies the rule to the measured words and writes only the pause byte, and a 2 s present-attestation sampler that prints the counters and the body's words (bounded: a line per state change, every thirtieth second otherwise, at most 80 periodic lines) |
| `port/test/code_hooks_test.cpp` | the pure parser/rule/classification checks, the image-shape refusals on a synthetic image buffer, the counting thunk's layout/refusals and a real call-through, and the present-state change key |

**The hold** ([title_watchdog.h](../port/src/title_watchdog.h), `hold_step`). One observation is `loading`, `stage`,
`heartbeat`, `heartbeat_advanced`, `watchdog_stopped` and the pause byte. The rule's phases are `idle`, `waiting`
(the measured wait is outstanding but the heartbeat has not been seen yet, so nothing is written), `holding`,
`released-resolved`, `released-bound` and `refused-watchdog-stopped`; a hold writes 1 every 100 ms because the title's
own code clears the byte, exactly as the peers re-write theirs. The write is a single `volatile` byte store to
`image+0x71B6330`; on release the monitor writes back the value it observed before its first write (normally 0, and it
does not touch the byte when the title had already set it). No title code is patched by the hold, no other title word is
written, and outside the switch the code path does not run at all.

**The attestation** ([title_watchdog.h](../port/src/title_watchdog.h) and the sampler in `port/src/main.cpp`). A 26-byte
counting thunk (`mov rax, counter; lock inc qword ptr [rax]; mov rax, original; jmp rax`) is placed within 2 GiB of the
image and every direct `E8`/`E9` branch to `image+0x22185A0` is redirected to it, in this process's mapped image only;
the thunk touches no stack and no argument register and is registered as a leaf. Each sample prints the console queue
proxy's PresentX/submit counts, the allocator proxy's per-presented-frame count, the body entries, the body's gate and
the low bytes of its two keys, the two semaphores and the classification. The body's words are read as dwords the way the
state trace reads them and masked to the key byte, so a neighbouring 0xff cannot be mistaken for a set key.

**Switches** (both off by default; unset and every off spelling keep the tree's path byte-identical):

- `PALMETTO_TITLE_WATCHDOG_HOLD`: unset/empty/`0`/`off`/`false`/`no` = off; `watch` = read the measured words and log,
  never write; a whole number of seconds in 1..600 = hold with that hard bound. Anything else is refused by name at
  startup (`PALMETTO_TITLE_WATCHDOG_HOLD is not off, watch or a whole number of seconds in 1..600`, or above the 600 s
  maximum), so a typo cannot silently arm a hold with a bound nobody chose.
- `PALMETTO_PRESENT_ATTEST`: set and not `0`/`off`/`false`/`no` = install the counting hook and start the sampler.

**Named refusals** (printed once, never guessed at): switch off; not off/watch/seconds or bound above 600; the image is
not mapped or is too small for the measured RVAs; `image+0x2214A90` is not the measured pause setter; the watchdog does
not read the measured pause byte; `image+0x2215C35` is not the measured abort; `image+0x22185A0` is not the measured
present body; no executable page within 2 GiB for a thunk; no direct call site of the body could be redirected; the
title's watchdog stop byte is set; the loading wait is outstanding but the heartbeat has not been seen; the hard bound
expired while the wait is still outstanding (sticky for that wait).

### 13.3 Tests

`port/test/code_hooks_test.cpp` gains the section-13 block in its own `check` group; the target prints `210 checks, 0
failures` (the file's class-0xa block had taken it to 105 before this slice). The block covers:

- **Switch parsers**: unset/empty/`0`/`off`/`false`/`no`/case+blank spellings; `watch`; 1 s and 600 s accepted; 601 s,
  `1.5`, `-3`, `0x10` and an unknown word refused by name; a refused value leaves the hold off.
- **Image shape**: a null image, a zero size and an image shorter than the measured RVAs refused; the measured setter,
  watchdog read, abort site and body entry accepted on a synthetic buffer; a one-byte-short buffer refused for both the
  watchdog block and the body entry; each of the four measured byte sites flipped and refused with its own name; the
  body entry is required only when the attestation asks for it.
- **Counting thunk**: the 26-byte layout (mov/lock inc/mov/jmp), short-buffer, null-buffer, null-counter and
  null-original refusals; allocation near the function and a real call-through that reaches the original, counts one
  entry per call and preserves both 64-bit arguments (the thunk changes no argument register).
- **The pause-hold rule**: an outstanding wait starts a hold with one write and re-writes while the title can clear it;
  a resolved wait releases with the held time recorded and returns to idle; a 5 s bound releases while the wait is still
  outstanding, does not re-arm for that wait, and a new wait after the resolution holds again (two holds counted); an
  outstanding wait before the heartbeat has been seen is `waiting` and writes nothing, then holds once the heartbeat has
  been seen; the title's stopped watchdog is refused and writes nothing; watch and off modes never write; a stage outside
  2..3 and a loading word outside 2..5 are not held.
- **Present attestation**: the four classification states and the line's change key.

The tree's GPU-free gate subset still passes in the same run: `frame_dump`, `unimplemented_policy` (98 checks),
`graphics_record`, `graphics_pso`, `buffer_va_index`, `dstorage`, `xapu_chad`, `deferred_compile` (50 checks),
`arena_size`, `xbox_present`, `xs_proxy_mock`, `xmem`, plus `code_hooks` (210 checks) and `frame_pacer`; the
pre-existing quarantined `d3d12_fence_timing_test` is not part of the subset.

### 13.4 A/B measurement

One light pair, one boot at a time under `_setup/boot_lock.ps1` (`-Who portwatchdog`), the boot template's content
environment (section 12's), `PALMETTO_TITLE_CLASS0A_SIZE=1` in both arms, one binary for both arms (SHA-256 first 16
`3BE83E409FE9A0D0`, the tree's gate build for the slice; the final tree rebuild after the two print/mask fixes of 13.5
has first 16 `B66AFC9E2B2BFF56`), 240 s limit per arm. The OFF arm adds
`PALMETTO_TITLE_WATCHDOG_HOLD=watch` and `PALMETTO_PRESENT_ATTEST=1` (read-only diagnostics); the ON arm adds
`PALMETTO_TITLE_WATCHDOG_HOLD=180` and the same attestation, so the pair's only difference is the hold's writes. Each
arm recorded one `[rt] replay: the Vulkan device was lost` and then stopped in the tree's post-device-loss crash family;
two losses in a row ended the boots per the task rule, so no stage-3 arm exists in this pair.

| measurement (one arm per column) | OFF: watch only | ON: hold, hard bound 180 s |
|---|---|---|
| verdict | STOP `game+0x4D9F636` read `0x223000007d0`, first exception at trace 55.5 s (harness wall 81.1 s), exit `0xC0000005` | STOP `game+0x86F97E` read `0x1a30001bfac`, first exception at trace 61.0 s (harness wall 146.2 s), exit `0xC0000005` |
| device loss | 1, between the 52.7 s and 54.8 s monitor samples | 1, between the 59.0 s and 61.0 s samples |
| stage 3 | not reached (`PALMETTO_STAGE3_PUMP` took no sample; state words stayed init 2 / loading 2 / session 1) | not reached (same) |
| state words through the last clean sample | init 2 at 20.0 s, loading 1 at 20.0 s, loading 2 and session 1 at 52.3 s, machine current/next 2/2 | init 2 at 18.0 s, loading 1 at 18.0 s, loading 2 and session 1 at 58.0 s, machine current/next 2/2 at 60.0 s |
| movie/front-end words | movie slot bound at 52.3 s (entity non-null, active 1, armed 1, media id `0x540714f3`), player state 0x1, `buffers 0`; front-end ready 1 running 1 | movie slot bound at 58.0 s, player state 0x3, probe 1, open 1, `buffers 0`; front-end ready 1 running 1 |
| watchdog words | pause byte `0x0` and stop byte `0x0` in every sample to 54.8 s; heartbeat advanced in every sample from 19.5 s; loading read 1 until 50.2 s, 2 at 51.4/52.7/54.8 s (phase waited, no write) | pause byte `0x0` in every sample to 57.0 s, `0x1` from the hold at 57.2 s (19 writes by 59.0 s, 39 by 61.0 s); stop byte `0x0`; heartbeat advanced throughout |
| hold | not armed (watch only): no write, `holds 0` | armed at 57.2 s on exactly the measured window (loading 2, stage 2, heartbeat seen), one hold, hard bound never reached before the loss |
| present attestation | 2.0 s: body-entries 164, presents 0; 32.1 s: presents 602, submits 1,204, frame-callbacks 603, body-entries 1,442, gate `0x2`, keys `0x0`/`0x0`, `body-exits-on-keys` | 2.0 s: body-entries 165, presents 0; 32.0 s: presents 656, submits 1,314, frame-callbacks 658, body-entries 3,001, gate `0x2`, keys `0x0`/`0x0`, `body-exits-on-keys` |
| `PresentX ok` / failed lines at the stop | 1,605 / 1 | 2,159 / 1 |
| presenter captures | 9 (frames 1-1,601), 2564x1421, every one mean rgb `0.00 0.00 0.00` | 11 (frames 1-2,001), 2564x1421, every one mean rgb `0.00 0.00 0.00` |
| exceptions recorded in the boot trace (all access violations; the first is the stop site, the rest follow the loss) | 6, including the same `game+0x86F97E` family the ON arm stopped at | 4, including the `game+0x52716EC` site the OFF arm also recorded |

**Verdict: the switches were exercised, the pair is not a stage-3 pair, and it is parity where it can be read.** In the
measured pre-loss window the control's pause byte was 0 during the loading wait (the title did not hold its own watchdog
there), and the ON arm's monitor engaged on exactly that window, wrote the pause byte and left the stop byte alone; the
attestation counted the body's thousands of entries and classified the present side as `body-exits-on-keys` in both
arms, with the same gate and key values and every capture black. Both arms then stopped in the same post-device-loss
crash family a second or two after the loss (the ON arm even after the OFF arm's secondary `0x52716EC` site appears in
its own list), so the difference between the columns is which thread faulted first after the host GPU dropped, not a
port effect. The prior abort point the plan wanted to run past (the watchdog's ~30 s after the heartbeat's last advance,
[3d_textures.md](3d_textures.md) section 10.6) is not reached in either arm because stage 3 is not reached; the pair
therefore does not claim that the hold keeps the title alive past it.

The same host dropped the device in a concurrent boot of this tree from the pre-slice binary, with neither switch set
(the concurrent slice's evidence, outside this repository), and that boot also stopped at a title site after the loss;
the pair's stops are the host family the FSR pages record, not a regression this slice can attribute.

### 13.5 Limits and the next falsifier

- **The A/B is device-loss-limited.** Two arms, two losses, no stage-3 window: the hold's engagement and the
  attestation's counts are measured, the stage-3 watchdog question is not. The next falsifier is a clean-host pair at
  the same environment with `PALMETTO_TITLE_WATCHDOG_HOLD=watch` in the control and `=180` in the ON arm, carried to
  the first stage-3 sample and beyond: compare the pause-byte series (does the title hold its own byte at stage 3?),
  the heartbeat's advance count and age, the stage/session words, the movie player state and the captures. If the pause
  byte is already 1 at stage 3 in the control, the hold is parity and should be recorded as such; if it is 0 while the
  heartbeat is stale, the hold is the measured difference and a longer ON arm says whether it defers the abort.
- **The A/B binary predates two polish changes.** After the pair, the attestation's key words are masked to the byte the
  body compares and a phase/classification change is logged even past the periodic-line cap. The hold rule, the write
  shape and the switches' off path are unchanged; the pair's logs printed the dword reads (the measured low bytes were
  0/0, so the quoted classification is the same) and its last printed sample was the 61.0 s holding line, so the
  post-loss transitions are unmeasured in that arm.
- **The hold's console-faithfulness is a model, not a console trace.** The title's setter, the watchdog's read and the
  byte's meaning are measured; that the console's loading code would hold the byte exactly while this title's loading
  word reads 2..5 is this slice's reading. A different image build changes the RVAs; the validation refuses by name
  rather than writing a byte whose meaning was not measured there.
- **Masking risk is bounded and named.** A hold that never released would mask a real hang; the hard bound, the sticky
  bound expiry, the stop-byte refusal and the loading/stage window are the bounds, and the unit tests cover each. The
  bound is the caller's number (1..600 s) and the monitor prints every phase change.
- **The presented plane's identity and the capture mean are not part of this attestation.** They stay the presenter
  path's measurements ([frame_capture.md](frame_capture.md), [window.md](window.md)); P2's remaining half is one line
  in the presenter capture hook, which is outside this slice's file scope.
- **The attestation hooks the body's direct callers only.** A body entered through a computed pointer would not be
  counted; the measured entry is reached by direct calls (the counter moved from the first seconds), and the body's own
  words are sampled regardless. The thunk's page lives for the process (the mapped image is this process's private
  copy; nothing on disk changes).

### 13.6 Files, gates and the index row

- **Files changed by this slice**: the new include-only header [title_watchdog.h](../port/src/title_watchdog.h),
  `port/src/main.cpp` (the include, the two monitor classes, and the opt-in start block after the class-0xa install),
  `port/test/code_hooks_test.cpp` (the section-13 block) and this section of `docs/peer_port_first_ui.md`. No shim,
  presenter, build-file, README or NOTICE file changed; `port/src/main.cpp`'s default path is unchanged when both
  switches are unset.
- **Gates on this tree**: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints
  `TESTS_BUILT`, the GPU-free subset of section 13.3 (14 targets including `code_hooks` and `frame_pacer`) exits 0, and
  of the docs checks all files pass except the environment-variable check, which reports one concurrent slice's
  variable (`PALMETTO_GPU_VIEW_PAIR_TRACE` in `port/shims/xmem.cpp`) and not this slice's two, which this page names. The
  two new variable names are `PALMETTO_TITLE_WATCHDOG_HOLD` and `PALMETTO_PRESENT_ATTEST`.
- **Index row**: [docs/README.md](README.md) already carries this page's row, so no index change is needed for the
  section; the lead may extend that row's description with "implemented watchdog hold and present attestation" if the
  row is refreshed. The [NOTICE.md](../NOTICE.md) row needed is the section-13.7 attribution, appended to the existing
  "Implemented peer ports (reference only)" paragraph.
- **No git write**: the lead commits the uncommitted list. Evidence is outside the repository under
  `D:\leonida-work\fsrstable\boot-watchdog-off`, `boot-watchdog-on`, `capture-watchdog-off`, `capture-watchdog-on` and
  `D:\leonida-work\portwatchdog` (the analyzer and the static measurement scripts).

### 13.7 Attribution

This slice was implemented from this project's own measurements (the watchdog's bytes and call sites, the body's entry
bytes and the two boots above). The behaviour was cross-checked against the public GPL projects named in section 11,
files and commits as follows: PercyScarlett's `runtime/victory/apps/victory/src/run.rs` at `e44c465` (its watchdog hold
sets the title's byte at `+0x71b6330` through the setter at `+0x2214a90` every 200 ms, from the same image revision),
VIctory's `apps/victory/src/run.rs` at `63bcca4` (the `VICTORY_POKE` poke that writes a title byte twice a second and names
the 30 s tick), and Five Star Recomp's [debugging.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/debugging.md)
at `45071d52` (the watchdog flag and its type-`0x24` set event, the present-pass chain this tree measured as
unreachable). No code, comment text, identifier or table from those projects was copied into this tree: the measured
constants, the rule, the thunk, the parsers and the tests are ours, derived from the call sites and boots above. The
peers remain the copyright of their contributors and their licences continue to apply to their own trees.

## 14. The implemented port: the measured display answers, the UI page/resource probe, and what the probe found (P4, P6)

**Status: implemented, tested and measured with two light arms (one 69 s arm that lost the host device, one clean
300 s arm that reached and held stage 3); no presented pixel changed, and the UI question is answered.** The display capability answers the peers serve are now this tree's defaults: the HDR answer
follows the host desktop through DXGI (the five-star mechanism) instead of a switch, the display timeout deferral is
served with a Windows display-required power request (every peer that implements it does), and the switches that force
either side are kept and refused by name when misspelled. The UI page/resource probe answered the audit's P4 question
with a positive and a negative: the title's resource-type dispatcher resolves the handler for **RBF0**-typed resources
**131 times in the 69 s arm and 141 times in the clean arm, all of them before the stage-3 entry** (so the
resource-type/page-data layer *does* run), while the three front-end page owners probed beside it - profile-name text
entry, the profile card and the first-run flow state machine - have **0 hits in both arms**, and every presenter capture
is still mean rgb `0.00 0.00 0.00`. The producer the peers show is therefore above the page-data layer: the title loads
and identifies its RBF0 page data, and the first-run page/screen flow that would draw it never runs. The first arm
stopped on the host's first Vulkan device loss at 69.2 s (before stage 3); the second was clean for its full 300 s and
held stage 3 for the last 230 s, and the stop rule (two losses) was therefore never reached. Both arms are in 14.4.

### 14.1 What was measured before the port

The display surface is one thunk and two wrappers; the page layer is a registered type dispatcher. All RVAs are the
retail image's (preferred base `0x140000000`); no game data is quoted.

| measurement | value and where |
|---|---|
| the title's display import surface | `tooling/gdk_runtime_calls.py` finds exactly **one** `XDisplayImpl` thunk in the executable, referenced once: `XDisplayTryEnableHdrMode` (class `03f0fe74-…`, interface `35f07670-…`, slot 4). The title cannot ask the runtime any other display capability: IXDisplayImpl's methods are the docs' unknown slot 3 and this slot 4 only |
| the two callers | `game+0x2481D68` (inside `game+0x2481CA0..0x2481E73`) and `game+0x5A38971` (inside `game+0x5A38910..0x5A38B1B`); `game+0x2481CA0` is called once from `game+0x247AB19`, `game+0x5A38910` once from `game+0x5A388A6` |
| the preference and info shape | disassembled `game+0x2481CA0`: `mov ecx,0; mov edx,0; call XDisplayTryEnableHdrMode` - preference **PreferHdr** (0) and a **null** info pointer. `game+0x5A38910`: the same preference and a **non-null** info block in its own stack; on result 0x1 it copies the three floats to its display settings (`+0x274`, `+0x27C`) and sets the enabled byte (`+0x280`), on 0x2 it takes the disable path |
| the display-mode answer the title consumes | the same wrapper `game+0x2481CA0` then enumerates a mode list through an indirect display call (a `0x20`-byte record per mode: width +0x0, height +0x4, DXGI_RATIONAL refresh +0x8, format +0x10, scanline order +0x14, scaling +0x18 - the console mode-record shape PercyScarlett's layer answers) and compares each record's refresh numerator with `0x78` (120). It stores the byte `1` at `image+0x760E660` when a 120 Hz mode is offered and the qword DXGI_RATIONAL `{120,1}` or `{60,1}` at `image+0xA9438D4` |
| the mode-list source in this tree | the title's DXGI calls are this tree's proxy (`port/shims/dxgi_com.cpp`), whose `GetDisplayModeList`/`GetDisplayModeList1` answer the host's `EnumDisplaySettingsW` modes plus the current mode (`collect_modes`). The peers differ here: PercyScarlett answers **one** console mode (4K by default, `VICTORY_DISPLAY_MODE` overrides it, 60 Hz progressive) and its own comment names this exact consumer ("the title looks for a 120 Hz one to allow 120 Hz frame intervals"); five-star answers the host's modes |
| the consumed display words (pre-port logs) | not sampled before this slice; section 14.4's state-trace line now reads them: high-refresh flag and refresh rational |
| display capability queries in prior arms | four `QueryApiImpl XDisplayImpl` requests at +2.23/+2.82/+23.31/+25.73 s in `boot-long1` (and +1.07/+1.67/+16.28/+18.46 s in `boot-input1`); the old shim logged its one HDR answer once ("HDR disabled (SDR output)") and the title continued. No arm has a `XLauncherImpl slot 4`/`slot 5` line: the display timeout deferral was **never called** in the measured windows |
| the deferral wrappers | `game+0x23541D0` (`XDisplayAcquireTimeoutDeferral` wrapper: calls `game+0x55DEC88` at `game+0x2354221` and keeps the handle at its display object's `+0x4A8`) and `game+0x2354240` (`XDisplayCloseTimeoutDeferralHandle`: calls `game+0x55DECF8` at `game+0x235425A` and clears the field); both are address-taken in one display object's vtable (`.rdata` VA64 at `0xA1B2D58`/`0xA1B2D80`), never called in the measured windows |
| the host display this port was measured on | the same DXGI enumeration the shim uses, run against the host: `\\.\DISPLAY1` colour space 0 (SDR, G22/P709), 8 bits per colour, min 0.5 / max 270 / max-full-frame 270 nits. So the auto answer on this machine is Disabled - the pre-slice answer - and the boot is a parity arm for HDR by construction |
| the RBF0 magic test (static) | the only `.text` site that compares a dword with `"RBF0"` (little-endian `0x30464252`) is `game+0x135A290`: `cmp ecx,0x30464252; sete al; ret` (10 bytes, inside no larger compare ladder). A fixed scan finds **no** `E8`/`E9` rel32 branch, no 8-byte VA and no 32-bit RVA data reference to it - it is reached through a register call, and the probe in 14.4 names the caller |
| the RBF0 dispatcher (static) | `.pdata` gives the caller of `game+0x135A290` as the 142-byte function `game+0x135A200..0x135A28E`: it walks an array of matcher entries (`count` at `+0x20`, array at `+0x18`, each entry a callback at `+0x10` with an optional context at `+0x8`), calls each with the 32-bit identifier, and returns the first matcher's index or `0xFFFFFFFF`. The RBF0 predicate is one registered matcher; the identifier reaches it in `ecx` (`0x30464252` in every measured hit), so the title asks this dispatcher to resolve the handler for an RBF0-typed resource |
| front-end page owners (prior arms) | profile-name text entry `0x2234A40` and profile card `0x3DAF310` had 0 hits in the two arms of `decomp_first_ui.md` section 17.6; the first-run flow state machine `0x22364A0` (entered from `0xB1AE2F`, `0x27D25E8`, `0x27D4E93`) and the 2D batch path (`0x517A290` with 82 direct call sites, rectangle/fill `0x3A1B860`, transform copy `0x58C8950`, submit `0x2AACF0`) were not probed |

Three readings made the design decisions:

1. **The HDR answer is a host answer, not a policy.** A console answers what its output can do; five-star reports the
   Windows desktop's own HDR10 state and luminance through `IDXGIOutput6::GetDesc1`, and the VIctory family answers
   Disabled only because its swap chain cannot present HDR, with its own note to report the host's luminance once it can.
   This tree's presenter already composes and presents HDR10 planes (`port/shims/xbox_present.cpp`, exercised by its
   pixel tests), so the host answer is the honest one here. On this SDR host it equals the pre-slice answer, which is
   what makes the arm a parity measurement.
2. **The deferral is a console answer too.** The title asks the system to keep the display on for as long as a deferral
   is open; five-star, VIctory and PercyScarlett all map it to a Windows `PowerCreateRequest`/`PowerSetRequest` display
   requirement. Refusing it with `E_FAIL` was the stand-in, not a console behaviour.
3. **The UI question is one probe set, not a rewrite.** The audit's falsifier is "no UI page or batch ask exists in the
   window". The probe therefore instruments both sides of that sentence in one boot: the page-data gate (the RBF0 magic
   matcher) and the page owners that would consume a loaded page, with the same bounded int3 machinery the movie probes
   use, and reads the display state the title consumed from its own words instead of another hook.

### 14.2 What was implemented

| change | what it is |
|---|---|
| `port/shims/xgameruntime_offline.cpp`, the HDR answer | per call, the switch is read (no cached value): `auto`/unset/empty = the host answer; `1`/`on`/`true`/`yes` = Enabled with the documented 0.01/1000/1000 nits; `0`/`off`/`false`/`no` = Disabled; anything else is refused by name once and auto applies. The host answer loads the real `dxgi.dll` by name (`CreateDXGIFactory1` through `GetProcAddress`, no build-file or link change), takes the first adapter with an output and its output 0, and reads `IDXGIOutput6::GetDesc1`: Enabled with the measured min/max/max-full-frame luminance when the colour space is `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` (HDR10), else Disabled; no output answering is Disabled. The info pointer stays optional, the output is zeroed when Disabled, and every call is logged (first 8, then every 16th) with the preference, whether the caller passed an info pointer, and the answer's source |
| `port/shims/xgameruntime_offline.cpp`, the display timeout deferral | `XDisplayAcquireTimeoutDeferral` creates one `PowerCreateRequest` with a display-required simple reason per deferral, sets `PowerSetRequest(PowerRequestDisplayRequired)`, and hands the request out as the handle after recording it in a file-local table; `XDisplayCloseTimeoutDeferralHandle` clears and closes only a handle in that table (null, double close and a foreign handle do nothing). A Windows failure is returned as `HRESULT_FROM_WIN32(GetLastError())` with a null handle; a null output pointer is `E_INVALIDARG`. `PALMETTO_DISPLAY_DEFERRAL=0`/`off`/`false`/`no` restores the pre-slice `E_FAIL`; a value that is neither side is refused by name once and the console answer applies |
| `port/shims/xgameruntime_offline.cpp`, test hooks | the answer counters, the switch parser, a synthetic-host setter and the open-deferral count, so the unit test is deterministic on any machine and never depends on the host display |
| `port/src/main.cpp`, the display state line | the existing `PALMETTO_STATE_TRACE` sampler gains one line: the high-refresh flag byte at `image+0x760E660` and the chosen DXGI_RATIONAL refresh at `image+0xA9438D4` (numerator/denominator). No hook, no write: it reads the words the title's own display service stored |
| `port/src/main.cpp`, the UI probe | `PALMETTO_UI_PROBE=pages` installs four one-shot int3 entry probes through the existing poke machinery: profile-name text entry `0x2234A40`, profile card `0x3DAF310`, the first-run flow state machine `0x22364A0`, and the RBF0 magic matcher `0x135A290` (with a byte-shape check of `cmp ecx,0x30464252` before poking: if the bytes are not the measured compare the probe refuses the site by name and installs the other three). `=draw` is the next arm's set: batch build `0x517A290`, rectangle/fill `0x3A1B860`, transform copy `0x58C8950`, submit `0x2AACF0`. Unknown groups are refused by name. Both switches are off by default; the default path installs no probe and reads no extra word |

No build file, CMake list, presenter, DXGI proxy, README or NOTICE file changed. The default answer of every other class
in the file is untouched.

### 14.3 Tests

`port/test/xgameruntime_offline_test.cpp` gains the section-14 block; the target prints **116 checks, 0 failures** in
both ctest variants (`xgameruntime_offline` with no switch, and `xgameruntime_offline_hdr` with `PALMETTO_HDR=1`). The
block covers:

- **The HDR switch parser**: unset/empty/`auto`/padded ` AUTO ` = auto; `1`/`on`/`TRUE`/` yes ` = forced on;
  `0`/`off`/`false`/` NO ` = forced off; `2`/`banana` = refused by value.
- **The answer rule against a synthetic host**: auto on an SDR host = Disabled with the info cleared; auto on an HDR10
  host = Enabled with the host's 0.5/270/260 nits; auto with no host output = Disabled; the host is probed once per
  answer; `PALMETTO_HDR=1` = Enabled with 0.01/1000/1000 nits and the host not asked; `PALMETTO_HDR=0` on an HDR10 host
  = Disabled; `auto` spelled out = the host answer; a refused value applies auto; the counters count the calls and the
  Enabled answers; the info pointer is optional.
- **The display timeout deferral**: two deferrals are two requests; the open count follows them; close clears and a
  double close, a null close and a foreign handle change nothing; a null output pointer is `E_INVALIDARG`; the
  `PALMETTO_DISPLAY_DEFERRAL=0`/`off` fallback restores `E_FAIL` with a null handle and leaves nothing open; an unknown
  switch value is refused by name and the console answer applies.

### 14.4 The arms

Two boots under the shared lock, the boot template's content environment (section-12/17 environment: deferred compile,
2 s state trace, alloc watch, stage-3 pump, presenter captures every 200 frames to 12), the mitigation
`PALMETTO_TITLE_CLASS0A_SIZE=1` and `PALMETTO_UI_PROBE=pages`; `-Light`. Arm A ran to a 180 s limit, arm B to a 300 s
limit. The binaries are two gate builds from the same tree: A `5B13C561086BD0DD…`, B `5B35ADCFC0DD0758…` (SHA-256 first
16); the only object that changed between them is the concurrent XG-upload slice's `port/shims/xg_xs.cpp` (its object
was rebuilt at 06:52:56, after arm A), while this slice's two runtime objects were already final before both links, so
the two arms ran the same display answers and the same probe set. The host display is SDR (14.1).

| measurement | arm A: `boot-portui1` | arm B: `boot-portui2` |
|---|---|---|
| limit / verdict | 180 s limit; STOP `game+0x6D90A2` reading `0x4f747d2c654`, exit `0xC0000005`, 1 exception, wall 69.2 s | 300 s limit; **ALIVE 300.2 s**, killed by the harness timeout, 0 exceptions |
| device loss | **1**, `[rt] replay: the Vulkan device was lost` at timeline t=67,495.87 ms (state-trace clock ~66 s), before the stop | **0** |
| stage/session words | init 2 from 22.0 s, loading 1 from 22.0 s, session 0 -> 1 at 64.0 s, loading 1 -> 2 at 66.0 s; stage 3 **not reached**; machine 2/2; boot-media gate 2; platform gate 0x101; media semaphores 0x714/0x71c | init 2 at 20.0 s, loading 1 at 20.0 s, loading 1 -> 2 and session 0 -> 1 at 64.0 s, **init 2 -> 3, session 1 -> 3 and machine pending 2 -> 3 at 68.0 s**, held to the 298.0 s sample; loading 2; platform gate 0x101; boot-media gate 2; media semaphores 0x714/0x71c; the stage-3 pump samples stage 3 at 66.6 s with the heartbeat advancing (1,232 advances, 26 since stage 3, last 0.1 s before the +2 s sample) |
| display queries (the shim's per-call lines) | 4 calls, all preference 0: one with an info block (the `game+0x5A38910` wrapper) in the first seconds, three with a null info pointer (the `game+0x2481CA0` wrapper) at about 2.8 s, 20-22 s and 24-26 s; every answer **Disabled (auto: host desktop SDR)** | identical: 4 calls, same shapes and the same order, every answer **Disabled (auto: host desktop SDR)** |
| display answer the title consumed | high-refresh flag **0x0**, refresh **60/1** from the 4.0 s sample to the last sample | high-refresh flag **0x0**, refresh **60/1** from the 4.0 s sample to the 298.0 s sample |
| display timeout deferrals | **0** calls (no `XLauncherImpl slot 4/5` line) | **0** calls |
| UI probe installed | 4 sites poked (`0x2234A40`, `0x3DAF310`, `0x22364A0`, `0x135A290`); `PALMETTO_UI_PROBE=pages: 4 front-end page-owner probe(s)` | the same 4 sites and the same line |
| UI probe hits | text entry **0**, profile card **0**, first-run flow state machine **0**, RBF0 magic matcher **131**, every hit with `ecx` = `0x30464252` and return address `game+0x135A247` (inside the dispatcher `game+0x135A200`); distribution 1 in the first seconds, 22 in the 20-30 s window, 108 in the 60-67 s window | text entry **0**, profile card **0**, first-run flow state machine **0**, RBF0 magic matcher **141**, same `ecx`/return address; distribution 1 in the first seconds, 22 in the 20-40 s window, 118 in the 60-68 s window |
| RBF0 hits after stage 3 | not applicable (stage 3 not reached) | **0** from the stage-3 entry (line 251,920 of 1,299,391) to the 300 s kill; the last hit is line 251,448, before the entry |
| front-end manager | created, ready 1, running 1 from the 40.0 s sample, all seven service holders live with their measured vtables (`…654D830` … `…654D350`) to the last sample | created, ready 1, running 1 from the 40.0 s sample, the same seven services and vtables to the 298.0 s sample |
| movie chain | slot 0 bound by the 66.0 s sample (entity non-null, active 1, armed 1, media-id `0x540714f3`); player state 0x3, probe 1, open 1, **buffers 0** | slot 0 bound; from stage 3 to 298.0 s player state **0x9**, non-null Bink handle, open 1, probe 0, error 0, **buffers 0** |
| presenter captures | 12 (frames 1-2201), 2564x1421, every one mean rgb `0.00 0.00 0.00` | 12 (frames 1-2201), 2564x1421, every one mean rgb `0.00 0.00 0.00` |
| content readbacks | none armed in this arm (`PALMETTO_READBACK_FULL` off; the template's centre readback wrote no PPM) | four 128x128 centre readbacks: draws 4800 and 4802 mean `0 0 0`, peak 0 (`0/16,384` non-black); draws 4801 and 4803 mean 119, peak 204 - the layer's own write-control colour `51 102 204` (`16,384/16,384` non-black), not title content |
| pipeline creation at the last sample | at 60.1 s: 54,905 graphics and 4,825 compute programs created by the proxy, 2 graphics and 321 compute driver pipelines | at 295.1 s: 55,055 graphics and 4,825 compute proxy programs, 4 graphics and 362 compute driver pipelines - no UI batch program is separately nameable, and the count barely moves after the load window |
| D3D12 calls / trace events | 146,903 calls, 8,083 trace events | 1,111,478 calls, 51,920 trace events |

The second arm is the one that answers the stage-3 question: the **clean 300 s arm** reached stage 3 at 68.0 s and
held it to the 298.0 s sample (`init 3`, `loading 2`, `session 3`, machine current/next 3/3) with **0 exceptions and 0
device losses**, and in that arm the RBF0 dispatcher's 141 hits all precede the stage-3 entry: after stage 3 the same
probe records **0 hits for the next 230 s**, the three page owners stay at 0 hits, the movie player holds state `0x9`
with `buffers 0`, the front-end manager stays ready and running, every one of the 12 presenter captures is mean rgb
`0.00 0.00 0.00`, and the four 128x128 centre readbacks read `0 0 0` on the two render-target draws and the layer's own
write-control colour `51 102 204` on the two control draws.

The verdict, from both arms: **the page/resource layer runs in the load window and the page flow does not.** The title
asks its resource dispatcher for the RBF0 handler 131/141 times, in three clusters that line up with the boot's own
resource steps (one in the first seconds, 22 in the 20-40 s working-set step, and 108-118 in the loading 1 -> 2 step
that ends at stage 3) - so RBF0-typed resources are being matched to their handler before stage 3, not missing. The
parse itself is one step below this measurement (the dispatcher returns the handler index; the handler entry is the
next probe), and the hits are the strongest reach signal a page-data probe can show without naming a resource id. The
three page owners beside it never run in either arm, the front-end manager renders throughout, the movie slot binds,
and the presented frame stays black. The audit's P4 sentence is therefore answered the other way round from the plan's
expectation: the UI resource/page layer is *not* the missing producer; the missing producer is the first-run
page/screen flow above it (the same state-step conclusion sections 6 and 12 reached by other routes).

### 14.5 Verdict, limits and the next falsifier

- **The HDR answer is a parity change on this host and a real answer on an HDR one.** On the SDR host the auto answer
  equals the pre-slice Disabled answer; the arm cannot show what the title does with Enabled. A clean HDR desktop would
  be the measurement: the same boot on an HDR10 output should show the Enabled answer with the output's luminance and
  the title's plane/frame path taking its HDR branch (the presenter's HDR10 path is already tested, so the falsifier is
  a capture or a plane colour-space change, not a crash).
- **The display-mode answer is measured, not changed.** The consumer is the title's 120 Hz check, and this tree's
  answer comes from `port/shims/dxgi_com.cpp` (host modes). The two peer answers differ (PercyScarlett/VIctory: one
  console mode at 60 Hz; five-star: host modes). On this 60 Hz host all three produce `{60,1}`; on a 120 Hz host ours
  would produce `{120,1}` while PercyScarlett's fixed answer keeps 60. The next falsifier is a boot on a 120 Hz output
  comparing the consumed refresh word and the title's frame interval. Changing that answer means `dxgi_com.cpp`, outside
  this slice's file scope; the state-trace line is the measurement it needs.
- **The deferral answer is installed and unexercised.** The title never calls it in the measured windows, so the port is
  parity by construction; a boot in which the title acquires one (a menu or a load that wants the screen on) is the
  falsifier that would exercise the handle lifecycle in the title, not only in the unit test.
- **Two arms, one loss and one clean long window.** The 69 s arm lost the host device once (the first of the session's
  two-loss rule, which was not reached because the second arm was clean); the 300 s arm held stage 3 for 230 s with no
  exception and no loss. The RBF0/post-stage-3 negative is therefore measured in a clean window, and the page owners'
  zero is measured twice. The pre-stage-3 second-count difference between the arms (131 vs 141 hits, 108 vs 118 in the
  load burst) is the arms' own timing, not a switch effect: both ran the same binary and the same probe set.
- **The probe names a layer, not a schema.** All 131 dispatcher hits pass the RBF0 magic, but the RBF0 corpus carries UI
  framework/data schemas *and* movie-subtitle and tuning/config schemas, so the count measures "RBF0-typed resources are
  being identified", not "UI pages specifically". A probe that names the loaded resource ids would be game data and is
  not made; the page owners' zero hits is what separates page data from page flow.
- **The int3 probes are entry-only and the same machinery the movie probes use.** A hit costs an exception and a
  single-step; 131 hits in 69 s is far below the rate that distorts a boot, and the three quiet sites cost nothing. An
  interior branch is not separated, and a function reached through a computed pointer still fires (that is how the RBF0
  matcher was found after the static scan missed its address-taken form).

### 14.6 Files, gates and the index row

- **Files changed by this slice**: `port/src/main.cpp` (the display constants and state-trace line, the
  `PALMETTO_UI_PROBE` groups and their shape check), `port/shims/xgameruntime_offline.cpp` (the HDR answer, the deferral
  answer, the test hooks and the header comment), `port/test/xgameruntime_offline_test.cpp` (the section-14 block) and
  this section of `docs/peer_port_first_ui.md`. No CMake, presenter, DXGI proxy, README or NOTICE file changed.
- **Gates on this tree**: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK`, `_setup\build_tests.cmd` prints
  `TESTS_BUILT`, the 16-target GPU-free gate subset passes 16 of 16 (including `xgameruntime_offline` and
  `xgameruntime_offline_hdr`), `xgameruntime_offline_test` prints **116 checks, 0 failures** in both variants, and all
  12 `tests/test_docs_*.py` files pass (run with the session's broken `GIT_CONFIG_*` variables cleared, per the rule the
  earlier sections record). This section names `PALMETTO_UI_PROBE` and `PALMETTO_DISPLAY_DEFERRAL` (the two new
  variables) as whole words, so the environment-variable check passes, and it keeps `PALMETTO_HDR`'s meaning.
- **Stale neighbour rows**: [runtime_imports.md](runtime_imports.md) still describes `XDisplayTryEnableHdrMode` as
  "answers Disabled, because the host output is SDR" and the deferral as failing with `E_FAIL`. That page is outside
  this slice's write scope; the lead should refresh those two bullets when refreshing the peer pages.
- **Index row**: [docs/README.md](README.md) already carries this page's row; the lead may extend its description with
  "display answers and UI page probe (sections 14)". The [NOTICE.md](../NOTICE.md) row needed is 14.7's attribution,
  appended to the existing "Implemented peer ports (reference only)" paragraph.
- **Evidence (outside the repository)**: `D:\leonida-work\fsrstable\boot-portui1` and `boot-portui2` (stderr, stdout,
  summary, boot trace, meta with the exact environment), `capture-portui1`/`capture-portui2` (12 presenter PPMs +
  `index.txt` each) and `readback-portui2` (4 centre PPMs); the static display decompiles under
  `D:\leonida-work\decomp\out-uidisplay`, `out-uidisplay-dis` and the RBF0 dispatcher under `out-uirbf0fn`; the host
  display probe (`hosthdr.cpp`) and the arm analysis scripts under `D:\leonida-work\uiprobe`. RVAs, counts, times,
  register values and image means only; no game data and no peer code.
- **No git write**: the lead commits the four uncommitted files.

### 14.7 Attribution

This slice was implemented from this project's own measurements (the two display wrappers and their disassembly, the
consumed display words, the resource dispatcher and its matcher, the arms in 14.4 and the unit-test host override). The
behaviour was cross-checked against the public projects named in section 11, read as evidence only, at the refreshed
clones: Five Star Recomp's `src/FiveStar.Loader/Shims/Gdk/DisplayService.cs`, `HdrDisplay.cs`, `DisplayProbe.cs` and
`XDisplayShim.cs` at `ce2b333b` (the HDR answer reports the Windows desktop's HDR10 state and luminance from
`IDXGIOutput6::GetDesc1`, and the display timeout deferral holds a `PowerCreateRequest` display requirement); VIctory's
`crates/victory-hle/src/modules/display.rs` at `63bcca43` and PercyScarlett's
`PercyPort/crates/percyport-hle/src/modules/display.rs` at `e44c465e` (Disabled while the presentation is SDR, with the
note to report the host luminance once HDR can be presented; deferrals as power requests), and PercyScarlett's
`runtime/victory/crates/victory-gpu/src/dxgi.rs` at `e44c465e` (the single 4K/configured 60 Hz progressive mode record
whose consumer is the title's 120 Hz check); XCL's `crates/xcl/src/hle/xgameruntime/gdk_table.rs` at `623d02e4` (the
`XDisplayTryEnableHdrMode` class/interface/slot entry). No code, comment text, identifier or table from those projects
was copied: the answer rule, the DXGI probe, the power-request table, the probes and the tests are ours, derived from
the call sites, the runtime arms and the unit tests above. The peers remain the copyright of their contributors and
their licences continue to apply to their own trees.

## 15. The implemented port: presenter fit/scale/colour-space attestation and a synthetic non-black present control (P7)

This section is the record of the P7 slice in section 7. It was built from this tree's own measurements of the user's
image, with the peers' mechanisms read as evidence only, as [upstream_port.md](upstream_port.md) requires; section 15.7
repeats the credit. Status in one statement: **implemented, opt-in and off by default, with the pure and GPU tests green
and two light arms taken; the arms attest the presenter end to end (a known non-black pattern reaches the presenter
capture with its exact expected values, in both the measured stretch and the fitted rule) and both then lost the host
device, so the two-loss stop rule ended the boots before stage 3.** The fit rule is the one measured divergence and is
opt-in: the default present path is unchanged byte for byte.

### 15.1 What was measured before the port

Everything below is either a measurement of the current tree's present path in the two arms of 15.4 or a fact already
recorded on a cited page. No game data and no package path is quoted: formats, extents, words and means only.

| measurement | value and where |
|---|---|
| the window the presenter adopts | the title's own `sgaWindowClass` window: client 2564x1421 (aspect 1.804), outer 2580x1460; the monitor as the process sees it is 2560x1440 at 0,0 with a 2560x1392 work area (15.4's geometry line; [window.md](window.md) section 5) |
| the process's DPI view | the host's physical output is 3840x2160 at 150% (the host probe of [peer_port_first_ui.md](peer_port_first_ui.md) section 14.1's arm was SDR; the resolution is this slice's host measurement), while the boot process reports a 2560x1440 monitor: the process is DPI-unaware, so the window's 2580x1460 outer rectangle is 20x20 pixels larger than its virtualized monitor. The title's window is therefore the "oversized window" a PC layer has to fit |
| the swapchain | 2564x1421 (the window's client area, the presenter's `extent_fn`), `VK_FORMAT_B8G8R8A8_UNORM` (format 44), colour space 0 (`VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`), source format family UNORM, no HDR10 request and no ST.2084 surface format; the capture copies the same extent and format |
| the presented plane | 1 plane: 3840x2160, D3D12 format 28 (`R8G8B8A8_UNORM`, the replay layer's `VK_FORMAT_R8G8B8A8_UNORM`, "format 37"); colour-space word 0 (G22/P709 SDR), `ScaleFilter` 0, `Flags` 0x0; source rectangle 0,0..3840,2160 and placement 0,0..1,1, i.e. the whole plane to the whole output. The present path's placement validation maps the whole-output placement to none, so the plain blit path runs and the compose pass does not, which the arms confirm: the synthetic control substitutes only inside `present()`, and both arms' captures are the pattern |
| the default fit | the plain blit stretches the whole source to the whole client area: scale x 2564/3840 = 0.6677, y 1421/2160 = 0.6579, so the 16:9 plane is shown about 1.5% horizontally stretched against its own aspect ratio and there are no bars |
| the default scale filter | bilinear when the scale differs (the source format allows linear filtering), nearest when the blit is 1:1; the capture copies the swapchain image's bytes, so the mean is in the output's encoded 8-bit values and no transfer decode is applied to the readback |
| the colour-space rule | the swapchain's format family follows the presented source's format family: an sRGB source gets an sRGB swapchain (the blit's decode and encode cancel), any other source a UNORM one; an HDR10 plane asks for the surface's ST.2084 format. On the measured plane (UNORM 8-bit, colour-space word 0) the chosen output is UNORM 8-bit with the sRGB-nonlinear colour space, and the UNORM-to-UNORM blit is an exact value copy: 15.4's band means are the pattern's own bytes |
| the console/peer model | a console presents its output plane to a fixed-aspect display, so the picture's aspect ratio is the plane's; Five Star Recomp fits its oversized window to the monitor, scales the plane into the client area as its aspect rectangle over black bars (its PresentScaler) and picks the swap-chain colour space from the plane's format family, and the other peers present the console plane whole. The colour-space family rule is what the tree already measured as its own behaviour; the fit is not |
| the divergence | geometry only: the tree stretches where the console/peer model preserves the plane's aspect ratio, and it does not fit the title's oversized window. The scale filter and the colour-space family rule are measured equal |

The two readings that decided the design: (1) the picture geometry is the only measured difference, so the port has to be a
fit rule and nothing else about which texture, which colour space or which filter; (2) the present path cannot be judged
from a black content frame, so the control has to present a known non-black source through the same call, and the capture
has to read its exact values.

### 15.2 What was implemented

| change | what it is |
|---|---|
| new include-only header [port/src/present_fit.h](../port/src/present_fit.h) | the pure fit/scale rule: `Rule` `fill`/`fit`, `scale_rect` (integer pixels, nearest-pixel rounding, source aspect preserved, centred, the rest of the output black), `has_bars`, and the `PALMETTO_PRESENT_FIT` parser with a named refusal. No Vulkan dependency, so the whole rule is testable without a GPU |
| [port/src/vulkan_present.h](../port/src/vulkan_present.h), the fit rule | `record()` blits into the rule's destination rectangle; under `fit` the swapchain image is cleared to black first when the rectangle leaves bars, and a device without `vkCmdClearColorImage` keeps the stretch rather than writing uninitialised pixels. The filter stays the measured one (nearest only when source and destination are the same size). The compose path keeps the title's own placements: the measured plane never composes |
| [port/src/vulkan_present.h](../port/src/vulkan_present.h), the attestation | `PALMETTO_PRESENT_GEOMETRY=1` prints once the swapchain extent, chosen format name and number, colour space number, source format family, HDR10 request/availability and capture state, so one boot attests the colour-space decision instead of inferring it from code |
| [port/src/vulkan_present.h](../port/src/vulkan_present.h), the synthetic control | `PALMETTO_PRESENT_SYNTHETIC=N` (1..8) presents the presenter's own known 16:9 pattern instead of the title's plane for the first N presents of the title's own plain present path. The pattern is a 16x9 `R8G8B8A8_UNORM` image of four known vertical bands - (204,51,102), (51,204,102), (51,102,204), (255,255,255), four texels each - filled once from a mapped staging buffer and blitted by the same command buffer, swapchain, format family and capture the title's present uses; the title's own resource is not read and its tracked layout is not touched. Bounded to 8 presents; the resources are made lazily and a device that cannot make them is refused by name once, after which the title's plane is presented as before; unset/empty/non-numeric or out-of-range values are refused by name and the title's plane is presented |
| [port/shims/xbox_present.cpp](../port/shims/xbox_present.cpp), the geometry attestation | `PALMETTO_PRESENT_GEOMETRY=1` prints once the window's client/outer rectangle, the monitor and work area as this process sees them, and once the plane words (count, width, height, format, colour space, scale filter, flags, source rectangle, placement), the swapchain extent, the fit rule and the resulting picture rectangle with its x/y scale. Measurement only: the path answers nothing differently when it is on |
| [port/test/vk_presenter_sync_test.cpp](../port/test/vk_presenter_sync_test.cpp), [port/test/vk_presenter_capture_colour_test.cpp](../port/test/vk_presenter_capture_colour_test.cpp) | the pure rule/parser cases and the GPU cases of 15.3 |

**The switches.** All three are off by default and read once per process. `PALMETTO_PRESENT_FIT` takes
unset/empty/`0`/`off`/`false`/`no`/`fill`/`stretch` as `fill` (the tree's measured path) and
`1`/`on`/`true`/`yes`/`fit`/`aspect`/`letterbox` as `fit`; anything else is refused by name, leaves `fill`, and is printed
once (`PALMETTO_PRESENT_FIT is not fill/off or fit/on: refusing to guess the present geometry`).
`PALMETTO_PRESENT_SYNTHETIC` takes a whole number 1..8; the refusal is
`PALMETTO_PRESENT_SYNTHETIC is not a whole number of presents in 1..8`. `PALMETTO_PRESENT_GEOMETRY=1` (or any value that is
not `0`) turns the two attestation lines on.

**The switch-off proof.** With all three unset the presenter's chosen rule is `fill`, whose destination rectangle is the
whole output and whose `has_bars` is false, so `record()` records exactly the same barriers, blit offsets and filter as
before, the synthetic branch is never taken, and the attestation prints nothing; the only added work is three `getenv`
reads at construction. The A/B for the fit rule is therefore the two arms of 15.4, whose captures are the synthetic
pattern in both, and the GPU test pins the same two rectangles.

### 15.3 Tests

| target | what it covers | result |
|---|---|---|
| `vk_presenter_capture_colour` (`port/test/vk_presenter_capture_colour_test.cpp`) | the pure fit/scale rule and parser: `fill` covers the whole output for three aspect ratios; the measured plane in the measured window fits to 19,0..2545,1421 with 19-pixel bars; exact aspect leaves no bars; a 16:9 source in a 128x96 window becomes 0,12..128,84; a square source is pillarboxed to 96 pixels with 16 on each side; a 4:3 source in the measured window is 1895 wide and centred at 334; every 1..40x1..40 output keeps the picture inside the output with bars within one pixel of each other; a zero, negative or negative-size source keeps the whole-output rectangle (never a guessed geometry); the parser accepts all off/fill and on/fit spellings (case and blanks included) and refuses `2`, `banana`, `fitt`, `offf` and `fit fill` with the named reason while leaving the caller's value alone | the target exits 0 and prints `present fit/scale rule: fill, fit and the switch spellings ok` (47 `EXPECT` checks in the file, most inside the 1..40 loops) |
| `vk_presenter_sync` (`port/test/vk_presenter_sync_test.cpp`, section 2d) | the GPU path through the same `present()` the title uses: a 16:9 source cleared to (51,153,230) covers the whole 128x96 window under `fill` and, under `fit`, sits in 0,12..128,84 with black bars above and below; the synthetic control then presents its four bands through the same call and capture, each band's centre reads its own exact colour (tolerance 2), the title resource's tracked layout is unchanged, `synthetic_ready()` becomes true and the armed count is consumed; the same pattern under `fit` reads inside 0,12..128,84 with a black bar at row 6 | exits 0 after the four section-5 lines; the test's 110 `EXPECT` checks include the new ones |
| the tree's existing presenter tests | `xbox_present` (the whole PresentX/frame-event/window suite), `xs_proxy_mock` (the hook-only proxy path), `frame_pacer` and `unimplemented_policy` | all pass unchanged: `xbox_present_test: all cases pass`, `xs_proxy_mock_test: PASS`, `frame_pacer_test: all cases pass`, `unimplemented_policy_test: 98 checks, 0 failed` |

### 15.4 The arms

Two light boots under the shared lock, one at a time, with the section-12 content environment and
`PALMETTO_TITLE_CLASS0A_SIZE=1` in both, and the arm's own switches added: `PALMETTO_PRESENT_GEOMETRY=1`,
`PALMETTO_PRESENT_SYNTHETIC=8`, `PALMETTO_CAPTURE_EVERY=1`, `PALMETTO_CAPTURE_LIMIT=8`; arm A adds
`PALMETTO_PRESENT_FIT=fill` (the default rule) and arm B `PALMETTO_PRESENT_FIT=fit`. The two binaries differ only by a
printout improvement made between them (the plane line prints the source rectangle and placement numerically in arm B);
the fit/synthetic behaviour is identical. Each arm reached the first present, presented the pattern for its first eight
frames, and then the host dropped the device: one loss in each arm, so the two-loss stop rule ended the boots (no third
arm, no stage-3 window).

| measurement | arm A: rule `fill` | arm B: rule `fit` |
|---|---|---|
| binary (SHA-256 first 16) | `988FAE9EED74D47D` | `4D894B2E2959D64C` |
| limit / verdict | 90 s limit; STOP `game+0x559468F` reading `0x1ea00000008`, exit `0xC0000005`, wall 69.9 s | 90 s limit; STOP `game+0x86F9DF` reading `0x3506e`, exit `0xC0000005`, wall 66.8 s |
| device loss | 1, `[rt] replay: the Vulkan device was lost` at timeline t=69,492.0 ms, before the stop | 1, same line at timeline t=69,605.5 ms, before the stop |
| title window line | client 2564x1421 (aspect 1.804), outer 2580x1460; monitor 2560x1440 at 0,0, work 2560x1392 (the process's DPI-virtualized view) | identical |
| presented plane line | 1 plane, plane 0 3840x2160 format 28 colour space 0 scale filter 0 flags 0x0, source rect and placement set to the whole plane/output; the presenter's swapchain 2564x1421 | the same, with the rectangles printed numerically: source rect 0,0..3840,2160, placement 0,0..1,1 |
| presenter colour line | swapchain 2564x1421, format 44 (`VK_FORMAT_B8G8R8A8_UNORM`), colour space 0, source format family UNORM, HDR10 request no (surface has the format: no), capture on | identical |
| fit rule line / picture rectangle | `fill`: picture 0,0..2564,1421, scale 0.6677 x 0.6579 (the plane stretched to the window) | `fit`: picture 19,0..2545,1421, scale 0.6578 x 0.6579 (aspect preserved, 19-pixel black bars left and right) |
| synthetic control / capture frame count | on, 8 presents; 8 captures, extent 2564x1421, format 44 | on, 8 presents; 8 captures, extent 2564x1421, format 44 |
| captured band means (left to right) | 204,51,102 / 51,204,102 / 51,102,204 / 255,255,255 | 204,51,102 / 51.24,203.76,102 / 51,102,204 / 254.68,254.76,254.92 |
| bars | none (the picture is the whole window) | left and right bars mean `0.00 0.00 0.00` |
| picture mean / whole-frame mean | picture = whole window: `140.25 153.00 165.75` = the four bands' exact mean | picture `140.24 152.94 165.74`; whole frame `138.16 150.67 163.28` (= the picture mean times the 2526/2564 content fraction, within 0.06) |
| frames identical | all 8 captures have the same whole-frame mean | all 8 captures have the same whole-frame mean |
| `PresentX` at the stop | 2,618 ok / 1 failed (the present attempted after the loss) | 1,773 ok / 1 failed (the same) |
| state words at the last sample | at 66.0 s: init 2 (change 20.0 s), loading 2 (64.0 s), session 1 (64.0 s), machine current/next 2/2, boot-media gate 0x2, platform gate 0x101, loading-active 0x1, movie player state 0x3 with `buffers 0`, front-end ready 1 running 1, high-refresh flag 0 refresh 60/1 | at 64.0 s: init 2 (change 30.0 s), loading 2 (62.0 s), session 1 (62.0 s), machine 2/2, the same gate, platform, loading-active, movie and front-end words, high-refresh 0 refresh 60/1 |

The state-word columns are the same words at each arm's last sample; only the change times differ, and those differ by the
arms' own boot timing (the same difference the earlier sections record between arms of one binary). Both arms stop in the
post-device-loss title crash family the page's earlier sections record, one loss each.

### 15.5 Verdict, limits and the next falsifier

**Verdict: the presenter shows known content end to end, and the only measured geometry divergence was real.** The
synthetic pattern reached the presenter capture through the title's own `PresentX` present path with its exact expected
values in both arms (each band's bytes, and the fitted picture's mean), so the blit, the swapchain format family, the
colour-space choice and the capture are not the reason the content frames read black: the content frames' source is black
(`0/8,294,400` non-black in the earlier readbacks). The geometry attestation shows the measured tree stretched a 16:9 plane
into a 1.804-aspect client area (0.6677 x 0.6579) and left no bars, while the opt-in `fit` rule produces a centred
19-pixel-pillarboxed picture (0.6578 x 0.6579). The title state does not move with the rule: the same state words and the
same movie/front-end words at each arm's last sample, and the synthetic control only replaces the presented picture. The
colour-space rule measured as the peers' - the swapchain family follows the plane's format family, and a UNORM-to-UNORM
blit copies values exactly - is recorded as parity, not as a port.

Limits:

- **The control substitutes the source, it does not read the title's resource.** The title's resource identity as the
  presented plane is already measured ([vertex_buffer_va.md](vertex_buffer_va.md) section 10.2, the write control's
  `51 102 204` readback and the movie-frame present test in `port/test/d3d12_boot_movie_test.cpp`); the control proves the
  conversion and capture after that resource, not the resource selection.
- **The fit rule is opt-in and this content proves the geometry, not the picture.** There is no non-black content frame to
  judge, so the default stays `fill` (the tree's measured path, byte for byte). What would make `fit` the default is a boot
  with a real content frame in the window whose correct picture is the plane's aspect ratio and whose title UI is not laid
  out for the stretched window: the same two arms then compare a content capture, not the pattern.
- **No window fitting was implemented.** The attestation measures the title's oversized window (2580x1460 outer on a
  2560x1440 virtualized monitor) and the DPI-unaware process view; resizing the title's own window is a behaviour change
  outside the presenter's present path (five-star fits its own window, not the title's), and the next falsifier for it is
  a boot in which the title's window rectangle is outside the monitor in physical pixels as well.
- **The colour path is attested on this host only.** SDR, 8-bit, UNORM, sRGB-nonlinear output; an HDR10 surface, a
  10-bit swapchain and an sRGB-family source are covered by the compose pass's existing tests, not by these two arms, and
  the next falsifier is the same boot on an HDR10 output where the geometry line must print the ST.2084 format and the
  capture's colour space number must change accordingly.
- **The compose path is not refitted.** A plane with its own placement keeps the title's placement; only the plain blit
  path (the measured path, which this title always takes) gets the rule. A title-presented second plane or an explicit
  placement would need a rule of its own.
- **The two arms lost the host device.** Both losses are the host family the page records, each at about 69.5 s of trace
  clock, after the eight synthetic captures; no stage-3 window exists in this pair and the two-loss rule ended the boots.
  The synthetic captures and the geometry lines are all before the loss and are unaffected.

### 15.6 Files, gates and the index row

- **Files changed by this slice**: the new include-only [port/src/present_fit.h](../port/src/present_fit.h),
  [port/src/vulkan_present.h](../port/src/vulkan_present.h) (the fit rule in `record()`, the swapchain colour/geometry
  attestation in `recreate()`, the synthetic control and its resources, the three switch parsers, the test hooks),
  [port/shims/xbox_present.cpp](../port/shims/xbox_present.cpp) (the window/plane geometry attestation and its
  switch), [port/test/vk_presenter_capture_colour_test.cpp](../port/test/vk_presenter_capture_colour_test.cpp) (the pure
  rule/parser section), [port/test/vk_presenter_sync_test.cpp](../port/test/vk_presenter_sync_test.cpp) (the GPU section 2d)
  and this section of `docs/peer_port_first_ui.md`. No shim interface, device proxy, build file, README or NOTICE file
  changed; `xbox_present.h` and `vulkan_present_compose.h` are unchanged, and the default present path is unchanged when
  the three switches are unset.
- **Gates on this tree**: `_setup\build_gta6rt.cmd` prints `PALMETTO_OK` on the final tree,
  `_setup\build_tests.cmd` prints `TESTS_BUILT`, the GPU-free targets above pass, the two GPU presenter targets pass on the
  RTX 4090 host (`vk_presenter_sync_test: ok`, `xbox_present_test: all cases pass`), and the docs checks run with the
  session's broken `GIT_CONFIG_*` variables cleared. This section names `PALMETTO_PRESENT_FIT`,
  `PALMETTO_PRESENT_GEOMETRY` and `PALMETTO_PRESENT_SYNTHETIC` as whole words, so the environment-variable check finds
  them.
- **Index row**: [docs/README.md](README.md) already carries this page's row; the lead may extend its description with
  "presenter fit/scale/colour-space attestation and a synthetic non-black control (section 15)".
- **Stale neighbour rows**: [first_visible.md](first_visible.md) T3 ("a synthetic non-black present control ... not
  covered") and its D5 row ("the presenter stretches and presents the plane") are now measured by this section; that page
  is outside this slice's write scope, so the lead should refresh those two cells.
- **NOTICE row needed**: the attribution paragraph of 15.7, appended to the existing "Implemented peer ports (reference
  only)" section of [NOTICE.md](../NOTICE.md), with the peer commits refreshed to `ce2b333b` for Five Star Recomp.
- **Evidence (outside the repository)**: `D:\leonida-work\fsrstable\boot-portfit-fill1`, `boot-portfit-fit1`,
  `capture-portfit-fill1` and `capture-portfit-fit1` (stderr/stdout/summary/boot trace/meta plus the eight presenter PPMs
  each) and `D:\leonida-work\portpresentfit\present_fit_arm.py`, the band/bar analysis of those captures. Extents, format
  numbers, words and image means only; no game data and no peer code.
- **No git write**: the lead commits the uncommitted list.

### 15.7 Attribution

This slice was implemented from this project's own measurements (the window, plane and swapchain words above, the two
arms and the capture analysis, and the pure/GPU tests). The behaviour was cross-checked against the public GPL projects
named in section 11, read as evidence only: Five Star Recomp's
[d3d12x-shim.md](https://github.com/ThatRecompDev/five-star-recomp/blob/45071d52/docs/d3d12x-shim.md) at `45071d52`
(Present: an oversized window fitted to the monitor, the plane scaled into the client area as its aspect rectangle over
black bars, and the swap-chain colour space picked from the plane's format family), refreshed at `ce2b333b`; VIctory's
presenter at `63bcca4` and PercyScarlett's present and stretch files at `e44c465` (a console plane presented whole, and the
stretch stand-in) as the other ends of the comparison. No code, comment text, identifier or table from those projects was
copied into this tree: the fit rule, the attestation lines, the synthetic pattern, the resource handling, the switch
parsers and the tests are ours, derived from the measurements above. The peers remain the copyright of their contributors and their licences continue to apply to their own trees.

## 16. The DPI-unaware title window fitted to its work area (P-25)

This section records a separate outer-window decision from section 15's picture fit: section 15 fits pixels *inside* the client area, while this slice optionally fits the adopted title window itself. The port result is intentionally conservative: **the default remains unchanged; only `PALMETTO_WINDOW_FIT=workarea` asks the presenter to fit the measured title window into the nearest monitor's work area.** No process-wide DPI mode, title style, window procedure, title text or visibility is changed.

### 16.1 Measurements and peer divergence

The local measurements below were recorded before this slice. They are not inferred from a peer's output:

| measurement | local evidence |
|---|---|
| title window | `sgaWindowClass`, client 2564x1421 (aspect 1.804), outer 2580x1460 at 0,0; the geometry attestation from section 15 ([section 15.1](#151-what-was-measured-before-the-port), [window.md](window.md) section 5) |
| process view of the display | monitor 2560x1440 at 0,0; work area 2560x1392; this is the Win32 view reported in the boot process and is the coordinate space used by this slice |
| host display and DPI inference | host display 3840x2160 at 150%; compared with the process's 2560x1440 monitor view, the process is DPI-unaware. In this virtualized view the outer window exceeds the monitor by 20x20 pixels. At 150%, 2580x1460 would map to 3870x2190, 30x30 physical pixels past the host output; that is the implied scaling, not a separate post-fit capture |
| style | the section-15 local geometry record did not include `GWL_STYLE`. Five Star Recomp's measurement of the same title class records `WS_OVERLAPPEDWINDOW` ([d3d12x-shim.md](https://github.com/ThatRecompDev/five-star-recomp/blob/ce2b333b/docs/d3d12x-shim.md), refreshed clone `ce2b333b`). This slice therefore gates the change on that style and refuses any other style instead of assuming its frame geometry |
| section-15 picture rule | `PALMETTO_PRESENT_FIT` changes the picture rectangle only; it does not resize the title's outer window. The 20x20 virtual-monitor overrun remains outside that pixel-fit rule |

The local geometry trace now also reports the window style/ex-style and, where Windows exposes them, the window's DPI-awareness context and effective DPI when `PALMETTO_PRESENT_GEOMETRY=1`. Those fields were not captured from a new title boot here; the direct title-window style/context query remains the next boot's falsifier. We did not substitute a monitor/DPI answer in `xgameruntime_offline.cpp`: no consumed display-answer path was measured for this slice, and the measured presenter-side `MonitorFromWindow`/`GetMonitorInfoW` rectangle is sufficient for fitting in the caller's own coordinate view.

The peer trees do not present one shared window model. Five Star Recomp at `ce2b333b` documents an unaware 3840x2160 title window expanding to 5760x3240 at 150% (only its top-left part remains visible), with `WS_OVERLAPPEDWINDOW`; its loader then sets per-monitor-v2 awareness before window creation and the presenter fits the largest 16:9 client into `rcWork` on first present ([DpiAwareness.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/ce2b333b/src/FiveStar.Loader/Host/DpiAwareness.cs), [GameWindowFit.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/ce2b333b/src/FiveStar.Loader/Shims/D3D12X/Present/GameWindowFit.cs)). XCL (`623d02e4`) uses a separate host window and a cloaked frameless 1920x1080 title-window path; its own-window positioning can center or clamp into the monitor work area ([user.rs](https://github.com/xcl-dev/xcl/blob/623d02e4/crates/xcl/src/hle/user.rs), [present.rs](https://github.com/xcl-dev/xcl/blob/623d02e4/crates/xcl/src/hle/d3d12x/present.rs)). VIctory, PercyScarlett and victory-fork create their presenter windows with ordinary overlapped styles and `AdjustWindowRect`; the inspected snippets do not set per-monitor awareness, but they are not proof of each full process DPI context. This port does not import Five Star's process-wide awareness change or its source-plane sizing rule: the title window already exists before this runtime's first present, and the local client/frame measurements give a narrower, testable adjustment.

### 16.2 The independently measured fit rule

[window_fit.h](../port/src/window_fit.h) is a Win32-free integer planner over `RECT`-shaped coordinates. It reads no DPI factor and receives the outer rectangle, the measured client rectangle and `rcWork` from the same process view. For the recorded geometry, the outer/client differences are a 16-pixel horizontal frame and a 39-pixel vertical frame. The 2560x1392 work area leaves at most a 2544x1353 client after those margins; preserving the measured 2564x1421 client aspect gives a planned client 2441x1353 and outer 2457x1392 at (51,0)..(2508,1392). The fitted outer rectangle stays within the reported work area while the client aspect and measured frame margins are preserved (nearest-pixel rounding can differ by one pixel).

At the first PresentX adoption, the runtime applies that planner only when all of these hold: `PALMETTO_PRESENT_ADOPT` remains enabled; the adopted class is exactly `sgaWindowClass`; its style includes the peer-cross-checked `WS_OVERLAPPEDWINDOW` mask; it is not maximized; and its outer/client/nearest-monitor work-area rectangles are valid. The requested move uses `SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE`, so it does not take focus or block a render thread waiting on the window thread. If only position is outside the work area, the planner translates the window minimally; it does not resize a window whose size already fits.

`PALMETTO_WINDOW_FIT` is opt-in. Unset/empty/blank, `off`, `false`, `no`, `0` or `leave` returns before any additional fit-specific geometry query or `SetWindowPos`, keeps the existing default adoption log text, and the Win32 test verifies the outer rectangle is unchanged. `on`, `true`, `yes`, `1`, `fit`, `workarea` or `work-area` requests the work-area rule. A too-long or unknown value is refused by name; the runtime also names refusals for a non-title candidate, a maximized window, an unsupported style, unmeasurable geometry, impossible frame/work-area dimensions, and a failed `SetWindowPos`. The separately named `PALMETTO_PRESENT_FIT` continues to control content scaling only. A fallback `PalmettoPresentX` window is never resized by this option.

### 16.3 Tests and build state

The pure half in `port/test/xbox_present_test.cpp` covers switch parsing and typo refusal, the exact measured geometry above, unchanged fitting rectangles, position-only correction and invalid/frame-too-large refusals. Its Win32 half constructs an oversized test `sgaWindowClass` window, verifies the asynchronous request reaches the computed `rcWork` bounds without destroying the HWND, and confirms a popup-style title-class window is refused unchanged. The default adoption case asserts the HWND remains alive and the default rectangle is unchanged.

The shared tree was rebuilt once the other in-flight slices settled: `PALMETTO_OK` and `TESTS_BUILT` pass, and `xbox_present_test` passes all its cases, including the fit planner and the Win32 window path. The documentation checks pass too. The fit remains opt-in and there is still no title arm with a real content frame to judge it against, so the measured plan is the deliverable and the next falsifier is a real-frame fit arm.

### 16.4 Arms, limits and next falsifier

No title arm was run for P-25. The preceding section-15 slice already consumed its two light arms, and both ended with a host-device loss; its two-loss stop rule has been reached, so another boot would violate the stability gate. The pure and synthetic-window tests exercise the rule without game data; they do not prove that the live title accepts the asynchronous resize or that the DPI context query returns code 0. At a later stable host window, the smallest useful arm is one short A/B: leave `PALMETTO_WINDOW_FIT` unset once, then set it to `workarea`, enable `PALMETTO_PRESENT_GEOMETRY=1`, and compare client/outer bounds, style, DPI-awareness code/DPI and monitor/work-area answers. Stop at the first device loss; do not retry after two total losses.

### 16.5 Attribution and NOTICE row

This slice follows this repository's measurements and its own pure plan. The refreshed clone-head commit authors are ThatRecompDev (Five Star Recomp), Prime1997 (XCL), Marvis (VIctory), rpatty-dev (PercyScarlett) and fxceless (victory-fork). The work-area fit and DPI-awareness comparison were cross-checked as evidence only against Five Star Recomp (GPL-3.0-or-later; [`DpiAwareness.cs`](https://github.com/ThatRecompDev/five-star-recomp/blob/ce2b333b/src/FiveStar.Loader/Host/DpiAwareness.cs), [`GameWindowFit.cs`](https://github.com/ThatRecompDev/five-star-recomp/blob/ce2b333b/src/FiveStar.Loader/Shims/D3D12X/Present/GameWindowFit.cs), and [`d3d12x-shim.md`](https://github.com/ThatRecompDev/five-star-recomp/blob/ce2b333b/docs/d3d12x-shim.md), clone `ce2b333b`); XCL (GPL-2.0-only, [`present.rs`](https://github.com/xcl-dev/xcl/blob/623d02e4/crates/xcl/src/hle/d3d12x/present.rs) and [`user.rs`](https://github.com/xcl-dev/xcl/blob/623d02e4/crates/xcl/src/hle/user.rs), clone `623d02e4`); VIctory (GPL-3.0-or-later, [`host.rs`](https://github.com/Marvisak/VIctory/blob/63bcca43/crates/victory-gpu/src/host.rs), clone `63bcca43`); PercyScarlett (GPL-2.0-only, [`host.rs`](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465e/runtime/victory/crates/victory-gpu/src/host.rs), clone `e44c465e`); and victory-fork (GPL-3.0-or-later, [`host.rs`](https://github.com/fxceless/victory-fork/blob/9a9091d1/crates/victory-gpu/src/host.rs), clone `9a9091d1`). The mechanisms above are described from their public evidence, not copied; this implementation's switch parser, refusal set, frame-preserving planner, style gate and tests are independently written from the local geometry. Add a matching attribution row to the existing "Implemented peer ports (reference only)" section of [NOTICE.md](../NOTICE.md); no NOTICE file was changed in this slice.

## 17. The opt-in per-origin PresentX pacing extension

**Status:** implementation and test sources added; the lead's serialized full gate is pending. The option remains off by default. No title boot or game data was used for this slice, and no git write was made.

### 17.1 The remaining gap, measured from this tree's state machine

The source-level baseline in [frame_pacer.h](../port/src/frame_pacer.h) and [xbox_present.cpp](../port/shims/xbox_present.cpp) had four coupled limits: `PresentX` refused planes whose `Token` differed; `commit_present_token(token, plane_count, ...)` ignored `plane_count`; present-driven flights were calculated as `issued_ - flipped_`; and frame statistics always returned plane index 0. One global present cadence also mixed all tokened presents.

The false-free-slot gap is numerically reproducible from those existing branches. With tokens 1, 2 and 3 issued and token 1 committed, `issued_ - flipped_` reports 2 flights. A later commit of token 3 advances the single `flipped_` watermark to 3, so the old calculation reports 0 even though token 2 has never been committed. The new pure test records the before/after sequence: legacy switch-off is 2 then 0; exact per-origin accounting is 2 then 1 after token 3, and 0 only when token 2 is also committed. This is a state-machine measurement, not a console or title measurement.

| surface | existing behavior | effect of the gap |
|---|---|---|
| token validation | all planes must carry plane 0's token; each token is checked against one global last-presented watermark | independently issued origins cannot be represented on two planes |
| flight count | one high-watermark subtraction (`issued_ - flipped_`) | out-of-order completion of a newer origin falsely retires every older origin below it |
| statistics | `plane_count` is ignored and each returned record has `plane_index == 0` | plane identity is lost even when a token is valid |
| cadence | one global `last_present_us_` and `cadence_us_` | one plane's timing can influence the other plane's pacing |

### 17.2 The opt-in implementation

`PALMETTO_PRESENT_PER_ORIGIN` is a separate opt-in and is accepted only with `PALMETTO_PRESENT_DRIVEN=1`. It accepts case-insensitive `on`, `1`, `true` or `yes`; unset, blank, `off`, `0`, `false` or `no` keeps the default path. Unknown or overlong values are refused by the named switch; enabling it without `PALMETTO_PRESENT_DRIVEN` logs the requirement and retains legacy behavior. The old single-token validation, commit, flight-count and trace branches remain selected when this option is off.

When enabled, the pacer tracks exact outstanding token origins (up to 64, matching its statistics retention window); a successful present removes only the tokens it names, even if a later token already flipped. At capacity, the oldest flight is evicted; the overflow counter is logged on the next successful tokened present and included in `PALMETTO_PRESENT_PACING=1` output. This bound prevents unbounded state if a title keeps acquiring tokens without presenting.

The presenter validates each plane token against that plane's presented-token high-water mark and still rejects tokens the pacer never issued. The two-plane limit and `ResourceCount == 1` rule remain. A single host present uses the greatest lateness among its distinct plane origins for its tear decision. Per-plane cadence samples use the existing 1/8 smoothing and declared-period bounds; the shared origin stream follows the slowest active plane. A held-wait dead-man still releases after four observed cadences, but it does not silently consume a different outstanding token. `GetFrameStatisticsX` reports the last plane that named a token; if the same token is carried on both planes, its one record's plane index is the final plane index.

### 17.3 Tests and gates

* [frame_pacer_test.cpp](../port/test/frame_pacer_test.cpp) adds parser spelling/refusal and present-driven prerequisite cases, the numerical legacy false-free-slot case, independent token retirement, per-plane statistics and cadence samples, a mixed-plane tear-timing check, a dead-man release that preserves older origins, and the 64-entry overflow counter. These are pure fake-clock tests.
* [xbox_present_test.cpp](../port/test/xbox_present_test.cpp) retains the legacy default assertion that differing plane tokens return `E_INVALIDARG`; it adds a separate fake-clock, Vulkan-backed `PresentX` case with tokens 4 and 3 on planes 0 and 1, validates per-token plane statistics, checks that an unissued token still returns the named `E_INVALIDARG` refusal, and captures the opaque overlay pixels.
* [vk_presenter_sync_test.cpp](../port/test/vk_presenter_sync_test.cpp) now queues a two-texture composition behind held GPU work and checks the second plane's captured colour after the queue is released. This verifies the presenter path/order, not token validation.

Per the lead's serialized build instruction, no build, relink, test executable, docs test or title arm was run in this slice; the full gate runs after slices report. Thus `PALMETTO_OK`, `TESTS_BUILT`, and GPU test results are pending, not claimed. No game data was read or changed.

### 17.4 Limits and next falsifier

This closes a local state-machine gap, not a console measurement. The title's actual per-plane token cadence, the console's behavior for mixed plane tokens, and its stale/repeated-token semantics remain unmeasured. The integration test source covers the GPU present/capture shape, but its result is pending the lead's gate and depends on host GPU availability. The queue's 64-origin overflow deliberately drops its oldest entry and is observable via the counter/trace; it is a safety bound, not evidence that a real title reaches it. There is no title boot, so this slice makes no claim that the new handshake advances a first-run or media state.

### 17.5 Attribution and NOTICE row draft

The implementation is independently structured from this tree's state-machine gap and tests; peer code, comments, identifiers and measurement tables were not copied. The behavioral cross-checks were the per-frame/per-plane present notes in Five Star Recomp (`34b4dfed`, GPL-3.0-or-later), the token/flip clock in VIctory (`63bcca4`, GPL-3.0-or-later), XCL's frame/present token handling (`623d02e`, GPL-2.0-only), PercyScarlett's vsync-present pacing (`e44c465`, GPL-2.0-only), and the inherited frame path in victory-fork (`9a9091d`, GPL-3.0-or-later); see also the existing P1 attribution in section 12.7.

**Suggested NOTICE row (not applied; NOTICE.md is outside this slice's write scope):** “The opt-in per-origin PresentX pacing extension ([docs/peer_port_first_ui.md](peer_port_first_ui.md), section 17) was informed by Five Star Recomp (`34b4dfed`, GPL-3.0-or-later) per-plane pacing notes; VIctory (`63bcca4`, GPL-3.0-or-later) frame-token handling; XCL (`623d02e`, GPL-2.0-only) frame/present handling; PercyScarlett (`e44c465`, GPL-2.0-only) vsync-present pacing; and victory-fork (`9a9091d`, GPL-3.0-or-later) frame-clock evidence. No source, comment, identifier, or measurement table was copied; the implementation and tests are original to this repository.”

This slice changes [frame_pacer.h](../port/src/frame_pacer.h), [xbox_present.h](../port/shims/xbox_present.h), [xbox_present.cpp](../port/shims/xbox_present.cpp), the three tests above, and this page only. It does not change `NOTICE.md`, `README.md` or CMake. The lead owns the NOTICE/index updates and the serialized validation gate.

## 18. Bounded timed first-run input; live screen signatures refused (P-02)

**Status:** the timed-only script and synthetic matcher are implemented. This is the input half of section 7's P5 plan, narrowed deliberately: the measured presenter path supplies no frame-signature feed to GameInput, so the live `screen:` trigger is refused rather than guessed. No first-run arm was run for this slice, no game data was read, and no Git history/index write was made; the source and documentation edits remain uncommitted. The lead's serialized post-fix gate remains pending.

### 18.1 The input answers and first-run timeline already measured

| evidence | observed answer or timeline |
|---|---|
| No paired device ([input_backend.md](input_backend.md), section 5, arm A) | `GameInputCreate` at +2.33 s and `RegisterDeviceCallback` both answer `S_OK`; over 130 s the title makes no reading call, does not ask for `GetGamepadState`, and makes no device-method call. The absence of a pad means the title never reaches its first input read. |
| Paired neutral pad ([decomp_first_ui.md](decomp_first_ui.md), section 12.3) | The title enumerates one neutral pad; at +24.75 s `GetCurrentReading(Gamepad)` answers `S_OK` and `GetGamepadState` returns `true`, with no script set. `XGameSaveInitializeProvider` also answers `S_OK` at +24.75 s. The trace records the successful read, not the returned button bits. |
| Prior timed-input measurement ([input_backend.md](input_backend.md), section 5, arm B-long) | `GameInputCreate` is +2.39 s; the first read is +25.44–25.46 s (`S_OK` / `true`), before the configured `PALMETTO_FAKE_INPUT=100` presses begin at +100 s. Both pads are resting at that read and no button values are logged, so this establishes a read window, not an accepted press. The trace is bounded and records fewer than ten reading calls in the full 220 s arm. |
| Screen-owner probes ([decomp_first_ui.md](decomp_first_ui.md), section 17.6) | Accessibility executes once at about +20 s (`long2`) / +16 s (`input1`); the sign-in owner executes once at +22 s / +18 s, with `XUserAddAsync`/`XUserAddResult` at +24.57 s / +20.24 s. Profile-name text entry and profile card each have zero hits. |
| State and press outcome ([decomp_first_ui.md](decomp_first_ui.md), sections 12.3 and 17.6–17.7) | The clean default boot remains `init=2`, `loading=1`, `session=0` for 300 s with black captures; the later `long2`/`input1` arms reach stage 3 but remain black. `input1` used `PALMETTO_FAKE_INPUT=70` and stopped at +58.1 s on a host device loss, before the first press, so no key effect or input-driven state advance was measured. |

The +20.55 s read in the earlier section-12.1 arm and the later +24.75 / +25.44 s reads are boot-speed variation, not one stable poll clock. The title's `GetGamepadState` success is known, but its actual button state and any response to a press are not. No screen transition is credited to input.

### 18.2 The opt-in timed script

`PALMETTO_FIRST_RUN_SCRIPT=<seconds>` configures one bounded schedule relative to the GameInput backend-initialization epoch (the first shim initialization / `GameInputCreate` path), not the time a synthetic pad later attaches. The value is read once at backend initialization. Unset, empty, or exact `off` disables it; `0` starts immediately. Only a plain non-negative decimal delay is accepted: 0–300 seconds and at most 31 characters. Signs, exponents, other text, a delay over 300 seconds, and a value too long for the environment buffer are refused by name.

The script is fixed: A, Menu, A, Menu, starting at the requested delay and repeating every 500 ms. Each button is held for 250 ms and explicitly released for the next 250 ms; the last release ends the sequence at delay +1.75 s. The script pad remains attached neutral after completion. It cannot accept arbitrary keys, screen templates, or caller-defined durations. If any nonempty, non-`off` `PALMETTO_FAKE_INPUT` value is also configured, the new script is refused with a named legacy-schedule conflict; no second script pad is added. The existing `PALMETTO_FAKE_INPUT` behavior remains unchanged when the new switch is unset.

The same bounded schedule is implemented in the SDL and always-built no-SDL input paths using the existing backends; it adds no SDL dependency. With the new switch unset, the SDL path adds no synthetic device and the no-SDL path retains its prior neutral default pad and answers—there are no added presses or altered snapshots. The only new side effect is the opt-in schedule.

### 18.3 Signature matcher, named limits, and honest refusal

[`fake_input.h`](../port/src/fake_input.h) contains the pure `ScreenSignature` helper: exactly 32×18 8-bit luma samples, compared by mean absolute error with a maximum tolerance of 32 levels. The test uses only synthetic arrays. The matcher names null samples, wrong sample count, and overly loose tolerance; the script parser separately names off/empty, invalid delay, length and 300-second limits, screen-feed unavailability, and conflict with the legacy schedule.

A `screen:` value is explicitly refused as `SignatureFeedUnavailable`: this tree has no measured presented-frame feed of the required shape wired to the input shim. The helper is not called by the live script, contains no title signature, and does not capture or read pixels. A future screen-trigger mode needs a measured presenter-to-input sample path before it can be enabled.

### 18.4 Tests, arm readiness, and next falsifier

[`active_input_test.cpp`](../port/test/active_input_test.cpp) covers the parser/refusals, held/released state machine, and synthetic signature matcher; the lead's gate reported 12/12 cases passing. [`gameinput_nosdl_test.cpp`](../port/test/gameinput_nosdl_test.cpp) adds a GPU-free `GameInputCreate` ABI path checking neutral → A → release → Menu → neutral. A targeted rebuild of that test after the clock correction passed 10/10 cases, and ten consecutive executions of the resulting binary also passed 10/10. A controlled replay of the old future-clock schedule reproduced the failure: `GetCurrentReading` returned `S_OK`, `GetGamepadState` returned true, but at the A hold the check saw expected `0x04`, actual `0x00`; elapsed was 1 µs, step `-1`, and both scheduled buttons and pad buttons were `0x00`. The test had supplied `poll_at(epoch + elapsed)` timestamps in the future, then `GetCurrentReading`'s `poll_if_due` re-polled at QPC time; unsigned elapsed underflow triggered that poll, `now < epoch` clamped the schedule to zero, and neutral replaced A. The fix uses monotone real `now_us()` poll timestamps and shifts only `epoch_us` to simulate each bounded step. Failure diagnostics capture expected/actual buttons, reading timestamp, epoch, poll elapsed, scheduled step, and pad state. The focused result resolves the no-SDL harness failure; the project-wide gate remains with the lead. No `palmetto` target was built or relinked for this repair. The configured local build has no SDL3 package, so the optional SDL adapter changes were not compiled in this environment.

No task-197 arm was run. `luna2-boots` reported two consecutive device losses on its P-31 overlap-on retry; the two-loss stop rule fired and its lock is now released, but host stability is not established. No further arm is appropriate in this series. After a separate stable-host check and lead authorization, the next falsifier is one light diagnostic with `PALMETTO_FIRST_RUN_SCRIPT=21.85` if the backend epoch is the measured `GameInputCreate`: that offset places the 250 ms pulses over the measured +24.75/+25.44 s read windows. Record the exact backend epoch, `GetCurrentReading` time and `GetGamepadState` button bits, plus the screen-owner hits, save-provider result, state words and presenter captures. If the read sees the intended held button but the owner/state/pixels do not move, input alone is not the gate. If the read is neutral, the epoch or timing estimate is falsified; adjust only from the measured same-arm clock delta. Do not retry after further device loss.

### 18.5 Peer credit and NOTICE row draft

This implementation follows Palmetto's own call/timeline measurements and reimplements the timed schedule, parser, refusal set, state machine, and test matcher. The behavior comparisons are evidence only: Five Star Recomp's [`first-run.input`](https://github.com/ThatRecompDev/five-star-recomp/blob/34b4dfed/docs/first-run.input), [`InputScriptWatcher.cs`](https://github.com/ThatRecompDev/five-star-recomp/blob/34b4dfed/src/FiveStar.Loader/Shims/Input/InputScriptWatcher.cs) and [`ScreenSignature.cs`](https://github.com/ThatRecompDev/five-star-recomp/blob/34b4dfed/src/FiveStar.Loader/Shims/Input/ScreenSignature.cs) (GPL-3.0-or-later); XCL's [`gameinput.rs`](https://github.com/xcl-dev/xcl/blob/623d02e/crates/xcl/src/hle/gameinput.rs) (GPL-2.0-only); VIctory's [`gameinput.rs`](https://github.com/Marvisak/VIctory/blob/63bcca4/crates/victory-hle/src/modules/gameinput.rs) (GPL-3.0-or-later); victory-fork's [README](https://github.com/fxceless/victory-fork/blob/9a9091d/README.md), which records the inherited input path (GPL-3.0-or-later); and PercyScarlett's [`vpad.rs`](https://github.com/rpatty-dev/PercyScarlett/blob/e44c465/crates/gameinput/src/vpad.rs) (GPL-2.0-only). No peer code, comments, identifiers, signatures, or game data were copied.

**Suggested NOTICE row (not applied; `NOTICE.md` is outside this task's write scope):** “The bounded first-run input slice ([docs/peer_port_first_ui.md](peer_port_first_ui.md), section 18) was informed as behavior evidence only by Five Star Recomp (`34b4dfed`, GPL-3.0-or-later; first-run input and signature-watcher documentation), XCL (`623d02e`, GPL-2.0-only; GameInput), VIctory (`63bcca4`, GPL-3.0-or-later; GameInput), victory-fork (`9a9091d`, GPL-3.0-or-later; inherited VIctory input), and PercyScarlett (`e44c465`, GPL-2.0-only; virtual pad). The local timed parser, bounded state machine, named refusal set, and synthetic matcher are independently written; no peer source or game data was copied.”
