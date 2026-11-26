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

